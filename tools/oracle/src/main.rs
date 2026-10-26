//! Differential oracle for backbone-coinshift: runs LayerTwo Labs' coinshift-rs v0.14.8 lib
//! (their State, their types, their codecs) on the same inputs as the C++ port and prints
//! one line per step, for the C++ side to compare.
//!
//!   cs_oracle replay <fixture.jsonl>     their State after each block (digest lines)
//!   cs_oracle sim <seed> <blocks>        random chains built by their wallet/state code
//!   cs_oracle wallet <mnemonic>          addresses for indexes 0..19 (their derivation)
use std::io::{BufRead, Write};

use coinshift::{
    authorization::BatchVerificationContext,
    heed,
    state::State,
    types::{
        Body, Header,
        proto::{self, mainchain},
    },
};

mod digest;
mod jsoncases;
mod sim;

pub(crate) fn open_env(dir: &std::path::Path) -> sneed::Env<heed::WithoutTls> {
    std::fs::create_dir_all(dir).unwrap();
    let mut opts = heed::EnvOpenOptions::new().read_txn_without_tls();
    opts.map_size(1024 * 1024 * 1024).max_dbs(State::NUM_DBS + 30);
    unsafe { sneed::Env::open(&opts, dir) }.unwrap()
}

pub(crate) fn deposit_event(seq: u64, txid_display: &str, vout: u32, address_hex: &str, value: u64) -> mainchain::BlockEvent {
    use mainchain::generated;
    use proto::common::{Hex, ReverseHex};
    let g = generated::Deposit {
        sequence_number: Some(seq),
        outpoint: Some(generated::OutPoint {
            txid: Some(ReverseHex { hex: Some(txid_display.to_string()) }),
            vout: Some(vout),
        }),
        output: Some(generated::deposit::Output {
            address: Some(Hex { hex: Some(address_hex.to_string()) }),
            value_sats: Some(value),
        }),
    };
    mainchain::BlockEvent::Deposit(g.try_into().expect("their Deposit conversion"))
}

pub(crate) fn bundle_event(m6id_display: &str, status: &str) -> mainchain::BlockEvent {
    use coinshift::types::{M6id, WithdrawalBundleEvent, WithdrawalBundleStatus};
    let status = match status {
        "submitted" => WithdrawalBundleStatus::Submitted,
        "confirmed" => WithdrawalBundleStatus::Confirmed,
        _ => WithdrawalBundleStatus::Failed,
    };
    let m6id: bitcoin::Txid = m6id_display.parse().unwrap();
    mainchain::BlockEvent::WithdrawalBundle(WithdrawalBundleEvent { m6id: M6id(m6id), status })
}

pub(crate) fn window_from_json(w: &serde_json::Value) -> mainchain::TwoWayPegData {
    let mut twpd = mainchain::TwoWayPegData::default();
    for b in w.as_array().unwrap() {
        let hash: bitcoin::BlockHash = b["hash"].as_str().unwrap().parse().unwrap();
        let mut events = Vec::new();
        for e in b["events"].as_array().unwrap() {
            match e["kind"].as_str().unwrap() {
                "deposit" => events.push(deposit_event(
                    e["seq"].as_u64().unwrap(),
                    e["txid"].as_str().unwrap(),
                    e["vout"].as_u64().unwrap() as u32,
                    e["address"].as_str().unwrap(),
                    e["value"].as_u64().unwrap(),
                )),
                _ => events.push(bundle_event(e["m6id"].as_str().unwrap(), e["status"].as_str().unwrap())),
            }
        }
        twpd.block_info.replace(hash, mainchain::BlockInfo { bmm_commitment: None, events });
    }
    twpd
}

fn replay(path: &str) {
    let dir = std::env::temp_dir().join(format!("cs-oracle-replay-{}", std::process::id()));
    drop(std::fs::remove_dir_all(&dir));
    let env = open_env(&dir);
    let state = State::new(&env).unwrap();
    let ctxt = BatchVerificationContext::new(&mut rand::rng());
    let out = std::io::stdout();
    let mut out = out.lock();
    for line in std::io::BufReader::new(std::fs::File::open(path).unwrap()).lines() {
        let line = line.unwrap();
        if line.is_empty() {
            continue;
        }
        let v: serde_json::Value = serde_json::from_str(&line).unwrap();
        let height = v["height"].as_u64().unwrap();
        let bytes = hex::decode(v["block"].as_str().unwrap()).unwrap();
        let (header, body): (Header, Body) = bincode::deserialize(&bytes).unwrap();
        let twpd = window_from_json(&v["window"]);
        let mut rwtxn = env.write_txn().unwrap();
        let res = (|| -> Result<(), coinshift::state::Error> {
            let pre = state.prevalidate_block(&rwtxn, &ctxt, &header, &body)?;
            let _ = state.connect_prevalidated_block(&mut rwtxn, &header, &body, pre)?;
            state.connect_two_way_peg_data(&mut rwtxn, &twpd, None, None)?;
            Ok(())
        })();
        match res {
            Ok(()) => {
                rwtxn.commit().unwrap();
                let rotxn = env.read_txn().unwrap();
                writeln!(out, "{height} {}", digest::state_digest(&state, &rotxn)).unwrap();
            }
            Err(err) => {
                writeln!(out, "{height} REJECT {err:#}").unwrap();
                break;
            }
        }
    }
    drop(std::fs::remove_dir_all(&dir));
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("replay") => replay(&args[2]),
        Some("sim") => sim::run(args[2].parse().unwrap(), args[3].parse().unwrap()),
        Some("json") => jsoncases::run(),
        Some("wallet") => sim::wallet_addresses(&args[2]),
        _ => {
            eprintln!("usage: cs_oracle replay <fixture.jsonl> | sim <seed> <blocks> | wallet <mnemonic>");
            std::process::exit(2);
        }
    }
}
