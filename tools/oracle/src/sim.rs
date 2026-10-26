//! Random chains through THEIR State (coinshift-rs v0.14.8): blocks built by their types and
//! signing code, with swaps (create/accept/claim, open and pre-specified, expiry, node-local
//! L1 updates and cancels), deposits, withdrawals, bundle events, mutated/invalid blocks,
//! mempool verdicts and disconnects. One JSON line per step with their verdict and digest;
//! tests/test_coinshift_sim.cpp replays the steps through ours.
use std::{
    collections::{BTreeMap, HashMap},
    io::Write,
    panic::{AssertUnwindSafe, catch_unwind},
};

use bitcoin::hashes::Hash as _;
use coinshift::{
    authorization::{self, BatchVerificationContext, SigningKey, VerifyingKey},
    state::State,
    types::{
        Accumulator, Address, AuthorizedTransaction, Body, Header, OutPoint,
        Output, OutputContent, ParentChainType, PointedOutput, SwapId, SwapState,
        SwapTxId, Transaction, TxData, hash, proto::mainchain,
    },
};
use fallible_iterator::FallibleIterator as _;
use serde_json::json;

use crate::{digest::state_digest, open_env};

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: u64) -> u64 {
        if n == 0 { 0 } else { self.next() % n }
    }
    fn chance(&mut self, pct: u64) -> bool {
        self.below(100) < pct
    }
}

fn key(i: u8) -> SigningKey {
    let mut b = [0u8; 32];
    b[0] = i.wrapping_mul(7).wrapping_add(3);
    b[1] = i;
    SigningKey::from_scalar(curve25519_dalek::Scalar::from_bytes_mod_order(b)).unwrap()
}

fn addr_of(k: &SigningKey) -> Address {
    authorization::get_address(VerifyingKey::from(k))
}

const MAIN_ADDRS: [&str; 3] = [
    "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
    "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2",
    "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx",
];
const CHAINS: [ParentChainType; 3] = [ParentChainType::Regtest, ParentChainType::Signet, ParentChainType::BTC];

struct Sim {
    rng: Rng,
    keys: Vec<SigningKey>,
    addrs: Vec<Address>,
    env: sneed::Env<coinshift::heed::WithoutTls>,
    state: State,
    ctxt: BatchVerificationContext,
    /// per block hash: accumulator (archive), header, body, window
    accs: HashMap<coinshift::types::BlockHash, Vec<u8>>,
    chain: Vec<(Header, Body, mainchain::TwoWayPegData, serde_json::Value)>,
    deposit_seq: u64,
    main_counter: u64,
    out: std::io::StdoutLock<'static>,
}

fn acc_bytes(a: &Accumulator) -> Vec<u8> {
    let mut v = Vec::new();
    a.0.serialize(&mut v).unwrap();
    v
}

impl Sim {
    fn digest(&self) -> String {
        let rotxn = self.env.read_txn().unwrap();
        state_digest(&self.state, &rotxn)
    }

    fn utxos(&self) -> Vec<(OutPoint, Output)> {
        let rotxn = self.env.read_txn().unwrap();
        let mut v = Vec::new();
        let mut it = self.state.utxos.iter(&rotxn).unwrap();
        while let Some((k, o)) = it.next().unwrap() {
            v.push((k.into(), o));
        }
        v
    }

    fn locked(&self, op: &OutPoint) -> Option<SwapId> {
        let rotxn = self.env.read_txn().unwrap();
        self.state.is_output_locked_to_swap(&rotxn, op).unwrap()
    }

    fn key_for(&self, a: &Address) -> Option<usize> {
        self.addrs.iter().position(|x| x == a)
    }

    fn rand_addr(&mut self) -> Address {
        let i = self.rng.below(self.addrs.len() as u64) as usize;
        self.addrs[i]
    }

    fn main_hash(&mut self) -> bitcoin::BlockHash {
        self.main_counter += 1;
        let mut b = [0u8; 32];
        b[..8].copy_from_slice(&self.main_counter.to_le_bytes());
        b[31] = 0x5a;
        bitcoin::BlockHash::from_byte_array(b)
    }

    fn sign(&mut self, tx: Transaction, signer_override: Option<usize>) -> AuthorizedTransaction {
        // sign each input with the owner key (or a chosen key)
        let rotxn = self.env.read_txn().unwrap();
        let msg = borsh::to_vec(&tx).unwrap();
        let mut auths = Vec::new();
        for (op, _) in &tx.inputs {
            let k = match signer_override {
                Some(i) => i,
                None => {
                    let o = self.state.utxos.try_get(&rotxn, &coinshift::types::OutPointKey::from(op)).unwrap();
                    o.and_then(|o| self.key_for(&o.address)).unwrap_or(0)
                }
            };
            let sk = &self.keys[k];
            auths.push(authorization::Authorization {
                verifying_key: VerifyingKey::from(sk),
                signature: sk.sign(rand::rng(), &msg),
            });
        }
        drop(rotxn);
        AuthorizedTransaction { transaction: tx, authorizations: auths }
    }

    fn with_proof(&self, mut tx: Transaction) -> Transaction {
        let rotxn = self.env.read_txn().unwrap();
        let _ = self.state.regenerate_proof(&rotxn, &mut tx);
        tx
    }

    /// A random transaction (often valid, sometimes mutated); None if nothing to spend.
    fn random_tx(&mut self, height: u32) -> Option<AuthorizedTransaction> {
        let utxos = self.utxos();
        if utxos.is_empty() {
            return None;
        }
        let rotxn = self.env.read_txn().unwrap();
        let swaps = self.state.load_all_swaps(&rotxn).unwrap();
        drop(rotxn);
        let pick = |rng: &mut Rng, n: usize| rng.below(n as u64) as usize;
        let kind = self.rng.below(100);
        let mut ins: Vec<(OutPoint, Output)> = Vec::new();
        let mut signer = None;
        let mut data = TxData::Regular;
        let mut outputs: Vec<Output> = Vec::new();
        // spendable non-withdrawal coins
        let spendable: Vec<_> = utxos.iter().filter(|(_, o)| !o.content.is_withdrawal()).cloned().collect();
        if spendable.is_empty() {
            return None;
        }
        let n_in = 1 + self.rng.below(2) as usize;
        for _ in 0..n_in {
            let c = spendable[pick(&mut self.rng, spendable.len())].clone();
            if !ins.iter().any(|(o, _)| *o == c.0) || self.rng.chance(3) {
                ins.push(c);
            }
        }
        let total: u64 = ins.iter().map(|(_, o)| coinshift::types::GetValue::get_value(o).to_sat()).fold(0u64, |a, b| a.saturating_add(b));
        let fee = if self.rng.chance(50) { 0 } else { self.rng.below(total / 10 + 1) };
        let avail = total.saturating_sub(fee);
        if kind < 30 {
            // transfer
            let parts = 1 + self.rng.below(3);
            let mut left = avail;
            for i in 0..parts {
                let v = if i + 1 == parts { left } else { self.rng.below(left + 1) };
                left -= v;
                let a = self.rand_addr();
                outputs.push(Output { address: a, content: OutputContent::Value(bitcoin::Amount::from_sat(v)) });
            }
        } else if kind < 40 {
            // withdrawal
            let mf = self.rng.below(avail / 4 + 1);
            let v = avail - mf;
            let a = self.rand_addr();
            let ma = MAIN_ADDRS[pick(&mut self.rng, 3)].parse().unwrap();
            outputs.push(Output {
                address: a,
                content: OutputContent::Withdrawal { value: bitcoin::Amount::from_sat(v), main_fee: bitcoin::Amount::from_sat(mf), main_address: ma },
            });
        } else if kind < 60 {
            // swap create
            let pc = CHAINS[pick(&mut self.rng, 3)];
            let open = self.rng.chance(50);
            let rec = if open { None } else { Some(self.rand_addr()) };
            let l2 = if self.rng.chance(5) { 0 } else { 1 + self.rng.below(avail.max(1)) };
            let l1a = 1 + self.rng.below(100_000);
            let l1r = format!("l1-recipient-{}", self.rng.below(5));
            let sender = ins[0].1.address;
            let mut sid = SwapId::from_l2_to_l1(&l1r, bitcoin::Amount::from_sat(l1a), &sender, rec.as_ref());
            if self.rng.chance(5) {
                sid.0[0] ^= 1;
            }
            let escrow = if self.rng.chance(5) { l2 / 2 } else { l2 };
            let esc_addr = rec.unwrap_or(sender);
            outputs.push(Output { address: esc_addr, content: OutputContent::SwapPending { value: bitcoin::Amount::from_sat(escrow.min(avail)), swap_id: sid.0 } });
            let a = self.rand_addr();
            outputs.push(Output { address: a, content: OutputContent::Value(bitcoin::Amount::from_sat(avail.saturating_sub(escrow.min(avail)))) });
            data = TxData::SwapCreate {
                swap_id: sid.0,
                parent_chain: pc,
                l1_txid_bytes: vec![0u8; 32],
                required_confirmations: pc.default_confirmations(),
                l2_recipient: rec,
                l2_amount: l2,
                l1_recipient_address: l1r,
                l1_amount: l1a,
            };
        } else if kind < 72 && !swaps.is_empty() {
            // swap accept (claimer = owner of the first input, sometimes not)
            let s = &swaps[pick(&mut self.rng, swaps.len())];
            let claimer = if self.rng.chance(85) { ins[0].1.address } else { self.rand_addr() };
            let a = self.rand_addr();
            outputs.push(Output { address: a, content: OutputContent::Value(bitcoin::Amount::from_sat(avail)) });
            data = TxData::SwapAccept { swap_id: s.id.0, l2_claimer_address: claimer };
        } else if kind < 90 && !swaps.is_empty() {
            // swap claim: spend locked outputs of a swap
            let s = swaps[pick(&mut self.rng, swaps.len())].clone();
            let mut claim_ins: Vec<(OutPoint, Output)> = utxos
                .iter()
                .filter(|(op, _)| self.locked(op) == Some(s.id))
                .cloned()
                .collect();
            if claim_ins.is_empty() || self.rng.chance(5) {
                claim_ins = vec![ins[0].clone()];
            }
            if self.rng.chance(15) {
                // add another SwapPending output (any swap, locked or not)
                if let Some(c) = utxos.iter().find(|(_, o)| o.content.is_swap_pending()) {
                    claim_ins.push(c.clone());
                }
            }
            ins = claim_ins;
            let total: u64 = ins.iter().map(|(_, o)| coinshift::types::GetValue::get_value(o).to_sat()).fold(0u64, |a, b| a.saturating_add(b));
            let rotxn = self.env.read_txn().unwrap();
            let res = self.state.get_swap_reservation(&rotxn, &s.id).unwrap();
            drop(rotxn);
            let recipient = s.l2_recipient.or(res.map(|r| r.claimer)).unwrap_or_else(|| self.rand_addr());
            let pay_to = if self.rng.chance(85) { recipient } else { self.rand_addr() };
            let pay = if self.rng.chance(85) { total } else { total / 2 };
            outputs.push(Output { address: pay_to, content: OutputContent::Value(bitcoin::Amount::from_sat(pay)) });
            let declared = match self.rng.below(3) {
                0 => None,
                1 => Some(recipient),
                _ => Some(self.rand_addr()),
            };
            data = TxData::SwapClaim { swap_id: s.id.0, l2_claimer_address: declared, proof_data: None };
            signer = Some(self.rng.below(self.keys.len() as u64) as usize);
        } else {
            // regular spend of anything (incl. locked or SwapPending outputs)
            let any = utxos[pick(&mut self.rng, utxos.len())].clone();
            ins = vec![any];
            let total = coinshift::types::GetValue::get_value(&ins[0].1).to_sat();
            let a = self.rand_addr();
            outputs.push(Output { address: a, content: OutputContent::Value(bitcoin::Amount::from_sat(total)) });
        }
        let _ = height;
        let mut inputs: Vec<(OutPoint, coinshift::types::Hash)> =
            ins.iter().map(|(op, o)| (*op, hash(&PointedOutput { outpoint: *op, output: o.clone() }))).collect();
        if self.rng.chance(3) && !inputs.is_empty() {
            inputs[0].1[0] ^= 1; // wrong utxo hash
        }
        let tx = Transaction { inputs, proof: Default::default(), outputs, data };
        let tx = self.with_proof(tx);
        let mut at = self.sign(tx, signer);
        if self.rng.chance(3) && !at.authorizations.is_empty() {
            let k = self.rng.below(self.keys.len() as u64) as usize;
            at.authorizations[0].verifying_key = VerifyingKey::from(&self.keys[k]); // signature for another key
        }
        Some(at)
    }

    fn window(&mut self, height: u32) -> (mainchain::TwoWayPegData, serde_json::Value) {
        let mut twpd = mainchain::TwoWayPegData::default();
        let mut wj = Vec::new();
        let nblocks = 1 + self.rng.below(2);
        for _ in 0..nblocks {
            let mh = self.main_hash();
            let mut events = Vec::new();
            let mut ej = Vec::new();
            if self.rng.chance(35) {
                let n = 1 + self.rng.below(2);
                for _ in 0..n {
                    let a = self.rand_addr();
                    let addr_str = if self.rng.chance(50) { a.format_for_deposit() } else if self.rng.chance(90) { a.to_string() } else { "garbage".into() };
                    let addr_hex = hex::encode(addr_str.as_bytes());
                    let txid = format!("{:064x}", self.rng.next() as u128 * 7919 + height as u128);
                    let v = 1 + self.rng.below(1_000_000);
                    let seq = self.deposit_seq;
                    self.deposit_seq += 1;
                    events.push(crate::deposit_event(seq, &txid, 0, &addr_hex, v));
                    ej.push(json!({"kind":"deposit","seq":seq,"txid":txid,"vout":0,"address":addr_hex,"value":v}));
                }
            }
            if self.rng.chance(20) {
                // a bundle event for the pending bundle, a known one, or an unknown m6id
                let rotxn = self.env.read_txn().unwrap();
                let pending = self.state.get_pending_withdrawal_bundle(&rotxn).unwrap().map(|(b, h)| (b.compute_m6id(), h));
                let submitted: Vec<String> = {
                    let mut v = Vec::new();
                    let mut it = self.state.stxos.iter(&rotxn).unwrap();
                    while let Some((_, so)) = it.next().unwrap() {
                        if let coinshift::types::InPoint::Withdrawal { m6id } = so.inpoint {
                            v.push(m6id.0.to_string());
                        }
                    }
                    v
                };
                drop(rotxn);
                let (m6id, st) = match pending {
                    Some((m, h)) if h + 1 == height && self.rng.chance(85) => (m.0.to_string(), "submitted"),
                    _ if !submitted.is_empty() && self.rng.chance(70) => {
                        let m = submitted[self.rng.below(submitted.len() as u64) as usize].clone();
                        (m, if self.rng.chance(50) { "confirmed" } else { "failed" })
                    }
                    Some((m, _)) if self.rng.chance(10) => (m.0.to_string(), "submitted"),
                    _ => (format!("{:064x}", self.rng.next()), ["submitted", "confirmed", "failed"][self.rng.below(3) as usize]),
                };
                events.push(crate::bundle_event(&m6id, st));
                ej.push(json!({"kind":"bundle","m6id":m6id,"status":st}));
            }
            if !events.is_empty() {
                twpd.block_info.replace(mh, mainchain::BlockInfo { bmm_commitment: None, events });
                wj.push(json!({"hash": mh.to_string(), "events": ej}));
            }
        }
        (twpd, serde_json::Value::Array(wj))
    }

    fn emit(&mut self, v: serde_json::Value) {
        writeln!(self.out, "{}", v).unwrap();
    }

    fn block_step(&mut self) {
        let rotxn = self.env.read_txn().unwrap();
        let tip = self.state.try_get_tip(&rotxn).unwrap();
        let height = self.state.try_get_height(&rotxn).unwrap().map_or(0, |h| h + 1);
        let acc_now = self.state.get_accumulator(&rotxn).unwrap();
        drop(rotxn);
        // transactions: random, then mempool verdicts, valid ones into the block
        let mut txs: Vec<AuthorizedTransaction> = Vec::new();
        let ntx = self.rng.below(4);
        for _ in 0..ntx {
            let Some(at) = self.random_tx(height) else { break };
            let rotxn = self.env.read_txn().unwrap();
            let verdict = catch_unwind(AssertUnwindSafe(|| self.state.validate_transaction(&rotxn, &self.ctxt, &at)));
            drop(rotxn);
            let txhex = hex::encode(bincode::serialize(&at).unwrap());
            match verdict {
                Ok(Ok(fee)) => {
                    self.emit(json!({"op":"tx","tx":txhex,"expect":"ok","fee":fee.to_sat()}));
                    if !txs.iter().any(|t| t.transaction.inputs.iter().any(|i| at.transaction.inputs.iter().any(|j| j.0 == i.0))) || self.rng.chance(10) {
                        txs.push(at);
                    }
                }
                Ok(Err(e)) => {
                    self.emit(json!({"op":"tx","tx":txhex,"expect":"reject","err":format!("{e:#}")}));
                    if self.rng.chance(15) {
                        txs.push(at); // invalid tx in a block
                    }
                }
                Err(_) => self.emit(json!({"op":"tx","tx":txhex,"expect":"panic"})),
            }
        }
        // fill + merkle root + roots
        let rotxn = self.env.read_txn().unwrap();
        let mut filled = Vec::new();
        let mut fill_ok = true;
        for t in &txs {
            match self.state.fill_authorized_transaction(&rotxn, t.clone()) {
                Ok(f) => filled.push(f.transaction),
                Err(_) => fill_ok = false,
            }
        }
        drop(rotxn);
        let fees: u64 = filled.iter().filter_map(|f| f.get_fee().ok()).map(|a| a.to_sat()).fold(0, |a, b| a.saturating_add(b));
        let mut coinbase = Vec::new();
        if fees > 0 || self.rng.chance(5) {
            let extra = if self.rng.chance(5) { 1 } else { 0 };
            let a = self.rand_addr();
            coinbase.push(Output { address: a, content: OutputContent::Value(bitcoin::Amount::from_sat(fees.saturating_add(extra))) });
        }
        let mut acc = acc_now;
        let (merkle_root, roots) = if fill_ok {
            match catch_unwind(AssertUnwindSafe(|| Body::modify_memforest(&coinbase, &filled, &mut acc.0))) {
                Ok(Ok(mr)) => (mr, acc.get_roots()),
                _ => (Default::default(), Vec::new()),
            }
        } else {
            (Default::default(), Vec::new())
        };
        let body = Body::new(txs, coinbase);
        let mut header = Header { merkle_root, prev_side_hash: tip, prev_main_hash: self.main_hash(), roots };
        if self.rng.chance(2) {
            header.roots.pop();
        }
        let (twpd, wj) = self.window(height);
        let blockhex = hex::encode(bincode::serialize(&(&header, &body)).unwrap());
        let mut rwtxn = self.env.write_txn().unwrap();
        let res = catch_unwind(AssertUnwindSafe(|| -> Result<(), coinshift::state::Error> {
            let pre = self.state.prevalidate_block(&rwtxn, &self.ctxt, &header, &body)?;
            let _ = self.state.connect_prevalidated_block(&mut rwtxn, &header, &body, pre)?;
            self.state.connect_two_way_peg_data(&mut rwtxn, &twpd, None, None)?;
            Ok(())
        }));
        match res {
            Ok(Ok(())) => {
                let acc = self.state.get_accumulator(&rwtxn).unwrap();
                self.accs.insert(header.hash(), acc_bytes(&acc));
                rwtxn.commit().unwrap();
                self.chain.push((header, body, twpd, wj.clone()));
                let d = self.digest();
                self.emit(json!({"op":"block","block":blockhex,"window":wj,"expect":"ok","digest":d}));
            }
            Ok(Err(e)) => {
                drop(rwtxn);
                self.emit(json!({"op":"block","block":blockhex,"window":wj,"expect":"reject","err":format!("{e:#}")}));
            }
            Err(_) => {
                drop(rwtxn);
                self.emit(json!({"op":"block","block":blockhex,"window":wj,"expect":"panic"}));
            }
        }
    }

    fn disconnect_step(&mut self) {
        let Some((header, body, twpd, wj)) = self.chain.pop() else { return };
        let blockhex = hex::encode(bincode::serialize(&(&header, &body)).unwrap());
        let mut rwtxn = self.env.write_txn().unwrap();
        let res = catch_unwind(AssertUnwindSafe(|| -> Result<(), coinshift::state::Error> {
            self.state.disconnect_two_way_peg_data(&mut rwtxn, &twpd)?;
            self.state.disconnect_tip(&mut rwtxn, &header, &body)?;
            // net_task.rs disconnect_tip_: the archive's accumulator of the new tip
            match self.state.try_get_tip(&rwtxn)? {
                Some(t) => {
                    let bytes = self.accs.get(&t).unwrap().clone();
                    let acc: Accumulator = bincode::deserialize(&bincode::serialize(&bytes).unwrap()).unwrap();
                    self.state.utreexo_accumulator.put(&mut rwtxn, &(), &acc).unwrap();
                }
                None => {
                    self.state.utreexo_accumulator.delete(&mut rwtxn, &()).unwrap();
                }
            }
            Ok(())
        }));
        match res {
            Ok(Ok(())) => {
                rwtxn.commit().unwrap();
                let d = self.digest();
                self.emit(json!({"op":"disconnect","block":blockhex,"window":wj,"expect":"ok","digest":d}));
            }
            other => {
                drop(rwtxn);
                let err = match other {
                    Ok(Err(e)) => format!("{e:#}"),
                    _ => "panic".into(),
                };
                self.emit(json!({"op":"disconnect","block":blockhex,"window":wj,"expect":"reject","err":err}));
                self.chain.push((header, body, twpd, wj));
            }
        }
    }

    fn local_step(&mut self) {
        let rotxn = self.env.read_txn().unwrap();
        let swaps = self.state.load_all_swaps(&rotxn).unwrap();
        let tip = self.state.try_get_tip(&rotxn).unwrap();
        let height = self.state.try_get_height(&rotxn).unwrap();
        drop(rotxn);
        if swaps.is_empty() || tip.is_none() {
            return;
        }
        let s = swaps[self.rng.below(swaps.len() as u64) as usize].clone();
        let pre_l2 = if self.rng.chance(50) { Some(self.rand_addr()) } else { None };
        let pre_creator = if self.rng.chance(70) { s.l2_creator_address } else { Some(self.rand_addr()) };
        let env = self.env.clone();
        let mut rwtxn = env.write_txn().unwrap();
        if self.rng.chance(70) {
            let mut t = [0u8; 32];
            t[0] = self.rng.below(4) as u8;
            t[31] = 1;
            let conf = self.rng.below(8) as u32;
            let l2 = pre_l2;
            let r = self.state.update_swap_l1_txid(&mut rwtxn, &s.id, SwapTxId::Hash32(t), conf, None, l2, tip.unwrap(), height.unwrap());
            let expect = if r.is_ok() { "ok" } else { "reject" };
            if r.is_ok() {
                rwtxn.commit().unwrap();
            } else {
                drop(rwtxn);
            }
            let d = self.digest();
            self.emit(json!({"op":"l1","swap_id":hex::encode(s.id.0),"l1_txid":hex::encode(t),"conf":conf,"l2_claimer":l2.map(|a| a.to_string()),"expect":expect,"err":r.err().map(|e| format!("{e:#}")),"digest":d}));
        } else {
            let creator = pre_creator;
            let r = self.state.cancel_swap(&mut rwtxn, &s.id, creator.as_ref());
            let expect = if r.is_ok() { "ok" } else { "reject" };
            if r.is_ok() {
                rwtxn.commit().unwrap();
            } else {
                drop(rwtxn);
            }
            let d = self.digest();
            self.emit(json!({"op":"cancel","swap_id":hex::encode(s.id.0),"creator":creator.map(|a| a.to_string()),"expect":expect,"err":r.err().map(|e| format!("{e:#}")),"digest":d}));
        }
    }
}

pub fn run(seed: u64, blocks: u32) {
    std::panic::set_hook(Box::new(|_| {}));
    let dir = std::env::temp_dir().join(format!("cs-oracle-sim-{}-{seed}", std::process::id()));
    drop(std::fs::remove_dir_all(&dir));
    let env = open_env(&dir);
    let state = State::new(&env).unwrap();
    let keys: Vec<SigningKey> = (0..6).map(key).collect();
    let addrs = keys.iter().map(addr_of).collect();
    let out = Box::leak(Box::new(std::io::stdout()));
    let mut sim = Sim {
        rng: Rng(seed.wrapping_mul(0x9e3779b97f4a7c15) | 1),
        keys,
        addrs,
        env,
        state,
        ctxt: BatchVerificationContext::new(&mut rand::rng()),
        accs: HashMap::new(),
        chain: Vec::new(),
        deposit_seq: 0,
        main_counter: seed << 20,
        out: out.lock(),
    };
    let mut steps = 0;
    while (sim.chain.len() as u32) < blocks && steps < blocks * 4 {
        steps += 1;
        let r = sim.rng.below(100);
        if r < 8 && sim.chain.len() > 1 {
            let n = 1 + sim.rng.below(3);
            for _ in 0..n {
                sim.disconnect_step();
            }
        } else if r < 14 {
            sim.local_step();
        } else {
            sim.block_step();
        }
    }
    let _ = BTreeMap::<u8, u8>::new();
    let _ = SwapState::Pending;
    drop(std::fs::remove_dir_all(&dir));
}

pub fn wallet_addresses(mnemonic: &str) {
    let dir = std::env::temp_dir().join(format!("cs-oracle-wallet-{}", std::process::id()));
    drop(std::fs::remove_dir_all(&dir));
    let w = coinshift::wallet::Wallet::new(&dir).unwrap();
    w.set_seed_from_mnemonic(mnemonic).unwrap();
    for _ in 0..20 {
        println!("{}", w.get_new_address().unwrap());
    }
    drop(std::fs::remove_dir_all(&dir));
}
