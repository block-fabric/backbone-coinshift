// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Port of coinshift-rs v0.14.8 lib/state/{mod.rs, block.rs, swap.rs, two_way_peg_data.rs}.
// Branch order, error texts and panics follow theirs line by line; comments cite their code.
#include <coinshift/state.h>

#include <bt/log.h>

#include <algorithm>

namespace coinshift {

using bt::utreexo::MemForest;
using bt::utreexo::NodeHash;
using RB = bt::sidechain::RollBack<BundleStatus>;

namespace {
[[noreturn]] void fail(const std::string& m) { throw StateError(m); }
[[noreturn]] void panic(const std::string& m) { throw StateError(m, true); }
[[noreturn]] void invalid_tx(const std::string& m) { throw StateError("Invalid transaction: " + m); }

std::string hexs(const Hash32& h) { return bt::hex_encode(h); }
std::string m6id_str(const Hash32& m) { return bt::hex_encode_rev(m.span()); } // bitcoin::Txid Display
std::string outpoint_str(const OutPoint& o) { return o.to_string(); }

Bytes key37(const OutPoint& o)
{
    auto k = o.key();
    return Bytes(k.begin(), k.end());
}
const Bytes& unit_key()
{
    static const Bytes u{};
    return u;
}
} // namespace

const char* to_string(BundleStatus s)
{
    switch (s) {
    case BundleStatus::Confirmed: return "Confirmed";
    case BundleStatus::Failed: return "Failed";
    case BundleStatus::Submitted: return "Submitted";
    }
    return "?";
}

std::string debug_string(const SwapState& s)
{
    switch (s.kind) {
    case SwapState::Pending: return "Pending";
    case SwapState::WaitingConfirmations: return "WaitingConfirmations(" + std::to_string(s.current) + ", " + std::to_string(s.required) + ")";
    case SwapState::ReadyToClaim: return "ReadyToClaim";
    case SwapState::Completed: return "Completed";
    case SwapState::Cancelled: return "Cancelled";
    }
    return "?";
}

std::string amount_display(uint64_t sats)
{
    // bitcoin-units 0.1 Amount Display: 8 decimals unless a whole number of BTC
    uint64_t whole = sats / 100000000, frac = sats % 100000000;
    if (frac == 0) return std::to_string(whole) + " BTC";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08llu", (unsigned long long)frac);
    return std::to_string(whole) + "." + buf + " BTC";
}

// ---------------------------------------------------------------- AccumulatorDiff
void AccumulatorDiff::erase(const Hash32& h)
{
    auto it = idx_.find(h);
    size_t i = it->second;
    idx_.erase(it);
    order_.erase(order_.begin() + long(i));
    for (auto& [k, v] : idx_)
        if (v > i) v--;
}

void AccumulatorDiff::insert(const Hash32& h)
{
    auto it = idx_.find(h);
    if (it != idx_.end()) {
        if (!order_[it->second].second) erase(h); // cancels a removal
        return;
    }
    idx_[h] = order_.size();
    order_.emplace_back(h, true);
}

void AccumulatorDiff::remove(const Hash32& h)
{
    auto it = idx_.find(h);
    if (it != idx_.end()) {
        if (order_[it->second].second) erase(h); // cancels an insertion
        return;
    }
    idx_[h] = order_.size();
    order_.emplace_back(h, false);
}

std::pair<std::vector<NodeHash>, std::vector<NodeHash>> AccumulatorDiff::split() const
{
    std::vector<NodeHash> ins, del;
    for (auto& [h, add] : order_) (add ? ins : del).push_back(NodeHash::some(h));
    return {ins, del};
}

// ---------------------------------------------------------------- encodings
Bytes State::u32le(uint32_t v)
{
    Writer w;
    w.u32(v);
    return w.buf;
}

namespace {
void enc_spent(Writer& w, const SpentOutput& s)
{
    bincode(w, s.output);
    w.u32(s.inpoint.kind);
    if (s.inpoint.kind == InPoint::Regular) {
        w.raw(s.inpoint.id);
        w.u32(s.inpoint.vin);
    } else {
        w.bincode_bytes(s.inpoint.id.span()); // M6id(bitcoin::Txid) serde = bytes
    }
}
SpentOutput dec_spent(Reader& r)
{
    SpentOutput s;
    s.output = de_output(r);
    uint32_t k = r.u32();
    if (k > 1) throw DecodeError("invalid InPoint");
    s.inpoint.kind = InPoint::Kind(k);
    if (s.inpoint.kind == InPoint::Regular) {
        s.inpoint.id = r.array<32>();
        s.inpoint.vin = r.u32();
    } else {
        Bytes b = r.bincode_bytes();
        s.inpoint.id = Hash32::from_span(b);
    }
    return s;
}
void enc_utxo_map(Writer& w, const std::map<OutPoint, Output>& m)
{
    w.u64(m.size());
    for (auto& [op, o] : m) {
        bt::sidechain::bincode(w, op);
        bincode(w, o);
    }
}
std::map<OutPoint, Output> dec_utxo_map(Reader& r)
{
    std::map<OutPoint, Output> m;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        OutPoint op = bt::sidechain::de_outpoint(r);
        m[op] = de_output(r);
    }
    return m;
}
void enc_bundle(Writer& w, const WithdrawalBundle& b)
{
    enc_utxo_map(w, b.spend_utxos);
    w.u64(b.outputs.size());
    for (auto& o : b.outputs) {
        w.u64(o.value);
        w.bincode_bytes(o.script_pubkey);
    }
}
WithdrawalBundle dec_bundle(Reader& r)
{
    WithdrawalBundle b;
    b.spend_utxos = dec_utxo_map(r);
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        bt::btc::TxOut o;
        o.value = r.u64();
        o.script_pubkey = r.bincode_bytes();
        b.outputs.push_back(std::move(o));
    }
    return b;
}
Bytes m6id_key(const Hash32& m)
{
    Writer w;
    w.bincode_bytes(m.span()); // SerdeBincode<M6id>
    return w.buf;
}
void enc_state(Writer& w, const SwapState& s)
{
    w.u32(s.kind);
    if (s.kind == SwapState::WaitingConfirmations) {
        w.u32(s.current);
        w.u32(s.required);
    }
}
SwapState dec_state(Reader& r)
{
    SwapState s;
    uint32_t k = r.u32();
    if (k > 4) throw DecodeError("invalid SwapState");
    s.kind = SwapState::Kind(k);
    if (s.kind == SwapState::WaitingConfirmations) {
        s.current = r.u32();
        s.required = r.u32();
    }
    return s;
}
/** SerdeBincode<(ParentChainType, SwapTxId)> */
Bytes l1txid_key(ParentChain pc, const SwapTxId& id)
{
    Writer w;
    w.u32(uint32_t(pc));
    w.u32(id.kind);
    if (id.kind == SwapTxId::Hash32) w.raw(ByteSpan(id.bytes.data(), 32));
    else w.bincode_bytes(id.bytes);
    return w.buf;
}
} // namespace

// ---------------------------------------------------------------- getters
std::optional<Hash32> State::tip(const Txn& t) const
{
    auto v = t.get(T_TIP, unit_key());
    if (!v) return std::nullopt;
    return Hash32::from_span(*v);
}

std::optional<uint32_t> State::height(const Txn& t) const
{
    auto v = t.get(T_HEIGHT, unit_key());
    if (!v) return std::nullopt;
    Reader r(*v);
    return r.u32();
}

void State::set_tip(Txn& t, const Hash32& h, uint32_t height) const
{
    t.put(T_TIP, unit_key(), h.span());
    t.put(T_HEIGHT, unit_key(), u32le(height));
}

std::optional<Output> State::utxo(const Txn& t, const OutPoint& op) const
{
    auto v = t.get(T_UTXOS, key37(op));
    if (!v) return std::nullopt;
    Reader r(*v);
    return de_output(r);
}

std::optional<SpentOutput> State::stxo(const Txn& t, const OutPoint& op) const
{
    auto v = t.get(T_STXOS, key37(op));
    if (!v) return std::nullopt;
    Reader r(*v);
    return dec_spent(r);
}

void State::put_utxo(Txn& t, const OutPoint& op, const Output& o) const
{
    Writer w;
    bincode(w, o);
    t.put(T_UTXOS, key37(op), w.buf);
}
bool State::del_utxo(Txn& t, const OutPoint& op) const { return t.del(T_UTXOS, key37(op)); }
void State::put_stxo(Txn& t, const OutPoint& op, const SpentOutput& s) const
{
    Writer w;
    enc_spent(w, s);
    t.put(T_STXOS, key37(op), w.buf);
}
bool State::del_stxo(Txn& t, const OutPoint& op) const { return t.del(T_STXOS, key37(op)); }

std::vector<std::pair<OutPoint, Output>> State::utxos(const Txn& t) const
{
    std::vector<std::pair<OutPoint, Output>> r;
    t.scan(T_UTXOS, [&](ByteSpan k, ByteSpan v) {
        Reader rd(v);
        r.emplace_back(OutPoint::from_key(k), de_output(rd));
        return true;
    });
    return r;
}

std::vector<std::pair<OutPoint, SpentOutput>> State::stxos(const Txn& t) const
{
    std::vector<std::pair<OutPoint, SpentOutput>> r;
    t.scan(T_STXOS, [&](ByteSpan k, ByteSpan v) {
        Reader rd(v);
        r.emplace_back(OutPoint::from_key(k), dec_spent(rd));
        return true;
    });
    return r;
}

std::optional<std::pair<WithdrawalBundle, uint32_t>> State::pending_bundle(const Txn& t) const
{
    auto v = t.get(T_PENDING_BUNDLE, unit_key());
    if (!v) return std::nullopt;
    Reader r(*v);
    auto b = dec_bundle(r);
    uint32_t h = r.u32();
    return std::make_pair(std::move(b), h);
}

void State::put_pending(Txn& t, const WithdrawalBundle& b, uint32_t height) const
{
    Writer w;
    enc_bundle(w, b);
    w.u32(height);
    t.put(T_PENDING_BUNDLE, unit_key(), w.buf);
}

std::optional<std::pair<WithdrawalBundleInfo, RB>> State::bundle(const Txn& t, const Hash32& m6id) const
{
    auto v = t.get(T_BUNDLES, m6id_key(m6id));
    if (!v) return std::nullopt;
    Reader r(*v);
    WithdrawalBundleInfo i;
    uint32_t k = r.u32();
    if (k > 2) throw DecodeError("invalid WithdrawalBundleInfo");
    i.kind = WithdrawalBundleInfo::Kind(k);
    if (i.kind == WithdrawalBundleInfo::Known) i.bundle = dec_bundle(r);
    else if (i.kind == WithdrawalBundleInfo::UnknownConfirmed) i.spend_utxos = dec_utxo_map(r);
    RB rb;
    uint64_t n = r.u64();
    for (uint64_t j = 0; j < n; j++) {
        uint32_t s = r.u32();
        if (s > 2) throw DecodeError("invalid WithdrawalBundleStatus");
        uint32_t h = r.u32();
        rb.v.push_back({BundleStatus(s), h});
    }
    if (rb.v.empty()) throw DecodeError("empty RollBack");
    return std::make_pair(std::move(i), std::move(rb));
}

void State::put_bundle(Txn& t, const Hash32& m6id, const WithdrawalBundleInfo& i, const RB& s) const
{
    Writer w;
    w.u32(i.kind);
    if (i.kind == WithdrawalBundleInfo::Known) enc_bundle(w, i.bundle);
    else if (i.kind == WithdrawalBundleInfo::UnknownConfirmed) enc_utxo_map(w, i.spend_utxos);
    w.u64(s.v.size());
    for (auto& hs : s.v) {
        w.u32(uint32_t(hs.value));
        w.u32(hs.height);
    }
    t.put(T_BUNDLES, m6id_key(m6id), w.buf);
}

std::optional<std::pair<uint32_t, Hash32>> State::latest_failed_bundle(const Txn& t) const
{
    auto v = t.get(T_LATEST_FAILED, unit_key());
    if (!v) return std::nullopt;
    Reader r(*v);
    uint64_t n = r.u64();
    Hash32 m6id;
    for (uint64_t i = 0; i < n; i++) {
        m6id = Hash32::from_span(r.bincode_bytes());
        r.u32();
    }
    auto b = bundle(t, m6id);
    if (!b) panic("Inconsistent DBs: latest failed m6id should exist in withdrawal_bundles");
    auto latest = b->second.latest();
    if (latest.value != BundleStatus::Failed) panic("assertion failed: latest failed bundle status is Failed");
    return std::make_pair(latest.height, m6id);
}

namespace {
bt::sidechain::RollBack<Hash32> dec_latest_failed(ByteSpan v)
{
    Reader r(v);
    bt::sidechain::RollBack<Hash32> rb;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        Hash32 m = Hash32::from_span(r.bincode_bytes());
        uint32_t h = r.u32();
        rb.v.push_back({m, h});
    }
    if (rb.v.empty()) throw DecodeError("empty RollBack");
    return rb;
}
Bytes enc_latest_failed(const bt::sidechain::RollBack<Hash32>& rb)
{
    Writer w;
    w.u64(rb.v.size());
    for (auto& hs : rb.v) {
        w.bincode_bytes(hs.value.span());
        w.u32(hs.height);
    }
    return w.buf;
}
} // namespace

std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> State::seq_table(const Txn& t, uint8_t table) const
{
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> r;
    t.scan(table, [&](ByteSpan k, ByteSpan v) {
        Reader kr(k), vr(v);
        uint32_t seq = kr.u32();
        Bytes hb = vr.bincode_bytes();
        uint32_t h = vr.u32();
        r.emplace_back(seq, std::make_pair(Hash32::from_span(hb), h));
        return true;
    });
    return r;
}

std::optional<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> State::seq_last(const Txn& t, uint8_t table) const
{
    auto l = t.last(table); // memcmp-largest key, as LMDB last() over SerdeBincode<u32> (LE) keys
    if (!l) return std::nullopt;
    Reader kr(l->first), vr(l->second);
    uint32_t seq = kr.u32();
    Bytes hb = vr.bincode_bytes();
    uint32_t h = vr.u32();
    return std::make_pair(seq, std::make_pair(Hash32::from_span(hb), h));
}

std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> State::deposit_blocks(const Txn& t) const { return seq_table(t, T_DEPOSIT_BLOCKS); }
std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> State::bundle_event_blocks(const Txn& t) const
{
    return seq_table(t, T_BUNDLE_EVENT_BLOCKS);
}

std::optional<Hash32> State::last_deposit_block_hash(const Txn& t) const
{
    auto l = seq_last(t, T_DEPOSIT_BLOCKS);
    if (!l) return std::nullopt;
    return l->second.first;
}
std::optional<Hash32> State::last_bundle_event_block_hash(const Txn& t) const
{
    auto l = seq_last(t, T_BUNDLE_EVENT_BLOCKS);
    if (!l) return std::nullopt;
    return l->second.first;
}

BlockIndexEvents State::block_index_events(const Txn& t, uint32_t h) const
{
    BlockIndexEvents ev;
    auto v = t.get(T_BLOCK_INDEX_EVENTS, u32le(h));
    if (!v) return ev;
    Reader r(*v);
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        OutPoint op = bt::sidechain::de_outpoint(r);
        ev.deposits.emplace_back(op, de_output(r));
    }
    uint64_t m = r.u64();
    for (uint64_t i = 0; i < m; i++) {
        OutPoint op = bt::sidechain::de_outpoint(r);
        ev.bundle_spends.emplace_back(op, Hash32::from_span(r.bincode_bytes()));
    }
    return ev;
}

uint64_t State::sidechain_wealth(const Txn& t) const
{
    uint64_t dep_utxo = 0, dep_stxo = 0, wd_stxo = 0;
    for (auto& [op, o] : utxos(t))
        if (op.kind == OutPoint::Deposit) {
            uint64_t v = o.get_value();
            if (dep_utxo + v < dep_utxo) fail("Bitcoin amount overflow");
            dep_utxo += v;
        }
    for (auto& [op, s] : stxos(t)) {
        uint64_t v = s.output.get_value();
        if (op.kind == OutPoint::Deposit) {
            if (dep_stxo + v < dep_stxo) fail("Bitcoin amount overflow");
            dep_stxo += v;
        }
        if (s.inpoint.kind == InPoint::Withdrawal) {
            if (wd_stxo + v < wd_stxo) fail("Bitcoin amount overflow");
            wd_stxo += v;
        }
    }
    uint64_t total = dep_utxo + dep_stxo;
    if (total < dep_utxo || total < wd_stxo) fail("Bitcoin amount overflow"); // checked_sub -> AmountOverflowError (sic)
    return total - wd_stxo;
}

MemForest State::accumulator(const Txn& t) const
{
    auto v = t.get(T_ACCUMULATOR, unit_key());
    if (!v) return MemForest{};
    auto f = MemForest::deserialize(*v);
    if (!f) throw StateError("accumulator deserialize error");
    return std::move(*f);
}

void State::put_accumulator(Txn& t, const MemForest& f) const { t.put(T_ACCUMULATOR, unit_key(), f.serialize()); }

std::optional<MemForest> State::block_accumulator(const Txn& t, const Hash32& block) const
{
    auto v = t.get(T_ACC_BY_BLOCK, block.span());
    if (!v) return std::nullopt;
    return MemForest::deserialize(*v);
}

void State::apply_diff(Txn& t, const AccumulatorDiff& d) const
{
    MemForest f = accumulator(t);
    auto [ins, del] = d.split();
    if (auto e = f.modify(ins, del)) fail("utreexo error: " + *e);
    put_accumulator(t, f);
}

void State::restore_accumulator_for_tip(Txn& t) const
{
    if (auto tp = tip(t)) {
        auto v = t.get(T_ACC_BY_BLOCK, tp->span());
        if (!v) fail("archive error"); // Archive NoAccumulator
        t.put(T_ACCUMULATOR, unit_key(), *v);
    } else {
        t.del(T_ACCUMULATOR, unit_key());
    }
}

// ---------------------------------------------------------------- swaps (state/mod.rs)
std::optional<Swap> State::swap(const Txn& t, const Hash32& id) const
{
    auto v = t.get(T_SWAPS, id.span());
    if (!v) return std::nullopt;
    try {
        Reader r(*v);
        return de_swap(r);
    } catch (const DecodeError&) {
        return std::nullopt; // get_swap: a record that does not deserialize is "no swap"
    }
}

std::vector<Swap> State::swaps(const Txn& t) const
{
    std::vector<Swap> r;
    t.scan(T_SWAPS, [&](ByteSpan, ByteSpan v) {
        try {
            Reader rd(v);
            r.push_back(de_swap(rd));
        } catch (const DecodeError&) {
        }
        return true;
    });
    return r;
}

std::vector<Swap> State::swaps_by_recipient(const Txn& t, const Address& a) const
{
    std::vector<Swap> r;
    auto v = t.get(T_SWAPS_BY_RECIPIENT, a.b.span());
    if (!v) return r;
    Reader rd(*v);
    uint64_t n = rd.u64();
    for (uint64_t i = 0; i < n; i++)
        if (auto s = swap(t, rd.array<32>())) r.push_back(*s);
    return r;
}

std::optional<Hash32> State::locked_to(const Txn& t, const OutPoint& op) const
{
    auto v = t.get(T_LOCKED, key37(op));
    if (!v) return std::nullopt;
    return Hash32::from_span(*v);
}

void State::lock_output(Txn& t, const OutPoint& op, const Hash32& id) const { t.put(T_LOCKED, key37(op), id.span()); }
void State::unlock_output(Txn& t, const OutPoint& op) const { t.del(T_LOCKED, key37(op)); }

std::optional<SwapReservation> State::reservation(const Txn& t, const Hash32& id) const
{
    auto v = t.get(T_RESERVATIONS, id.span());
    if (!v) return std::nullopt;
    Reader r(*v);
    return de_reservation(r);
}

void State::reserve_swap(Txn& t, const Hash32& id, const Address& claimer, uint32_t height) const
{
    Writer w;
    bincode(w, SwapReservation{claimer, height});
    t.put(T_RESERVATIONS, id.span(), w.buf);
}

void State::clear_reservation(Txn& t, const Hash32& id) const { t.del(T_RESERVATIONS, id.span()); }

std::optional<Address> State::entitled_claimer_at(const Txn& t, const Swap& s, uint32_t height) const
{
    if (s.l2_recipient) return s.l2_recipient;
    auto r = reservation(t, s.id);
    if (!r) return std::nullopt;
    if (r->is_live_at(s.parent_chain, height)) return r->claimer;
    return std::nullopt;
}

void State::save_swap(Txn& t, const Swap& s) const
{
    t.del(T_SWAPS, s.id.span());
    Writer w;
    bincode(w, s);
    t.put(T_SWAPS, s.id.span(), w.buf);
    t.put(T_SWAPS_BY_L1TXID, l1txid_key(s.parent_chain, s.l1_txid), s.id.span());
    if (s.l2_recipient) {
        std::vector<Hash32> ids;
        if (auto v = t.get(T_SWAPS_BY_RECIPIENT, s.l2_recipient->b.span())) {
            Reader r(*v);
            uint64_t n = r.u64();
            for (uint64_t i = 0; i < n; i++) ids.push_back(r.array<32>());
        }
        if (std::find(ids.begin(), ids.end(), s.id) == ids.end()) {
            ids.push_back(s.id);
            Writer iw;
            iw.u64(ids.size());
            for (auto& i : ids) iw.raw(i);
            t.put(T_SWAPS_BY_RECIPIENT, s.l2_recipient->b.span(), iw.buf);
        }
    }
}

uint32_t State::unlock_all_outputs_for_swap(Txn& t, const Hash32& id) const
{
    std::vector<OutPoint> ops;
    t.scan(T_LOCKED, [&](ByteSpan k, ByteSpan v) {
        if (Hash32::from_span(v) == id) ops.push_back(OutPoint::from_key(k));
        return true;
    });
    for (auto& op : ops) unlock_output(t, op);
    return uint32_t(ops.size());
}

void State::delete_swap_unchecked(Txn& t, const Hash32& id) const
{
    if (auto s = swap(t, id)) {
        t.del(T_SWAPS_BY_L1TXID, l1txid_key(s->parent_chain, s->l1_txid));
        if (s->l2_recipient) {
            if (auto v = t.get(T_SWAPS_BY_RECIPIENT, s->l2_recipient->b.span())) {
                Reader r(*v);
                uint64_t n = r.u64();
                std::vector<Hash32> ids;
                for (uint64_t i = 0; i < n; i++) {
                    auto x = r.array<32>();
                    if (x != s->id) ids.push_back(x);
                }
                if (ids.empty()) t.del(T_SWAPS_BY_RECIPIENT, s->l2_recipient->b.span());
                else {
                    Writer iw;
                    iw.u64(ids.size());
                    for (auto& i : ids) iw.raw(i);
                    t.put(T_SWAPS_BY_RECIPIENT, s->l2_recipient->b.span(), iw.buf);
                }
            }
        }
    } else {
        unlock_all_outputs_for_swap(t, id);
    }
    t.del(T_SWAPS, id.span());
}

void State::cancel_swap(Txn& t, const Hash32& id, const std::optional<Address>& creator) const
{
    auto s = swap(t, id);
    if (!s) fail("Swap not found: " + hexs(id));
    if (!s->l2_creator_address || !creator || *creator != *s->l2_creator_address) fail("Only the swap creator can cancel or delete this swap");
    if (s->state.kind != SwapState::Pending)
        invalid_tx("Swap " + hexs(id) + " cannot be cancelled (state: " + debug_string(s->state) + "). Only Pending swaps can be cancelled.");
    unlock_all_outputs_for_swap(t, id);
    s->state = SwapState{SwapState::Cancelled};
    save_swap(t, *s);
}

void State::update_swap_l1_txid(Txn& t, const Hash32& id, const SwapTxId& l1_txid, uint32_t confirmations, const std::optional<std::string>& l1_claimer,
                                const std::optional<Address>& l2_claimer, const Hash32& block_hash, uint32_t block_height) const
{
    auto s = swap(t, id);
    if (!s) fail("Swap not found: " + hexs(id));
    if (s->state.kind != SwapState::Pending)
        invalid_tx("Swap " + hexs(id) + " cannot be updated (state: " + debug_string(s->state) +
                   "). Only Pending swaps can be filled with an L1 transaction.");
    if (confirmations == 0)
        invalid_tx("Swap " + hexs(id) + ": L1 tx confirmations must be > 0 (got 0); only confirmed transactions are accepted");
    uint32_t max_age = max_l1_tx_age_blocks(s->parent_chain);
    if (confirmations > max_age)
        invalid_tx("Swap " + hexs(id) + ": L1 tx is too old (" + std::to_string(confirmations) + " confirmations exceeds max age of " +
                   std::to_string(max_age) + " blocks for " + parent_chain_name(s->parent_chain) + ")");
    if (auto ex = t.get(T_SWAPS_BY_L1TXID, l1txid_key(s->parent_chain, l1_txid))) {
        Hash32 eid = Hash32::from_span(*ex);
        if (auto es = swap(t, eid); es && es->id != id)
            fail("L1 txid already used by another swap: " + hexs(es->id) + " (requested for " + hexs(id) + ")");
    }
    SwapTxId old = s->l1_txid;
    s->l1_txid = l1_txid;
    if (l1_claimer) s->l1_claimer_address = *l1_claimer;
    if (l2_claimer) s->l2_claimer_address = *l2_claimer;
    s->l1_txid_validated_at_block_hash = block_hash;
    s->l1_txid_validated_at_height = block_height;
    if (confirmations >= s->required_confirmations) s->state = SwapState{SwapState::ReadyToClaim};
    else s->state = SwapState{SwapState::WaitingConfirmations, confirmations, s->required_confirmations};
    t.del(T_SWAPS_BY_L1TXID, l1txid_key(s->parent_chain, old));
    t.put(T_SWAPS_BY_L1TXID, l1txid_key(s->parent_chain, l1_txid), id.span());
    save_swap(t, *s);
}

uint32_t State::cleanup_orphaned_locks(Txn& t) const
{
    std::vector<std::pair<OutPoint, Hash32>> locks;
    t.scan(T_LOCKED, [&](ByteSpan k, ByteSpan v) {
        locks.emplace_back(OutPoint::from_key(k), Hash32::from_span(v));
        return true;
    });
    uint32_t n = 0;
    for (auto& [op, id] : locks)
        if (!swap(t, id)) {
            unlock_output(t, op);
            n++;
        }
    return n;
}

// ---------------------------------------------------------------- transactions
std::vector<Output> State::fill_spent(const Txn& t, const Transaction& tx) const
{
    std::vector<Output> r;
    r.reserve(tx.inputs.size());
    for (auto& [op, h] : tx.inputs) {
        auto u = utxo(t, op);
        if (!u) fail("utxo " + outpoint_str(op) + " doesn't exist");
        r.push_back(std::move(*u));
    }
    return r;
}

std::vector<Output> State::fill_spent_from_stxos(const Txn& t, const Transaction& tx) const
{
    std::vector<Output> r;
    for (auto& [op, h] : tx.inputs) {
        auto s = stxo(t, op);
        if (!s) fail("stxo " + outpoint_str(op) + " doesn't exist");
        r.push_back(s->output);
    }
    return r;
}

uint64_t State::validate_filled(const FilledTransaction& ft) const
{
    // validate_utxo_hashes
    for (size_t i = 0; i < ft.tx->inputs.size() && i < ft.spent_utxos.size(); i++) {
        auto& [op, h] = ft.tx->inputs[i];
        Hash32 c = pointed_output_hash(op, ft.spent_utxos[i]);
        if (c != h) fail("Computed Utxo hash (" + hexs(c) + ") for input (" + outpoint_str(op) + ") does not match input hash (" + hexs(h) + ")");
    }
    uint64_t in = 0, out = 0;
    for (auto& u : ft.spent_utxos) {
        if (u.content.is_withdrawal()) fail("withdrawal output cannot be spent by a transaction");
        uint64_t v = u.get_value();
        if (in + v < in) fail("Bitcoin amount overflow");
        in += v;
    }
    for (auto& o : ft.tx->outputs) {
        uint64_t v = o.get_value();
        if (out + v < out) fail("Bitcoin amount overflow");
        out += v;
    }
    if (out > in) fail("value in is less than value out");
    return in - out;
}

void State::regenerate_proof(const Txn& t, Transaction& tx) const
{
    MemForest f = accumulator(t);
    std::vector<NodeHash> targets;
    for (auto& [op, h] : tx.inputs) targets.push_back(NodeHash::some(h));
    auto p = f.prove(targets);
    if (!p) fail("utreexo error: could not prove the inputs");
    tx.proof = *p;
}

uint32_t State::validating_height(const Txn& t) const
{
    auto h = height(t);
    return h ? *h + 1 : 0;
}

// ---------------------------------------------------------------- swap validation (state/swap.rs)
void State::validate_swap_create(const Txn& t, const Transaction& tx, const FilledTransaction& ft) const
{
    auto& d = tx.data;
    if (ft.spent_utxos.empty()) invalid_tx("SwapCreate must have inputs");
    Hash32 computed = swap_id_l2_to_l1(d.l1_recipient_address, d.l1_amount, ft.spent_utxos.front().address, d.l2_recipient);
    if (computed != d.swap_id) invalid_tx("Swap ID mismatch: expected " + hexs(d.swap_id) + ", computed " + hexs(computed));
    if (swap(t, computed)) invalid_tx("Swap " + hexs(computed) + " already exists");
    if (d.l2_amount == 0) invalid_tx("L2 amount must be greater than zero");
    if (tx.outputs.empty()) invalid_tx("Transaction must have at least one output");
    for (auto& [op, h] : tx.inputs) {
        auto locked = locked_to(t, op);
        if (locked && *locked != d.swap_id) {
            if (swap(t, *locked)) invalid_tx("Input " + outpoint_str(op) + " is locked to swap " + hexs(*locked));
            invalid_tx("Input " + outpoint_str(op) + " is locked to non-existent swap " + hexs(*locked) +
                       " (orphaned lock). Please run cleanup_orphaned_locks to fix this.");
        }
    }
    uint64_t total = 0;
    for (auto& u : ft.spent_utxos) {
        uint64_t v = u.get_value();
        if (total + v < total) invalid_tx("Input value overflow");
        total += v;
    }
    if (total < d.l2_amount) invalid_tx("Insufficient funds: need " + amount_display(d.l2_amount) + ", have " + amount_display(total));
    uint64_t escrowed = 0;
    for (auto& o : tx.outputs)
        if (o.content.kind == Content::SwapPending && o.content.swap_id == d.swap_id) {
            if (escrowed + o.content.value < escrowed) invalid_tx("SwapPending output value overflow");
            escrowed += o.content.value;
        }
    if (escrowed < d.l2_amount)
        invalid_tx("SwapCreate must lock at least " + amount_display(d.l2_amount) + " in SwapPending outputs for swap " + hexs(computed) + ", but locks " +
                   amount_display(escrowed));
}

void State::validate_no_locked_outputs(const Txn& t, const Transaction& tx) const
{
    if (tx.data.kind == TxData::SwapClaim) return;
    for (auto& [op, h] : tx.inputs)
        if (auto l = locked_to(t, op)) invalid_tx("Cannot spend locked output " + outpoint_str(op) + " (locked to swap " + hexs(*l) + ")");
}

void State::validate_swap_accept(const Txn& t, const Transaction& tx, const FilledTransaction& ft) const
{
    const Hash32& id = tx.data.swap_id;
    const Address claimer = tx.data.l2_claimer_address ? *tx.data.l2_claimer_address : Address{};
    validate_no_locked_outputs(t, tx);
    auto s = swap(t, id);
    if (!s) fail("Swap not found: " + hexs(id));
    if (s->l2_recipient) invalid_tx("Swap " + hexs(id) + " is not an open swap; its recipient was fixed at creation");
    uint32_t h = validating_height(t);
    if (s->expires_at_height && h >= *s->expires_at_height)
        invalid_tx("Swap " + hexs(id) + " expired at height " + std::to_string(*s->expires_at_height) + ", cannot be accepted at " + std::to_string(h));
    if (auto r = reservation(t, id); r && r->is_live_at(s->parent_chain, h))
        invalid_tx("Swap " + hexs(id) + " is already reserved by " + r->claimer.to_base58() + " until height " +
                   std::to_string(r->expires_at(s->parent_chain)));
    bool owns = std::any_of(ft.spent_utxos.begin(), ft.spent_utxos.end(), [&](const Output& o) { return o.address == claimer; });
    if (!owns) invalid_tx("SwapAccept must spend an input owned by " + claimer.to_base58() + ", the address it reserves for");
}

void State::validate_swap_claim(const Txn& t, const Transaction& tx) const
{
    const Hash32& id = tx.data.swap_id;
    auto s = swap(t, id);
    if (!s) fail("Swap not found: " + hexs(id));
    if (s->state.kind != SwapState::ReadyToClaim)
        invalid_tx("Swap " + hexs(id) + " is not ready to claim (state: " + debug_string(s->state) + ")");
    if (!s->l2_recipient && s->l1_txid.is_unset()) invalid_tx("Open swap cannot be claimed until L1 transaction is detected");
    validate_swap_claim_consensus(t, tx);
}

void State::validate_swap_claim_consensus(const Txn& t, const Transaction& tx) const
{
    const Hash32& id = tx.data.swap_id;
    bool found = false;
    for (auto& [op, h] : tx.inputs)
        if (auto l = locked_to(t, op)) {
            if (*l != id) invalid_tx("Input " + outpoint_str(op) + " is locked to different swap " + hexs(*l));
            found = true;
        }
    if (!found) invalid_tx("SwapClaim must spend at least one output locked to the swap");
    uint32_t h = validating_height(t);
    auto s = swap(t, id);
    if (!s) fail("Swap not found: " + hexs(id));
    auto expected = entitled_claimer_at(t, *s, h);
    if (!expected)
        invalid_tx("Open swap " + hexs(id) + " has no live reservation at height " + std::to_string(h) +
                   "; it must be reserved by a SwapAccept before it can be claimed");
    if (tx.data.l2_claimer_address && *tx.data.l2_claimer_address != *expected)
        invalid_tx("SwapClaim declares claimer " + tx.data.l2_claimer_address->to_base58() + " but swap " + hexs(id) + " is reserved for " +
                   expected->to_base58());
    uint64_t paid = 0;
    for (auto& o : tx.outputs)
        if (o.address == *expected) {
            uint64_t v = o.get_value();
            if (paid + v < paid) invalid_tx("Output value overflow in SwapClaim");
            paid += v;
        }
    if (paid < s->l2_amount)
        invalid_tx("SwapClaim must pay at least " + amount_display(s->l2_amount) + " to " + expected->to_base58() + ", but pays " + amount_display(paid));
}

void State::validate_block_transaction(const Txn& t, const Transaction& tx, const FilledTransaction& ft) const
{
    switch (tx.data.kind) {
    case TxData::SwapCreate: validate_swap_create(t, tx, ft); break;
    case TxData::SwapClaim: validate_swap_claim_consensus(t, tx); break;
    case TxData::SwapAccept: validate_swap_accept(t, tx, ft); break;
    case TxData::Regular: validate_no_locked_outputs(t, tx); break;
    }
}

void State::policy_check(const Txn& t, const AuthorizedTransaction& atx) const
{
    // Policy only (mempool, relay, templates, wallet); blocks are judged by consensus alone.
    const Transaction& tx = atx.transaction;
    if (tx.data.kind == TxData::SwapClaim) {
        auto spent = fill_spent(t, tx);
        for (size_t i = 0; i < spent.size() && i < atx.authorizations.size(); i++) {
            if (!spent[i].content.is_swap_pending()) continue;
            if (bt::sidechain::address_from_vk(atx.authorizations[i].verifying_key) == spent[i].address) continue;
            if (locked_to(t, tx.inputs[i].first) != std::optional<Hash32>(tx.data.swap_id))
                throw StateError("policy: a swap claim may only spend escrow outputs of its own swap");
        }
    }
    if (tx.data.kind != TxData::SwapClaim) {
        // An escrow output whose swap has not expired by height may be unlocked here only by this
        // node's own cancel_swap or L1 view: spending it would be refused by other nodes.
        uint32_t h = validating_height(t);
        for (auto& [op, uh] : tx.inputs) {
            auto u = utxo(t, op);
            if (!u || !u->content.is_swap_pending()) continue;
            auto s = swap(t, u->content.swap_id);
            if (s && !(s->expires_at_height && h >= *s->expires_at_height))
                throw StateError("policy: escrow of an unexpired swap (unlocked only on this node)");
        }
    }
    if (tx.data.kind == TxData::SwapAccept) {
        auto s = swap(t, tx.data.swap_id);
        if (s && s->state.kind != SwapState::Pending && s->state.kind != SwapState::WaitingConfirmations)
            throw StateError("policy: the swap is no longer open (state: " + debug_string(s->state) + ")");
    }
}

uint64_t State::validate_transaction(const Txn& t, const AuthorizedTransaction& atx) const
{
    const Transaction& tx = atx.transaction;
    FilledTransaction ft{&tx, fill_spent(t, tx)};
    switch (tx.data.kind) {
    case TxData::SwapCreate: validate_swap_create(t, tx, ft); break;
    case TxData::SwapClaim: validate_swap_claim(t, tx); break;
    case TxData::SwapAccept: validate_swap_accept(t, tx, ft); break;
    case TxData::Regular: validate_no_locked_outputs(t, tx); break;
    }
    bool claim = tx.data.kind == TxData::SwapClaim;
    for (size_t i = 0; i < atx.authorizations.size() && i < ft.spent_utxos.size(); i++) { // zip
        if (claim && ft.spent_utxos[i].content.is_swap_pending()) continue;
        if (bt::sidechain::address_from_vk(atx.authorizations[i].verifying_key) != ft.spent_utxos[i].address) fail("wrong public key for address");
    }
    if (!verify_authorized_transaction(atx)) fail("failed to verify authorization");
    return validate_filled(ft);
}

// ---------------------------------------------------------------- blocks (state/block.rs)
Prevalidated State::prevalidate(const Txn& t, const Header& h, const Body& b) const
{
    auto tip_hash = tip(t);
    if (h.prev_side_hash != tip_hash) {
        auto dbg = [](const std::optional<Hash32>& x) { return x ? "Some(" + hexs(*x) + ")" : std::string("None"); };
        fail("invalid header: expected previous sidechain block hash " + dbg(tip_hash) + ", but received " + dbg(h.prev_side_hash));
    }
    auto ht = height(t);
    uint32_t next_height = ht ? *ht + 1 : 0;
    if (b.authorizations.size() > body_sigops_limit(next_height)) fail("too many sigops");
    if (borsh_bytes(b).size() > body_size_limit(next_height)) fail("body too large");
    MemForest acc = accumulator(t);

    Prevalidated pre;
    pre.next_height = next_height;
    pre.txs = b.transactions;
    std::vector<std::array<uint8_t, 37>> all_input_keys;
    std::vector<std::pair<bool, Hash32>> diff_txs;
    uint64_t total_fees = 0;
    for (auto& tx : pre.txs) {
        Hash32 txid = tx.txid();
        FilledTransaction ft{&tx, {}};
        std::vector<NodeHash> spent_hashes;
        for (auto& [op, uh] : tx.inputs) {
            auto u = utxo(t, op);
            if (!u) fail("utxo " + outpoint_str(op) + " doesn't exist");
            all_input_keys.push_back(op.key());
            ft.spent_utxos.push_back(std::move(*u));
            spent_hashes.push_back(NodeHash::some(uh));
            diff_txs.emplace_back(false, uh);
        }
        for (uint32_t vout = 0; vout < tx.outputs.size(); vout++)
            diff_txs.emplace_back(true, pointed_output_hash(OutPoint{OutPoint::Regular, txid, vout}, tx.outputs[vout]));
        auto ok = acc.verify(tx.proof, spent_hashes);
        if (!ok) fail("utreexo error: proof verification error");
        if (!*ok) fail("Utreexo proof verification failed for tx " + hexs(txid));
        validate_block_transaction(t, tx, ft);
        uint64_t fee = validate_filled(ft);
        if (total_fees + fee < total_fees) fail("Bitcoin amount overflow");
        total_fees += fee;
        pre.filled.push_back(std::move(ft));
    }
    bool pnc = false;
    auto root = compute_merkle_root(b.coinbase, pre.filled, &pnc);
    if (pnc) panic("Amount addition error");
    if (!root) fail("failed to compute merkle root");
    if (*root != h.merkle_root) fail("invalid body: expected merkle root " + hexs(h.merkle_root) + ", but computed " + hexs(*root));
    std::sort(all_input_keys.begin(), all_input_keys.end());
    if (std::adjacent_find(all_input_keys.begin(), all_input_keys.end()) != all_input_keys.end()) fail("utxo double spent");
    uint64_t coinbase_value = 0;
    for (uint32_t vout = 0; vout < b.coinbase.size(); vout++) {
        uint64_t v = b.coinbase[vout].get_value();
        if (coinbase_value + v < coinbase_value) fail("Bitcoin amount overflow");
        coinbase_value += v;
        pre.diff.insert(pointed_output_hash(OutPoint{OutPoint::Coinbase, *root, vout}, b.coinbase[vout]));
    }
    for (auto& [ins, hh] : diff_txs) {
        if (ins) pre.diff.insert(hh);
        else pre.diff.remove(hh);
    }
    if (coinbase_value > total_fees) fail("total fees less than coinbase value");
    // block.rs:156-184: known upstream behaviour reproduced; details held for coordinated disclosure
    std::set<size_t> claim_pending;
    size_t off = 0;
    for (auto& ft : pre.filled) {
        if (ft.tx->data.kind == TxData::SwapClaim)
            for (size_t i = 0; i < ft.spent_utxos.size(); i++)
                if (ft.spent_utxos[i].content.is_swap_pending()) claim_pending.insert(off + i);
        off += ft.spent_utxos.size();
    }
    {
        size_t idx = 0;
        for (auto& ft : pre.filled)
            for (auto& u : ft.spent_utxos) {
                if (idx >= b.authorizations.size()) goto zip_done; // zip stops at the shorter side
                if (!claim_pending.count(idx) && bt::sidechain::address_from_vk(b.authorizations[idx].verifying_key) != u.address)
                    fail("wrong public key for address");
                idx++;
            }
    zip_done:;
    }
    if (!verify_authorizations(b)) fail("failed to verify authorization");
    {
        MemForest f = acc;
        auto [ins, del] = pre.diff.split();
        if (auto e = f.modify(ins, del)) fail("utreexo error: " + *e);
        if (f.get_roots() != h.roots) fail("Computed Utreexo roots do not match the header roots");
    }
    pre.merkle_root = *root;
    pre.total_fees = total_fees;
    pre.coinbase_value = coinbase_value;
    for (size_t i = 0; i < pre.filled.size(); i++) pre.filled[i].tx = &pre.txs[i];
    return pre;
}

void State::connect_prevalidated(Txn& t, const Header& h, const Body& b, Prevalidated& pre) const
{
    auto tip_hash = tip(t);
    if (tip_hash != h.prev_side_hash) fail("invalid header: previous sidechain block hash mismatch");
    if (pre.merkle_root != h.merkle_root) fail("invalid body: expected merkle root " + hexs(pre.merkle_root) + ", but computed " + hexs(h.merkle_root));
    for (uint32_t vout = 0; vout < b.coinbase.size(); vout++) put_utxo(t, OutPoint{OutPoint::Coinbase, pre.merkle_root, vout}, b.coinbase[vout]);
    for (size_t i = 0; i < pre.filled.size(); i++) {
        auto& ft = pre.filled[i];
        const Transaction& tx = *ft.tx;
        Hash32 txid = tx.txid();
        for (uint32_t vin = 0; vin < tx.inputs.size(); vin++) {
            auto& op = tx.inputs[vin].first;
            auto u = utxo(t, op);
            if (!u) fail("utxo " + outpoint_str(op) + " doesn't exist");
            del_utxo(t, op);
            put_stxo(t, op, SpentOutput{*u, InPoint{InPoint::Regular, txid, vin}});
        }
        for (uint32_t vout = 0; vout < tx.outputs.size(); vout++) put_utxo(t, OutPoint{OutPoint::Regular, txid, vout}, tx.outputs[vout]);
        auto& d = tx.data;
        switch (d.kind) {
        case TxData::SwapCreate: {
            uint32_t cur = pre.next_height;
            Swap s;
            s.id = d.swap_id;
            s.direction = 1;
            s.parent_chain = d.parent_chain;
            s.l1_txid = SwapTxId::from_bytes(d.l1_txid_bytes);
            s.required_confirmations = d.required_confirmations;
            s.state = SwapState{SwapState::Pending};
            s.l2_recipient = d.l2_recipient;
            s.l2_amount = d.l2_amount;
            s.l1_recipient_address = d.l1_recipient_address;
            s.l1_amount = d.l1_amount;
            s.created_at_height = cur;
            s.expires_at_height = cur + default_swap_expiration_blocks(d.parent_chain); // u32 `+` (wraps in release)
            if (!ft.spent_utxos.empty()) s.l2_creator_address = ft.spent_utxos.front().address;
            for (uint32_t vout = 0; vout < tx.outputs.size(); vout++)
                if (tx.outputs[vout].content.kind == Content::SwapPending && tx.outputs[vout].content.swap_id == d.swap_id)
                    lock_output(t, OutPoint{OutPoint::Regular, txid, vout}, d.swap_id);
            save_swap(t, s);
            break;
        }
        case TxData::SwapClaim: {
            auto s = swap(t, d.swap_id);
            if (!s) fail("Swap not found: " + hexs(d.swap_id));
            // block.rs:410-424: known upstream behaviour reproduced; details held for coordinated disclosure
            if (s->state.kind != SwapState::ReadyToClaim) {
                s->state = SwapState{SwapState::ReadyToClaim};
                save_swap(t, *s);
            }
            if (!s->l2_recipient && !d.l2_claimer_address) invalid_tx("Open swap claim requires l2_claimer_address");
            for (auto& [op, uh] : tx.inputs)
                if (locked_to(t, op) == std::optional<Hash32>(d.swap_id)) unlock_output(t, op);
            s->state = SwapState{SwapState::Completed};
            save_swap(t, *s);
            break;
        }
        case TxData::SwapAccept:
            reserve_swap(t, d.swap_id, d.l2_claimer_address ? *d.l2_claimer_address : Address{}, pre.next_height);
            break;
        case TxData::Regular: break;
        }
    }
    set_tip(t, h.hash(), pre.next_height);
    apply_diff(t, pre.diff);
}

void State::disconnect_tip(Txn& t, const Header& h, const Body& b) const
{
    auto tp = tip(t);
    if (!tp) fail("no tip");
    Hash32 bh = h.hash();
    if (*tp != bh) fail("invalid header: expected block hash " + hexs(*tp) + ", but computed " + hexs(bh));
    AccumulatorDiff diff;
    for (auto it = b.transactions.rbegin(); it != b.transactions.rend(); ++it) {
        const Transaction& tx = *it;
        Hash32 txid = tx.txid();
        auto& d = tx.data;
        switch (d.kind) {
        case TxData::SwapCreate:
            for (uint32_t vout = uint32_t(tx.outputs.size()); vout-- > 0;)
                if (tx.outputs[vout].content.is_swap_pending()) {
                    OutPoint op{OutPoint::Regular, txid, vout};
                    if (locked_to(t, op) == std::optional<Hash32>(d.swap_id)) unlock_output(t, op);
                }
            clear_reservation(t, d.swap_id);
            delete_swap_unchecked(t, d.swap_id);
            break;
        case TxData::SwapClaim: {
            auto s = swap(t, d.swap_id);
            if (!s) fail("Swap not found: " + hexs(d.swap_id));
            for (auto in = tx.inputs.rbegin(); in != tx.inputs.rend(); ++in)
                if (!locked_to(t, in->first)) lock_output(t, in->first, d.swap_id); // block.rs:821-829
            if (s->state.kind == SwapState::Completed) {
                s->state = SwapState{SwapState::ReadyToClaim};
                save_swap(t, *s);
            }
            break;
        }
        case TxData::SwapAccept: clear_reservation(t, d.swap_id); break;
        case TxData::Regular: break;
        }
        for (uint32_t vout = uint32_t(tx.outputs.size()); vout-- > 0;) {
            OutPoint op{OutPoint::Regular, txid, vout};
            diff.remove(pointed_output_hash(op, tx.outputs[vout]));
            if (!del_utxo(t, op)) fail("utxo " + outpoint_str(op) + " doesn't exist");
        }
        for (auto in = tx.inputs.rbegin(); in != tx.inputs.rend(); ++in) {
            auto s = stxo(t, in->first);
            if (!s) fail("stxo " + outpoint_str(in->first) + " doesn't exist");
            diff.insert(in->second);
            del_stxo(t, in->first);
            put_utxo(t, in->first, s->output);
        }
    }
    for (uint32_t vout = uint32_t(b.coinbase.size()); vout-- > 0;) {
        OutPoint op{OutPoint::Coinbase, h.merkle_root, vout};
        diff.remove(pointed_output_hash(op, b.coinbase[vout]));
        if (!del_utxo(t, op)) fail("utxo " + outpoint_str(op) + " doesn't exist");
    }
    auto ht = height(t);
    if (!ht) panic("Height should not be None");
    if (!h.prev_side_hash && *ht == 0) {
        t.del(T_TIP, unit_key());
        t.del(T_HEIGHT, unit_key());
    } else if (!h.prev_side_hash || *ht == 0) {
        fail("no tip");
    } else {
        set_tip(t, *h.prev_side_hash, *ht - 1);
    }
    apply_diff(t, diff);
    restore_accumulator_for_tip(t); // net_task.rs disconnect_tip_: archive accumulator of the new tip
}

std::vector<NodeHash> State::roots_after(MemForest acc, const Body& b, const Hash32& merkle_root)
{
    std::vector<NodeHash> add, del;
    for (uint32_t vout = 0; vout < b.coinbase.size(); vout++)
        add.push_back(NodeHash::some(pointed_output_hash(OutPoint{OutPoint::Coinbase, merkle_root, vout}, b.coinbase[vout])));
    for (auto& tx : b.transactions) {
        Hash32 txid = tx.txid();
        for (auto& [op, h] : tx.inputs) del.push_back(NodeHash::some(h));
        for (uint32_t vout = 0; vout < tx.outputs.size(); vout++)
            add.push_back(NodeHash::some(pointed_output_hash(OutPoint{OutPoint::Regular, txid, vout}, tx.outputs[vout])));
    }
    if (auto e = acc.modify(add, del)) fail("utreexo error: " + *e);
    return acc.get_roots();
}

// ---------------------------------------------------------------- withdrawal bundles
std::optional<WithdrawalBundle> State::make_withdrawal_bundle(uint32_t block_height, uint64_t fee, std::map<OutPoint, Output> spend,
                                                              std::vector<bt::btc::TxOut> payouts)
{
    Writer w;
    w.borsh_len(spend.size() + 1);
    for (auto& [op, _] : spend) bt::sidechain::borsh(w, op);
    bt::sidechain::borsh(w, OutPoint{OutPoint::Regular, Hash32{}, block_height});
    Hash32 commitment = bt::blake3(w.buf);
    Writer fw;
    fw.u64be(fee);
    WithdrawalBundle b;
    b.spend_utxos = std::move(spend);
    b.outputs.push_back({0, bt::btc::op_return_push(fw.buf)});
    b.outputs.push_back({0, bt::btc::op_return_push(commitment.span())});
    for (auto& p : payouts) b.outputs.push_back(std::move(p));
    if (bt::btc::inputless_tx_weight(b.outputs) > 400000) return std::nullopt; // BundleTooHeavy
    return b;
}

std::optional<WithdrawalBundle> State::collect_withdrawal_bundle(const Txn& t, uint32_t block_height) const
{
    struct Agg {
        bt::btc::Address addr;
        uint64_t value = 0, main_fee = 0;
        std::map<OutPoint, Output> utxos;
    };
    std::vector<Agg> aggs; // HashMap keyed by Address
    for (auto& [op, o] : utxos(t)) {
        if (!o.content.is_withdrawal()) continue;
        auto it = std::find_if(aggs.begin(), aggs.end(), [&](const Agg& a) { return a.addr == o.content.main_address; });
        if (it == aggs.end()) {
            aggs.push_back(Agg{o.content.main_address, 0, 0, {}});
            it = std::prev(aggs.end());
        }
        if (it->value + o.content.value < it->value) fail("Bitcoin amount overflow");
        it->value += o.content.value;
        if (it->main_fee + o.content.main_fee < it->main_fee) fail("Bitcoin amount overflow");
        it->main_fee += o.content.main_fee;
        it->utxos[op] = o;
    }
    if (aggs.empty()) return std::nullopt;
    std::sort(aggs.begin(), aggs.end(), [](const Agg& a, const Agg& b) { // Reverse((main_fee, value, main_address))
        if (a.main_fee != b.main_fee) return a.main_fee > b.main_fee;
        if (a.value != b.value) return a.value > b.value;
        return b.addr < a.addr;
    });
    uint64_t fee = 0;
    std::map<OutPoint, Output> spend;
    std::vector<bt::btc::TxOut> outs;
    uint32_t txouts_size = 0;
    for (auto& a : aggs) {
        Bytes spk = a.addr.script_pubkey();
        uint64_t n_outputs = outs.size() + 1;
        if (n_outputs > UINT32_MAX) break;
        if (spk.size() > UINT32_MAX) continue;
        uint64_t ts = 8ull + bt::btc::compact_size_len(spk.size()) + spk.size();
        if (ts > UINT32_MAX) continue;
        uint64_t sum = uint64_t(txouts_size) + ts;
        if (sum > UINT32_MAX) break;
        txouts_size = uint32_t(sum);
        if (!bt::sidechain::predict_weight_ok(uint32_t(n_outputs), txouts_size)) break;
        for (auto& [op, o] : a.utxos) spend[op] = o;
        outs.push_back({a.value, spk});
        if (fee + a.main_fee < fee) panic("Amount addition error");
        fee += a.main_fee;
    }
    auto b = make_withdrawal_bundle(block_height, fee, std::move(spend), std::move(outs));
    if (!b) fail("Withdrawal bundle error");
    return b;
}

// ---------------------------------------------------------------- two-way peg (two_way_peg_data.rs)
void State::connect_bundle_submitted(Txn& t, uint32_t height, AccumulatorDiff& d, BlockIndexEvents& ev, const Hash32& m6id) const
{
    auto p = pending_bundle(t);
    if (p && p->first.m6id() == m6id) {
        if (p->second != height - 1) panic("assertion `left == right` failed: bundle_block_height == block_height - 1");
        uint64_t tw = 0, ts = 0; // `.sum()` of Amounts in their log line panics on overflow
        for (size_t i = 2; i < p->first.outputs.size(); i++) {
            if (tw + p->first.outputs[i].value < tw) panic("Amount addition error");
            tw += p->first.outputs[i].value;
        }
        for (auto& [op, o] : p->first.spend_utxos) {
            if (ts + o.get_value() < ts) panic("Amount addition error");
            ts += o.get_value();
        }
        for (auto& [op, o] : p->first.spend_utxos) {
            d.remove(pointed_output_hash(op, o));
            del_utxo(t, op);
            put_stxo(t, op, SpentOutput{o, InPoint{InPoint::Withdrawal, m6id, 0}});
            ev.bundle_spends.emplace_back(op, m6id);
        }
        WithdrawalBundleInfo info;
        info.kind = WithdrawalBundleInfo::Known;
        info.bundle = p->first;
        put_bundle(t, m6id, info, RB::make(BundleStatus::Submitted, height));
        t.del(T_PENDING_BUNDLE, unit_key());
    } else if (auto b = bundle(t, m6id)) {
        if (b->second.earliest().value != BundleStatus::Submitted) panic("assertion `left == right` failed: earliest status Submitted");
    } else {
        WithdrawalBundleInfo info;
        info.kind = WithdrawalBundleInfo::Unknown;
        put_bundle(t, m6id, info, RB::make(BundleStatus::Submitted, height));
    }
}

void State::connect_bundle_confirmed(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& event_block, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) fail("Unknown withdrawal bundle: " + m6id_str(m6id));
    auto& [info, st] = *b;
    if (st.latest().value == BundleStatus::Confirmed) return;
    if (st.latest().value != BundleStatus::Submitted) panic("assertion `left == right` failed: latest status Submitted");
    if (info.kind == WithdrawalBundleInfo::Known) {
        uint64_t tw = 0;
        for (size_t i = 2; i < info.bundle.outputs.size(); i++) {
            if (tw + info.bundle.outputs[i].value < tw) panic("Amount addition error");
            tw += info.bundle.outputs[i].value;
        }
    } else { // !is_known()
        if (height != 0) fail("Unknown withdrawal bundle confirmed in " + m6id_str(event_block) + ": " + m6id_str(m6id));
        std::map<OutPoint, Output> all;
        for (auto& [op, o] : utxos(t)) all[op] = o;
        for (auto& [op, o] : all) {
            put_stxo(t, op, SpentOutput{o, InPoint{InPoint::Withdrawal, m6id, 0}});
            d.remove(pointed_output_hash(op, o));
        }
        t.clear(T_UTXOS);
        info.kind = WithdrawalBundleInfo::UnknownConfirmed;
        info.spend_utxos = std::move(all);
    }
    if (!st.push(BundleStatus::Confirmed, height)) panic("Push confirmed status should be valid");
    put_bundle(t, m6id, info, st);
}

void State::connect_bundle_failed(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) fail("Unknown withdrawal bundle: " + m6id_str(m6id));
    auto& [info, st] = *b;
    if (st.latest().value == BundleStatus::Failed) return;
    if (st.latest().value != BundleStatus::Submitted) panic("assertion `left == right` failed: latest status Submitted");
    if (info.kind == WithdrawalBundleInfo::Known) {
        uint64_t tw = 0;
        for (size_t i = 2; i < info.bundle.outputs.size(); i++) {
            if (tw + info.bundle.outputs[i].value < tw) panic("Amount addition error");
            tw += info.bundle.outputs[i].value;
        }
    }
    if (!st.push(BundleStatus::Failed, height)) panic("Push failed status should be valid");
    if (info.kind == WithdrawalBundleInfo::Known) {
        for (auto& [op, o] : info.bundle.spend_utxos) {
            del_stxo(t, op);
            put_utxo(t, op, o);
            d.insert(pointed_output_hash(op, o));
        }
        bt::sidechain::RollBack<Hash32> nl;
        if (auto v = t.get(T_LATEST_FAILED, unit_key())) {
            nl = dec_latest_failed(*v);
            if (!nl.push(m6id, height)) panic("Push latest failed m6id should be valid");
        } else {
            nl = bt::sidechain::RollBack<Hash32>::make(m6id, height);
        }
        t.put(T_LATEST_FAILED, unit_key(), enc_latest_failed(nl));
    }
    put_bundle(t, m6id, info, st);
}

void State::disconnect_bundle_submitted(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) {
        auto p = pending_bundle(t);
        if (p && p->first.m6id() == m6id) return; // already applied
        fail("Unknown withdrawal bundle: " + m6id_str(m6id));
    }
    auto latest = b->second.latest();
    if (latest.height > height)
        fail("Unexpected status for withdrawal bundle " + m6id_str(m6id) + ": " + to_string(latest.value) + " at height " + std::to_string(latest.height) +
             ", while disconnecting block at height " + std::to_string(height));
    if (latest.value != BundleStatus::Submitted || latest.height != height) return; // applied by an earlier block
    if (b->first.kind == WithdrawalBundleInfo::Known) {
        auto& su = b->first.bundle.spend_utxos;
        for (auto it = su.rbegin(); it != su.rend(); ++it) {
            if (!del_stxo(t, it->first)) fail("stxo " + outpoint_str(it->first) + " doesn't exist");
            put_utxo(t, it->first, it->second);
            d.insert(pointed_output_hash(it->first, it->second));
        }
        put_pending(t, b->first.bundle, latest.height - 1);
    }
    t.del(T_BUNDLES, m6id_key(m6id));
}

void State::disconnect_bundle_confirmed(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) fail("Unknown withdrawal bundle: " + m6id_str(m6id));
    auto info = b->first;
    auto [prev, latest] = b->second.pop();
    if (latest.value == BundleStatus::Submitted) return; // already applied
    if (latest.value != BundleStatus::Confirmed) panic("assertion `left == right` failed: latest status Confirmed");
    if (latest.height != height) panic("assertion `left == right` failed: latest status height");
    if (!prev) panic("Pop confirmed bundle status should be valid");
    if (prev->latest().value != BundleStatus::Submitted) panic("assertion `left == right` failed: previous status Submitted");
    if (info.kind == WithdrawalBundleInfo::UnknownConfirmed) {
        for (auto& [op, o] : info.spend_utxos) {
            put_utxo(t, op, o);
            if (!del_stxo(t, op)) fail("stxo " + outpoint_str(op) + " doesn't exist");
            d.insert(pointed_output_hash(op, o));
        }
        info.kind = WithdrawalBundleInfo::Unknown;
        info.spend_utxos.clear();
    }
    put_bundle(t, m6id, info, *prev);
}

void State::disconnect_bundle_failed(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& m6id) const
{
    auto b = bundle(t, m6id);
    if (!b) fail("Unknown withdrawal bundle: " + m6id_str(m6id));
    auto [prev, latest] = b->second.pop();
    if (latest.value == BundleStatus::Submitted) return; // already applied
    if (latest.value != BundleStatus::Failed) panic("assertion `left == right` failed: latest status Failed");
    if (latest.height != height) panic("assertion `left == right` failed: latest status height");
    if (!prev) panic("Pop failed bundle status should be valid");
    if (prev->latest().value != BundleStatus::Submitted) panic("assertion `left == right` failed: previous status Submitted");
    if (b->first.kind == WithdrawalBundleInfo::Known) {
        auto& su = b->first.bundle.spend_utxos;
        for (auto it = su.rbegin(); it != su.rend(); ++it) {
            put_stxo(t, it->first, SpentOutput{it->second, InPoint{InPoint::Withdrawal, m6id, 0}});
            if (!del_utxo(t, it->first)) fail("utxo " + outpoint_str(it->first) + " doesn't exist");
            d.remove(pointed_output_hash(it->first, it->second));
        }
        auto v = t.get(T_LATEST_FAILED, unit_key());
        if (!v) panic("latest failed withdrawal bundle should exist");
        auto [prest, platest] = dec_latest_failed(*v).pop();
        if (platest.value != m6id) panic("assertion `left == right` failed: latest failed m6id");
        if (platest.height != height) panic("assertion `left == right` failed: latest failed height");
        if (prest) t.put(T_LATEST_FAILED, unit_key(), enc_latest_failed(*prest));
        else t.del(T_LATEST_FAILED, unit_key());
    }
    put_bundle(t, m6id, b->first, *prev);
}

void State::process_swaps(Txn& t, uint32_t height, const Hash32& block_hash) const
{
    struct Reversal {
        Hash32 id;
        SwapState prev;
        std::vector<OutPoint> unlocked;
    };
    std::vector<Reversal> reversals;
    for (Swap s : swaps(t)) {
        if (s.state.kind != SwapState::Pending && s.state.kind != SwapState::WaitingConfirmations) continue;
        if (s.expires_at_height && height >= *s.expires_at_height) {
            Reversal r{s.id, s.state, {}};
            std::vector<OutPoint> ops;
            t.scan(T_LOCKED, [&](ByteSpan k, ByteSpan v) {
                if (Hash32::from_span(v) == s.id) ops.push_back(OutPoint::from_key(k));
                return true;
            });
            for (auto& op : ops) {
                unlock_output(t, op);
                r.unlocked.push_back(op);
            }
            LOG_INFO("Swap expired, unlocking outputs and marking as cancelled: swap %s at height %u", hexs(s.id).c_str(), height);
            s.state = SwapState{SwapState::Cancelled};
            save_swap(t, s);
            reversals.push_back(std::move(r));
            continue;
        }
        // L1 lookup on the swap's parent chain, when this node has an L1 RPC config for it
        if (!l1_config || !l1_find) continue;
        auto cfg = l1_config(s.parent_chain);
        if (!cfg) continue;
        try {
            auto found = l1_find(*cfg, s.l1_recipient_address, s.l1_amount);
            if (!found) {
                LOG_WARN("Failed to query L1 for swap %s; swap will stay pending until RPC succeeds or l1_txid is set manually", hexs(s.id).c_str());
                continue;
            }
            std::vector<L1Match> matches;
            uint32_t max_age = max_l1_tx_age_blocks(s.parent_chain);
            for (auto& m : *found)
                if (m.confirmations != 0 && m.blockheight && m.confirmations <= max_age) matches.push_back(m);
            if (matches.empty()) continue;
            auto& m = matches.front();
            auto raw = bt::hex_decode(m.txid);
            if (m.txid.size() != 64 || !raw || raw->size() != 32) {
                LOG_WARN("Failed to query L1 for swap %s: Invalid response format", hexs(s.id).c_str());
                continue;
            }
            std::reverse(raw->begin(), raw->end());
            SwapTxId l1 = SwapTxId::from_bytes(*raw);
            bool updated = false;
            if (s.l1_txid.is_unset()) {
                // uniqueness: an L1 tx already associated with another swap is refused
                if (auto ex = t.get(T_SWAPS_BY_L1TXID, l1txid_key(s.parent_chain, l1))) {
                    auto es = swap(t, Hash32::from_span(*ex));
                    if (es && es->id != s.id) continue;
                }
                s.l1_txid = l1;
                s.l1_txid_validated_at_block_hash = block_hash;
                s.l1_txid_validated_at_height = height;
                if (m.confirmations >= s.required_confirmations) s.state = SwapState{SwapState::ReadyToClaim};
                else s.state = SwapState{SwapState::WaitingConfirmations, m.confirmations, s.required_confirmations};
                updated = true;
            } else {
                uint32_t cur = s.state.kind == SwapState::WaitingConfirmations ? s.state.current : 0;
                if (m.confirmations > cur) {
                    if (m.confirmations >= s.required_confirmations) s.state = SwapState{SwapState::ReadyToClaim};
                    else s.state = SwapState{SwapState::WaitingConfirmations, m.confirmations, s.required_confirmations};
                    updated = true;
                }
            }
            if (updated) save_swap(t, s);
        } catch (const StateError&) {
            throw;
        } catch (const std::exception& e) {
            LOG_WARN("Failed to query L1 for swap %s: %s", hexs(s.id).c_str(), e.what());
        }
    }
    if (!reversals.empty()) {
        Writer w;
        w.u64(reversals.size());
        for (auto& r : reversals) {
            w.raw(r.id);
            enc_state(w, r.prev);
            w.u64(r.unlocked.size());
            for (auto& op : r.unlocked) bt::sidechain::bincode(w, op);
        }
        t.put(T_EXPIRED_SWAPS, u32le(height), w.buf);
    }
}

void State::connect_2wpd(Txn& t, const bt::mainchain::TwoWayPegData& d) const
{
    auto ht = height(t);
    if (!ht) fail("no tip");
    uint32_t h = *ht;
    AccumulatorDiff diff;
    BlockIndexEvents ev;
    std::optional<Hash32> latest_deposit_block, latest_bundle_event_block;
    for (auto& [event_block, bi] : d.block_info) {
        for (auto& e : bi.events) {
            if (auto dep = std::get_if<bt::mainchain::Deposit>(&e)) {
                OutPoint op{OutPoint::Deposit, dep->txid, dep->vout};
                Output o;
                o.address = bt::sidechain::deposit_address(dep->address, THIS_SIDECHAIN);
                o.content = Content{Content::Value, dep->value_sats};
                put_utxo(t, op, o);
                diff.insert(pointed_output_hash(op, o));
                ev.deposits.emplace_back(op, o);
                latest_deposit_block = event_block;
            } else {
                auto& we = std::get<bt::mainchain::WithdrawalBundleEvent>(e);
                switch (we.status) {
                case bt::mainchain::BundleStatus::Submitted: connect_bundle_submitted(t, h, diff, ev, we.m6id); break;
                case bt::mainchain::BundleStatus::Confirmed: connect_bundle_confirmed(t, h, diff, event_block, we.m6id); break;
                case bt::mainchain::BundleStatus::Failed: connect_bundle_failed(t, h, diff, we.m6id); break;
                }
                latest_bundle_event_block = event_block;
            }
        }
    }
    if (!ev.empty()) {
        Writer w;
        w.u64(ev.deposits.size());
        for (auto& [op, o] : ev.deposits) {
            bt::sidechain::bincode(w, op);
            bincode(w, o);
        }
        w.u64(ev.bundle_spends.size());
        for (auto& [op, m] : ev.bundle_spends) {
            bt::sidechain::bincode(w, op);
            w.bincode_bytes(m.span());
        }
        t.put(T_BLOCK_INDEX_EVENTS, u32le(h), w.buf);
    }
    auto tp = tip(t);
    if (!tp) fail("no tip");
    process_swaps(t, h, *tp);
    auto enc_ref = [](const Hash32& bh, uint32_t height) {
        Writer w;
        w.bincode_bytes(bh.span());
        w.u32(height);
        return w.buf;
    };
    if (latest_deposit_block) {
        auto l = seq_last(t, T_DEPOSIT_BLOCKS);
        uint32_t seq = l ? l->first + 1 : 0;
        t.put(T_DEPOSIT_BLOCKS, u32le(seq), enc_ref(*latest_deposit_block, h));
    }
    if (latest_bundle_event_block) {
        auto l = seq_last(t, T_BUNDLE_EVENT_BLOCKS);
        uint32_t seq = l ? l->first + 1 : 0;
        t.put(T_BUNDLE_EVENT_BLOCKS, u32le(seq), enc_ref(*latest_bundle_event_block, h));
    }
    auto lf = latest_failed_bundle(t);
    uint32_t last_failure_height = lf ? lf->first : 0;
    if (uint32_t(h - last_failure_height) >= WITHDRAWAL_BUNDLE_FAILURE_GAP && !t.get(T_PENDING_BUNDLE, unit_key())) {
        if (auto b = collect_withdrawal_bundle(t, h)) put_pending(t, *b, h);
    }
    apply_diff(t, diff);
    // net_task.rs connect_tip_: archive.put_accumulator(block_hash, state accumulator)
    auto accb = t.get(T_ACCUMULATOR, unit_key());
    t.put(T_ACC_BY_BLOCK, tp->span(), accb ? *accb : MemForest{}.serialize());
}

void State::disconnect_2wpd(Txn& t, const bt::mainchain::TwoWayPegData& d) const
{
    auto ht = height(t);
    if (!ht) panic("Height should not be None");
    uint32_t h = *ht;
    AccumulatorDiff diff;
    std::optional<Hash32> latest_deposit_block, latest_bundle_event_block;
    t.del(T_BLOCK_INDEX_EVENTS, u32le(h));
    if (auto v = t.get(T_EXPIRED_SWAPS, u32le(h))) {
        Reader r(*v);
        uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
            Hash32 id = r.array<32>();
            SwapState prev = dec_state(r);
            uint64_t m = r.u64();
            std::vector<OutPoint> ops;
            for (uint64_t j = 0; j < m; j++) ops.push_back(bt::sidechain::de_outpoint(r));
            if (auto s = swap(t, id)) {
                s->state = prev;
                save_swap(t, *s);
            }
            for (auto& op : ops) lock_output(t, op, id);
        }
        t.del(T_EXPIRED_SWAPS, u32le(h));
    }
    for (auto bit = d.block_info.rbegin(); bit != d.block_info.rend(); ++bit) {
        auto& [event_block, bi] = *bit;
        for (auto eit = bi.events.rbegin(); eit != bi.events.rend(); ++eit) {
            if (auto dep = std::get_if<bt::mainchain::Deposit>(&*eit)) {
                OutPoint op{OutPoint::Deposit, dep->txid, dep->vout};
                Output o;
                o.address = bt::sidechain::deposit_address(dep->address, THIS_SIDECHAIN);
                o.content = Content{Content::Value, dep->value_sats};
                if (!del_utxo(t, op)) fail("utxo " + outpoint_str(op) + " doesn't exist");
                diff.remove(pointed_output_hash(op, o));
                if (!latest_deposit_block) latest_deposit_block = event_block;
            } else {
                auto& we = std::get<bt::mainchain::WithdrawalBundleEvent>(*eit);
                switch (we.status) {
                case bt::mainchain::BundleStatus::Submitted: disconnect_bundle_submitted(t, h, diff, we.m6id); break;
                case bt::mainchain::BundleStatus::Confirmed: disconnect_bundle_confirmed(t, h, diff, we.m6id); break;
                case bt::mainchain::BundleStatus::Failed: disconnect_bundle_failed(t, h, diff, we.m6id); break;
                }
                if (!latest_bundle_event_block) latest_bundle_event_block = event_block;
            }
        }
    }
    if (latest_bundle_event_block) {
        auto l = seq_last(t, T_BUNDLE_EVENT_BLOCKS);
        if (!l) fail("Withdrawal bundle event block doesn't exist");
        if (l->second.first != *latest_bundle_event_block) panic("assertion `left == right` failed: withdrawal bundle event block hash");
        if (l->second.second != h) panic("assertion `left == right` failed: withdrawal bundle event block height");
        if (!t.del(T_BUNDLE_EVENT_BLOCKS, u32le(l->first))) fail("Withdrawal bundle event block doesn't exist");
    }
    auto lf = latest_failed_bundle(t);
    uint32_t last_failure_height = lf ? lf->first : 0;
    if (uint32_t(h - last_failure_height) >= WITHDRAWAL_BUNDLE_FAILURE_GAP) {
        if (auto p = pending_bundle(t); p && p->second == h) t.del(T_PENDING_BUNDLE, unit_key());
    }
    if (latest_deposit_block) {
        auto l = seq_last(t, T_DEPOSIT_BLOCKS);
        if (!l) fail("deposit block doesn't exist");
        if (l->second.first != *latest_deposit_block) panic("assertion `left == right` failed: deposit block hash");
        if (l->second.second != h) panic("assertion `left == right` failed: deposit block height");
        if (!t.del(T_DEPOSIT_BLOCKS, u32le(l->first))) fail("deposit block doesn't exist");
    }
    apply_diff(t, diff);
}

} // namespace coinshift
