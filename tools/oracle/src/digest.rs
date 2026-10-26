//! The same state digest as coinshift::fixture::state_digest (C++), from THEIR State.
use coinshift::{
    heed,
    state::State,
    types::{InPoint, OutPoint},
};
use fallible_iterator::FallibleIterator as _;

fn h16(bytes: &[u8]) -> String {
    hex::encode(blake3::hash(bytes).as_bytes())[..16].to_string()
}

pub fn state_digest(state: &State, rotxn: &sneed::RoTxn<'_, heed::WithoutTls>) -> String {
    let mut r = String::new();
    let tip = state.try_get_tip(rotxn).unwrap();
    let height = state.try_get_height(rotxn).unwrap();
    r += &format!(
        "tip={} height={}",
        tip.map(|t| hex::encode(t.0)).unwrap_or("none".into()),
        height.map(|h| h.to_string()).unwrap_or("none".into())
    );
    let acc = state.get_accumulator(rotxn).unwrap();
    let mut roots = String::new();
    for n in acc.get_roots() {
        let b: &[u8; 32] = &n;
        roots += &hex::encode(b)[..16];
        roots += ",";
    }
    r += &format!(" leaves={} roots={}", acc.0.leaves, roots);
    {
        let mut buf = Vec::new();
        let mut n = 0;
        let mut it = state.utxos.iter(rotxn).unwrap();
        while let Some((k, o)) = it.next().unwrap() {
            let op: OutPoint = k.into();
            borsh::to_writer(&mut buf, &op).unwrap();
            borsh::to_writer(&mut buf, &o).unwrap();
            n += 1;
        }
        r += &format!(" utxos={}:{}", n, h16(&buf));
    }
    {
        let mut buf = Vec::new();
        let mut n = 0;
        let mut it = state.stxos.iter(rotxn).unwrap();
        while let Some((k, s)) = it.next().unwrap() {
            let op: OutPoint = k.into();
            borsh::to_writer(&mut buf, &op).unwrap();
            borsh::to_writer(&mut buf, &s.output).unwrap();
            match s.inpoint {
                InPoint::Regular { txid, vin } => {
                    buf.push(0);
                    buf.extend_from_slice(&txid.0);
                    buf.extend_from_slice(&vin.to_le_bytes());
                }
                InPoint::Withdrawal { m6id } => {
                    buf.push(1);
                    let b: &[u8; 32] = m6id.0.as_ref();
                    buf.extend_from_slice(b);
                    buf.extend_from_slice(&0u32.to_le_bytes());
                }
            }
            n += 1;
        }
        r += &format!(" stxos={}:{}", n, h16(&buf));
    }
    {
        let mut buf = Vec::new();
        let mut n = 0;
        let mut it = state.swaps.iter(rotxn).unwrap();
        while let Some((_, s)) = it.next().unwrap() {
            buf.extend(bincode::serialize(&s).unwrap());
            n += 1;
        }
        r += &format!(" swaps={}:{}", n, h16(&buf));
    }
    {
        let mut buf = Vec::new();
        let mut n = 0;
        let mut it = state.locked_swap_outputs.iter(rotxn).unwrap();
        while let Some((k, id)) = it.next().unwrap() {
            buf.extend_from_slice(k.as_bytes());
            buf.extend_from_slice(&id.0);
            n += 1;
        }
        r += &format!(" locked={}:{}", n, h16(&buf));
    }
    {
        let mut buf = Vec::new();
        let mut n = 0;
        let mut it = state.swap_reservations.iter(rotxn).unwrap();
        while let Some((id, res)) = it.next().unwrap() {
            buf.extend_from_slice(&id.0);
            buf.extend(bincode::serialize(&res).unwrap());
            n += 1;
        }
        r += &format!(" reservations={}:{}", n, h16(&buf));
    }
    let pending = state.get_pending_withdrawal_bundle(rotxn).unwrap();
    r += &format!(
        " pending={}",
        pending.map(|(b, h)| format!("{}@{}", b.compute_m6id(), h)).unwrap_or("none".into())
    );
    r += &format!(" wealth={}", state.sidechain_wealth(rotxn).unwrap().to_sat());
    r
}
