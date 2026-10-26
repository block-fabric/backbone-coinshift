// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <coinshift/types.h>

#include <bt/sidechain/cbmt.h>

#include <cmath>

namespace coinshift {

using bt::utreexo::NodeHash;

// ---------------------------------------------------------------- parent chains (types/swap.rs)
const char* parent_chain_name(ParentChain c)
{
    switch (c) {
    case ParentChain::BTC: return "BTC";
    case ParentChain::BCH: return "BCH";
    case ParentChain::LTC: return "LTC";
    case ParentChain::Signet: return "Signet";
    case ParentChain::Regtest: return "Regtest";
    }
    return "?";
}

std::optional<ParentChain> parent_chain_from_name(std::string_view s)
{
    for (uint8_t i = 0; i <= 4; i++)
        if (s == parent_chain_name(ParentChain(i))) return ParentChain(i);
    return std::nullopt;
}

uint32_t default_confirmations(ParentChain c) { return c == ParentChain::BTC ? 6 : 3; }

uint32_t default_swap_expiration_blocks(ParentChain c)
{
    switch (c) {
    case ParentChain::BTC: return 1008;
    case ParentChain::BCH:
    case ParentChain::LTC:
    case ParentChain::Signet: return 432;
    case ParentChain::Regtest: return 50;
    }
    return 0;
}

uint32_t accept_expiration_blocks(ParentChain c)
{
    switch (c) {
    case ParentChain::BTC: return 144;
    case ParentChain::BCH:
    case ParentChain::LTC:
    case ParentChain::Signet: return 72;
    case ParentChain::Regtest: return 20;
    }
    return 0;
}

uint32_t max_l1_tx_age_blocks(ParentChain c)
{
    switch (c) {
    case ParentChain::BTC:
    case ParentChain::BCH:
    case ParentChain::Signet: return 2016;
    case ParentChain::LTC: return 8064;
    case ParentChain::Regtest: return 500;
    }
    return 0;
}

const char* coin_name(ParentChain c)
{
    switch (c) {
    case ParentChain::BTC: return "Bitcoin";
    case ParentChain::BCH: return "Bitcoin Cash";
    case ParentChain::LTC: return "Litecoin";
    case ParentChain::Signet: return "Bitcoin Signet";
    case ParentChain::Regtest: return "Bitcoin Regtest";
    }
    return "?";
}

uint32_t SwapReservation::expires_at(ParentChain c) const
{
    uint64_t e = uint64_t(accepted_at_height) + accept_expiration_blocks(c); // saturating_add
    return e > UINT32_MAX ? UINT32_MAX : uint32_t(e);
}

Hash32 swap_id_l2_to_l1(const std::string& l1_recipient_address, uint64_t l1_amount_sats, const Address& l2_sender,
                        const std::optional<Address>& l2_recipient)
{
    Bytes d(l1_recipient_address.begin(), l1_recipient_address.end());
    for (int i = 0; i < 8; i++) d.push_back(uint8_t(l1_amount_sats >> (8 * i)));
    append(d, l2_sender.b.span());
    if (l2_recipient) append(d, l2_recipient->b.span());
    else {
        static const char open[] = "OPEN_SWAP";
        d.insert(d.end(), open, open + 9);
    }
    return bt::blake3(d);
}

SwapTxId SwapTxId::from_bytes(ByteSpan b)
{
    SwapTxId t;
    t.kind = b.size() == 32 ? Hash32 : Hash;
    t.bytes.assign(b.begin(), b.end());
    return t;
}

bool SwapTxId::is_unset() const
{
    if (kind == Hash32) {
        for (auto x : bytes)
            if (x) return false;
        return true;
    }
    for (auto x : bytes)
        if (x) return false;
    return true; // empty or all zero
}

std::string SwapTxId::to_hex() const { return bt::hex_encode(bytes); }

// ---------------------------------------------------------------- content
uint64_t Content::get_value() const
{
    if (kind == Withdrawal) {
        uint64_t s = value + main_fee;
        return s < value ? UINT64_MAX : s;
    }
    return value;
}

bool Content::operator==(const Content& o) const
{
    if (kind != o.kind || value != o.value) return false;
    if (kind == Withdrawal) return main_fee == o.main_fee && main_address == o.main_address;
    if (kind == SwapPending) return swap_id == o.swap_id;
    return true;
}

// ---------------------------------------------------------------- borsh
void borsh(Writer& w, const Address& a) { w.raw(a.b); }

void borsh(Writer& w, const Content& c)
{
    w.u8(c.kind);
    w.u64(c.value);
    if (c.kind == Content::Withdrawal) {
        w.u64(c.main_fee);
        w.borsh_bytes(c.main_address.script_pubkey()); // borsh_serialize_bitcoin_address: spk bytes
    } else if (c.kind == Content::SwapPending) {
        w.raw(c.swap_id);
    }
}

void borsh(Writer& w, const Output& o)
{
    borsh(w, o.address);
    borsh(w, o.content);
}

namespace {
void borsh_opt_addr(Writer& w, const std::optional<Address>& a)
{
    if (a) {
        w.u8(1);
        w.raw(a->b);
    } else {
        w.u8(0);
    }
}
void bincode_opt_addr(Writer& w, const std::optional<Address>& a) { borsh_opt_addr(w, a); } // same bytes
std::optional<Address> de_opt_addr(Reader& r)
{
    uint8_t t = r.u8();
    if (t == 0) return std::nullopt;
    if (t != 1) throw DecodeError("invalid Option tag");
    Address a;
    a.b = r.array<20>();
    return a;
}
} // namespace

void borsh(Writer& w, const TxData& d)
{
    w.u8(d.kind);
    switch (d.kind) {
    case TxData::Regular: break;
    case TxData::SwapCreate:
        w.raw(d.swap_id);
        w.u8(uint8_t(d.parent_chain));
        w.borsh_bytes(d.l1_txid_bytes);
        w.u32(d.required_confirmations);
        borsh_opt_addr(w, d.l2_recipient);
        w.u64(d.l2_amount);
        w.borsh_bytes(ByteSpan((const uint8_t*)d.l1_recipient_address.data(), d.l1_recipient_address.size()));
        w.u64(d.l1_amount);
        break;
    case TxData::SwapClaim:
        w.raw(d.swap_id);
        borsh_opt_addr(w, d.l2_claimer_address);
        if (d.proof_data) {
            w.u8(1);
            w.borsh_bytes(*d.proof_data);
        } else {
            w.u8(0);
        }
        break;
    case TxData::SwapAccept:
        w.raw(d.swap_id);
        w.raw(d.l2_claimer_address ? d.l2_claimer_address->b : bt::Hash20{});
        break;
    }
}

void borsh(Writer& w, const Transaction& t)
{
    w.borsh_len(t.inputs.size());
    for (auto& [op, h] : t.inputs) {
        bt::sidechain::borsh(w, op);
        w.raw(h);
    }
    w.borsh_len(t.outputs.size());
    for (auto& o : t.outputs) borsh(w, o);
    borsh(w, t.data);
}

void borsh(Writer& w, const Authorization& a)
{
    w.raw(a.verifying_key);
    w.raw(a.signature.R);
    w.raw(a.signature.z);
}

void borsh(Writer& w, const Body& b)
{
    w.borsh_len(b.coinbase.size());
    for (auto& o : b.coinbase) borsh(w, o);
    w.borsh_len(b.transactions.size());
    for (auto& t : b.transactions) borsh(w, t);
    w.borsh_len(b.authorizations.size());
    for (auto& a : b.authorizations) borsh(w, a);
}

void borsh(Writer& w, const Header& h)
{
    w.raw(h.merkle_root);
    if (h.prev_side_hash) {
        w.u8(1);
        w.raw(*h.prev_side_hash);
    } else {
        w.u8(0);
    }
    w.raw(h.prev_main_hash);
    w.borsh_len(h.roots.size());
    for (auto& r : h.roots) w.raw(r.bytes()); // Deref: Empty/Placeholder -> zeros
}

Bytes Transaction::canonical_bytes() const { return borsh_bytes(*this); }
Hash32 Transaction::txid() const { return bt::blake3(canonical_bytes()); }
Hash32 Header::hash() const { return coinshift::hash(*this); }

Hash32 pointed_output_hash(const OutPoint& op, const Output& out)
{
    Writer w;
    bt::sidechain::borsh(w, op);
    borsh(w, out);
    return bt::blake3(w.buf);
}

size_t Body::inputs_len() const
{
    size_t n = 0;
    for (auto& t : transactions) n += t.inputs.size();
    return n;
}

std::vector<AuthorizedTransaction> Body::authorized_transactions() const
{
    std::vector<AuthorizedTransaction> r;
    size_t k = 0;
    for (auto& t : transactions) {
        AuthorizedTransaction at;
        at.transaction = t;
        for (size_t i = 0; i < t.inputs.size() && k < authorizations.size(); i++) at.authorizations.push_back(authorizations[k++]);
        r.push_back(std::move(at));
    }
    return r;
}

Body Body::from_authorized(std::vector<AuthorizedTransaction> txs, std::vector<Output> coinbase)
{
    Body b;
    b.coinbase = std::move(coinbase);
    for (auto& t : txs) {
        for (auto& a : t.authorizations) b.authorizations.push_back(a);
        b.transactions.push_back(std::move(t.transaction));
    }
    return b;
}

std::optional<uint64_t> FilledTransaction::value_in() const
{
    uint64_t in = 0;
    for (auto& o : spent_utxos) {
        uint64_t v = o.get_value();
        if (in + v < in) return std::nullopt;
        in += v;
    }
    return in;
}

std::optional<uint64_t> FilledTransaction::value_out() const
{
    uint64_t out = 0;
    for (auto& o : tx->outputs) {
        uint64_t v = o.get_value();
        if (out + v < out) return std::nullopt;
        out += v;
    }
    return out;
}

std::optional<uint64_t> FilledTransaction::fee() const
{
    auto in = value_in();
    if (!in) return std::nullopt;
    auto out = value_out();
    if (!out) return std::nullopt;
    if (*in < *out) return std::nullopt; // Underfunded
    return *in - *out;
}

// ---------------------------------------------------------------- bincode
void bincode(Writer& w, const Address& a) { w.raw(a.b); }

void bincode(Writer& w, const Content& c)
{
    // content::SerdeRepr = IfIsHumanReadable<FromInto<DefaultRepr>, FromInto<HumanReadableRepr>>:
    // serde_with's first parameter is the HUMAN-READABLE form, so bincode (not human readable)
    // uses HumanReadableRepr: amounts as sats, and swap_id through hex::serde, i.e. a 64-char
    // lowercase hex string (quirk reproduced).
    w.u32(c.kind);
    w.u64(c.value);
    if (c.kind == Content::Withdrawal) {
        w.u64(c.main_fee);
        w.bincode_str(c.main_address.to_string()); // Address serde = Display string
    } else if (c.kind == Content::SwapPending) {
        w.bincode_str(bt::hex_encode(c.swap_id));
    }
}

void bincode(Writer& w, const Output& o)
{
    bincode(w, o.address);
    bincode(w, o.content);
}

void bincode(Writer& w, const TxData& d)
{
    w.u32(d.kind);
    switch (d.kind) {
    case TxData::Regular: break;
    case TxData::SwapCreate:
        w.raw(d.swap_id);
        w.u32(uint32_t(d.parent_chain));
        w.bincode_bytes(d.l1_txid_bytes);
        w.u32(d.required_confirmations);
        bincode_opt_addr(w, d.l2_recipient);
        w.u64(d.l2_amount);
        w.bincode_str(d.l1_recipient_address);
        w.u64(d.l1_amount);
        break;
    case TxData::SwapClaim:
        w.raw(d.swap_id);
        bincode_opt_addr(w, d.l2_claimer_address);
        if (d.proof_data) {
            w.u8(1);
            w.bincode_bytes(*d.proof_data);
        } else {
            w.u8(0);
        }
        break;
    case TxData::SwapAccept:
        w.raw(d.swap_id);
        w.raw(d.l2_claimer_address ? d.l2_claimer_address->b : bt::Hash20{});
        break;
    }
}

void bincode(Writer& w, const NodeHash& h)
{
    w.u32(h.tag);
    if (h.tag == NodeHash::Some) w.raw(h.h);
}

void bincode(Writer& w, const bt::utreexo::Proof& p)
{
    w.bincode_len(p.targets.size());
    for (auto t : p.targets) w.u64(t);
    w.bincode_len(p.hashes.size());
    for (auto& h : p.hashes) bincode(w, h);
}

void bincode(Writer& w, const Transaction& t)
{
    w.bincode_len(t.inputs.size());
    for (auto& [op, h] : t.inputs) {
        bt::sidechain::bincode(w, op);
        w.raw(h);
    }
    bincode(w, t.proof);
    w.bincode_len(t.outputs.size());
    for (auto& o : t.outputs) bincode(w, o);
    bincode(w, t.data);
}

void bincode(Writer& w, const Authorization& a)
{
    w.raw(a.verifying_key); // serdect::array -> tuple, 32 bytes
    w.bincode_len(64);      // serdect::slice -> bytes with u64 length
    w.raw(a.signature.R);
    w.raw(a.signature.z);
}

void bincode(Writer& w, const AuthorizedTransaction& t)
{
    bincode(w, t.transaction);
    w.bincode_len(t.authorizations.size());
    for (auto& a : t.authorizations) bincode(w, a);
}

void bincode(Writer& w, const Body& b)
{
    w.bincode_len(b.coinbase.size());
    for (auto& o : b.coinbase) bincode(w, o);
    w.bincode_len(b.transactions.size());
    for (auto& t : b.transactions) bincode(w, t);
    w.bincode_len(b.authorizations.size());
    for (auto& a : b.authorizations) bincode(w, a);
}

void bincode(Writer& w, const Header& h)
{
    w.raw(h.merkle_root);
    if (h.prev_side_hash) {
        w.u8(1);
        w.raw(*h.prev_side_hash);
    } else {
        w.u8(0);
    }
    w.bincode_bytes(h.prev_main_hash.span()); // bitcoin::BlockHash serde = bytes
    w.bincode_len(h.roots.size());
    for (auto& r : h.roots) bincode(w, r);
}

void bincode(Writer& w, const Block& b)
{
    bincode(w, b.header);
    bincode(w, b.body);
}

void bincode(Writer& w, const Swap& s)
{
    w.raw(s.id);
    w.u32(s.direction);
    w.u32(uint32_t(s.parent_chain));
    w.u32(s.l1_txid.kind);
    if (s.l1_txid.kind == SwapTxId::Hash32) w.raw(ByteSpan(s.l1_txid.bytes.data(), 32));
    else w.bincode_bytes(s.l1_txid.bytes);
    w.u32(s.required_confirmations);
    w.u32(s.state.kind);
    if (s.state.kind == SwapState::WaitingConfirmations) {
        w.u32(s.state.current);
        w.u32(s.state.required);
    }
    bincode_opt_addr(w, s.l2_recipient);
    w.u64(s.l2_amount);
    w.bincode_str(s.l1_recipient_address);
    w.u64(s.l1_amount);
    if (s.l1_claimer_address) {
        w.u8(1);
        w.bincode_str(*s.l1_claimer_address);
    } else {
        w.u8(0);
    }
    bincode_opt_addr(w, s.l2_claimer_address);
    w.u32(s.created_at_height);
    if (s.expires_at_height) {
        w.u8(1);
        w.u32(*s.expires_at_height);
    } else {
        w.u8(0);
    }
    if (s.l1_txid_validated_at_block_hash) {
        w.u8(1);
        w.raw(*s.l1_txid_validated_at_block_hash);
    } else {
        w.u8(0);
    }
    if (s.l1_txid_validated_at_height) {
        w.u8(1);
        w.u32(*s.l1_txid_validated_at_height);
    } else {
        w.u8(0);
    }
    bincode_opt_addr(w, s.l2_creator_address);
}

void bincode(Writer& w, const SwapReservation& r)
{
    w.raw(r.claimer.b);
    w.u32(r.accepted_at_height);
}

Address de_address(Reader& r)
{
    Address a;
    a.b = r.array<20>();
    return a;
}

Content de_content(Reader& r)
{
    Content c;
    uint32_t tag = r.u32();
    if (tag > 2) throw DecodeError("invalid Content tag");
    c.kind = Content::Kind(tag);
    c.value = r.u64();
    if (c.kind == Content::Withdrawal) {
        c.main_fee = r.u64();
        std::string s = r.bincode_str();
        auto a = bt::btc::Address::parse(s);
        if (!a) throw DecodeError("invalid main_address");
        c.main_address = *a;
    } else if (c.kind == Content::SwapPending) {
        // hex::serde::deserialize::<[u8; 32]>: exactly 64 hex digits, either case
        std::string s = r.bincode_str();
        auto h = bt::hex_to_array<32>(s);
        if (s.size() != 64 || !h) throw DecodeError("invalid swap_id hex");
        c.swap_id = *h;
    }
    return c;
}

Output de_output(Reader& r)
{
    Output o;
    o.address = de_address(r);
    o.content = de_content(r);
    return o;
}

TxData de_txdata(Reader& r)
{
    TxData d;
    uint32_t tag = r.u32();
    if (tag > 3) throw DecodeError("invalid TxData tag");
    d.kind = TxData::Kind(tag);
    switch (d.kind) {
    case TxData::Regular: break;
    case TxData::SwapCreate: {
        d.swap_id = r.array<32>();
        uint32_t pc = r.u32();
        if (pc > 4) throw DecodeError("invalid ParentChainType tag");
        d.parent_chain = ParentChain(pc);
        d.l1_txid_bytes = r.bincode_bytes();
        d.required_confirmations = r.u32();
        d.l2_recipient = de_opt_addr(r);
        d.l2_amount = r.u64();
        d.l1_recipient_address = r.bincode_str();
        d.l1_amount = r.u64();
        break;
    }
    case TxData::SwapClaim: {
        d.swap_id = r.array<32>();
        d.l2_claimer_address = de_opt_addr(r);
        uint8_t t = r.u8();
        if (t == 1) d.proof_data = r.bincode_bytes();
        else if (t != 0) throw DecodeError("invalid Option tag");
        break;
    }
    case TxData::SwapAccept: {
        d.swap_id = r.array<32>();
        Address a;
        a.b = r.array<20>();
        d.l2_claimer_address = a;
        break;
    }
    }
    return d;
}

NodeHash de_nodehash(Reader& r)
{
    uint32_t tag = r.u32();
    if (tag == 0) return NodeHash::empty();
    if (tag == 1) return NodeHash{NodeHash::Placeholder, {}};
    if (tag == 2) return NodeHash::some(r.array<32>());
    throw DecodeError("invalid NodeHash tag");
}

bt::utreexo::Proof de_proof(Reader& r)
{
    bt::utreexo::Proof p;
    size_t n = r.bincode_len(8);
    p.targets.reserve(n);
    for (size_t i = 0; i < n; i++) p.targets.push_back(r.u64());
    size_t m = r.bincode_len(4);
    for (size_t i = 0; i < m; i++) p.hashes.push_back(de_nodehash(r));
    return p;
}

Transaction de_transaction(Reader& r)
{
    Transaction t;
    size_t n = r.bincode_len(4 + 4 + 32 + 32);
    for (size_t i = 0; i < n; i++) {
        OutPoint op = bt::sidechain::de_outpoint(r);
        Hash32 h = r.array<32>();
        t.inputs.emplace_back(op, h);
    }
    t.proof = de_proof(r);
    size_t m = r.bincode_len(20 + 4 + 8);
    for (size_t i = 0; i < m; i++) t.outputs.push_back(de_output(r));
    t.data = de_txdata(r);
    return t;
}

Authorization de_authorization(Reader& r)
{
    Authorization a;
    a.verifying_key = bt::sidechain::de_vk_raw(r);
    Bytes sig = r.bincode_bytes();
    a.signature = bt::sidechain::de_sig64(sig);
    return a;
}

AuthorizedTransaction de_authorized_transaction(Reader& r)
{
    AuthorizedTransaction t;
    t.transaction = de_transaction(r);
    size_t n = r.bincode_len(32 + 8 + 64);
    for (size_t i = 0; i < n; i++) t.authorizations.push_back(de_authorization(r));
    return t;
}

Body de_body(Reader& r)
{
    Body b;
    size_t c = r.bincode_len(20 + 4 + 8);
    for (size_t i = 0; i < c; i++) b.coinbase.push_back(de_output(r));
    size_t n = r.bincode_len(8 + 16 + 8 + 4);
    for (size_t i = 0; i < n; i++) b.transactions.push_back(de_transaction(r));
    size_t m = r.bincode_len(32 + 8 + 64);
    for (size_t i = 0; i < m; i++) b.authorizations.push_back(de_authorization(r));
    return b;
}

Header de_header(Reader& r)
{
    Header h;
    h.merkle_root = r.array<32>();
    uint8_t opt = r.u8();
    if (opt == 1) h.prev_side_hash = r.array<32>();
    else if (opt != 0) throw DecodeError("invalid Option tag");
    Bytes pm = r.bincode_bytes();
    if (pm.size() != 32) throw DecodeError("invalid block hash length");
    h.prev_main_hash = Hash32::from_span(pm);
    size_t n = r.bincode_len(4);
    for (size_t i = 0; i < n; i++) h.roots.push_back(de_nodehash(r));
    return h;
}

Swap de_swap(Reader& r)
{
    Swap s;
    s.id = r.array<32>();
    uint32_t dir = r.u32();
    if (dir > 1) throw DecodeError("invalid SwapDirection");
    s.direction = uint8_t(dir);
    uint32_t pc = r.u32();
    if (pc > 4) throw DecodeError("invalid ParentChainType");
    s.parent_chain = ParentChain(pc);
    uint32_t tk = r.u32();
    if (tk == 0) {
        auto a = r.array<32>();
        s.l1_txid.kind = SwapTxId::Hash32;
        s.l1_txid.bytes.assign(a.begin(), a.end());
    } else if (tk == 1) {
        s.l1_txid.kind = SwapTxId::Hash;
        s.l1_txid.bytes = r.bincode_bytes();
    } else {
        throw DecodeError("invalid SwapTxId");
    }
    s.required_confirmations = r.u32();
    uint32_t st = r.u32();
    if (st > 4) throw DecodeError("invalid SwapState");
    s.state.kind = SwapState::Kind(st);
    if (s.state.kind == SwapState::WaitingConfirmations) {
        s.state.current = r.u32();
        s.state.required = r.u32();
    }
    s.l2_recipient = de_opt_addr(r);
    s.l2_amount = r.u64();
    s.l1_recipient_address = r.bincode_str();
    s.l1_amount = r.u64();
    uint8_t t = r.u8();
    if (t == 1) s.l1_claimer_address = r.bincode_str();
    else if (t != 0) throw DecodeError("invalid Option tag");
    s.l2_claimer_address = de_opt_addr(r);
    s.created_at_height = r.u32();
    t = r.u8();
    if (t == 1) s.expires_at_height = r.u32();
    else if (t != 0) throw DecodeError("invalid Option tag");
    t = r.u8();
    if (t == 1) s.l1_txid_validated_at_block_hash = r.array<32>();
    else if (t != 0) throw DecodeError("invalid Option tag");
    t = r.u8();
    if (t == 1) s.l1_txid_validated_at_height = r.u32();
    else if (t != 0) throw DecodeError("invalid Option tag");
    s.l2_creator_address = de_opt_addr(r);
    return s;
}

SwapReservation de_reservation(Reader& r)
{
    SwapReservation x;
    x.claimer.b = r.array<20>();
    x.accepted_at_height = r.u32();
    return x;
}

// ---------------------------------------------------------------- borsh decode
Output borsh_de_output(Reader& r)
{
    Output o;
    o.address.b = r.array<20>();
    uint8_t tag = r.u8();
    if (tag > 2) throw DecodeError("invalid Content tag");
    o.content.kind = Content::Kind(tag);
    o.content.value = r.u64();
    if (o.content.kind == Content::Withdrawal) {
        o.content.main_fee = r.u64();
        o.content.main_address = bt::sidechain::address_from_spk(r.borsh_bytes());
    } else if (o.content.kind == Content::SwapPending) {
        o.content.swap_id = r.array<32>();
    }
    return o;
}

Transaction borsh_de_transaction(Reader& r)
{
    Transaction t;
    size_t n = r.borsh_len(37 + 32);
    for (size_t i = 0; i < n; i++) {
        OutPoint op = bt::sidechain::borsh_de_outpoint(r);
        Hash32 h = r.array<32>();
        t.inputs.emplace_back(op, h);
    }
    size_t m = r.borsh_len(29);
    for (size_t i = 0; i < m; i++) t.outputs.push_back(borsh_de_output(r));
    uint8_t tag = r.u8();
    if (tag > 3) throw DecodeError("invalid TxData tag");
    t.data.kind = TxData::Kind(tag);
    switch (t.data.kind) {
    case TxData::Regular: break;
    case TxData::SwapCreate: {
        t.data.swap_id = r.array<32>();
        uint8_t pc = r.u8();
        if (pc > 4) throw DecodeError("invalid ParentChainType tag");
        t.data.parent_chain = ParentChain(pc);
        t.data.l1_txid_bytes = r.borsh_bytes();
        t.data.required_confirmations = r.u32();
        t.data.l2_recipient = de_opt_addr(r);
        t.data.l2_amount = r.u64();
        Bytes s = r.borsh_bytes();
        t.data.l1_recipient_address.assign(s.begin(), s.end());
        if (!Reader::utf8_valid(t.data.l1_recipient_address)) throw DecodeError("invalid utf-8");
        t.data.l1_amount = r.u64();
        break;
    }
    case TxData::SwapClaim: {
        t.data.swap_id = r.array<32>();
        t.data.l2_claimer_address = de_opt_addr(r);
        uint8_t o = r.u8();
        if (o == 1) t.data.proof_data = r.borsh_bytes();
        else if (o != 0) throw DecodeError("invalid Option tag");
        break;
    }
    case TxData::SwapAccept: {
        t.data.swap_id = r.array<32>();
        Address a;
        a.b = r.array<20>();
        t.data.l2_claimer_address = a;
        break;
    }
    }
    return t;
}

// ---------------------------------------------------------------- merkle
namespace {
struct CbmtNode {
    Hash32 commitment;
    uint64_t fees = 0;
    uint64_t canonical_size = 0;
};
} // namespace

std::optional<Hash32> compute_merkle_root(const std::vector<Output>& coinbase, const std::vector<FilledTransaction>& txs, bool* panic)
{
    std::vector<CbmtNode> leaves;
    leaves.reserve(txs.size());
    for (auto& ft : txs) {
        auto fee = ft.fee();
        if (!fee) return std::nullopt; // ComputeMerkleRootError(TxFee)
        uint64_t size = ft.tx->canonical_size();
        Writer w; // CbmtLeafPreCommitment
        w.u64(*fee);
        w.u64(size);
        borsh(w, *ft.tx);
        leaves.push_back(CbmtNode{bt::blake3(w.buf), *fee, size});
    }
    bool overflow = false;
    auto root = bt::sidechain::cbmt_root(leaves, [&](const CbmtNode& l, const CbmtNode& r) -> std::optional<CbmtNode> {
        CbmtNode n;
        n.fees = l.fees + r.fees;
        if (n.fees < l.fees) overflow = true; // `Amount + Amount` panics on overflow
        n.canonical_size = l.canonical_size + r.canonical_size; // u64 `+` wraps in release
        Writer w; // CbmtNodePreCommitment
        w.raw(l.commitment);
        w.u64(n.fees);
        w.u64(n.canonical_size);
        w.raw(r.commitment);
        n.commitment = bt::blake3(w.buf);
        return n;
    });
    if (overflow) {
        if (panic) *panic = true;
        return std::nullopt;
    }
    Hash32 coinbase_root = bt::blake3([&] {
        Writer w;
        w.borsh_len(coinbase.size());
        for (auto& o : coinbase) borsh(w, o);
        return w.buf;
    }());
    Writer w;
    w.raw(coinbase_root);
    w.raw(root->commitment);
    return bt::blake3(w.buf);
}

// ---------------------------------------------------------------- limits
double powi(double a, int b)
{
    const bool recip = b < 0;
    double r = 1;
    while (true) {
        if (b & 1) r *= a;
        b /= 2;
        if (b == 0) break;
        a *= a;
    }
    return recip ? 1 / r : r;
}

static uint64_t growing_limit(uint64_t start, uint32_t height)
{
    uint32_t month = height / (6 * 24 * 30);
    if (month < 120) {
        double v = double(start) * powi(1.04, int(month));
        return uint64_t(std::floor(v));
    }
    return start * 111;
}

uint64_t body_sigops_limit(uint32_t height) { return growing_limit(42800, height); }
uint64_t body_size_limit(uint32_t height) { return growing_limit(8ull * 1024 * 1024, height); }

// ---------------------------------------------------------------- authorizations
bool verify_authorizations(const Body& body)
{
    size_t required = body.inputs_len();
    if (body.authorizations.size() != required) return false;
    if (required == 0) return true;
    size_t k = 0;
    for (auto& tx : body.transactions) {
        if (tx.inputs.empty()) continue;
        Bytes msg = tx.canonical_bytes();
        for (size_t i = 0; i < tx.inputs.size(); i++, k++) {
            auto& a = body.authorizations[k];
            if (!bt::frost::verify(a.verifying_key, a.signature, msg)) return false;
        }
    }
    return true;
}

bool verify_authorized_transaction(const AuthorizedTransaction& tx)
{
    if (tx.authorizations.size() != tx.transaction.inputs.size()) return false;
    if (tx.authorizations.empty()) return true; // nothing to sign
    Bytes msg = tx.transaction.canonical_bytes();
    for (auto& a : tx.authorizations)
        if (!bt::frost::verify(a.verifying_key, a.signature, msg)) return false;
    return true;
}

} // namespace coinshift
