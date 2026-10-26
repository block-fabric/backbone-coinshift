//! `cs_oracle json`: serde_json of deterministic sample values of every RPC type, one per line
//! as `<name> <json>`, and serde_json parse errors as `<name> ERR <message>`. The C++ side
//! (tests/test_coinshift_json.cpp) rebuilds the same values and compares.
use std::collections::BTreeMap;

use coinshift::{
    authorization::{Authorization, Signature, VerifyingKey},
    types::{
        Address, Block, BlockHash, BlockIndex, BlockIndexDeposit, BlockIndexSpend, BlockIndexTx, Body, Header,
        M6id, MainchainSyncPhase, MainchainSyncProgress, MempoolTx, MerkleRoot, OutPoint, Output, OutputContent,
        ParentChainType, PointedOutput, Swap, SwapDirection, SwapId, SwapState, SwapTxId, Transaction, TxData,
        Txid, WithdrawalBundle,
    },
    wallet::{Balance, TransferDests},
};
use rustreexo::accumulator::{node_hash::BitcoinNodeHash, proof::Proof};

// ristretto255 basepoint: a valid R / verifying key on both sides
const BASEPOINT: &str = "e2f2ae0a6abc4e71a884a961c500515f58e30b6aa582dd8db6a65945e08d2d76";

fn h(b: u8) -> [u8; 32] {
    let mut x = [0u8; 32];
    for (i, v) in x.iter_mut().enumerate() {
        *v = b.wrapping_add(i as u8);
    }
    x
}
fn addr(b: u8) -> Address {
    Address([b; 20])
}
fn btc(s: &str) -> bitcoin::Address<bitcoin::address::NetworkUnchecked> {
    s.parse().unwrap()
}
fn sat(v: u64) -> bitcoin::Amount {
    bitcoin::Amount::from_sat(v)
}

fn outputs() -> Vec<(&'static str, Output)> {
    vec![
        ("output_value", Output { address: addr(1), content: OutputContent::Value(sat(12345)) }),
        (
            "output_withdrawal_main",
            Output {
                address: addr(2),
                content: OutputContent::Withdrawal {
                    value: sat(89000),
                    main_fee: sat(10000),
                    main_address: btc("bc1qm38x97c87wv26xmmxdf2lcmaaq7ukqz5npmslx"),
                },
            },
        ),
        (
            "output_withdrawal_p2pkh",
            Output {
                address: addr(3),
                content: OutputContent::Withdrawal {
                    value: sat(10000),
                    main_fee: sat(10000),
                    main_address: btc("1A7pUaYFaXHq1JTTJfLfgy2FwFcjNsVXMT"),
                },
            },
        ),
        (
            "output_withdrawal_test",
            Output {
                address: addr(4),
                content: OutputContent::Withdrawal {
                    value: sat(1),
                    main_fee: sat(0),
                    main_address: btc("tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx"),
                },
            },
        ),
        (
            "output_swap_pending",
            Output { address: addr(5), content: OutputContent::SwapPending { value: sat(10000), swap_id: h(0x58) } },
        ),
    ]
}

fn outpoints() -> Vec<OutPoint> {
    vec![
        OutPoint::Regular { txid: Txid(h(1)), vout: 3 },
        OutPoint::Coinbase { merkle_root: MerkleRoot::from(h(2)), vout: 0 },
        OutPoint::Deposit(bitcoin::OutPoint { txid: "ea76bdc8e596b594304a33670c9100b4576c1a0f6038a39ac9e8097e0bcca9d2".parse().unwrap(), vout: 7 }),
    ]
}

fn txdatas() -> Vec<(&'static str, TxData)> {
    vec![
        ("txdata_regular", TxData::Regular),
        (
            "txdata_create_open",
            TxData::SwapCreate {
                swap_id: h(0x58),
                parent_chain: ParentChainType::BTC,
                l1_txid_bytes: vec![0u8; 32],
                required_confirmations: 6,
                l2_recipient: None,
                l2_amount: 10000,
                l1_recipient_address: "bc1qax4x32ydk562snyyx2rnl3gleyuhjg5w2536m9".into(),
                l1_amount: 10000,
            },
        ),
        (
            "txdata_create_to",
            TxData::SwapCreate {
                swap_id: h(9),
                parent_chain: ParentChainType::Regtest,
                l1_txid_bytes: vec![1, 2, 3],
                required_confirmations: 3,
                l2_recipient: Some(addr(7)),
                l2_amount: 5,
                l1_recipient_address: "x\"y".into(),
                l1_amount: 18446744073709551615,
            },
        ),
        ("txdata_claim", TxData::SwapClaim { swap_id: h(3), l2_claimer_address: Some(addr(8)), proof_data: Some(vec![9, 8]) }),
        ("txdata_claim_none", TxData::SwapClaim { swap_id: h(3), l2_claimer_address: None, proof_data: None }),
        ("txdata_accept", TxData::SwapAccept { swap_id: h(4), l2_claimer_address: addr(6) }),
    ]
}

fn proof() -> Proof {
    Proof::new(vec![5, 9], vec![BitcoinNodeHash::new(h(0x20)), BitcoinNodeHash::Empty, BitcoinNodeHash::Placeholder])
}

fn sample_tx(data: TxData) -> Transaction {
    let ops = outpoints();
    Transaction {
        inputs: vec![(ops[0], h(0x40)), (ops[2], h(0x41))],
        proof: proof(),
        outputs: outputs().into_iter().map(|(_, o)| o).collect(),
        data,
    }
}

fn authorization() -> Authorization {
    let bp = hex::decode(BASEPOINT).unwrap();
    let mut sig = bp.clone();
    let mut z = [0u8; 32];
    z[0] = 1;
    sig.extend_from_slice(&z);
    Authorization {
        verifying_key: VerifyingKey::deserialize(&bp).unwrap(),
        signature: Signature::deserialize(&sig).unwrap(),
    }
}

fn header(roots: bool, prev: bool) -> Header {
    Header {
        merkle_root: MerkleRoot::from(h(0x66)),
        prev_side_hash: prev.then(|| BlockHash(h(0x5e))),
        prev_main_hash: "00000000000000004a8ba51316f643532cf6654b5bdcb59db428e98b86f50d83".parse().unwrap(),
        roots: if roots { vec![BitcoinNodeHash::new(h(0x20)), BitcoinNodeHash::Empty] } else { vec![] },
    }
}

fn body() -> Body {
    Body {
        coinbase: vec![Output { address: addr(9), content: OutputContent::Value(sat(1000)) }],
        transactions: vec![sample_tx(TxData::Regular)],
        authorizations: vec![authorization(), authorization()],
    }
}

fn swaps() -> Vec<(&'static str, Swap)> {
    let base = Swap::new(
        SwapId(h(0x58)),
        SwapDirection::L2ToL1,
        ParentChainType::BTC,
        SwapTxId::Hash32([0u8; 32]),
        Some(6),
        None,
        sat(10000),
        "bc1qax4x32ydk562snyyx2rnl3gleyuhjg5w2536m9".into(),
        sat(10000),
        2144,
        Some(3152),
        Some(addr(0x1a)),
    );
    let mut waiting = base.clone();
    waiting.state = SwapState::WaitingConfirmations(1, 6);
    waiting.l1_txid = SwapTxId::Hash(vec![1, 2, 3]);
    waiting.l2_recipient = Some(addr(3));
    waiting.l1_claimer_address = Some("tb1qclaimer".into());
    waiting.l2_claimer_address = Some(addr(4));
    waiting.l1_txid_validated_at_block_hash = Some(BlockHash(h(0x11)));
    waiting.l1_txid_validated_at_height = Some(17);
    let mut ready = base.clone();
    ready.state = SwapState::ReadyToClaim;
    ready.parent_chain = ParentChainType::Signet;
    ready.expires_at_height = None;
    ready.l2_creator_address = None;
    let mut completed = base.clone();
    completed.state = SwapState::Completed;
    completed.parent_chain = ParentChainType::LTC;
    let mut cancelled = base.clone();
    cancelled.state = SwapState::Cancelled;
    cancelled.parent_chain = ParentChainType::BCH;
    vec![
        ("swap_pending", base),
        ("swap_waiting", waiting),
        ("swap_ready", ready),
        ("swap_completed", completed),
        ("swap_cancelled", cancelled),
    ]
}

fn line<T: serde::Serialize>(name: &str, v: &T) {
    println!("{name} {}", serde_json::to_string(v).unwrap());
}
fn err<T: serde::de::DeserializeOwned>(name: &str, s: &str) {
    match serde_json::from_str::<T>(s) {
        Ok(_) => println!("{name} OK"),
        Err(e) => println!("{name} ERR {e}"),
    }
}

pub fn run() {
    for (n, o) in outputs() {
        line(n, &o);
    }
    for (i, op) in outpoints().into_iter().enumerate() {
        line(&format!("outpoint_{i}"), &op);
        line(&format!("pointed_{i}"), &PointedOutput { outpoint: op, output: outputs()[i].1.clone() });
    }
    for (n, d) in txdatas() {
        line(n, &d);
        line(&format!("tx_{n}"), &sample_tx(d));
    }
    line("proof", &proof());
    line("authorization", &authorization());
    line("header_full", &header(true, true));
    line("header_genesis", &header(false, false));
    line("body", &body());
    line("block", &Block { header: header(true, true), body: body() });
    {
        let mut spend = BTreeMap::new();
        for (i, op) in outpoints().into_iter().enumerate() {
            spend.insert(op, outputs()[i + 1].1.clone());
        }
        let outs = vec![bitcoin::TxOut {
            value: sat(89000),
            script_pubkey: btc("bc1qm38x97c87wv26xmmxdf2lcmaaq7ukqz5npmslx").assume_checked().script_pubkey(),
        }];
        line("withdrawal_bundle", &WithdrawalBundle::new(1526, sat(10000), spend, outs).unwrap());
    }
    for (n, s) in swaps() {
        line(n, &s);
        line(&format!("{n}_state"), &s.state);
    }
    line("swap_id", &SwapId(h(0x58)));
    {
        let tx = sample_tx(TxData::SwapAccept { swap_id: h(4), l2_claimer_address: addr(6) });
        line(
            "mempool_tx",
            &MempoolTx {
                txid: tx.txid(),
                size: tx.canonical_size(),
                raw: hex::encode(tx.canonical_encoding()),
                tx: tx.clone(),
            },
        );
        let ops = outpoints();
        line(
            "block_index",
            &BlockIndex {
                txs: vec![BlockIndexTx { txid: tx.txid(), size: tx.canonical_size(), raw: hex::encode(tx.canonical_encoding()) }],
                deposits: vec![BlockIndexDeposit { outpoint: ops[2], output: outputs()[0].1.clone() }],
                bundle_spends: vec![BlockIndexSpend {
                    outpoint: ops[0],
                    m6id: M6id("4e20f32b53eadfe696573226badea5ffc535eb537e1b9b72e82dccbd1b3fa098".parse().unwrap()),
                }],
            },
        );
        line("block_index_empty", &BlockIndex { txs: vec![], deposits: vec![], bundle_spends: vec![] });
    }
    line("balance", &Balance { total: sat(670000), available: sat(570000) });
    for (n, p) in [
        ("sync_idle", MainchainSyncPhase::Idle),
        ("sync_headers", MainchainSyncPhase::Headers),
        ("sync_writing", MainchainSyncPhase::Writing),
        ("sync_state", MainchainSyncPhase::State),
    ] {
        line(n, &MainchainSyncProgress { phase: p, done: 3, total: 9, tip_height: 973923 });
    }
    for (i, pc) in ParentChainType::all().iter().enumerate() {
        line(&format!("parent_chain_{i}"), pc);
    }
    // parse errors (serde_json::from_str of a lone value; jsonrpsee reports the same cause)
    err::<Address>("addr_ok", "\"11111111111111111111\"");
    err::<Address>("addr_bs58", "\"0OIl\"");
    err::<Address>("addr_len", "\"abc\"");
    err::<Address>("addr_num", "5");
    err::<SwapId>("swapid_short", "[1,2]");
    err::<SwapId>("swapid_str", "\"58a9\"");
    err::<SwapId>("swapid_big", &format!("[{}256]", "0,".repeat(31)));
    err::<ParentChainType>("pc_bad", "\"Doge\"");
    err::<ParentChainType>("pc_num", "3");
    err::<TransferDests>("dests_dup", "{\"11111111111111111111\":1,\"11111111111111111111\":2}");
    err::<TransferDests>("dests_ok", "{\"11111111111111111111\":1}");
    err::<TransferDests>("dests_neg", "{\"11111111111111111111\":-1}");
    err::<BlockHash>("blockhash_short", "\"abcd\"");
    err::<BlockHash>("blockhash_odd", "\"abc\"");
    err::<BlockHash>("blockhash_char", &format!("\"{}zz\"", "0".repeat(62)));
    err::<Txid>("txid_num", "1");
    err::<u64>("u64_neg", "-1");
    err::<u64>("u64_float", "1.5");
    err::<u64>("u64_str", "\"1\"");
    err::<u32>("u32_big", "4294967296");
    err::<Option<Address>>("optaddr_null", "null");
    err::<bitcoin::Address<bitcoin::address::NetworkUnchecked>>("btcaddr_bad", "\"bc1qxyz\"");
    err::<bitcoin::Address<bitcoin::address::NetworkUnchecked>>("btcaddr_b58", "\"1A7pUaYFaXHq1JTTJfLfgy2FwFcjNsVXMx\"");
    err::<bitcoin::BlockHash>("btcblockhash_bad", "\"xyz\"");
    err::<std::net::SocketAddr>("sockaddr_bad", "\"1.2.3.4\"");
    err::<std::net::SocketAddr>("sockaddr_ok", "\"[::1]:4255\"");
    err::<Block>("block_missing", "{\"header\":{}}");
    err::<Block>("block_ok_roundtrip", &serde_json::to_string(&Block { header: header(true, true), body: body() }).unwrap());
}
