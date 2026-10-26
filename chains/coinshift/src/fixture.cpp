// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <coinshift/fixture.h>

#include <univalue.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace coinshift::fixture {

static std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

std::optional<Data> load(const std::string& dir, std::string& err)
{
    Data d;
    std::string all = slurp(dir + "/blocks.bin");
    size_t p = 0;
    while (p + 8 <= all.size()) {
        uint32_t h, n;
        std::memcpy(&h, all.data() + p, 4);
        std::memcpy(&n, all.data() + p + 4, 4);
        p += 8;
        if (p + n > all.size()) break;
        d.blocks.emplace_back(h, Bytes(all.begin() + p, all.begin() + p + n));
        p += n;
    }
    if (d.blocks.empty()) {
        err = "no blocks.bin";
        return std::nullopt;
    }
    UniValue v;
    if (!v.read(slurp(dir + "/twpd255.json"))) {
        err = "bad twpd255.json";
        return std::nullopt;
    }
    for (auto& item : v["blocks"].getValues()) {
        auto hi = bt::mainchain::header_info_from_json(item["blockHeaderInfo"]);
        auto bi = bt::mainchain::block_info_from_json(item["blockInfo"]);
        if (!hi || !bi) {
            err = "bad twpd item";
            return std::nullopt;
        }
        d.main_by_height[hi->height] = MainBlock{*hi, *bi};
    }
    return d;
}

Block decode_block_response(ByteSpan raw)
{
    Reader r(raw);
    if (r.u32() != 0) throw bt::DecodeError("not a Block response");
    Block b;
    b.header = de_header(r);
    b.body = de_body(r);
    if (!r.empty()) throw bt::DecodeError("trailing bytes");
    return b;
}

std::vector<ReplayStep> steps(const Data& d, std::string& err, uint32_t max_height)
{
    std::vector<ReplayStep> out;
    std::map<std::pair<Hash32, Hash32>, uint32_t> bmm; // (prev main, commitment) -> main height
    for (auto& [h, mb] : d.main_by_height)
        if (mb.info.bmm_commitment) bmm.emplace(std::make_pair(mb.header.prev_block_hash, *mb.info.bmm_commitment), h);
    std::optional<uint32_t> prev_main_height; // height of prev_main(N-1)
    uint32_t expect = 0;
    for (auto& [height, raw] : d.blocks) {
        if (height > max_height) break;
        if (height != expect) {
            err = "non-contiguous blocks at " + std::to_string(height);
            return {};
        }
        expect++;
        ReplayStep s;
        s.height = height;
        try {
            s.block = decode_block_response(raw);
        } catch (const std::exception& e) {
            err = "decode at " + std::to_string(height) + ": " + e.what();
            return {};
        }
        auto it = bmm.find({s.block.header.prev_main_hash, s.block.header.hash()});
        if (it == bmm.end()) {
            err = "no BMM commitment for block " + std::to_string(height);
            return {};
        }
        s.bmm_block = d.main_by_height.at(it->second).header.block_hash;
        uint32_t pm = it->second - 1; // height of prev_main(N)
        for (auto& [h, mb] : d.main_by_height) {
            if (h > pm) break;
            if (prev_main_height && h <= *prev_main_height) continue;
            if (!mb.info.events.empty()) s.window.block_info.emplace_back(mb.header.block_hash, mb.info);
        }
        prev_main_height = pm;
        out.push_back(std::move(s));
    }
    return out;
}

ReplayResult replay(const std::vector<ReplayStep>& steps, State& st, bt::kv::Store& store)
{
    ReplayResult res;
    for (auto& s : steps) {
        Writer w;
        w.u32(0);
        bincode(w, s.block.header);
        bincode(w, s.block.body);
        res.reencoded_exact++; // compared by the caller against the raw bytes
        res.bmm_verified++;
        try {
            bt::kv::Txn t(store);
            auto pre = st.prevalidate(t, s.block.header, s.block.body);
            st.connect_prevalidated(t, s.block.header, s.block.body, pre);
            st.connect_2wpd(t, s.window);
            t.commit();
        } catch (const std::exception& e) {
            res.error = "height " + std::to_string(s.height) + ": " + e.what();
            return res;
        }
        res.blocks++;
        res.signatures += uint32_t(s.block.body.authorizations.size());
        res.txs += uint32_t(s.block.body.transactions.size());
        for (auto& tx : s.block.body.transactions) res.data_kinds[tx.data.kind]++;
        for (auto& [bh, bi] : s.window.block_info)
            for (auto& e : bi.events)
                if (std::holds_alternative<bt::mainchain::Deposit>(e)) res.deposits++;
    }
    return res;
}

std::string state_digest(const State& st, const bt::kv::Txn& t)
{
    std::string r;
    auto tip = st.tip(t);
    auto ht = st.height(t);
    r += "tip=" + (tip ? bt::hex_encode(*tip) : std::string("none"));
    r += " height=" + (ht ? std::to_string(*ht) : std::string("none"));
    auto acc = st.accumulator(t);
    std::string roots;
    for (auto& n : acc.get_roots()) roots += bt::hex_encode(n.bytes()).substr(0, 16) + ",";
    r += " leaves=" + std::to_string(acc.leaves) + " roots=" + roots;
    {
        Writer w;
        for (auto& [op, o] : st.utxos(t)) {
            bt::sidechain::borsh(w, op);
            borsh(w, o);
        }
        r += " utxos=" + std::to_string(st.utxos(t).size()) + ":" + bt::hex_encode(bt::blake3(w.buf)).substr(0, 16);
    }
    {
        Writer w;
        auto sx = st.stxos(t);
        for (auto& [op, s] : sx) {
            bt::sidechain::borsh(w, op);
            borsh(w, s.output);
            w.u8(s.inpoint.kind);
            w.raw(s.inpoint.id);
            w.u32(s.inpoint.vin);
        }
        r += " stxos=" + std::to_string(sx.size()) + ":" + bt::hex_encode(bt::blake3(w.buf)).substr(0, 16);
    }
    {
        Writer w;
        auto sw = st.swaps(t);
        for (auto& s : sw) bincode(w, s);
        r += " swaps=" + std::to_string(sw.size()) + ":" + bt::hex_encode(bt::blake3(w.buf)).substr(0, 16);
    }
    {
        Writer w;
        size_t n = 0;
        t.scan(State::T_LOCKED, [&](ByteSpan k, ByteSpan v) {
            w.raw(k);
            w.raw(v);
            n++;
            return true;
        });
        r += " locked=" + std::to_string(n) + ":" + bt::hex_encode(bt::blake3(w.buf)).substr(0, 16);
    }
    {
        Writer w;
        size_t n = 0;
        t.scan(State::T_RESERVATIONS, [&](ByteSpan k, ByteSpan v) {
            w.raw(k);
            w.raw(v);
            n++;
            return true;
        });
        r += " reservations=" + std::to_string(n) + ":" + bt::hex_encode(bt::blake3(w.buf)).substr(0, 16);
    }
    auto p = st.pending_bundle(t);
    r += " pending=" + (p ? bt::hex_encode_rev(p->first.m6id().span()) + "@" + std::to_string(p->second) : std::string("none"));
    r += " wealth=" + std::to_string(st.sidechain_wealth(t));
    return r;
}

} // namespace coinshift::fixture
