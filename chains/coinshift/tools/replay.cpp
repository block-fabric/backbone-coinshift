// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// coinshift-replay: the betanet fixture as replay steps for the Rust oracle, and our own state
// digest after each block (tests/data/betanet).
//   coinshift-replay fixture <data dir> <out.jsonl>
//   coinshift-replay digests <data dir>
#include <coinshift/fixture.h>

#include <cstdio>
#include <fstream>

using namespace coinshift;

static std::string window_json(const bt::mainchain::TwoWayPegData& w)
{
    std::string s = "[";
    bool first = true;
    for (auto& [h, bi] : w.block_info) {
        if (!first) s += ",";
        first = false;
        s += "{\"hash\":\"" + bt::hex_encode_rev(h.span()) + "\",\"events\":[";
        bool fe = true;
        for (auto& e : bi.events) {
            if (!fe) s += ",";
            fe = false;
            if (auto d = std::get_if<bt::mainchain::Deposit>(&e)) {
                s += "{\"kind\":\"deposit\",\"seq\":" + std::to_string(d->sequence_number) + ",\"txid\":\"" + bt::hex_encode_rev(d->txid.span()) +
                     "\",\"vout\":" + std::to_string(d->vout) + ",\"address\":\"" + bt::hex_encode(d->address) + "\",\"value\":" + std::to_string(d->value_sats) + "}";
            } else {
                auto& b = std::get<bt::mainchain::WithdrawalBundleEvent>(e);
                const char* st = b.status == bt::mainchain::BundleStatus::Submitted ? "submitted" : b.status == bt::mainchain::BundleStatus::Confirmed ? "confirmed" : "failed";
                s += "{\"kind\":\"bundle\",\"m6id\":\"" + bt::hex_encode_rev(b.m6id.span()) + "\",\"status\":\"" + st + "\"}";
            }
        }
        s += "]}";
    }
    return s + "]";
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: coinshift-replay fixture <dir> <out.jsonl> | digests <dir>\n");
        return 2;
    }
    bt::frost::init();
    std::string mode = argv[1], err;
    auto d = fixture::load(argv[2], err);
    if (!d) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    auto steps = fixture::steps(*d, err);
    if (!err.empty()) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (mode == "fixture") {
        std::ofstream f(argv[3]);
        for (auto& s : steps) {
            Writer w;
            bincode(w, s.block.header);
            bincode(w, s.block.body);
            f << "{\"height\":" << s.height << ",\"block\":\"" << bt::hex_encode(w.buf) << "\",\"window\":" << window_json(s.window) << "}\n";
        }
        return 0;
    }
    bt::kv::MemStore store;
    State st;
    for (auto& s : steps) {
        bt::kv::Txn t(store);
        try {
            auto pre = st.prevalidate(t, s.block.header, s.block.body);
            st.connect_prevalidated(t, s.block.header, s.block.body, pre);
            st.connect_2wpd(t, s.window);
            t.commit();
        } catch (const std::exception& e) {
            std::printf("%u REJECT %s\n", s.height, e.what());
            return 0;
        }
        bt::kv::Txn r(store);
        std::printf("%u %s\n", s.height, fixture::state_digest(st, r).c_str());
    }
    return 0;
}
