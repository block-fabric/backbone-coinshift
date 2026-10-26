// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// serde_json forms of coinshift-rs v0.14.8 types (golden file: tests/data/oracle/json_cases.txt,
// produced by tools/oracle `json` from their crate).
#include <coinshift/json.h>

#include <bt/parse.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace coinshift::json {

static UniValue obj() { return UniValue(UniValue::VOBJ); }
static UniValue arr() { return UniValue(UniValue::VARR); }
static UniValue tagged(const std::string& k, const UniValue& v)
{
    UniValue r = obj();
    r.pushKV(k, v);
    return r;
}
template <class T, class F>
static UniValue opt(const std::optional<T>& v, F f)
{
    return v ? f(*v) : UniValue();
}

UniValue hash(const Hash32& h) { return UniValue(bt::hex_encode(h)); }
UniValue btc_hash(const Hash32& h) { return UniValue(bt::hex_encode_rev(h.span())); }
UniValue bytes_array(ByteSpan b)
{
    UniValue a = arr();
    for (uint8_t x : b) a.push_back(UniValue(int(x)));
    return a;
}
UniValue address(const Address& a) { return UniValue(a.to_base58()); }
UniValue swap_id(const Hash32& id) { return bytes_array(id.span()); }

UniValue outpoint(const OutPoint& o)
{
    if (o.kind == OutPoint::Deposit) return tagged("Deposit", UniValue(bt::hex_encode_rev(o.txid.span()) + ":" + std::to_string(o.vout)));
    UniValue inner = obj();
    inner.pushKV(o.kind == OutPoint::Regular ? "txid" : "merkle_root", hash(o.txid));
    inner.pushKV("vout", UniValue(uint64_t(o.vout)));
    return tagged(o.kind == OutPoint::Regular ? "Regular" : "Coinbase", inner);
}

// Content's JSON is its DefaultRepr (serde_with::IfIsHumanReadable's first parameter is the
// human-readable form): field names value / main_fee / main_address / swap_id.
UniValue content(const Content& c)
{
    switch (c.kind) {
    case Content::Value: return tagged("Value", UniValue(c.value));
    case Content::Withdrawal: {
        UniValue w = obj();
        w.pushKV("value", UniValue(c.value));
        w.pushKV("main_fee", UniValue(c.main_fee));
        w.pushKV("main_address", c.main_address.to_string());
        return tagged("Withdrawal", w);
    }
    case Content::SwapPending: {
        UniValue w = obj();
        w.pushKV("value", UniValue(c.value));
        w.pushKV("swap_id", bytes_array(c.swap_id.span()));
        return tagged("SwapPending", w);
    }
    }
    return UniValue();
}

UniValue output(const Output& o)
{
    UniValue r = obj();
    r.pushKV("address", address(o.address));
    r.pushKV("content", content(o.content));
    return r;
}

UniValue pointed_output(const OutPoint& op, const Output& o)
{
    UniValue r = obj();
    r.pushKV("outpoint", outpoint(op));
    r.pushKV("output", output(o));
    return r;
}

UniValue txdata(const TxData& d)
{
    UniValue x = obj();
    switch (d.kind) {
    case TxData::Regular: return UniValue("Regular");
    case TxData::SwapCreate:
        x.pushKV("swap_id", bytes_array(d.swap_id.span()));
        x.pushKV("parent_chain", parent_chain_name(d.parent_chain));
        x.pushKV("l1_txid_bytes", bytes_array(d.l1_txid_bytes));
        x.pushKV("required_confirmations", UniValue(uint64_t(d.required_confirmations)));
        x.pushKV("l2_recipient", opt(d.l2_recipient, address));
        x.pushKV("l2_amount", UniValue(d.l2_amount));
        x.pushKV("l1_recipient_address", d.l1_recipient_address);
        x.pushKV("l1_amount", UniValue(d.l1_amount));
        return tagged("SwapCreate", x);
    case TxData::SwapClaim:
        x.pushKV("swap_id", bytes_array(d.swap_id.span()));
        x.pushKV("l2_claimer_address", opt(d.l2_claimer_address, address));
        x.pushKV("proof_data", opt(d.proof_data, [](const Bytes& b) { return bytes_array(b); }));
        return tagged("SwapClaim", x);
    case TxData::SwapAccept:
        x.pushKV("swap_id", bytes_array(d.swap_id.span()));
        x.pushKV("l2_claimer_address", address(d.l2_claimer_address ? *d.l2_claimer_address : Address{}));
        return tagged("SwapAccept", x);
    }
    return UniValue();
}

static UniValue nodehash(const bt::utreexo::NodeHash& n)
{
    switch (n.tag) {
    case bt::utreexo::NodeHash::Empty: return UniValue("Empty");
    case bt::utreexo::NodeHash::Placeholder: return UniValue("Placeholder");
    case bt::utreexo::NodeHash::Some: return tagged("Some", bytes_array(n.h.span()));
    }
    return UniValue();
}

UniValue proof(const bt::utreexo::Proof& p)
{
    UniValue r = obj();
    UniValue t = arr();
    for (auto x : p.targets) t.push_back(UniValue(x));
    r.pushKV("targets", t);
    UniValue h = arr();
    for (auto& x : p.hashes) h.push_back(nodehash(x));
    r.pushKV("hashes", h);
    return r;
}

UniValue transaction(const Transaction& t)
{
    UniValue r = obj();
    UniValue ins = arr();
    for (auto& [op, h] : t.inputs) {
        UniValue p = arr();
        p.push_back(outpoint(op));
        p.push_back(bytes_array(h.span())); // Hash = [u8; 32]
        ins.push_back(p);
    }
    r.pushKV("inputs", ins);
    r.pushKV("proof", proof(t.proof));
    UniValue outs = arr();
    for (auto& o : t.outputs) outs.push_back(output(o));
    r.pushKV("outputs", outs);
    r.pushKV("data", txdata(t.data));
    return r;
}

UniValue authorization(const Authorization& a)
{
    UniValue r = obj();
    r.pushKV("verifying_key", bt::hex_encode(a.verifying_key));
    Bytes s(a.signature.R.begin(), a.signature.R.end());
    s.insert(s.end(), a.signature.z.begin(), a.signature.z.end());
    r.pushKV("signature", bt::hex_encode(s));
    return r;
}

UniValue body(const Body& b)
{
    UniValue r = obj();
    UniValue cb = arr();
    for (auto& o : b.coinbase) cb.push_back(output(o));
    r.pushKV("coinbase", cb);
    UniValue txs = arr();
    for (auto& t : b.transactions) txs.push_back(transaction(t));
    r.pushKV("transactions", txs);
    UniValue a = arr();
    for (auto& x : b.authorizations) a.push_back(authorization(x));
    r.pushKV("authorizations", a);
    return r;
}

UniValue header(const Header& h)
{
    UniValue r = obj();
    r.pushKV("merkle_root", hash(h.merkle_root));
    r.pushKV("prev_side_hash", h.prev_side_hash ? hash(*h.prev_side_hash) : UniValue());
    r.pushKV("prev_main_hash", btc_hash(h.prev_main_hash));
    UniValue roots = arr();
    for (auto& n : h.roots) roots.push_back(nodehash(n));
    r.pushKV("roots", roots);
    return r;
}

UniValue block(const Header& h, const Body& b)
{
    UniValue r = obj();
    r.pushKV("header", header(h));
    r.pushKV("body", body(b));
    return r;
}

UniValue withdrawal_bundle(const WithdrawalBundle& w)
{
    UniValue r = obj();
    UniValue su = arr();
    for (auto& [op, o] : w.spend_utxos) {
        UniValue p = arr();
        p.push_back(outpoint(op));
        p.push_back(output(o));
        su.push_back(p);
    }
    r.pushKV("spend_utxos", su);
    UniValue tx = obj();
    tx.pushKV("version", 2);
    tx.pushKV("lock_time", 0);
    tx.pushKV("input", arr());
    UniValue o = arr();
    for (auto& x : w.outputs) {
        UniValue t = obj();
        t.pushKV("value", UniValue(x.value));
        t.pushKV("script_pubkey", bt::hex_encode(x.script_pubkey));
        o.push_back(t);
    }
    tx.pushKV("output", o);
    r.pushKV("tx", tx);
    return r;
}

UniValue swap_state(const SwapState& s)
{
    switch (s.kind) {
    case SwapState::Pending: return UniValue("Pending");
    case SwapState::WaitingConfirmations: {
        UniValue p = arr();
        p.push_back(UniValue(uint64_t(s.current)));
        p.push_back(UniValue(uint64_t(s.required)));
        return tagged("WaitingConfirmations", p);
    }
    case SwapState::ReadyToClaim: return UniValue("ReadyToClaim");
    case SwapState::Completed: return UniValue("Completed");
    case SwapState::Cancelled: return UniValue("Cancelled");
    }
    return UniValue();
}

UniValue swap(const Swap& s)
{
    UniValue r = obj();
    r.pushKV("id", swap_id(s.id));
    r.pushKV("direction", s.direction == 1 ? "L2ToL1" : "L1ToL2");
    r.pushKV("parent_chain", parent_chain_name(s.parent_chain));
    r.pushKV("l1_txid", tagged(s.l1_txid.kind == SwapTxId::Hash32 ? "Hash32" : "Hash", bytes_array(s.l1_txid.bytes)));
    r.pushKV("required_confirmations", UniValue(uint64_t(s.required_confirmations)));
    r.pushKV("state", swap_state(s.state));
    r.pushKV("l2_recipient", opt(s.l2_recipient, address));
    r.pushKV("l2_amount", UniValue(s.l2_amount));
    r.pushKV("l1_recipient_address", s.l1_recipient_address);
    r.pushKV("l1_amount", UniValue(s.l1_amount));
    r.pushKV("l1_claimer_address", opt(s.l1_claimer_address, [](const std::string& x) { return UniValue(x); }));
    r.pushKV("l2_claimer_address", opt(s.l2_claimer_address, address));
    r.pushKV("created_at_height", UniValue(uint64_t(s.created_at_height)));
    r.pushKV("expires_at_height", opt(s.expires_at_height, [](uint32_t x) { return UniValue(uint64_t(x)); }));
    r.pushKV("l1_txid_validated_at_block_hash", opt(s.l1_txid_validated_at_block_hash, hash));
    r.pushKV("l1_txid_validated_at_height", opt(s.l1_txid_validated_at_height, [](uint32_t x) { return UniValue(uint64_t(x)); }));
    r.pushKV("l2_creator_address", opt(s.l2_creator_address, address));
    return r;
}

UniValue mempool_tx(const Transaction& t)
{
    UniValue r = obj();
    Bytes raw = t.canonical_bytes();
    r.pushKV("txid", hash(t.txid()));
    r.pushKV("size", UniValue(uint64_t(raw.size() / 8))); // canonical_size (sic: borsh length / 8)
    r.pushKV("raw", bt::hex_encode(raw));
    r.pushKV("tx", transaction(t));
    return r;
}

UniValue block_index(const Body& b, const BlockIndexEvents& ev)
{
    UniValue r = obj();
    UniValue txs = arr();
    for (auto& t : b.transactions) {
        Bytes raw = t.canonical_bytes();
        UniValue x = obj();
        x.pushKV("txid", hash(t.txid()));
        x.pushKV("size", UniValue(uint64_t(raw.size() / 8)));
        x.pushKV("raw", bt::hex_encode(raw));
        txs.push_back(x);
    }
    r.pushKV("txs", txs);
    UniValue deps = arr();
    for (auto& [op, o] : ev.deposits) {
        UniValue x = obj();
        x.pushKV("outpoint", outpoint(op));
        x.pushKV("output", output(o));
        deps.push_back(x);
    }
    r.pushKV("deposits", deps);
    UniValue spends = arr();
    for (auto& [op, m] : ev.bundle_spends) {
        UniValue x = obj();
        x.pushKV("outpoint", outpoint(op));
        x.pushKV("m6id", btc_hash(m));
        spends.push_back(x);
    }
    r.pushKV("bundle_spends", spends);
    return r;
}

UniValue balance(uint64_t total, uint64_t available)
{
    UniValue r = obj();
    r.pushKV("total_sats", UniValue(total));
    r.pushKV("available_sats", UniValue(available));
    return r;
}

UniValue sync_progress(const char* phase, uint32_t done, uint32_t total, uint32_t tip_height)
{
    UniValue r = obj();
    r.pushKV("phase", phase);
    r.pushKV("done", UniValue(uint64_t(done)));
    r.pushKV("total", UniValue(uint64_t(total)));
    r.pushKV("tip_height", UniValue(uint64_t(tip_height)));
    return r;
}

// ------------------------------------------------------------------ parsing
std::string serde_unexpected(const UniValue& v)
{
    if (v.isNull()) return "null";
    if (v.isBool()) return std::string("boolean `") + (v.get_bool() ? "true" : "false") + "`";
    if (v.isArray()) return "sequence";
    if (v.isObject()) return "map";
    if (v.isStr()) {
        std::string r = "string \"";
        for (unsigned char ch : v.get_str()) {
            if (ch == '"') r += "\\\"";
            else if (ch == '\\') r += "\\\\";
            else if (ch == '\n') r += "\\n";
            else if (ch == '\t') r += "\\t";
            else if (ch == '\r') r += "\\r";
            else if (ch < 0x20 || ch == 0x7f) {
                char b[16];
                std::snprintf(b, sizeof(b), "\\u{%x}", ch);
                r += b;
            } else r += char(ch);
        }
        return r + "\"";
    }
    const std::string& s = v.getValStr();
    if (s.find_first_of(".eE") != std::string::npos) {
        double d = std::strtod(s.c_str(), nullptr);
        char b[64];
        for (int prec = 1; prec <= 17; prec++) {
            std::snprintf(b, sizeof(b), "%.*g", prec, d);
            if (std::strtod(b, nullptr) == d) break;
        }
        std::string f = b;
        if (f.find('e') != std::string::npos && d == std::floor(d)) {
            std::snprintf(b, sizeof(b), "%.0f", d);
            f = b;
        }
        return "floating point `" + f + "`";
    }
    return "integer `" + s + "`";
}

static uint64_t parse_uint(const UniValue& v, uint64_t max, const char* type)
{
    if (!v.isNum()) throw Error("invalid type: " + serde_unexpected(v) + ", expected " + type, &v);
    const std::string& s = v.getValStr();
    if (s.find_first_of(".eE") != std::string::npos) throw Error("invalid type: " + serde_unexpected(v) + ", expected " + type, &v);
    auto x = s.empty() || s[0] == '-' ? std::nullopt : bt::parse_u64(s);
    if (!x || *x > max) throw Error("invalid value: " + serde_unexpected(v) + ", expected " + type, &v);
    return *x;
}

uint64_t parse_u64(const UniValue& v) { return parse_uint(v, UINT64_MAX, "u64"); }
uint32_t parse_u32(const UniValue& v) { return uint32_t(parse_uint(v, UINT32_MAX, "u32")); }
std::optional<uint64_t> parse_opt_u64(const UniValue& v)
{
    if (v.isNull()) return std::nullopt;
    return parse_u64(v);
}
std::optional<uint32_t> parse_opt_u32(const UniValue& v)
{
    if (v.isNull()) return std::nullopt;
    return parse_u32(v);
}

std::string parse_string(const UniValue& v)
{
    if (!v.isStr()) throw Error("invalid type: " + serde_unexpected(v) + ", expected a string", &v);
    return v.get_str();
}

static void require_struct(const UniValue& v, const char* name, std::initializer_list<const char*> fields)
{
    if (!v.isObject()) throw Error("invalid type: " + serde_unexpected(v) + ", expected struct " + name, &v);
    for (const char* f : fields)
        if (!v.exists(f)) throw Error(std::string("missing field `") + f + "`", &v);
}

static std::string rust_char_debug(unsigned char b)
{
    switch (b) {
    case 0: return "'\\0'";
    case '\t': return "'\\t'";
    case '\n': return "'\\n'";
    case '\r': return "'\\r'";
    case '\'': return "'\\''";
    case '\\': return "'\\\\'";
    }
    char buf[16];
    if (b < 0x20 || b == 0x7f) {
        std::snprintf(buf, sizeof(buf), "'\\u{%x}'", b);
        return buf;
    }
    if (b < 0x80) return std::string("'") + char(b) + "'";
    return std::string("'") + char(0xc0 | (b >> 6)) + char(0x80 | (b & 0x3f)) + "'";
}

// hex::serde::deserialize into [u8; 32] (the hex crate's FromHex for arrays): odd length,
// then the length, then the first non-hex character, with its Display texts.
Hash32 parse_hash(const UniValue& v)
{
    if (!v.isStr()) throw Error("invalid type: " + serde_unexpected(v) + ", expected a hex encoded string", &v);
    const std::string& s = v.get_str();
    if (s.size() % 2) throw Error("Odd number of digits", &v);
    if (s.size() != 64) throw Error("Invalid string length", &v);
    for (size_t i = 0; i < s.size(); i++)
        if (!std::isxdigit((unsigned char)s[i])) throw Error("Invalid character " + rust_char_debug((unsigned char)s[i]) + " at position " + std::to_string(i), &v);
    return *bt::hex_to_array<32>(s);
}

Hash32 parse_btc_hash(const UniValue& v)
{
    if (!v.isStr()) throw Error("invalid type: " + serde_unexpected(v) + ", expected an ASCII hex string", &v);
    auto h = bt::hash_from_rev_hex(v.get_str());
    if (!h) throw Error("failed to parse hex", &v);
    return *h;
}

static Hash32 parse_byte_array32(const UniValue& v)
{
    if (!v.isArray()) throw Error("invalid type: " + serde_unexpected(v) + ", expected an array of length 32", &v);
    Hash32 h;
    size_t n = v.size();
    for (size_t i = 0; i < n && i < 32; i++) {
        const UniValue& x = v[i];
        if (!x.isNum()) throw Error("invalid type: " + serde_unexpected(x) + ", expected u8", &x);
        h[i] = uint8_t(parse_uint(x, 255, "u8"));
    }
    if (n < 32) throw Error("invalid length " + std::to_string(n) + ", expected an array of length 32", &v);
    if (n > 32) throw Error("trailing characters", &v);
    return h;
}

Hash32 parse_swap_id(const UniValue& v) { return parse_byte_array32(v); }

static Bytes parse_byte_vec(const UniValue& v)
{
    if (!v.isArray()) throw Error("invalid type: " + serde_unexpected(v) + ", expected a sequence", &v);
    Bytes b;
    for (size_t i = 0; i < v.size(); i++) b.push_back(uint8_t(parse_uint(v[i], 255, "u8")));
    return b;
}

static Address address_from_str(const std::string& s, const UniValue* at)
{
    auto d = bt::base58_decode(s);
    if (!d) throw Error("bs58 error", at);
    if (d->size() != 20) throw Error("wrong address length " + std::to_string(d->size()) + " != 20", at);
    Address a;
    std::memcpy(a.b.data(), d->data(), 20);
    return a;
}

Address parse_address(const UniValue& v)
{
    if (!v.isStr()) throw Error("invalid type: " + serde_unexpected(v) + ", expected a string", &v);
    return address_from_str(v.get_str(), &v);
}

std::optional<Address> parse_opt_address(const UniValue& v)
{
    if (v.isNull()) return std::nullopt;
    return parse_address(v);
}

bt::btc::Address parse_btc_address(const UniValue& v)
{
    if (!v.isStr()) throw Error("invalid type: " + serde_unexpected(v) + ", expected a string", &v);
    auto a = bt::btc::Address::parse(v.get_str());
    if (!a) throw Error("base58 error", &v);
    return *a;
}

ParentChain parse_parent_chain(const UniValue& v)
{
    // serde_json deserialize_enum: a string (unit variant) or a one-entry map; otherwise "expected value"
    std::string name;
    if (v.isStr()) name = v.get_str();
    else if (v.isObject() && v.size() == 1) name = v.getKeys()[0];
    else throw Error("expected value", &v);
    auto pc = parent_chain_from_name(name);
    if (!pc) throw Error("unknown variant `" + name + "`, expected one of `BTC`, `BCH`, `LTC`, `Signet`, `Regtest`", &v);
    return *pc;
}

std::map<Address, uint64_t> parse_transfer_dests(const UniValue& v)
{
    if (!v.isObject()) throw Error("invalid type: " + serde_unexpected(v) + ", expected a map", &v);
    std::map<Address, uint64_t> r;
    auto& keys = v.getKeys();
    auto& vals = v.getValues();
    for (size_t i = 0; i < keys.size(); i++) {
        Address a = address_from_str(keys[i], &v);
        uint64_t x = parse_u64(vals[i]);
        if (!r.emplace(a, x).second) throw Error("invalid entry: found duplicate key", &v);
    }
    return r;
}

static OutPoint parse_outpoint(const UniValue& v)
{
    if (!v.isObject() || v.size() != 1) throw Error("expected value", &v);
    const std::string& k = v.getKeys()[0];
    const UniValue& x = v.getValues()[0];
    OutPoint o;
    if (k == "Deposit") {
        if (!x.isStr()) throw Error("invalid type: " + serde_unexpected(x) + ", expected an OutPoint", &x);
        auto s = x.get_str();
        auto c = s.rfind(':');
        if (c == std::string::npos) throw Error("colon missing", &x);
        o.kind = OutPoint::Deposit;
        auto h = bt::hash_from_rev_hex(s.substr(0, c));
        if (!h) throw Error("TXID parse error", &x);
        o.txid = *h;
        auto vout = bt::parse_u64(s.substr(c + 1));
        if (!vout || *vout > UINT32_MAX) throw Error("vout parse error", &x);
        o.vout = uint32_t(*vout);
        return o;
    }
    if (k != "Regular" && k != "Coinbase") throw Error("unknown variant `" + k + "`, expected one of `Regular`, `Coinbase`, `Deposit`", &v);
    o.kind = k == "Regular" ? OutPoint::Regular : OutPoint::Coinbase;
    const char* hk = k == "Regular" ? "txid" : "merkle_root";
    require_struct(x, k.c_str(), {hk, "vout"});
    o.txid = parse_hash(x[hk]);
    o.vout = parse_u32(x["vout"]);
    return o;
}

static Content parse_content(const UniValue& c)
{
    Content r;
    if (!c.isObject() || c.size() != 1) throw Error("expected value", &c);
    const std::string& k = c.getKeys()[0];
    const UniValue& x = c.getValues()[0];
    if (k == "Value") {
        r.kind = Content::Value;
        r.value = parse_u64(x);
    } else if (k == "Withdrawal") {
        require_struct(x, "Withdrawal", {"value", "main_fee", "main_address"});
        r.kind = Content::Withdrawal;
        r.value = parse_u64(x["value"]);
        r.main_fee = parse_u64(x["main_fee"]);
        r.main_address = parse_btc_address(x["main_address"]);
    } else if (k == "SwapPending") {
        require_struct(x, "SwapPending", {"value", "swap_id"});
        r.kind = Content::SwapPending;
        r.value = parse_u64(x["value"]);
        r.swap_id = parse_byte_array32(x["swap_id"]);
    } else {
        throw Error("unknown variant `" + k + "`, expected one of `Value`, `Withdrawal`, `SwapPending`", &c);
    }
    return r;
}

static Output parse_output(const UniValue& v)
{
    require_struct(v, "Output", {"address", "content"});
    Output o;
    o.address = parse_address(v["address"]);
    o.content = parse_content(v["content"]);
    return o;
}

static bt::utreexo::NodeHash parse_nodehash(const UniValue& v)
{
    if (v.isStr()) {
        if (v.get_str() == "Empty") return bt::utreexo::NodeHash::empty();
        if (v.get_str() == "Placeholder") return bt::utreexo::NodeHash{bt::utreexo::NodeHash::Placeholder, {}};
        if (v.get_str() == "Some") throw Error("invalid type: unit variant, expected newtype variant", &v);
        throw Error("unknown variant `" + v.get_str() + "`, expected one of `Empty`, `Placeholder`, `Some`", &v);
    }
    if (!v.isObject() || v.size() != 1) throw Error("expected value", &v);
    if (v.getKeys()[0] != "Some") throw Error("unknown variant `" + v.getKeys()[0] + "`, expected one of `Empty`, `Placeholder`, `Some`", &v);
    return bt::utreexo::NodeHash::some(parse_byte_array32(v.getValues()[0]));
}

static bt::utreexo::Proof parse_proof(const UniValue& v)
{
    require_struct(v, "Proof", {"targets", "hashes"});
    bt::utreexo::Proof p;
    const UniValue& t = v["targets"];
    if (!t.isArray()) throw Error("invalid type: " + serde_unexpected(t) + ", expected a sequence", &t);
    for (auto& x : t.getValues()) p.targets.push_back(parse_u64(x));
    const UniValue& h = v["hashes"];
    if (!h.isArray()) throw Error("invalid type: " + serde_unexpected(h) + ", expected a sequence", &h);
    for (auto& x : h.getValues()) p.hashes.push_back(parse_nodehash(x));
    return p;
}

static TxData parse_txdata(const UniValue& v)
{
    TxData d;
    if (v.isStr()) {
        if (v.get_str() == "Regular") return d;
        throw Error("unknown variant `" + v.get_str() + "`, expected one of `Regular`, `SwapCreate`, `SwapClaim`, `SwapAccept`", &v);
    }
    if (!v.isObject() || v.size() != 1) throw Error("expected value", &v);
    const std::string& k = v.getKeys()[0];
    const UniValue& x = v.getValues()[0];
    if (k == "SwapCreate") {
        require_struct(x, "SwapCreate",
                       {"swap_id", "parent_chain", "l1_txid_bytes", "required_confirmations", "l2_recipient", "l2_amount", "l1_recipient_address", "l1_amount"});
        d.kind = TxData::SwapCreate;
        d.swap_id = parse_byte_array32(x["swap_id"]);
        d.parent_chain = parse_parent_chain(x["parent_chain"]);
        d.l1_txid_bytes = parse_byte_vec(x["l1_txid_bytes"]);
        d.required_confirmations = parse_u32(x["required_confirmations"]);
        d.l2_recipient = parse_opt_address(x["l2_recipient"]);
        d.l2_amount = parse_u64(x["l2_amount"]);
        d.l1_recipient_address = parse_string(x["l1_recipient_address"]);
        d.l1_amount = parse_u64(x["l1_amount"]);
    } else if (k == "SwapClaim") {
        require_struct(x, "SwapClaim", {"swap_id"});
        d.kind = TxData::SwapClaim;
        d.swap_id = parse_byte_array32(x["swap_id"]);
        d.l2_claimer_address = parse_opt_address(x["l2_claimer_address"]);
        if (!x["proof_data"].isNull()) d.proof_data = parse_byte_vec(x["proof_data"]);
    } else if (k == "SwapAccept") {
        require_struct(x, "SwapAccept", {"swap_id", "l2_claimer_address"});
        d.kind = TxData::SwapAccept;
        d.swap_id = parse_byte_array32(x["swap_id"]);
        d.l2_claimer_address = parse_address(x["l2_claimer_address"]);
    } else {
        throw Error("unknown variant `" + k + "`, expected one of `Regular`, `SwapCreate`, `SwapClaim`, `SwapAccept`", &v);
    }
    return d;
}

static Transaction parse_transaction(const UniValue& v)
{
    require_struct(v, "Transaction", {"inputs", "proof", "outputs", "data"});
    Transaction t;
    const UniValue& ins = v["inputs"];
    if (!ins.isArray()) throw Error("invalid type: " + serde_unexpected(ins) + ", expected a sequence", &ins);
    for (auto& in : ins.getValues()) {
        if (!in.isArray() || in.size() != 2) throw Error("invalid type: " + serde_unexpected(in) + ", expected a tuple of size 2", &in);
        t.inputs.emplace_back(parse_outpoint(in[0]), parse_byte_array32(in[1]));
    }
    t.proof = parse_proof(v["proof"]);
    const UniValue& outs = v["outputs"];
    if (!outs.isArray()) throw Error("invalid type: " + serde_unexpected(outs) + ", expected a sequence", &outs);
    for (auto& o : outs.getValues()) t.outputs.push_back(parse_output(o));
    t.data = parse_txdata(v["data"]);
    return t;
}

static Authorization parse_authorization(const UniValue& v)
{
    require_struct(v, "Authorization", {"verifying_key", "signature"});
    Authorization a;
    auto vk = v["verifying_key"].isStr() ? bt::hex_to_array<32>(v["verifying_key"].get_str()) : std::nullopt;
    if (!vk || !bt::frost::valid_element(*vk)) throw Error("malformed verifying key encoding", &v["verifying_key"]);
    a.verifying_key = *vk;
    auto sig = v["signature"].isStr() ? bt::hex_decode(v["signature"].get_str()) : std::nullopt;
    if (!sig || sig->size() != 64) throw Error("malformed signature encoding", &v["signature"]);
    try {
        a.signature = bt::sidechain::de_sig64(*sig);
    } catch (const std::exception&) {
        throw Error("malformed signature encoding", &v["signature"]);
    }
    return a;
}

Header parse_header(const UniValue& v)
{
    require_struct(v, "Header", {"merkle_root", "prev_side_hash", "prev_main_hash", "roots"});
    Header h;
    h.merkle_root = parse_hash(v["merkle_root"]);
    if (!v["prev_side_hash"].isNull()) h.prev_side_hash = parse_hash(v["prev_side_hash"]);
    h.prev_main_hash = parse_btc_hash(v["prev_main_hash"]);
    const UniValue& r = v["roots"];
    if (!r.isArray()) throw Error("invalid type: " + serde_unexpected(r) + ", expected a sequence", &r);
    for (auto& x : r.getValues()) h.roots.push_back(parse_nodehash(x));
    return h;
}

Body parse_body(const UniValue& v)
{
    require_struct(v, "Body", {"coinbase", "transactions", "authorizations"});
    Body b;
    for (const char* k : {"coinbase", "transactions", "authorizations"})
        if (!v[k].isArray()) throw Error("invalid type: " + serde_unexpected(v[k]) + ", expected a sequence", &v[k]);
    for (auto& o : v["coinbase"].getValues()) b.coinbase.push_back(parse_output(o));
    for (auto& t : v["transactions"].getValues()) b.transactions.push_back(parse_transaction(t));
    for (auto& a : v["authorizations"].getValues()) b.authorizations.push_back(parse_authorization(a));
    return b;
}

std::pair<Header, Body> parse_block(const UniValue& v)
{
    if (!v.isObject()) throw Error("invalid type: " + serde_unexpected(v) + ", expected struct Block", &v);
    // serde derive: the fields' own errors while reading, then the first missing field
    Header h;
    Body b;
    if (v.exists("header")) h = parse_header(v["header"]);
    if (v.exists("body")) b = parse_body(v["body"]);
    require_struct(v, "Block", {"header", "body"});
    return {h, b};
}

std::string parse_socket_addr(const UniValue& v)
{
    if (!v.isStr()) throw Error("invalid type: " + serde_unexpected(v) + ", expected socket address", &v);
    // std::net::SocketAddr::from_str: an IP literal (IPv6 in brackets) and a port
    auto hp = bt::split_hostport(v.get_str(), std::nullopt, true);
    unsigned char buf[16];
    bool ip = hp && (inet_pton(AF_INET, hp->first.c_str(), buf) == 1 ||
                     (v.get_str().front() == '[' && inet_pton(AF_INET6, hp->first.c_str(), buf) == 1));
    if (!ip) throw Error("invalid socket address syntax", &v);
    return v.get_str();
}

} // namespace coinshift::json
