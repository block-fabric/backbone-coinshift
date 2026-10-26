// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <coinshift/fixture.h>
#include <coinshift/simcheck.h>

#include <univalue.h>

#include <fstream>

namespace coinshift::simcheck {

static bt::mainchain::TwoWayPegData window(const UniValue& w)
{
    bt::mainchain::TwoWayPegData d;
    for (auto& b : w.getValues()) {
        bt::mainchain::BlockInfo bi;
        for (auto& e : b["events"].getValues()) {
            if (e["kind"].get_str() == "deposit") {
                bt::mainchain::Deposit dep;
                dep.sequence_number = e["seq"].getInt<uint64_t>();
                dep.txid = *bt::hash_from_rev_hex(e["txid"].get_str());
                dep.vout = uint32_t(e["vout"].getInt<int64_t>());
                dep.address = *bt::hex_decode(e["address"].get_str());
                dep.value_sats = e["value"].getInt<uint64_t>();
                bi.events.emplace_back(dep);
            } else {
                bt::mainchain::WithdrawalBundleEvent we;
                we.m6id = *bt::hash_from_rev_hex(e["m6id"].get_str());
                std::string st = e["status"].get_str();
                we.status = st == "submitted" ? bt::mainchain::BundleStatus::Submitted
                            : st == "confirmed" ? bt::mainchain::BundleStatus::Confirmed
                                                : bt::mainchain::BundleStatus::Failed;
                bi.events.emplace_back(we);
            }
        }
        d.block_info.emplace_back(*bt::hash_from_rev_hex(b["hash"].get_str()), bi);
    }
    return d;
}

static Block block_of(const std::string& hex)
{
    auto raw = bt::hex_decode(hex);
    Reader r(*raw);
    Block b;
    b.header = de_header(r);
    b.body = de_body(r);
    return b;
}

Result run(const std::string& path, bool stop_at_first)
{
    Result res;
    bt::kv::MemStore store;
    State st;
    std::ifstream f(path);
    std::string line;
    auto problem = [&](const std::string& p) {
        if (res.first_problem.empty()) res.first_problem = "step " + std::to_string(res.steps) + ": " + p;
    };
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (stop_at_first && !res.first_problem.empty()) break;
        res.steps++;
        UniValue v;
        v.read(line);
        std::string op = v["op"].get_str(), expect = v["expect"].get_str();
        bool ok = false;
        std::string err;
        bt::kv::Txn t(store);
        if (op == "tx") {
            res.txs++;
            auto raw = bt::hex_decode(v["tx"].get_str());
            Reader r(*raw);
            AuthorizedTransaction at;
            uint64_t fee = 0;
            try {
                at = de_authorized_transaction(r);
                fee = st.validate_transaction(t, at);
                ok = true;
            } catch (const std::exception& e) {
                err = e.what();
            }
            if (ok != (expect == "ok")) {
                res.verdict_mismatches++;
                problem("tx verdict: theirs " + expect + " " + (v["err"].isStr() ? v["err"].get_str() : "") + " / ours " + (ok ? "ok" : err));
            } else if (ok && fee != v["fee"].getInt<uint64_t>()) {
                res.verdict_mismatches++;
                problem("tx fee");
            } else if (!ok && v["err"].isStr() && v["err"].get_str() != err) {
                res.message_mismatches++;
                res.message_examples[v["err"].get_str().substr(0, 90) + " | " + err.substr(0, 90)]++;
            }
            continue;
        }
        if (op == "block") {
            Block b = block_of(v["block"].get_str());
            auto w = window(v["window"]);
            try {
                auto pre = st.prevalidate(t, b.header, b.body);
                st.connect_prevalidated(t, b.header, b.body, pre);
                st.connect_2wpd(t, w);
                ok = true;
            } catch (const std::exception& e) {
                err = e.what();
            }
            if (ok) {
                t.commit();
                res.blocks_ok++;
            } else {
                res.blocks_rejected++;
            }
        } else if (op == "disconnect") {
            res.disconnects++;
            Block b = block_of(v["block"].get_str());
            auto w = window(v["window"]);
            try {
                st.disconnect_2wpd(t, w);
                st.disconnect_tip(t, b.header, b.body);
                ok = true;
            } catch (const std::exception& e) {
                err = e.what();
            }
            if (ok) t.commit();
        } else if (op == "l1" || op == "cancel") {
            res.local_ops++;
            Hash32 id = *bt::hex_to_array<32>(v["swap_id"].get_str());
            try {
                if (op == "l1") {
                    auto l1 = bt::hex_decode(v["l1_txid"].get_str());
                    std::optional<Address> l2;
                    if (v["l2_claimer"].isStr()) l2 = Address::from_base58(v["l2_claimer"].get_str());
                    st.update_swap_l1_txid(t, id, SwapTxId::from_bytes(*l1), uint32_t(v["conf"].getInt<int64_t>()), std::nullopt, l2, *st.tip(t),
                                           *st.height(t));
                } else {
                    std::optional<Address> c;
                    if (v["creator"].isStr()) c = Address::from_base58(v["creator"].get_str());
                    st.cancel_swap(t, id, c);
                }
                ok = true;
            } catch (const std::exception& e) {
                err = e.what();
            }
            if (ok) t.commit();
        }
        if (ok != (expect == "ok")) {
            res.verdict_mismatches++;
            problem(op + " verdict: theirs " + expect + " " + (v["err"].isStr() ? v["err"].get_str() : "") + " / ours " + (ok ? "ok" : err));
            break; // states diverged
        }
        if (!ok && v["err"].isStr() && v["err"].get_str() != err) {
            res.message_mismatches++;
            res.message_examples[v["err"].get_str().substr(0, 90) + " | " + err.substr(0, 90)]++;
        }
        if (v["digest"].isStr()) {
            bt::kv::Txn r(store);
            std::string ours = fixture::state_digest(st, r);
            if (ours != v["digest"].get_str()) {
                res.digest_mismatches++;
                problem(op + " digest:\n  theirs " + v["digest"].get_str() + "\n  ours   " + ours);
                break;
            }
        }
    }
    return res;
}

} // namespace coinshift::simcheck
