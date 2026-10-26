// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Port of coinshift-rs v0.14.8 lib/wallet/mod.rs (keys, coin selection, the transaction
// builders the RPC uses).
#include <coinshift/wallet.h>

#include <bt/crypto.h>
#include <bt/hd.h>
#include <bt/log.h>

#include <algorithm>
#include <cstring>

#include <sodium.h>

namespace coinshift {

using bt::kv::Txn;

enum : uint8_t { W_SEED = 1, W_INDEX_TO_ADDR, W_ADDR_TO_INDEX, W_UTXOS, W_STXOS };

Wallet::Wallet(const std::string& path, Node& node) : store_(bt::kv::open_sqlite(path)), node_(node) {}

// m/43'/1899'/0'/255'/0'/index over ristretto255 (wallet/mod.rs derive_signing_key, bip32.rs)
bt::frost::SigningKey Wallet::signing_key(const bt::ByteArray<64>& seed, uint32_t index)
{
    return bt::hd::sidechain_signing_key(seed, THIS_SIDECHAIN, index, bt::hd::Variant::V0_18_0);
}

static Bytes be32(uint32_t v)
{
    Writer w;
    w.u32be(v);
    return w.buf;
}
static uint32_t from_be32(ByteSpan b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
static Bytes key37(const OutPoint& o)
{
    auto k = o.key();
    return Bytes(k.begin(), k.end());
}

std::optional<bt::ByteArray<64>> Wallet::seed()
{
    Txn t(*store_);
    auto v = t.get(W_SEED, Bytes{0});
    if (!v || v->size() != 64) return std::nullopt;
    return bt::ByteArray<64>::from_span(*v);
}

bt::ByteArray<64> Wallet::need_seed()
{
    auto s = seed();
    if (!s) throw WalletError("wallet does not have a seed (set with RPC `set-seed-from-mnemonic`)");
    return *s;
}

std::optional<Hash32> Wallet::seed_digest(std::string_view label)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto s = seed();
    if (!s) return std::nullopt;
    const uint8_t zero = 0;
    Hash32 h = bt::sha256_parts({ByteSpan(reinterpret_cast<const uint8_t*>(label.data()), label.size()), ByteSpan(&zero, 1), s->span()});
    sodium_memzero(s->data(), s->size());
    return h;
}

bool Wallet::has_seed()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    return seed().has_value();
}

void Wallet::set_seed(const bt::ByteArray<64>& s)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto cur = seed();
    if (cur) {
        if (*cur == s) return;
        throw WalletError("seed has already been set");
    }
    Txn t(*store_);
    t.put(W_SEED, Bytes{0}, s.span());
    for (uint8_t tb : {W_INDEX_TO_ADDR, W_ADDR_TO_INDEX, W_UTXOS, W_STXOS}) t.clear(tb);
    t.commit();
}

void Wallet::set_seed_from_mnemonic(const std::string& mnemonic)
{
    auto p = bt::hd::parse_mnemonic(mnemonic);
    if (!p) throw WalletError("failed to parse mnemonic seed phrase: " + bt::hd::mnemonic_error(mnemonic));
    set_seed(bt::hd::mnemonic_to_seed(*p));
}

Address Wallet::get_new_address()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto s = need_seed();
    Txn t(*store_);
    auto last = t.last(W_INDEX_TO_ADDR);
    uint32_t index = last ? from_be32(last->first) + 1 : 0;
    Address a = bt::sidechain::address_from_vk(signing_key(s, index).verifying_key());
    t.put(W_INDEX_TO_ADDR, be32(index), a.b.span());
    t.put(W_ADDR_TO_INDEX, a.b.span(), be32(index));
    t.commit();
    return a;
}

Address Wallet::get_receive_address()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    {
        Txn t(*store_);
        auto last = t.last(W_INDEX_TO_ADDR);
        if (last) {
            Address a;
            a.b = bt::Hash20::from_span(last->second);
            bool received = false;
            auto check = [&](uint8_t tb) {
                t.scan(tb, [&](ByteSpan, ByteSpan v) {
                    Reader r(v);
                    if (de_output(r).address == a) received = true;
                    return !received;
                });
            };
            check(W_UTXOS);
            if (!received) check(W_STXOS);
            if (!received) return a;
        }
    }
    return get_new_address();
}

std::set<Address> Wallet::addresses()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::set<Address> r;
    Txn t(*store_);
    t.scan(W_INDEX_TO_ADDR, [&](ByteSpan, ByteSpan v) {
        Address a;
        a.b = bt::Hash20::from_span(v);
        r.insert(a);
        return true;
    });
    return r;
}

bool Wallet::is_mine(const Address& a)
{
    Txn t(*store_);
    return t.get(W_ADDR_TO_INDEX, a.b.span()).has_value();
}

std::map<OutPoint, Output> Wallet::utxos()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    std::map<OutPoint, Output> r;
    Txn t(*store_);
    t.scan(W_UTXOS, [&](ByteSpan k, ByteSpan v) {
        Reader rd(v);
        r[OutPoint::from_key(k)] = de_output(rd);
        return true;
    });
    return r;
}

Balance Wallet::balance()
{
    Balance b;
    for (auto& [op, o] : utxos()) {
        uint64_t v = o.get_value();
        if (b.total + v < b.total) throw WalletError("Bitcoin amount overflow");
        b.total += v;
        if (!o.content.is_withdrawal()) b.available += v;
    }
    return b;
}

void Wallet::put_utxos(const std::map<OutPoint, Output>& u)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    Txn t(*store_);
    for (auto& [op, o] : u) {
        Writer w;
        bincode(w, o);
        t.put(W_UTXOS, key37(op), w.buf);
    }
    t.commit();
}

void Wallet::update()
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto addrs = addresses();
    std::map<OutPoint, Output> fresh;
    std::vector<std::pair<OutPoint, SpentOutput>> spent;
    {
        std::lock_guard<std::recursive_mutex> nl(node_.lock());
        Txn nt(node_.store());
        for (auto& [op, o] : node_.state().utxos(nt))
            if (addrs.count(o.address)) fresh[op] = o;
        for (auto& [op, o] : utxos())
            if (auto s = node_.state().stxo(nt, op)) spent.emplace_back(op, *s);
    }
    put_utxos(fresh);
    Txn t(*store_);
    for (auto& [op, s] : spent) {
        auto k = key37(op);
        auto v = t.get(W_UTXOS, k);
        if (!v) continue;
        t.del(W_UTXOS, k);
        Writer w;
        Reader r(*v);
        bincode(w, de_output(r));
        w.u32(s.inpoint.kind);
        w.raw(s.inpoint.id);
        w.u32(s.inpoint.vin);
        t.put(W_STXOS, k, w.buf);
    }
    t.commit();
}

size_t Wallet::recover_addresses(const std::set<Address>& utxo_addresses)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    auto s = seed();
    if (!s || utxo_addresses.empty()) return 0;
    size_t gap = 0, recovered = 0;
    for (uint32_t index = 0; gap < 20 && index < 100000; index++) {
        Address a = bt::sidechain::address_from_vk(signing_key(*s, index).verifying_key());
        if (utxo_addresses.count(a)) {
            if (!is_mine(a)) {
                Txn t(*store_);
                t.put(W_INDEX_TO_ADDR, be32(index), a.b.span());
                t.put(W_ADDR_TO_INDEX, a.b.span(), be32(index));
                t.commit();
                recovered++;
            }
            gap = 0;
        } else {
            gap++;
        }
    }
    return recovered;
}

void Wallet::ensure_address_indexed(const Address& a)
{
    if (is_mine(a)) return;
    auto s = need_seed();
    for (uint32_t i = 0; i < 100000; i++)
        if (bt::sidechain::address_from_vk(signing_key(s, i).verifying_key()) == a) {
            Txn t(*store_);
            t.put(W_INDEX_TO_ADDR, be32(i), a.b.span());
            t.put(W_ADDR_TO_INDEX, a.b.span(), be32(i));
            t.commit();
            return;
        }
    throw WalletError("address " + a.to_base58() + " not derivable from seed in first 100000 indices");
}

// select_coins_with_filter: by value ascending, skip withdrawals and locked outputs, stop once covered
std::pair<uint64_t, std::vector<std::pair<OutPoint, Output>>> Wallet::select_coins(uint64_t value, const IsLocked& locked)
{
    auto u = utxos();
    std::vector<std::pair<OutPoint, Output>> all(u.begin(), u.end());
    std::stable_sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.second.get_value() < b.second.get_value(); });
    uint64_t total = 0;
    std::vector<std::pair<OutPoint, Output>> sel;
    std::optional<uint32_t> ht;
    {
        std::lock_guard<std::recursive_mutex> nl(node_.lock());
        Txn nt(node_.store());
        ht = node_.state().height(nt);
    }
    for (auto& [op, o] : all) {
        if (o.content.is_withdrawal()) continue;
        if (locked(op)) continue;
        if (o.content.is_swap_pending()) { // kept fix: only escrows of swaps expired by height
            std::lock_guard<std::recursive_mutex> nl(node_.lock());
            Txn nt(node_.store());
            auto s = node_.state().swap(nt, o.content.swap_id);
            uint32_t h = ht ? *ht + 1 : 0;
            if (s && !(s->expires_at_height && h >= *s->expires_at_height)) continue;
            // Kept fix (wallet only): an expired escrow is taken back only by the swap's creator; an escrow at this
            // wallet's address as a swap's pre-specified recipient is never spent as a coin (known upstream
            // behaviour reproduced in consensus; this wallet does not rely on it).
            if (!s || !s->l2_creator_address || !is_mine(*s->l2_creator_address)) continue;
        }
        if (total >= value) break;
        if (total + o.get_value() < total) throw WalletError("Bitcoin amount overflow");
        total += o.get_value();
        sel.emplace_back(op, o);
    }
    if (total < value) throw WalletError("not enough funds");
    return {total, sel};
}

std::vector<std::pair<OutPoint, Hash32>> Wallet::inputs_of(const std::vector<std::pair<OutPoint, Output>>& coins)
{
    std::vector<std::pair<OutPoint, Hash32>> r;
    for (auto& [op, o] : coins) r.emplace_back(op, pointed_output_hash(op, o));
    return r;
}

bt::utreexo::Proof Wallet::prove(const std::vector<std::pair<OutPoint, Hash32>>& inputs)
{
    std::lock_guard<std::recursive_mutex> nl(node_.lock());
    Txn nt(node_.store());
    std::vector<bt::utreexo::NodeHash> targets;
    for (auto& [op, h] : inputs) targets.push_back(bt::utreexo::NodeHash::some(h));
    auto p = node_.state().accumulator(nt).prove(targets);
    if (!p) throw WalletError("utreexo error: could not prove the inputs");
    return *p;
}

static uint64_t add(uint64_t a, uint64_t b)
{
    if (a + b < a) throw WalletError("Bitcoin amount overflow");
    return a + b;
}

Transaction Wallet::create_transaction_many(const std::map<Address, uint64_t>& dests, uint64_t fee, const IsLocked& locked)
{
    if (dests.empty()) throw WalletError("no transfer destination");
    uint64_t value = 0;
    for (auto& [a, v] : dests) value = add(value, v);
    auto [total, coins] = select_coins(add(value, fee), locked);
    Transaction tx;
    tx.inputs = inputs_of(coins);
    tx.proof = prove(tx.inputs);
    for (auto& [a, v] : dests) tx.outputs.push_back(Output{a, Content{Content::Value, v}});
    tx.outputs.push_back(Output{get_new_address(), Content{Content::Value, total - value - fee}});
    return tx;
}

Transaction Wallet::create_withdrawal(const bt::btc::Address& main_address, uint64_t value, uint64_t main_fee, uint64_t fee, const IsLocked& locked)
{
    auto [total, coins] = select_coins(add(add(value, fee), main_fee), locked);
    Transaction tx;
    tx.inputs = inputs_of(coins);
    tx.proof = prove(tx.inputs);
    Content w{Content::Withdrawal, value};
    w.main_fee = main_fee;
    w.main_address = main_address;
    tx.outputs.push_back(Output{get_new_address(), w});
    tx.outputs.push_back(Output{get_new_address(), Content{Content::Value, total - value - fee - main_fee}});
    return tx;
}

std::pair<Transaction, Hash32> Wallet::create_swap_create_tx(ParentChain pc, const std::string& l1_recipient, uint64_t l1_amount,
                                                              const std::optional<Address>& l2_recipient, uint64_t l2_amount,
                                                              std::optional<uint32_t> required_confirmations, uint64_t fee, const IsLocked& locked)
{
    auto [total, coins] = select_coins(add(l2_amount, fee), locked);
    Address sender = coins.front().second.address;
    Hash32 id = swap_id_l2_to_l1(l1_recipient, l1_amount, sender, l2_recipient);
    Transaction tx;
    tx.inputs = inputs_of(coins);
    tx.proof = prove(tx.inputs);
    Content sp{Content::SwapPending, l2_amount};
    sp.swap_id = id;
    tx.outputs.push_back(Output{l2_recipient ? *l2_recipient : sender, sp});
    tx.outputs.push_back(Output{get_new_address(), Content{Content::Value, total - l2_amount - fee}});
    tx.data.kind = TxData::SwapCreate;
    tx.data.swap_id = id;
    tx.data.parent_chain = pc;
    tx.data.l1_txid_bytes = Bytes(32, 0); // placeholder for L2 -> L1
    tx.data.required_confirmations = required_confirmations ? *required_confirmations : default_confirmations(pc);
    tx.data.l2_recipient = l2_recipient;
    tx.data.l2_amount = l2_amount;
    tx.data.l1_recipient_address = l1_recipient;
    tx.data.l1_amount = l1_amount;
    return {tx, id};
}

Transaction Wallet::create_swap_accept_tx(const Hash32& swap_id, const std::optional<Address>& claimer, uint64_t fee, const IsLocked& locked)
{
    std::vector<std::pair<OutPoint, Output>> cands;
    for (auto& [op, o] : utxos())
        if (!o.content.is_withdrawal() && !o.content.is_swap_pending() && !locked(op) && (!claimer || o.address == *claimer) && o.get_value() >= fee)
            cands.emplace_back(op, o);
    std::stable_sort(cands.begin(), cands.end(), [](auto& a, auto& b) { return a.second.get_value() < b.second.get_value(); });
    if (cands.empty()) throw WalletError("not enough funds");
    auto [op, o] = cands.front();
    Transaction tx;
    tx.inputs = {{op, pointed_output_hash(op, o)}};
    tx.proof = prove(tx.inputs);
    uint64_t change = o.get_value() - fee;
    if (change > 0) tx.outputs.push_back(Output{get_new_address(), Content{Content::Value, change}});
    tx.data.kind = TxData::SwapAccept;
    tx.data.swap_id = swap_id;
    tx.data.l2_claimer_address = claimer ? *claimer : o.address;
    return tx;
}

Transaction Wallet::create_swap_claim_tx(const Hash32& swap_id, const Address& recipient, const std::vector<std::pair<OutPoint, Output>>& locked_outputs,
                                         const std::optional<Address>& l2_claimer)
{
    Transaction tx;
    tx.inputs = inputs_of(locked_outputs);
    tx.proof = prove(tx.inputs);
    uint64_t total = 0;
    for (auto& [op, o] : locked_outputs) total = add(total, o.get_value());
    tx.outputs.push_back(Output{recipient, Content{Content::Value, total}});
    tx.data.kind = TxData::SwapClaim;
    tx.data.swap_id = swap_id;
    tx.data.l2_claimer_address = l2_claimer;
    return tx;
}

AuthorizedTransaction Wallet::authorize(const Transaction& tx)
{
    std::lock_guard<std::recursive_mutex> l(mu_);
    AuthorizedTransaction at;
    at.transaction = tx;
    Bytes msg = tx.canonical_bytes();
    bool claim = tx.data.kind == TxData::SwapClaim;
    auto s = need_seed();
    auto u = utxos();
    for (auto& [op, h] : tx.inputs) {
        auto it = u.find(op);
        if (it == u.end()) throw WalletError("utxo does not exist");
        Txn t(*store_);
        auto idx = t.get(W_ADDR_TO_INDEX, it->second.address.b.span());
        uint32_t index;
        if (idx) index = from_be32(*idx);
        else if (claim && it->second.content.is_swap_pending()) index = 0; // the claimer's own key (wallet/mod.rs authorize)
        else {
            ensure_address_indexed(it->second.address);
            Txn t2(*store_);
            index = from_be32(*t2.get(W_ADDR_TO_INDEX, it->second.address.b.span()));
        }
        auto k = signing_key(s, index);
        at.authorizations.push_back(Authorization{k.verifying_key(), k.sign(msg)});
    }
    return at;
}

} // namespace coinshift
