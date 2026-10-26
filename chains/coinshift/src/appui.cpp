// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <coinshift/appui.h>
#include <coinshift/json.h>
#include <coinshift/l1rpc.h>

#include <bt/appui/phrase.h>
#include <bt/appui/text.h>
#include <bt/fsperm.h>
#include <bt/hd.h>
#include <bt/log.h>
#include <bt/parse.h>

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <set>

namespace coinshift {

using bt::appui::Confirmed;
using bt::appui::HostEnv;
using bt::appui::HostError;
using bt::appui::Question;
using bt::appui::Row;
using bt::appui::RpcError;
using bt::kv::Txn;

std::optional<uint64_t> l1_amount_sats(const std::string& decimal)
{
    // A plain decimal ("0.5", "12.00010000"): digits, at most one point, at most 8 places, at most 21,000,000 coins.
    if (decimal.empty() || decimal.size() > 24) return std::nullopt;
    const size_t dot{decimal.find('.')};
    const std::string whole{decimal.substr(0, dot)}, frac{dot == std::string::npos ? std::string{} : decimal.substr(dot + 1)};
    if (whole.empty() || whole.size() > 8 || frac.size() > 8) return std::nullopt;
    if (whole.find_first_not_of("0123456789") != std::string::npos || frac.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    if (dot != std::string::npos && frac.empty()) return std::nullopt;
    uint64_t coins{0};
    for (char c : whole) coins = coins * 10 + uint64_t(c - '0');
    if (coins > 21'000'000) return std::nullopt;
    uint64_t sats{0};
    std::string f{frac};
    f.resize(8, '0');
    for (char c : f) sats = sats * 10 + uint64_t(c - '0');
    const uint64_t total{coins * 100'000'000 + sats};
    if (total > 21'000'000ULL * 100'000'000ULL) return std::nullopt;
    return total;
}

bool l1_address_matches(const std::string& reply, const std::string& wanted)
{
    if (reply.empty() || wanted.empty()) return false;
    if (reply == wanted) return true;
    // Bitcoin Cash's cashaddr: with or without its prefix (bitcoincash:, bchtest:, bchreg:).
    auto bare = [](const std::string& s) {
        for (const char* p : {"bitcoincash:", "bchtest:", "bchreg:"}) {
            if (s.rfind(p, 0) == 0) return s.substr(std::strlen(p));
        }
        return s;
    };
    const std::string a{bare(reply)}, b{bare(wanted)};
    return (a != reply || b != wanted) && a == b;
}

std::optional<uint64_t> l1_paid_to(const UniValue& tx, const std::string& address)
{
    if (!tx.isObject() || !tx.find_value("vout").isArray()) return std::nullopt;
    uint64_t paid{0};
    for (const UniValue& out : tx.find_value("vout").getValues()) {
        if (!out.isObject()) return std::nullopt;
        const UniValue& value{out.find_value("value")};
        const UniValue& spk{out.find_value("scriptPubKey")};
        if (!value.isNum() || !spk.isObject()) return std::nullopt;
        std::string to;
        if (spk.find_value("address").isStr()) to = spk.find_value("address").get_str();
        else if (spk.find_value("addresses").isArray() && spk.find_value("addresses").size() == 1 && spk.find_value("addresses")[0].isStr())
            to = spk.find_value("addresses")[0].get_str();
        if (!l1_address_matches(to, address)) continue;
        auto sats{l1_amount_sats(value.getValStr())};
        if (!sats) return std::nullopt;
        if (paid + *sats < paid) return std::nullopt;
        paid += *sats;
    }
    return paid;
}

namespace {
const char* WALLET_NAME = "main";
/** The LayerTwo read that stands for knowing the wallet's addresses: what tells a caller which outputs and swaps are
 *  the wallet's. */
const char* WALLET_READ = "get_wallet_addresses";
const char* VERSION_TEXT = "0.1.0 (coinshift 0.14.8 compatible)";
/** At most this many swaps listed (the chain has few; a page never gets an unbounded list). */
constexpr size_t MAX_SWAPS = 2000;

const UniValue NULLV;
const UniValue& param(const UniValue& p, const std::string& key) { return p.isObject() ? p.find_value(key) : NULLV; }

std::string str_param(const UniValue& p, const std::string& key)
{
    const UniValue& v{param(p, key)};
    if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, key + ": a string is needed");
    return v.get_str();
}

uint64_t u64_of(const UniValue& v, const std::string& what)
{
    if (!v.isNum()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a whole number is needed");
    const std::string s{v.getValStr()};
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a whole number is needed");
    auto n{bt::parse_u64(s)};
    if (!n) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": too large");
    return *n;
}

uint32_t u32_of(const UniValue& v, const std::string& what)
{
    const uint64_t n{u64_of(v, what)};
    if (n > UINT32_MAX) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": too large");
    return static_cast<uint32_t>(n);
}

std::string coins(uint64_t sats) { return bt::appui::format_sats(sats) + " ECX"; }

/** An amount of a parent chain (L1) in its own coin: "0.50000000 BTC (Bitcoin Regtest)". */
std::string l1_coins(uint64_t sats, ParentChain c)
{
    const char* ticker{c == ParentChain::BCH ? "BCH" : c == ParentChain::LTC ? "LTC" : "BTC"};
    return bt::appui::format_sats(sats) + " " + ticker;
}

std::string net_label(const std::string& network) { return network == "betanet" ? "" : network; }

/** Whether a mainchain address is of the mainchain network this node follows (betanet and forknet follow eCash's
 *  betanet, whose addresses are Bitcoin's main ones; regtest's are regtest ones, signet's test ones). */
bool main_address_of_network(const bt::btc::Address& a, const std::string& network)
{
    using A = bt::btc::Address;
    if (a.kind == A::Kind::Segwit) {
        if (network == "regtest") return a.hrp == A::Hrp::Regtest;
        if (network == "signet") return a.hrp == A::Hrp::Testnets;
        return a.hrp == A::Hrp::Mainnet;
    }
    if (network == "regtest" || network == "signet") return a.net == A::NetKind::Test;
    return a.net == A::NetKind::Main;
}

UniValue n64(uint64_t v) { return UniValue(v); }

std::optional<Hash32> parse_hash(const std::string& s)
{
    auto b{bt::hex_to_array<32>(s)};
    if (!b) return std::nullopt;
    return Hash32{*b};
}

/** A swap's id as the pages give it (64 hex digits, its bytes in order). */
Hash32 swap_hex(const UniValue& v, const std::string& what)
{
    if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a swap id (64 hex digits)");
    auto h{parse_hash(v.get_str())};
    if (!h) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a swap id (64 hex digits)");
    return *h;
}

/** A swap's id as LayerTwo's RPC takes it (an array of 32 numbers of 0 to 255). */
Hash32 swap_array(const UniValue& v, const std::string& what)
{
    if (!v.isArray() || v.size() != 32) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a swap id (32 numbers)");
    Hash32 h;
    for (size_t i{0}; i < 32; ++i) {
        const uint64_t b{u64_of(v[i], what)};
        if (b > 255) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a swap id (32 numbers of 0 to 255)");
        h.data()[i] = static_cast<uint8_t>(b);
    }
    return h;
}

const char* state_word(SwapState::Kind k)
{
    switch (k) {
    case SwapState::Pending: return "pending";
    case SwapState::WaitingConfirmations: return "confirming";
    case SwapState::ReadyToClaim: return "ready";
    case SwapState::Completed: return "completed";
    case SwapState::Cancelled: return "cancelled";
    }
    return "pending";
}

/** A text the page gives that a trusted dialog will show (an L1 address, a URL): at most `max` bytes, what core's
 *  display_text_problem takes, no space. */
std::string shown_text(const UniValue& v, const std::string& what, size_t max)
{
    if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a text is needed");
    const std::string& s{v.get_str()};
    if (s.empty() || s.size() > max) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": 1 to " + std::to_string(max) + " bytes");
    if (const std::string why{bt::appui::display_text_problem(s)}; !why.empty()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": " + why);
    if (s.find(' ') != std::string::npos) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": no space");
    return s;
}

/** Whether an L1 address can be one of the parent chain's (Bitcoin and its signet: parsed, of that network; Bitcoin
 *  Cash, Litecoin and a regtest (any local test chain, whose addresses may have their own prefix): their characters).
 *  The swap's L1 payment is checked against exactly this text. */
std::string l1_address_problem(const std::string& a, ParentChain c)
{
    if (c == ParentChain::BTC || c == ParentChain::Signet) {
        auto p{bt::btc::Address::parse(a)};
        if (!p) return "not a Bitcoin address";
        using A = bt::btc::Address;
        const bool ok{c == ParentChain::BTC ? (p->kind == A::Kind::Segwit ? p->hrp == A::Hrp::Mainnet : p->net == A::NetKind::Main)
                                            : (p->kind == A::Kind::Segwit ? p->hrp == A::Hrp::Testnets : p->net == A::NetKind::Test)};
        return ok ? "" : std::string{"an address of another network than "} + coin_name(c);
    }
    for (char ch : a) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != ':') return "not an address of " + std::string{coin_name(c)};
    }
    return "";
}
} // namespace

struct AppLayer::Impl {
    App& app;
    Node& node;
    Wallet& wallet;
    std::string network, datadir;
    bt::appui::AppOptions options;
    bt::jsonrpc::Server l2l;
    std::unique_ptr<bt::appui::AppStore> store;
    std::unique_ptr<bt::appui::LinkMonitor> link;
    std::unique_ptr<bt::appui::Poller> poller;
    bt::appui::PendingPhrases phrases;
    std::chrono::system_clock::time_point started{std::chrono::system_clock::now()};

    Impl(App& a, std::string net, std::string dir, bt::appui::AppOptions o)
        : app(a), node(a.node), wallet(a.wallet), network(std::move(net)), datadir(std::move(dir)), options(std::move(o))
    {
        // Every LayerTwo method, on a server that does not listen: the reads the pages make and the spends run after
        // a yes go through the very handlers (and JSON) of the LayerTwo RPC.
        register_rpc(l2l, app);
    }

    /** One LayerTwo method, through its own handler: its result, or its error as an app RPC error. */
    UniValue l2l_call(const std::string& method, const UniValue& params)
    {
        UniValue req{UniValue::VOBJ};
        req.pushKV("jsonrpc", "2.0");
        req.pushKV("id", 1);
        req.pushKV("method", method);
        req.pushKV("params", params.isNull() ? UniValue{UniValue::VARR} : params);
        UniValue resp;
        if (!resp.read(l2l.handle(req.write())) || !resp.isObject()) throw RpcError(bt::appui::RPC_INTERNAL_ERROR, "no answer from " + method);
        const UniValue& err{resp.find_value("error")};
        if (err.isObject()) {
            const UniValue& data{err.find_value("data")};
            throw RpcError(err.find_value("code").isNum() ? err.find_value("code").getInt<int>() : -1,
                           err.find_value("message").getValStr() + (data.isStr() ? ": " + data.get_str() : std::string{}));
        }
        return resp.find_value("result");
    }

    std::set<Address> addresses()
    {
        if (!wallet.has_seed()) return {};
        return wallet.addresses();
    }

    std::optional<std::pair<uint32_t, Hash32>> block_of(const Hash32& block)
    {
        auto h{node.block_height(block)};
        auto hd{node.header(block)};
        if (!h || !hd) return std::nullopt;
        return std::make_pair(*h, hd->prev_main_hash);
    }

    std::optional<uint64_t> main_time(const Hash32& main) { return link ? link->block_time(main) : std::nullopt; }

    std::optional<std::pair<Hash32, std::optional<uint32_t>>> followed()
    {
        auto tip{node.tip()};
        if (!tip) return std::nullopt;
        auto hd{node.header(*tip)};
        if (!hd) return std::nullopt;
        return std::make_pair(hd->prev_main_hash, link ? link->block_height(hd->prev_main_hash) : std::nullopt);
    }

    /** The height the next block will have (what a swap's expiry and a reservation are measured by). */
    uint32_t next_height()
    {
        auto h{node.height()};
        return h ? *h + 1 : 0;
    }

    template <class F>
    auto with_state(F f)
    {
        std::lock_guard<std::recursive_mutex> l(node.lock());
        Txn t(node.store());
        return f(node.state(), t);
    }

    std::optional<Swap> swap(const Hash32& id)
    {
        return with_state([&](const State& st, const Txn& t) { return st.swap(t, id); });
    }

    // ---------------------------------------------------------- wallet
    struct Balances {
        uint64_t available{0}, withdrawing{0}, pending_in{0}, pending_out{0}, in_swaps{0};
    };

    static uint64_t add(uint64_t a, uint64_t b) { return a + b < a ? UINT64_MAX : a + b; }

    Balances balances(const std::set<Address>& mine)
    {
        Balances b;
        for (auto& [op, o] : node.utxos_by_addresses(mine)) {
            if (o.content.is_withdrawal()) b.withdrawing = add(b.withdrawing, o.get_value());
            else if (o.content.is_swap_pending()) b.in_swaps = add(b.in_swaps, o.get_value());
            else b.available = add(b.available, o.get_value());
        }
        auto [txs, fees] = node.mempool_transactions(1000);
        (void)fees;
        for (auto& [atx, spent] : txs) {
            for (const Output& s : spent) {
                if (mine.contains(s.address) && s.content.is_value()) b.pending_out = add(b.pending_out, s.get_value());
            }
            for (const Output& o : atx.transaction.outputs) {
                if (mine.contains(o.address) && o.content.is_value()) b.pending_in = add(b.pending_in, o.get_value());
            }
        }
        return b;
    }

    std::string wallet_id() { return seed_fingerprint(); }

    /** bt::appui::seed_fingerprint of the wallet's seed (the same definition), "" without one. */
    std::string seed_fingerprint()
    {
        auto d{wallet.seed_digest("sidechain/dropin/fingerprint/v1")};
        return d ? bt::hex_encode(bt::ByteSpan{d->data(), 8}) : std::string{};
    }

    /** Set the wallet's seed from a phrase checked before; the phrase kept for the backup (owner-only store). */
    UniValue set_phrase(const std::string& phrase, const std::string& from)
    {
        auto p{bt::hd::parse_mnemonic(phrase)};
        if (!p) throw HostError(bt::hd::mnemonic_error(phrase));
        const bt::appui::WipeOnExit wipe_parsed{*p};
        auto seed{bt::hd::mnemonic_to_seed(*p)};
        const std::string fp{bt::appui::seed_fingerprint(seed.span())};
        try {
            wallet.set_seed(seed);
        } catch (const std::exception& e) {
            sodium_memzero(seed.data(), seed.size());
            throw HostError(bt::appui::RPC_WALLET_ERROR, std::string{"This wallet has a backup phrase already ("} + e.what() + ")");
        }
        sodium_memzero(seed.data(), seed.size());
        // A first address, as LayerTwo's wallet has once it is used (the pages show one).
        wallet.get_new_address();
        store->put("phrase", *p);
        store->put("phrase.from", from);
        if (poller) poller->poke();
        UniValue out{UniValue::VOBJ};
        out.pushKV("fingerprint", fp);
        return out;
    }

    /** The phrase the app layer kept, only when it is the one of the wallet's seed now (a wallet file replaced since: none). */
    std::optional<std::string> kept_phrase()
    {
        auto phrase{store->get("phrase")};
        if (!phrase || !wallet.has_seed()) return std::nullopt;
        auto p{bt::hd::parse_mnemonic(*phrase)};
        if (!p) {
            sodium_memzero(phrase->data(), phrase->size());
            return std::nullopt;
        }
        auto seed{bt::hd::mnemonic_to_seed(*p)};
        sodium_memzero(p->data(), p->size());
        const bool same{bt::appui::seed_fingerprint(seed.span()) == seed_fingerprint()};
        sodium_memzero(seed.data(), seed.size());
        if (!same) {
            sodium_memzero(phrase->data(), phrase->size());
            return std::nullopt;
        }
        return phrase;
    }

    UniValue seed_status()
    {
        UniValue s{UniValue::VOBJ};
        const bool has{wallet.has_seed()};
        std::optional<std::string> kept{has ? kept_phrase() : std::nullopt};
        s.pushKV("hasSeed", has);
        s.pushKV("phraseKept", kept.has_value());
        if (kept) sodium_memzero(kept->data(), kept->size());
        s.pushKV("from", has ? store->get("phrase.from").value_or("outside") : "");
        s.pushKV("fingerprint", has ? seed_fingerprint() : "");
        s.pushKV("derivation", "coinshift 0.14.8");
        return s;
    }

    static std::string words_block(const std::string& phrase)
    {
        std::string out;
        size_t n{0}, at{0};
        while (at < phrase.size()) {
            size_t sp{phrase.find(' ', at)};
            if (sp == std::string::npos) sp = phrase.size();
            ++n;
            out += (n < 10 ? " " : "") + std::to_string(n) + ". " + phrase.substr(at, sp - at) + (n % 3 == 0 ? "\n" : "    ");
            at = sp + 1;
        }
        return out;
    }

    // ---------------------------------------------------------- history
    struct Hist {
        std::string id, kind;
        int64_t delta{0};
        uint64_t received{0}, spent{0}, withdrawing{0}, escrow_in{0}, escrow_out{0};
        bool pending{false}, ours_in{false};
        std::optional<Hash32> txid, m6id, swap;
        std::vector<std::pair<std::string, uint64_t>> to; // outputs to others
        std::optional<uint64_t> fee;
        std::string main_address, own_address;
        std::optional<TxData::Kind> data;
    };

    std::optional<Hash32> block_of_tx(const Hash32& txid)
    {
        for (auto& [blk, idx] : node.tx_inclusions(txid)) {
            auto h{node.block_height(blk)};
            if (h && node.block_hash_at(*h) == blk) return blk;
        }
        return std::nullopt;
    }

    std::optional<BundleStatus> bundle_status(const Hash32& m6id)
    {
        return with_state([&](const State& st, const Txn& t) -> std::optional<BundleStatus> {
            auto b{st.bundle(t, m6id)};
            if (!b) return std::nullopt;
            return b->second.latest().value;
        });
    }

    std::string seen_key(const std::string& id) { return "seen:" + id; }

    std::optional<std::pair<int64_t, int64_t>> seen(const std::string& id)
    {
        auto v{store->get(seen_key(id))};
        if (!v) return std::nullopt;
        const size_t c{v->find(':')};
        if (c == std::string::npos) return std::nullopt;
        return std::make_pair(std::atoll(v->substr(0, c).c_str()), std::atoll(v->substr(c + 1).c_str()));
    }

    void note_seen(const std::string& id)
    {
        if (store->get(seen_key(id))) return;
        auto h{node.height()};
        const int64_t now{std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
        store->put(seen_key(id), std::to_string(h ? int64_t(*h) : int64_t{-1}) + ":" + std::to_string(now));
    }

    std::map<std::string, Hist> gather(const std::set<Address>& mine)
    {
        std::map<std::string, Hist> rows;
        auto row_of = [&](const OutPoint& op) -> Hist& {
            std::string id, kind;
            if (op.kind == OutPoint::Deposit) {
                id = "deposit:" + bt::hex_encode_rev(op.txid.span());
                kind = "deposit";
            } else if (op.kind == OutPoint::Coinbase) {
                id = "coinbase:" + bt::hex_encode(op.txid);
                kind = "mined";
            } else {
                id = bt::hex_encode(op.txid);
            }
            Hist& h{rows[id]};
            if (h.id.empty()) {
                h.id = id;
                h.kind = kind;
                if (op.kind == OutPoint::Regular) h.txid = op.txid;
            }
            return h;
        };
        auto credit = [&](const OutPoint& op, const Output& o) {
            Hist& h{row_of(op)};
            if (o.content.is_withdrawal()) {
                h.withdrawing = add(h.withdrawing, o.get_value());
                h.main_address = o.content.main_address.to_string();
            } else if (o.content.is_swap_pending()) {
                h.escrow_in = add(h.escrow_in, o.get_value());
                h.swap = o.content.swap_id;
            } else {
                h.received = add(h.received, o.get_value());
                h.delta += static_cast<int64_t>(o.get_value());
                if (h.own_address.empty()) h.own_address = o.address.to_base58();
            }
        };
        for (auto& [op, o] : node.utxos_by_addresses(mine)) credit(op, o);
        for (auto& [op, s] : node.stxos_by_addresses(mine)) {
            credit(op, s.output);
            std::string id;
            if (s.inpoint.kind == InPoint::Withdrawal) {
                id = "bundle:" + bt::hex_encode_rev(s.inpoint.id.span());
                Hist& h{rows[id]};
                if (h.id.empty()) {
                    h.id = id;
                    h.kind = "bundle";
                    h.m6id = s.inpoint.id;
                }
                if (s.output.content.is_withdrawal()) h.withdrawing = add(h.withdrawing, s.output.get_value());
                continue;
            }
            id = bt::hex_encode(s.inpoint.id);
            Hist& h{rows[id]};
            if (h.id.empty()) {
                h.id = id;
                h.txid = s.inpoint.id;
            }
            h.ours_in = true;
            if (s.output.content.is_swap_pending()) {
                h.escrow_out = add(h.escrow_out, s.output.get_value());
                h.swap = s.output.content.swap_id;
            } else if (!s.output.content.is_withdrawal()) {
                h.spent = add(h.spent, s.output.get_value());
                h.delta -= static_cast<int64_t>(s.output.get_value());
            }
        }
        // What waits in the mempool: our spends and what comes in.
        auto [txs, fees] = node.mempool_transactions(1000);
        (void)fees;
        for (auto& [atx, spent] : txs) {
            const Transaction& tx{atx.transaction};
            const Hash32 txid{tx.txid()};
            bool touches{false};
            for (const Output& s : spent) touches = touches || mine.contains(s.address);
            for (const Output& o : tx.outputs) touches = touches || mine.contains(o.address);
            if (!touches) continue;
            Hist& h{rows[bt::hex_encode(txid)]};
            h = Hist{};
            h.id = bt::hex_encode(txid);
            h.txid = txid;
            h.pending = true;
            uint64_t in_total{0}, out_total{0};
            bool all_inputs_ours{true};
            for (const Output& s : spent) {
                in_total = add(in_total, s.get_value());
                if (mine.contains(s.address)) {
                    h.ours_in = true;
                    if (s.content.is_swap_pending()) h.escrow_out = add(h.escrow_out, s.get_value());
                    else if (!s.content.is_withdrawal()) {
                        h.spent = add(h.spent, s.get_value());
                        h.delta -= static_cast<int64_t>(s.get_value());
                    }
                } else {
                    all_inputs_ours = false;
                }
            }
            for (const Output& o : tx.outputs) {
                out_total = add(out_total, o.get_value());
                if (mine.contains(o.address)) {
                    if (o.content.is_withdrawal()) {
                        h.withdrawing = add(h.withdrawing, o.get_value());
                        h.main_address = o.content.main_address.to_string();
                    } else if (o.content.is_swap_pending()) {
                        h.escrow_in = add(h.escrow_in, o.get_value());
                    } else {
                        h.received = add(h.received, o.get_value());
                        h.delta += static_cast<int64_t>(o.get_value());
                        if (h.own_address.empty()) h.own_address = o.address.to_base58();
                    }
                } else if (o.content.is_value()) {
                    h.to.emplace_back(o.address.to_base58(), o.get_value());
                }
            }
            h.data = tx.data.kind;
            if (tx.data.kind != TxData::Regular) h.swap = tx.data.swap_id;
            if (all_inputs_ours && in_total >= out_total) h.fee = in_total - out_total;
        }
        return rows;
    }

    static std::string kind_of(const Hist& h)
    {
        if (!h.kind.empty()) return h.kind;
        if (h.data == TxData::SwapCreate) return "swap-create";
        if (h.data == TxData::SwapAccept) return "swap-accept";
        if (h.data == TxData::SwapClaim) return h.received > 0 ? "swap-claim" : "swap-done";
        if (h.withdrawing > 0) return "withdrawal";
        if (!h.ours_in) return "receive";
        if (h.to.empty()) return "self";
        return "send";
    }

    UniValue history(size_t limit)
    {
        const auto mine{addresses()};
        auto rows{gather(mine)};
        struct Out {
            UniValue v;
            int64_t order_height;
            int64_t order_time;
        };
        std::vector<Out> list;
        for (auto& [id, h] : rows) {
            note_seen(id);
            std::optional<uint32_t> height;
            std::optional<uint64_t> time;
            std::string block_hex;
            if (h.txid && !h.pending) {
                if (auto t{node.transaction(*h.txid)}) {
                    if (auto blk{block_of_tx(*h.txid)}) {
                        block_hex = bt::hex_encode(*blk);
                        if (auto b{block_of(*blk)}) {
                            height = b->first;
                            time = main_time(b->second);
                        }
                    }
                    h.data = t->data.kind;
                    if (t->data.kind != TxData::Regular) h.swap = t->data.swap_id;
                    h.to.clear();
                    uint64_t out_total{0};
                    for (const Output& o : t->outputs) {
                        out_total = add(out_total, o.get_value());
                        if (!mine.contains(o.address) && o.content.is_value()) h.to.emplace_back(o.address.to_base58(), o.get_value());
                    }
                    // Every input ours (and no escrow): the fee.
                    if (h.ours_in && !t->inputs.empty() && h.escrow_out == 0 && h.spent >= out_total) h.fee = h.spent - out_total;
                }
            }
            bool approx{false};
            if (!height || !time) {
                if (auto s{seen(id)}) {
                    if (!height && s->first >= 0) height = static_cast<uint32_t>(s->first);
                    if (!time) time = static_cast<uint64_t>(s->second);
                    approx = true;
                }
            }
            UniValue o{UniValue::VOBJ};
            o.pushKV("id", id);
            o.pushKV("kind", kind_of(h));
            if (h.txid) o.pushKV("txid", bt::hex_encode(*h.txid));
            if (h.m6id) {
                o.pushKV("m6id", bt::hex_encode_rev(h.m6id->span()));
                if (auto b{bundle_status(*h.m6id)}) o.pushKV("bundleStatus", to_string(*b));
            }
            if (h.swap) o.pushKV("swap", bt::hex_encode(*h.swap));
            if (h.escrow_in) o.pushKV("escrowIn", n64(h.escrow_in));
            if (h.escrow_out) o.pushKV("escrowOut", n64(h.escrow_out));
            o.pushKV("amount", static_cast<int64_t>(h.delta));
            o.pushKV("received", n64(h.received));
            o.pushKV("sent", n64(h.spent));
            if (h.withdrawing) o.pushKV("withdrawing", n64(h.withdrawing));
            if (!h.main_address.empty()) o.pushKV("mainAddress", h.main_address);
            if (!h.own_address.empty()) o.pushKV("ownAddress", h.own_address);
            if (h.fee) o.pushKV("fee", n64(*h.fee));
            UniValue to{UniValue::VARR};
            for (auto& [a, v] : h.to) {
                UniValue t{UniValue::VOBJ};
                t.pushKV("address", a);
                t.pushKV("amount", n64(v));
                to.push_back(t);
                if (to.size() >= 50) break;
            }
            o.pushKV("to", to);
            o.pushKV("recipients", static_cast<uint64_t>(h.to.size()));
            o.pushKV("pending", h.pending);
            o.pushKV("height", height ? UniValue(uint64_t(*height)) : UniValue{});
            o.pushKV("time", time ? UniValue(*time) : UniValue{});
            o.pushKV("approxTime", approx);
            if (!block_hex.empty()) o.pushKV("block", block_hex);
            list.push_back(Out{o, h.pending ? INT64_MAX : height ? int64_t(*height) : -1, time ? int64_t(*time) : 0});
        }
        std::sort(list.begin(), list.end(), [](const Out& a, const Out& b) {
            if (a.order_height != b.order_height) return a.order_height > b.order_height;
            if (a.order_time != b.order_time) return a.order_time > b.order_time;
            return a.v.find_value("id").get_str() < b.v.find_value("id").get_str();
        });
        UniValue out{UniValue::VARR};
        for (auto& o : list) {
            if (out.size() >= limit) break;
            out.push_back(std::move(o.v));
        }
        return out;
    }

    // ---------------------------------------------------------- the node as events
    bt::appui::NodeLook look()
    {
        bt::appui::NodeLook l;
        l.height = node.height();
        if (auto t{node.tip()}) {
            l.tip = bt::hex_encode(*t);
            if (auto hd{node.header(*t)}) {
                if (auto tm{main_time(hd->prev_main_hash)}) l.tip_time = *tm;
            }
        }
        if (link && link->connected() && link->info().find_value("state").getValStr() == "ok") {
            if (auto tm{link->tip_time()}) l.tip_time = std::max(l.tip_time, *tm);
        }
        for (auto& p : node.peers()) l.connections += p.connected ? 1 : 0;
        l.wallet = wallet.has_seed();
        if (l.wallet) {
            const auto mine{addresses()};
            const Balances b{balances(mine)};
            l.trusted = b.available;
            l.pending = b.pending_in;
            l.immature = b.withdrawing;
            for (auto& [id, h] : gather(mine)) {
                l.txs[id] = !h.pending;
                note_seen(id);
            }
        }
        if (link && !link->connected()) l.warnings.push_back("The mainchain interface does not answer: this chain cannot follow it.");
        return l;
    }

    UniValue info()
    {
        UniValue i{UniValue::VOBJ};
        i.pushKV("name", "Coinshift");
        i.pushKV("version", VERSION_TEXT);
        i.pushKV("bugreport", "https://github.com/block-fabric/backbone-coinshift/issues");
        i.pushKV("network", network);
        i.pushKV("networkLabel", net_label(network));
        i.pushKV("datadir", "");
        i.pushKV("blockSpacing", 600);
        i.pushKV("walletEnabled", true);
        i.pushKV("devtools", false);
        i.pushKV("language", "");
        i.pushKV("test", false);
        i.pushKV("platform", "other");
        i.pushKV("dropin", "coinshift");
        i.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        return i;
    }

    // ---------------------------------------------------------- explorer
    UniValue block_summary(uint32_t height, const Hash32& hash, const Header& hd, const Body& b)
    {
        UniValue o{UniValue::VOBJ};
        o.pushKV("height", static_cast<uint64_t>(height));
        o.pushKV("hash", bt::hex_encode(hash));
        o.pushKV("mainHash", bt::hex_encode_rev(hd.prev_main_hash.span()));
        if (auto t{main_time(hd.prev_main_hash)}) o.pushKV("time", *t);
        uint64_t fees{0};
        for (const Output& c : b.coinbase) fees = add(fees, c.get_value());
        o.pushKV("txs", static_cast<uint64_t>(b.transactions.size()));
        o.pushKV("inputs", static_cast<uint64_t>(b.inputs_len()));
        size_t outs{0};
        for (const auto& tx : b.transactions) outs += tx.outputs.size();
        o.pushKV("outputs", static_cast<uint64_t>(outs));
        o.pushKV("fees", n64(fees));
        o.pushKV("bytes", static_cast<uint64_t>(borsh_bytes(b).size()));
        o.pushKV("limit", n64(Chain::MAX_BODY_SIZE));
        return o;
    }

    /** Which addresses are the wallet's, for a caller that may read the wallet's addresses; nothing for another (its chain
     *  reads must not tell it which outputs are the wallet's). */
    using Mine = std::optional<std::set<Address>>;
    Mine mine_for(HostEnv& env) { return env.may_call(WALLET_READ) ? Mine{addresses()} : std::nullopt; }

    UniValue output_json(const Output& o, const Mine& mine)
    {
        UniValue r{UniValue::VOBJ};
        r.pushKV("address", o.address.to_base58());
        r.pushKV("amount", n64(o.get_value()));
        r.pushKV("kind", o.content.is_withdrawal() ? "withdrawal" : o.content.is_swap_pending() ? "swap" : "value");
        if (o.content.is_withdrawal()) {
            r.pushKV("mainAddress", o.content.main_address.to_string());
            r.pushKV("mainFee", n64(o.content.main_fee));
        }
        if (o.content.is_swap_pending()) r.pushKV("swap", bt::hex_encode(o.content.swap_id));
        if (mine) r.pushKV("mine", mine->contains(o.address));
        return r;
    }

    UniValue data_json(const Transaction& tx)
    {
        if (tx.data.kind == TxData::Regular) return UniValue{};
        UniValue d{UniValue::VOBJ};
        d.pushKV("kind", tx.data.kind == TxData::SwapCreate ? "swap-create" : tx.data.kind == TxData::SwapAccept ? "swap-accept" : "swap-claim");
        d.pushKV("swap", bt::hex_encode(tx.data.swap_id));
        d.pushKV("detail", json::txdata(tx.data));
        return d;
    }

    UniValue block_detail(const Hash32& hash, const Mine& mine)
    {
        auto hd{node.header(hash)};
        auto b{node.body(hash)};
        auto at{block_of(hash)};
        if (!hd || !b || !at) throw HostError("No such block");
        UniValue o{block_summary(at->first, hash, *hd, *b)};
        o.pushKV("prevHash", hd->prev_side_hash ? bt::hex_encode(*hd->prev_side_hash) : "");
        o.pushKV("merkleRoot", bt::hex_encode(hd->merkle_root));
        o.pushKV("onChain", node.block_hash_at(at->first) == hash);
        UniValue cb{UniValue::VARR};
        for (const Output& c : b->coinbase) cb.push_back(output_json(c, mine));
        o.pushKV("coinbase", cb);
        UniValue txs{UniValue::VARR};
        for (const auto& tx : b->transactions) {
            if (txs.size() >= 1000) break;
            UniValue t{UniValue::VOBJ};
            t.pushKV("txid", bt::hex_encode(tx.txid()));
            t.pushKV("inputs", static_cast<uint64_t>(tx.inputs.size()));
            UniValue outs{UniValue::VARR};
            for (const Output& out : tx.outputs) outs.push_back(output_json(out, mine));
            t.pushKV("outputs", outs);
            if (tx.data.kind != TxData::Regular) t.pushKV("data", data_json(tx));
            txs.push_back(t);
        }
        o.pushKV("transactions", txs);
        return o;
    }

    UniValue tx_detail(const Hash32& txid, const Mine& mine)
    {
        auto t{node.transaction(txid)};
        UniValue o{UniValue::VOBJ};
        o.pushKV("txid", bt::hex_encode(txid));
        if (!t) throw HostError("No such transaction");
        auto f{node.filled_transaction(txid)};
        const bool pending{f && !f->second};
        o.pushKV("pending", pending);
        if (!pending) {
            if (auto blk{block_of_tx(txid)}) {
                o.pushKV("block", bt::hex_encode(*blk));
                if (auto b{block_of(*blk)}) {
                    o.pushKV("height", static_cast<uint64_t>(b->first));
                    if (auto tm{main_time(b->second)}) o.pushKV("time", *tm);
                    if (auto h{node.height()}) o.pushKV("confirmations", static_cast<uint64_t>(*h >= b->first ? *h - b->first + 1 : 0));
                }
            }
        }
        UniValue ins{UniValue::VARR};
        if (f) {
            for (const Output& s : f->first.second) ins.push_back(output_json(s, mine));
            FilledTransaction ft{&*t, f->first.second};
            if (auto fee{ft.fee()}) o.pushKV("fee", n64(*fee));
        } else {
            for (auto& [op, h] : t->inputs) {
                UniValue i{UniValue::VOBJ};
                i.pushKV("outpoint", json::outpoint(op));
                ins.push_back(i);
            }
        }
        if (t->data.kind != TxData::Regular) o.pushKV("data", data_json(*t));
        o.pushKV("inputs", ins);
        UniValue outs{UniValue::VARR};
        for (const Output& out : t->outputs) outs.push_back(output_json(out, mine));
        o.pushKV("outputs", outs);
        return o;
    }

    UniValue address_detail(const Address& a, const Mine& mine)
    {
        const std::set<Address> one{a};
        uint64_t balance{0};
        UniValue utxos{UniValue::VARR};
        for (auto& [op, o] : node.utxos_by_addresses(one)) {
            if (o.content.is_value()) balance = add(balance, o.get_value());
            if (utxos.size() < 500) {
                UniValue u{output_json(o, mine)};
                u.pushKV("outpoint", json::outpoint(op));
                utxos.push_back(u);
            }
        }
        UniValue stxos{UniValue::VARR};
        for (auto& [op, s] : node.stxos_by_addresses(one)) {
            if (stxos.size() >= 500) break;
            UniValue u{output_json(s.output, mine)};
            u.pushKV("outpoint", json::outpoint(op));
            u.pushKV("spentBy", s.inpoint.kind == InPoint::Withdrawal ? "bundle:" + bt::hex_encode_rev(s.inpoint.id.span()) : bt::hex_encode(s.inpoint.id));
            stxos.push_back(u);
        }
        UniValue o{UniValue::VOBJ};
        o.pushKV("address", a.to_base58());
        if (mine) o.pushKV("mine", mine->contains(a));
        o.pushKV("balance", n64(balance));
        o.pushKV("utxos", utxos);
        o.pushKV("spent", stxos);
        o.pushKV("depositAddress", a.to_base58()); // the bare address: what every LayerTwo chain credits (doc/app.md)
        return o;
    }

    // ---------------------------------------------------------- peg
    UniValue peg()
    {
        const auto mine{addresses()};
        UniValue o{UniValue::VOBJ};
        auto pending{node.pending_withdrawal_bundle()};
        std::set<OutPoint> in_pending;
        if (pending) {
            UniValue p{UniValue::VOBJ};
            p.pushKV("m6id", bt::hex_encode_rev(pending->m6id().span()));
            uint64_t total{0};
            for (auto& [op, out] : pending->spend_utxos) {
                total = add(total, out.get_value());
                in_pending.insert(op);
            }
            p.pushKV("withdrawals", static_cast<uint64_t>(pending->spend_utxos.size()));
            p.pushKV("total", n64(total));
            o.pushKV("pendingBundle", p);
        } else {
            o.pushKV("pendingBundle", UniValue{});
        }
        auto failed{node.latest_failed_withdrawal_bundle_height()};
        o.pushKV("lastFailedHeight", failed ? UniValue(uint64_t(*failed)) : UniValue{});
        UniValue w{UniValue::VARR};
        auto withdrawal = [&](const OutPoint& op, const Output& out, const std::string& state, const std::optional<Hash32>& m6id) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("outpoint", json::outpoint(op));
            r.pushKV("txid", bt::hex_encode(op.txid));
            r.pushKV("mainAddress", out.content.main_address.to_string());
            r.pushKV("amount", n64(out.content.value));
            r.pushKV("mainFee", n64(out.content.main_fee));
            r.pushKV("state", state);
            if (m6id) {
                r.pushKV("m6id", bt::hex_encode_rev(m6id->span()));
                if (auto b{bundle_status(*m6id)}) r.pushKV("bundleStatus", to_string(*b));
            }
            w.push_back(r);
        };
        for (auto& [op, out] : node.utxos_by_addresses(mine)) {
            if (!out.content.is_withdrawal()) continue;
            withdrawal(op, out, in_pending.contains(op) ? "in-pending-bundle" : "waiting", pending && in_pending.contains(op) ? std::optional<Hash32>{pending->m6id()} : std::nullopt);
        }
        for (auto& [op, s] : node.stxos_by_addresses(mine)) {
            if (!s.output.content.is_withdrawal() || s.inpoint.kind != InPoint::Withdrawal) continue;
            auto b{bundle_status(s.inpoint.id)};
            std::string state{"submitted"};
            if (b && *b == BundleStatus::Confirmed) state = "paid";
            else if (b && *b == BundleStatus::Failed) state = "failed";
            withdrawal(op, s.output, state, s.inpoint.id);
        }
        o.pushKV("withdrawals", w);
        o.pushKV("wealth", n64(node.sidechain_wealth()));
        o.pushKV("mainchain", link ? link->info() : UniValue{});
        o.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        return o;
    }

    // ---------------------------------------------------------- L1: the app's own check of a swap's payment
    // Coinshift's consensus takes a node's own view of the L1 payment (known upstream behaviour reproduced): our app
    // claims a swap only once it saw the payment itself, on an L1 node the user named (kept in the app's store, owner-
    // only; never the node's l1_rpc_configs.json, which stays LayerTwo's).
    static std::string l1_key(ParentChain c) { return std::string{"l1:"} + parent_chain_name(c); }

    std::optional<L1RpcConfig> l1_config(ParentChain c)
    {
        auto v{store->get(l1_key(c))};
        if (!v) return std::nullopt;
        UniValue j;
        if (!j.read(*v) || !j.isObject() || !j.find_value("url").isStr() || !j.find_value("user").isStr() || !j.find_value("password").isStr()) return std::nullopt;
        return L1RpcConfig{j.find_value("url").get_str(), j.find_value("user").get_str(), j.find_value("password").get_str()};
    }

    l1::Client l1_client(ParentChain c)
    {
        auto cfg{l1_config(c)};
        if (!cfg) throw HostError(bt::appui::RPC_WALLET_ERROR, std::string{"No "} + coin_name(c) + " node is set up in the app to check L1 payments: Swaps → L1 nodes");
        return l1::Client{*cfg};
    }

    UniValue l1_call(ParentChain c, const std::string& method, const UniValue& params)
    {
        std::string err;
        auto r{l1_client(c).call(method, params, err)};
        if (!r) throw HostError(std::string{"The "} + coin_name(c) + " node does not answer: " + err);
        return *r;
    }

    struct L1Check {
        std::string txid;
        uint64_t paid{0};
        uint32_t confirmations{0};
    };

    /** The L1 transaction `txid` as the user's L1 node has it: what it paid to the swap's L1 address and its
     *  confirmations. Refused when it pays less than the swap asks, when another swap of this node took it already, or
     *  when it is older than the chain lets a swap's payment be. */
    /** A swap's L1 address (text of the chain, anyone's) before the app asks an L1 node about it. */
    static void l1_address_usable(const Swap& s)
    {
        if (s.l1_recipient_address.empty() || s.l1_recipient_address.size() > 128 || !l1_address_problem(s.l1_recipient_address, s.parent_chain).empty())
            throw HostError("This swap's L1 address is not one the app checks payments to");
    }

    L1Check l1_check(const Swap& s, const std::string& txid_in)
    {
        l1_address_usable(s);
        std::string txid{txid_in};
        std::transform(txid.begin(), txid.end(), txid.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (txid.size() != 64 || !bt::hex_decode(txid)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "An L1 transaction id is 64 hex digits");
        // Another swap of this node with this L1 payment: one payment pays one swap.
        const SwapTxId mine_tx{SwapTxId::from_bytes(*bt::hex_decode(txid))};
        for (const Swap& other : with_state([&](const State& st, const Txn& t) { return st.swaps(t); })) {
            if (other.id != s.id && other.parent_chain == s.parent_chain && !other.l1_txid.is_unset() && other.l1_txid.to_hex() == mine_tx.to_hex())
                throw HostError("This L1 transaction is the payment of another swap already");
        }
        UniValue p{UniValue::VARR};
        p.push_back(txid);
        p.push_back(true);
        const UniValue tx{l1_call(s.parent_chain, "getrawtransaction", p)};
        auto paid{l1_paid_to(tx, s.l1_recipient_address)};
        if (!paid) throw HostError("The L1 node's answer about this transaction cannot be read");
        if (*paid < s.l1_amount)
            throw HostError("This L1 transaction pays " + l1_coins(*paid, s.parent_chain) + " to " + s.l1_recipient_address + ", not the " + l1_coins(s.l1_amount, s.parent_chain) + " of the swap");
        L1Check c;
        c.txid = txid;
        c.paid = *paid;
        const UniValue& conf{tx.find_value("confirmations")};
        c.confirmations = conf.isNum() && conf.getInt<int64_t>() > 0 ? static_cast<uint32_t>(std::min<int64_t>(conf.getInt<int64_t>(), UINT32_MAX)) : 0;
        if (c.confirmations > max_l1_tx_age_blocks(s.parent_chain)) throw HostError("This L1 transaction is older than a swap's payment may be");
        return c;
    }

    std::string paid_key(const Hash32& id) { return "paid:" + bt::hex_encode(id); }

    /** An address of this wallet with a plain coin of at least `fee` not spent by the mempool (the largest such coin's):
     *  what a reservation can name, its fee paid from it. */
    std::optional<Address> fee_address(uint64_t fee)
    {
        std::set<OutPoint> spending;
        for (const AuthorizedTransaction& atx : node.mempool())
            for (auto& [op, h] : atx.transaction.inputs) spending.insert(op);
        std::optional<std::pair<Address, uint64_t>> best;
        for (auto& [op, o] : wallet.utxos()) {
            if (!o.content.is_value() || spending.contains(op) || app.is_locked(op) || o.get_value() < fee) continue;
            if (!best || o.get_value() > best->second) best = std::make_pair(o.address, o.get_value());
        }
        if (!best) return std::nullopt;
        return best->first;
    }

    /** The address of this wallet that may claim the swap now (its pre-specified recipient, or the live reservation). */
    std::optional<Address> our_claimer(const Swap& s, const std::set<Address>& mine)
    {
        auto c{with_state([&](const State& st, const Txn& t) { return st.entitled_claimer_at(t, s, next_height()); })};
        if (c && mine.contains(*c)) return c;
        return std::nullopt;
    }

    // ---------------------------------------------------------- swaps as the pages see them
    UniValue swap_json(const Swap& s, const Mine& mine)
    {
        UniValue o{UniValue::VOBJ};
        o.pushKV("id", bt::hex_encode(s.id));
        o.pushKV("direction", s.direction == 1 ? "L2ToL1" : "L1ToL2");
        o.pushKV("parentChain", parent_chain_name(s.parent_chain));
        o.pushKV("coin", coin_name(s.parent_chain));
        o.pushKV("l1Address", s.l1_recipient_address);
        o.pushKV("l1Amount", n64(s.l1_amount));
        o.pushKV("l2Amount", n64(s.l2_amount));
        o.pushKV("l2Recipient", s.l2_recipient ? UniValue(s.l2_recipient->to_base58()) : UniValue{});
        o.pushKV("creator", s.l2_creator_address ? UniValue(s.l2_creator_address->to_base58()) : UniValue{});
        o.pushKV("open", !s.l2_recipient.has_value());
        UniValue st{UniValue::VOBJ};
        st.pushKV("kind", state_word(s.state.kind));
        if (s.state.kind == SwapState::WaitingConfirmations) {
            st.pushKV("current", static_cast<uint64_t>(s.state.current));
            st.pushKV("required", static_cast<uint64_t>(s.state.required));
        }
        o.pushKV("state", st);
        o.pushKV("requiredConfirmations", static_cast<uint64_t>(s.required_confirmations));
        o.pushKV("createdAt", static_cast<uint64_t>(s.created_at_height));
        o.pushKV("expiresAt", s.expires_at_height ? UniValue(uint64_t(*s.expires_at_height)) : UniValue{});
        o.pushKV("l1Txid", s.l1_txid.is_unset() ? "" : s.l1_txid.to_hex());
        const uint32_t next{next_height()};
        auto res{with_state([&](const State& stt, const Txn& t) { return stt.reservation(t, s.id); })};
        if (res) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("claimer", res->claimer.to_base58());
            r.pushKV("acceptedAt", static_cast<uint64_t>(res->accepted_at_height));
            r.pushKV("expiresAt", static_cast<uint64_t>(res->expires_at(s.parent_chain)));
            r.pushKV("live", res->is_live_at(s.parent_chain, next));
            if (mine) r.pushKV("mine", mine->contains(res->claimer));
            o.pushKV("reservation", r);
        } else {
            o.pushKV("reservation", UniValue{});
        }
        // What is locked for it on this chain (its escrow outputs).
        uint64_t escrow{0};
        size_t outputs{0};
        for (auto& [op, out] : with_state([&](const State& stt, const Txn& t) { return stt.utxos(t); })) {
            if (out.content.is_swap_pending() && out.content.swap_id == s.id) {
                escrow = add(escrow, out.get_value());
                ++outputs;
            }
        }
        o.pushKV("escrow", n64(escrow));
        o.pushKV("escrowOutputs", static_cast<uint64_t>(outputs));
        o.pushKV("nextHeight", static_cast<uint64_t>(next));
        // The wallet's part in it, only to a reader of the wallet.
        if (mine) {
            std::string role;
            if (s.l2_creator_address && mine->contains(*s.l2_creator_address)) role = "creator";
            else if (s.l2_recipient && mine->contains(*s.l2_recipient)) role = "recipient";
            else if (res && mine->contains(res->claimer)) role = "claimer";
            o.pushKV("role", role);
            auto paid{store->get(paid_key(s.id))};
            o.pushKV("paidWith", paid ? UniValue(*paid) : UniValue{});
            o.pushKV("canClaim", s.state.kind == SwapState::ReadyToClaim && our_claimer(s, *mine).has_value() && paid && !s.l1_txid.is_unset() &&
                                     s.l1_txid.to_hex() == *paid);
        }
        return o;
    }

    // ---------------------------------------------------------- questions of the spends
    Address side_address(const UniValue& v, const std::string& what)
    {
        if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a Coinshift address");
        auto a{Address::from_base58(v.get_str())};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": not a Coinshift address (" + v.get_str() + ")");
        return *a;
    }

    bt::btc::Address main_address(const UniValue& v)
    {
        if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "mainchain_address: an eCash address");
        auto a{bt::btc::Address::parse(v.get_str())};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Not an eCash address: " + v.get_str());
        if (!main_address_of_network(*a, network)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This eCash address is of another network than the one this node follows (" + network + ")");
        return *a;
    }

    void need_seed()
    {
        if (!wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "The wallet has no backup phrase yet: set it up first");
    }

    void enough(uint64_t need)
    {
        const uint64_t have{balances(addresses()).available};
        if (need > have) throw HostError(bt::appui::RPC_WALLET_ERROR, "Not enough available: " + coins(have) + " for " + coins(need));
    }

    static UniValue same_numbers(const UniValue& p, size_t n, const std::vector<size_t>& numbers)
    {
        if (!p.isArray() || p.size() < n) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: " + std::to_string(n) + " parameters");
        UniValue out{UniValue::VARR};
        for (size_t i{0}; i < p.size() && i < n; ++i) {
            if (std::find(numbers.begin(), numbers.end(), i) != numbers.end()) out.push_back(n64(u64_of(p[i], "param " + std::to_string(i))));
            else out.push_back(p[i]);
        }
        return out;
    }

    std::map<std::string, Confirmed> confirmed()
    {
        std::map<std::string, Confirmed> c;
        c["transfer"] = Confirmed{
            [](const UniValue& p) { return same_numbers(p, 3, {1, 2}); },
            [this](const UniValue& p) {
                need_seed();
                const Address to{side_address(p[0], "dest")};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to send");
                const uint64_t total{bt::appui::spend_total({value, fee})};
                enough(total);
                Question q;
                q.title = "Send " + coins(value);
                q.message = addresses().contains(to) ? "To an address of this wallet." : "To this address on Coinshift. A payment cannot be taken back.";
                q.accept = "Send";
                q.kind = "send";
                q.rows = {{"To", to.to_base58(), "address"}, {"Amount", coins(value), "amount"}, {"Fee", coins(fee), "amount"}, {"Total", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("transfer", p); }};
        c["transfer_many"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 2 || !p[0].isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: {address: satoshis}, fee");
                if (p[0].size() > 2000) throw HostError(bt::appui::HOST_INVALID_REQUEST, "At most 2000 recipients in one payment");
                UniValue dests{UniValue::VOBJ};
                for (size_t i{0}; i < p[0].size(); ++i) dests.pushKV(p[0].getKeys()[i], n64(u64_of(p[0].getValues()[i], "amount")));
                UniValue out{UniValue::VARR};
                out.push_back(dests);
                out.push_back(n64(u64_of(p[1], "fee_sats")));
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const UniValue& dests{p[0]};
                if (dests.size() == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No recipient");
                const uint64_t fee{p[1].getInt<uint64_t>()};
                uint64_t total{bt::appui::spend_total({fee})};
                Question q;
                q.kind = "send-many";
                q.accept = "Send";
                std::set<Address> seen;
                for (size_t i{0}; i < dests.size(); ++i) {
                    const Address a{side_address(UniValue{dests.getKeys()[i]}, "dest")};
                    if (!seen.insert(a).second) throw HostError(bt::appui::HOST_INVALID_REQUEST, "invalid entry: found duplicate key");
                    const uint64_t v{dests.getValues()[i].getInt<uint64_t>()};
                    total = bt::appui::spend_total({total, v});
                    q.rows.push_back({"To", a.to_base58(), "address"});
                    q.rows.push_back({"Amount", coins(v), "amount"});
                }
                enough(total);
                q.title = "Send " + coins(total - fee) + " to " + std::to_string(dests.size()) + (dests.size() == 1 ? " recipient" : " recipients");
                q.message = "One transaction on Coinshift. A payment cannot be taken back.";
                q.rows.push_back({"Fee", coins(fee), "amount"});
                q.rows.push_back({"Total", coins(total), "total"});
                return q;
            },
            [this](const UniValue& p) { return l2l_call("transfer_many", p); }};
        c["withdraw"] = Confirmed{
            [](const UniValue& p) { return same_numbers(p, 4, {1, 2, 3}); },
            [this](const UniValue& p) {
                need_seed();
                const bt::btc::Address to{main_address(p[0])};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()}, main_fee{p[3].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to withdraw");
                const uint64_t total{bt::appui::spend_total({value, fee, main_fee})};
                enough(total);
                Question q;
                q.title = "Withdraw " + coins(value) + " to eCash";
                q.message = "Your coins leave Coinshift now. eCash pays them to the address below once a withdrawal bundle with them is "
                            "accepted by eCash's miners, which takes a long time (their votes). Until then they show as being withdrawn. "
                            "Coinshift's withdrawal bundles follow a known upstream behaviour, reproduced by this node: a withdrawal may "
                            "stay unpaid for a long time.";
                q.level = "warning";
                q.accept = "Withdraw";
                q.kind = "withdraw";
                q.rows = {{"To (on eCash)", to.to_string(), "address"}, {"Amount", coins(value), "amount"}, {"Fee on eCash", coins(main_fee), "amount"},
                          {"Fee on Coinshift", coins(fee), "amount"}, {"Total", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("withdraw", p); }};
        c["create_deposit"] = Confirmed{
            [](const UniValue& p) { return same_numbers(p, 3, {1, 2}); },
            [this](const UniValue& p) {
                const Address to{side_address(p[0], "address")};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to deposit");
                const uint64_t total{bt::appui::spend_total({value, fee})};
                const bool ours{addresses().contains(to)};
                Question q;
                q.title = "Deposit " + coins(value) + " into Coinshift";
                q.message = "Paid on eCash by the wallet of the mainchain interface this node uses (" + node.enforcer().url() +
                            "), into Coinshift's slot " + std::to_string(int(THIS_SIDECHAIN)) + ". It is credited on Coinshift in the first block after eCash mines it.";
                if (!ours) {
                    q.message += " The address is NOT one of this wallet: the coins go to whoever holds it.";
                    q.level = "danger";
                }
                q.accept = "Deposit";
                q.kind = "deposit";
                q.rows = {{"Into (Coinshift)", to.to_base58(), "address"}, {"Amount", coins(value), "amount"}, {"Fee on eCash", coins(fee), "amount"},
                          {"Total (paid on eCash)", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("create_deposit", p); }};
        c["mine"] = Confirmed{
            [](const UniValue& p) {
                UniValue out{UniValue::VARR};
                if (p.isArray() && p.size() > 0 && !p[0].isNull()) out.push_back(n64(u64_of(p[0], "fee")));
                return out;
            },
            [this](const UniValue& p) {
                const uint64_t bid{bt::appui::spend_total({p.size() > 0 ? p[0].getInt<uint64_t>() : App::EMPTY_BLOCK_BMM_BRIBE})};
                Question q;
                q.title = "Merge-mine a block of Coinshift";
                q.message = "Asks eCash's miners to include the next block of Coinshift (blind merged mining), with a bid paid on eCash by the "
                            "wallet of the mainchain interface (" + node.enforcer().url() + "). The bid is paid whether or not the block is taken.";
                q.accept = "Bid";
                q.kind = "mine";
                q.rows = {{"Bid (at most)", p.size() > 0 ? coins(bid) : "the block's fees, or " + coins(App::EMPTY_BLOCK_BMM_BRIBE), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("mine", p); }};

        // ---- swaps: ECX on Coinshift for coins of a parent chain (L1). The creator locks the ECX; the claimer pays on L1
        // and takes it. LayerTwo's own parameters, in their order.
        c["create_swap"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 7) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [parent_chain, l1_recipient_address, l1_amount_sats, l2_recipient, l2_amount_sats, required_confirmations, fee_sats]");
                UniValue out{UniValue::VARR};
                const std::string chain{shown_text(p[0], "parent_chain", 16)};
                if (!parent_chain_from_name(chain)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "parent_chain: BTC, BCH, LTC, Signet or Regtest");
                out.push_back(chain);
                out.push_back(shown_text(p[1], "l1_recipient_address", 128));
                out.push_back(n64(u64_of(p[2], "l1_amount_sats")));
                if (p[3].isNull()) out.push_back(UniValue{});
                else out.push_back(p[3]);
                out.push_back(n64(u64_of(p[4], "l2_amount_sats")));
                if (p[5].isNull()) out.push_back(UniValue{});
                else out.push_back(n64(u32_of(p[5], "required_confirmations")));
                out.push_back(n64(u64_of(p[6], "fee_sats")));
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const ParentChain pc{*parent_chain_from_name(p[0].get_str())};
                const std::string l1{p[1].get_str()};
                if (const std::string why{l1_address_problem(l1, pc)}; !why.empty()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The L1 address: " + why);
                const uint64_t l1_amount{p[2].getInt<uint64_t>()}, l2_amount{p[4].getInt<uint64_t>()}, fee{p[6].getInt<uint64_t>()};
                if (l1_amount == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Ask for an amount on L1");
                if (l1_amount > 21'000'000ULL * 100'000'000ULL) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The L1 amount is above 21,000,000 coins");
                if (l2_amount == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Offer some ECX");
                std::optional<Address> recipient;
                if (!p[3].isNull()) recipient = side_address(p[3], "l2_recipient");
                const uint32_t conf{p[5].isNull() ? default_confirmations(pc) : static_cast<uint32_t>(p[5].getInt<uint64_t>())};
                if (conf == 0 || conf > 1000) throw HostError(bt::appui::HOST_INVALID_REQUEST, "required_confirmations: 1 to 1000");
                const auto mine{addresses()};
                if (recipient && mine.contains(*recipient)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The recipient is an address of this wallet: a swap is with someone else");
                const uint64_t total{bt::appui::spend_total({l2_amount, fee})};
                enough(total);
                Question q;
                q.title = "Offer " + coins(l2_amount) + " for " + l1_coins(l1_amount, pc);
                q.message = "Your ECX is locked on Coinshift now. " +
                            std::string{recipient ? "Only the recipient below can take it, " : "Anyone can reserve the swap, then take it "} +
                            "after paying " + l1_coins(l1_amount, pc) + " to your L1 address below. Coinshift does not check that payment on its own "
                            "chain (each node looks at L1 itself): " +
                            std::string{recipient ? "swap with a recipient you trust. " : "watch your L1 address. "} +
                            "If nobody takes it in " + std::to_string(default_swap_expiration_blocks(pc)) + " blocks of Coinshift, it expires and the ECX comes back.";
                q.accept = "Create the swap";
                q.kind = "swap.create";
                q.rows = {{"You offer (Coinshift)", coins(l2_amount), "amount"},
                          {"You ask (" + std::string{coin_name(pc)} + ")", l1_coins(l1_amount, pc), "amount"},
                          {"Paid to your L1 address", l1, "address"},
                          {"Taken by", recipient ? recipient->to_base58() : "anyone who reserves it first", recipient ? "address" : "text"},
                          {"L1 confirmations needed", std::to_string(conf), "text"},
                          {"Fee", coins(fee), "amount"},
                          {"Total locked and paid", coins(total), "total"}};
                if (!recipient) q.level = "warning";
                return q;
            },
            [this](const UniValue& p) { return l2l_call("create_swap", p); }};
        c["accept_swap"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 3) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [swap_id, l2_claimer_address, fee_sats]");
                UniValue out{UniValue::VARR};
                out.push_back(json::swap_id(swap_array(p[0], "swap_id")));
                if (!p[1].isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "l2_claimer_address: an address of this wallet");
                out.push_back(p[1]);
                out.push_back(n64(u64_of(p[2], "fee_sats")));
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const Hash32 id{swap_array(p[0], "swap_id")};
                const Address claimer{side_address(p[1], "l2_claimer_address")};
                const uint64_t fee{p[2].getInt<uint64_t>()};
                const auto mine{addresses()};
                if (!mine.contains(claimer)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The claimer is an address of this wallet (the ECX comes to it)");
                auto s{swap(id)};
                if (!s) throw HostError("No such swap");
                if (s->l2_recipient) throw HostError("This swap is for one recipient only: nothing to reserve");
                if (s->state.kind != SwapState::Pending) throw HostError(std::string{"This swap is not open any more ("} + state_word(s->state.kind) + ")");
                if (s->l2_creator_address && mine.contains(*s->l2_creator_address)) throw HostError("This is a swap of this wallet");
                const uint32_t next{next_height()};
                if (s->expires_at_height && next >= *s->expires_at_height) throw HostError("This swap has expired");
                auto res{with_state([&](const State& st, const Txn& t) { return st.reservation(t, id); })};
                if (res && res->is_live_at(s->parent_chain, next)) throw HostError("Someone reserved this swap already (until block " + std::to_string(res->expires_at(s->parent_chain)) + ")");
                enough(bt::appui::spend_total({fee}));
                // coinshift-rs pays the reservation's fee from one coin of the claimer's address.
                bool coin{false};
                for (auto& [op, o] : wallet.utxos()) coin = coin || (o.address == claimer && o.content.is_value() && o.get_value() >= fee && !app.is_locked(op));
                if (!coin) throw HostError(bt::appui::RPC_WALLET_ERROR, "The reservation's fee is paid from a coin of the claimer's address: " + claimer.to_base58() + " holds none");
                const uint32_t window{accept_expiration_blocks(s->parent_chain)};
                Question q;
                q.title = "Reserve the swap: " + coins(s->l2_amount) + " for " + l1_coins(s->l1_amount, s->parent_chain);
                q.message = "Reserves it for this wallet for " + std::to_string(window) + " blocks of Coinshift. Then pay exactly the amount below to the "
                            "L1 address below from your own " + coin_name(s->parent_chain) + " wallet, and claim the ECX here once the payment has its "
                            "confirmations. Do not pay before the reservation is in a block.";
                q.accept = "Reserve";
                q.kind = "swap.accept";
                q.rows = {{"You get (Coinshift)", coins(s->l2_amount), "amount"},
                          {"You pay (" + std::string{coin_name(s->parent_chain)} + ")", l1_coins(s->l1_amount, s->parent_chain), "amount"},
                          {"To the L1 address", bt::appui::display_text_shown(s->l1_recipient_address), "address"},
                          {"L1 confirmations needed", std::to_string(s->required_confirmations), "text"},
                          {"Reserved for", claimer.to_base58(), "address"},
                          {"Fee", coins(fee), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("accept_swap", p); }};
        c["claim_swap"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 1) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [swap_id, l2_claimer_address]");
                UniValue out{UniValue::VARR};
                out.push_back(json::swap_id(swap_array(p[0], "swap_id")));
                if (p.size() > 1 && !p[1].isNull()) {
                    if (!p[1].isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "l2_claimer_address: an address");
                    out.push_back(p[1]);
                } else {
                    out.push_back(UniValue{});
                }
                return out;
            },
            [this](const UniValue& p) {
                need_seed();
                const Hash32 id{swap_array(p[0], "swap_id")};
                auto s{swap(id)};
                if (!s) throw HostError("No such swap");
                const auto mine{addresses()};
                auto claimer{our_claimer(*s, mine)};
                if (!claimer) throw HostError("This wallet may not claim this swap (it is reserved for, or names, another address)");
                if (!p[1].isNull() && side_address(p[1], "l2_claimer_address") != *claimer) throw HostError("The swap is for " + claimer->to_base58());
                if (s->state.kind != SwapState::ReadyToClaim) throw HostError(std::string{"The swap is not ready to claim ("} + state_word(s->state.kind) + ")");
                // Our app claims only a swap whose L1 payment it saw itself (Swaps → Check the payment), and sees it again now.
                auto paid{store->get(paid_key(id))};
                if (!paid || s->l1_txid.is_unset() || s->l1_txid.to_hex() != *paid)
                    throw HostError("Check the L1 payment of this swap in the app first: it claims only a swap whose payment it saw on L1");
                const L1Check l1{l1_check(*s, *paid)};
                if (l1.confirmations < s->required_confirmations)
                    throw HostError("The L1 payment has " + std::to_string(l1.confirmations) + " of its " + std::to_string(s->required_confirmations) + " confirmations");
                Question q;
                q.title = "Claim " + coins(s->l2_amount);
                q.message = "Your L1 payment for this swap is confirmed on your " + std::string{coin_name(s->parent_chain)} + " node: the ECX locked for it comes to this wallet.";
                q.accept = "Claim";
                q.kind = "swap.claim";
                q.rows = {{"You get (Coinshift)", coins(s->l2_amount), "amount"},
                          {"To", claimer->to_base58(), "address"},
                          {"Your L1 payment", l1.txid, "text"},
                          {"It paid", l1_coins(l1.paid, s->parent_chain) + " to " + bt::appui::display_text_shown(s->l1_recipient_address), "text"},
                          {"L1 confirmations", std::to_string(l1.confirmations) + " (" + std::to_string(s->required_confirmations) + " needed)", "text"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("claim_swap", p); }};
        // Every spend's balance looked at again at once.
        for (auto& [name, conf] : c) {
            auto run{conf.run};
            conf.run = [this, run](const UniValue& p) {
                UniValue r{run(p)};
                if (poller) poller->poke();
                return r;
            };
        }
        return c;
    }
};

const std::vector<std::string>& AppLayer::l2l_reads()
{
    static const std::vector<std::string> reads{"balance", "format_deposit_address", "get_best_mainchain_block_hash", "get_best_sidechain_block_hash", "get_block",
                                                "get_block_hash", "get_block_index", "get_bmm_inclusions", "get_swap_status", "get_wallet_addresses",
                                                "get_wallet_utxos", "getblockcount", "latest_failed_withdrawal_bundle_height", "list_mempool", "list_peers",
                                                "list_swaps", "list_swaps_by_recipient", "list_utxos", "mainchain_sync_progress", "openapi_schema",
                                                "pending_withdrawal_bundle", "sidechain_wealth"};
    return reads;
}

const std::vector<std::string>& AppLayer::confirmed_methods()
{
    static const std::vector<std::string> m{"accept_swap", "claim_swap", "create_deposit", "create_swap", "mine", "transfer", "transfer_many", "withdraw"};
    return m;
}

const std::vector<std::string>& AppLayer::host_methods()
{
    static const std::vector<std::string> m{"dropin.overview", "dropin.history", "dropin.newAddress", "dropin.addresses", "dropin.blocks", "dropin.block",
                                            "dropin.transaction", "dropin.address", "dropin.search", "dropin.checkAddress", "dropin.peg", "dropin.peers",
                                            "dropin.peers.connect", "dropin.peers.forget", "dropin.seed.status", "dropin.seed.prepare", "dropin.seed.create",
                                            "dropin.seed.restore", "dropin.seed.show", "dropin.seed.save", "coinshift.swaps", "coinshift.swap",
                                            "coinshift.wallet", "coinshift.l1", "coinshift.l1.set", "coinshift.l1.find", "coinshift.payment"};
    return m;
}

namespace {
const char* const SHELL_READS[] = {"getmainchaininfo", "getbalances", "getblockchaininfo", "getnetworkinfo", "uptime"};
} // namespace

AppLayer::AppLayer(App& app, std::string network, std::string datadir, bt::appui::AppOptions options)
    : m_(std::make_unique<Impl>(app, std::move(network), std::move(datadir), std::move(options)))
{
}

AppLayer::~AppLayer() { stop(); }

uint16_t AppLayer::port() const { return server_ ? server_->port() : 0; }

UniValue AppLayer::history(size_t limit) { return m_->history(limit); }

bool AppLayer::start(std::string& err)
{
    Impl& m{*m_};
    if (!bt::fsperm::private_dir(m.datadir)) {
        err = "cannot make " + m.datadir;
        return false;
    }
    m.store = std::make_unique<bt::appui::AppStore>(m.datadir + "/backbone-app.sqlite");
    std::string why;
    bt::appui::ServerOptions so{bt::appui::server_options(m.options, m.datadir, why)};
    if (!why.empty()) {
        err = why;
        return false;
    }
    server_ = std::make_unique<bt::appui::RpcServer>(std::move(so));
    m.link = std::make_unique<bt::appui::LinkMonitor>(m.node.enforcer(), THIS_SIDECHAIN, [&m] { return m.followed(); });

    // ---- the app RPC's own methods: the LayerTwo reads, as theirs; the reads of the kit's shell; the setup of the wallet.
    for (const std::string& name : l2l_reads()) {
        server_->add(name, {}, [&m, name](const bt::appui::Call& c) { return m.l2l_call(name, c.params); });
    }
    server_->add("getmainchaininfo", {}, [&m](const bt::appui::Call&) { return m.link->info(); });
    server_->add("getbalances", {}, [&m](const bt::appui::Call&) {
        UniValue mine{UniValue::VOBJ};
        Impl::Balances b;
        if (m.wallet.has_seed()) b = m.balances(m.addresses());
        mine.pushKV("trusted", bt::appui::coins_value(b.available));
        mine.pushKV("untrusted_pending", bt::appui::coins_value(b.pending_in));
        mine.pushKV("immature", bt::appui::coins_value(b.withdrawing));
        UniValue out{UniValue::VOBJ};
        out.pushKV("mine", mine);
        return out;
    });
    server_->add("getblockchaininfo", {}, [&m](const bt::appui::Call&) {
        UniValue o{UniValue::VOBJ};
        auto h{m.node.height()};
        auto t{m.node.tip()};
        o.pushKV("chain", m.network);
        o.pushKV("blocks", h ? int64_t(*h) : int64_t{-1});
        o.pushKV("headers", h ? int64_t(*h) : int64_t{-1});
        o.pushKV("bestblockhash", t ? bt::hex_encode(*t) : "");
        o.pushKV("initialblockdownload", false);
        o.pushKV("verificationprogress", 1.0);
        o.pushKV("warnings", UniValue{UniValue::VARR});
        return o;
    });
    server_->add("getnetworkinfo", {}, [&m](const bt::appui::Call&) {
        UniValue o{UniValue::VOBJ};
        size_t n{0};
        for (auto& p : m.node.peers()) n += p.connected ? 1 : 0;
        o.pushKV("version", 1408);
        o.pushKV("subversion", std::string{"/backbone-coinshift:"} + VERSION_TEXT + "/");
        o.pushKV("connections", static_cast<uint64_t>(n));
        o.pushKV("networkactive", true);
        o.pushKV("warnings", UniValue{UniValue::VARR});
        return o;
    });
    server_->add("uptime", {}, [&m](const bt::appui::Call&) {
        return UniValue(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - m.started).count()));
    });
    // The setup of the wallet by the unified wallet (doc/app.md "One backup phrase"): its own user (the cookie) sets
    // the phrase it derived; refused when the wallet has another one. Never offered to the pages.
    server_->add("getwalletphraseinfo", {}, [&m](const bt::appui::Call&) { return m.seed_status(); });
    server_->add("setwalletphrase", {"phrase"}, [&m](const bt::appui::Call& c) {
        if (!c[0].isStr()) throw RpcError(bt::appui::RPC_INVALID_PARAMETER, "phrase: the 12 to 24 words");
        std::string phrase{c[0].get_str()};
        const bt::appui::WipeOnExit wipe_phrase{phrase};
        try {
            UniValue r{m.set_phrase(phrase, "unified")};
            LOG_INFO("app layer: the wallet's phrase was set by the app (fingerprint %s)", r.find_value("fingerprint").get_str().c_str());
            return r;
        } catch (const HostError& e) {
            throw RpcError(e.code, e.what());
        }
    });
    server_->add("stop", {}, [&m](const bt::appui::Call&) {
        if (m.app.on_stop) m.app.on_stop();
        return UniValue("Coinshift stopping");
    });
    server_->add("getappmanifest", {}, [this](const bt::appui::Call&) { return manifest(); });

    // ---- the host of the pages
    bt::appui::ChainHost chain;
    chain.name = "Coinshift";
    chain.stem = "coinshift";
    chain.network = m.network;
    chain.info = [&m] { return m.info(); };
    chain.wallet_id = [&m] { return m.wallet_id(); };
    for (const char* r : SHELL_READS) chain.page_reads.insert(r);
    for (const std::string& r : l2l_reads()) chain.page_reads.insert(r);
    chain.confirmed = m.confirmed();
    auto& H{chain.host};
    auto& P{chain.privileges};
    H["dropin.overview"] = [&m](const UniValue&, HostEnv&) {
        UniValue o{UniValue::VOBJ};
        const bool has{m.wallet.has_seed()};
        Impl::Balances b;
        if (has) b = m.balances(m.addresses());
        UniValue w{UniValue::VOBJ};
        w.pushKV("hasSeed", has);
        w.pushKV("available", n64(b.available));
        w.pushKV("withdrawing", n64(b.withdrawing));
        w.pushKV("pendingIn", n64(b.pending_in));
        w.pushKV("pendingOut", n64(b.pending_out));
        w.pushKV("inSwaps", n64(b.in_swaps));
        o.pushKV("wallet", w);
        auto h{m.node.height()};
        auto t{m.node.tip()};
        UniValue c{UniValue::VOBJ};
        c.pushKV("height", h ? int64_t(*h) : int64_t{-1});
        c.pushKV("tip", t ? bt::hex_encode(*t) : "");
        size_t peers{0};
        for (auto& p : m.node.peers()) peers += p.connected ? 1 : 0;
        c.pushKV("peers", static_cast<uint64_t>(peers));
        c.pushKV("wealth", n64(m.node.sidechain_wealth()));
        c.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        c.pushKV("network", m.network);
        o.pushKV("chain", c);
        o.pushKV("mainchain", m.link->info());
        return o;
    };
    P["dropin.overview"] = {"balance", "getblockcount"};
    H["dropin.history"] = [&m](const UniValue& p, HostEnv&) {
        const UniValue& lim{param(p, "limit")};
        const size_t limit{lim.isNum() ? static_cast<size_t>(std::clamp<int64_t>(lim.getInt<int64_t>(), 1, 5000)) : size_t{500}};
        if (!m.wallet.has_seed()) return UniValue{UniValue::VARR};
        return m.history(limit);
    };
    P["dropin.history"] = {"get_wallet_utxos"};
    H["dropin.newAddress"] = [&m](const UniValue&, HostEnv&) {
        if (!m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "The wallet has no backup phrase yet: set it up first");
        const Address a{m.wallet.get_new_address()};
        UniValue o{UniValue::VOBJ};
        o.pushKV("address", a.to_base58());
        o.pushKV("depositAddress", a.to_base58()); // the bare address: what every LayerTwo chain credits (doc/app.md)
        return o;
    };
    P["dropin.newAddress"] = {"get_new_address"};
    H["dropin.addresses"] = [&m](const UniValue&, HostEnv&) {
        UniValue out{UniValue::VARR};
        if (!m.wallet.has_seed()) return out;
        std::map<Address, std::pair<uint64_t, uint64_t>> got; // balance, received
        const auto mine{m.addresses()};
        for (auto& [op, o] : m.node.utxos_by_addresses(mine)) {
            if (!o.content.is_value()) continue;
            got[o.address].first = Impl::add(got[o.address].first, o.get_value());
            got[o.address].second = Impl::add(got[o.address].second, o.get_value());
        }
        for (auto& [op, s] : m.node.stxos_by_addresses(mine)) {
            if (s.output.content.is_value()) got[s.output.address].second = Impl::add(got[s.output.address].second, s.output.get_value());
        }
        for (const Address& a : mine) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("address", a.to_base58());
            r.pushKV("depositAddress", a.to_base58());
            r.pushKV("balance", n64(got[a].first));
            r.pushKV("received", n64(got[a].second));
            out.push_back(r);
            if (out.size() >= 5000) break;
        }
        return out;
    };
    P["dropin.addresses"] = {"get_wallet_addresses"};
    H["dropin.blocks"] = [&m](const UniValue& p, HostEnv&) {
        const UniValue& cnt{param(p, "count")};
        const UniValue& from{param(p, "from")};
        const int64_t count{cnt.isNum() ? std::clamp<int64_t>(cnt.getInt<int64_t>(), 1, 200) : int64_t{20}};
        UniValue out{UniValue::VARR};
        auto h{m.node.height()};
        if (!h) return out;
        int64_t at{from.isNum() ? std::min<int64_t>(from.getInt<int64_t>(), *h) : int64_t(*h)};
        for (; at >= 0 && static_cast<int64_t>(out.size()) < count; --at) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(at))};
            if (!hash) break;
            auto hd{m.node.header(*hash)};
            auto b{m.node.body(*hash)};
            if (!hd || !b) continue;
            out.push_back(m.block_summary(static_cast<uint32_t>(at), *hash, *hd, *b));
        }
        return out;
    };
    P["dropin.blocks"] = {"get_block"};
    H["dropin.block"] = [&m](const UniValue& p, HostEnv& env) {
        const UniValue& height{param(p, "height")};
        if (height.isNum()) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(std::clamp<int64_t>(height.getInt<int64_t>(), 0, UINT32_MAX)))};
            if (!hash) throw HostError("No block at this height");
            return m.block_detail(*hash, m.mine_for(env));
        }
        auto hash{parse_hash(str_param(p, "hash"))};
        if (!hash) throw HostError(bt::appui::HOST_INVALID_REQUEST, "hash: 64 hex digits");
        return m.block_detail(*hash, m.mine_for(env));
    };
    P["dropin.block"] = {"get_block"};
    H["dropin.transaction"] = [&m](const UniValue& p, HostEnv& env) {
        auto txid{parse_hash(str_param(p, "txid"))};
        if (!txid) throw HostError(bt::appui::HOST_INVALID_REQUEST, "txid: 64 hex digits");
        return m.tx_detail(*txid, m.mine_for(env));
    };
    P["dropin.transaction"] = {"get_block"};
    H["dropin.address"] = [&m](const UniValue& p, HostEnv& env) {
        auto a{Address::from_base58(str_param(p, "address"))};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Not a Coinshift address");
        return m.address_detail(*a, m.mine_for(env));
    };
    P["dropin.address"] = {"list_utxos"};
    H["dropin.search"] = [&m](const UniValue& p, HostEnv&) {
        std::string q{str_param(p, "query")};
        q.erase(0, q.find_first_not_of(" \t"));
        q.erase(q.find_last_not_of(" \t") + 1);
        UniValue o{UniValue::VOBJ};
        if (q.size() > 120) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Too long");
        if (!q.empty() && q.size() <= 10 && q.find_first_not_of("0123456789") == std::string::npos) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(std::min<uint64_t>(std::stoull(q), UINT32_MAX)))};
            if (hash) {
                o.pushKV("kind", "block");
                o.pushKV("hash", bt::hex_encode(*hash));
                return o;
            }
        } else if (auto h{parse_hash(q)}) {
            if (m.node.header(*h)) {
                o.pushKV("kind", "block");
                o.pushKV("hash", q);
                return o;
            }
            if (m.swap(*h)) {
                o.pushKV("kind", "swap");
                o.pushKV("id", q);
                return o;
            }
            o.pushKV("kind", "transaction");
            o.pushKV("txid", q);
            return o;
        } else if (Address::from_base58(q)) {
            o.pushKV("kind", "address");
            o.pushKV("address", q);
            return o;
        } else if (auto inner{Address::from_deposit_address(q, THIS_SIDECHAIN)}) {
            o.pushKV("kind", "address");
            o.pushKV("address", inner->to_base58());
            return o;
        }
        o.pushKV("kind", "none");
        return o;
    };
    P["dropin.search"] = {"get_block"};
    H["dropin.checkAddress"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string text{str_param(p, "address")};
        const std::string side{param(p, "side").isStr() ? param(p, "side").get_str() : "coinshift"};
        UniValue o{UniValue::VOBJ};
        if (side == "mainchain") {
            auto a{bt::btc::Address::parse(text)};
            o.pushKV("valid", a.has_value() && main_address_of_network(*a, m.network));
            if (a && !main_address_of_network(*a, m.network)) o.pushKV("error", "an eCash address of another network");
            else if (!a) o.pushKV("error", Address::from_base58(text) ? "a Coinshift address, not an eCash one" : "not an eCash address");
            return o;
        }
        auto a{Address::from_base58(text)};
        o.pushKV("valid", a.has_value());
        if (a) {
            if (env.may_call(WALLET_READ)) o.pushKV("mine", m.addresses().contains(*a));
        } else {
            o.pushKV("error", bt::btc::Address::parse(text) ? "an eCash address, not a Coinshift one" : text.rfind("s255_", 0) == 0 ? "a deposit address: use it on eCash" : "not a Coinshift address");
        }
        return o;
    };
    P["dropin.checkAddress"] = {};
    H["dropin.peg"] = [&m](const UniValue&, HostEnv&) { return m.peg(); };
    P["dropin.peg"] = {"pending_withdrawal_bundle", "get_wallet_utxos"};
    H["dropin.peers"] = [&m](const UniValue&, HostEnv&) {
        UniValue out{UniValue::VARR};
        for (auto& pv : m.node.peers()) {
            UniValue o{UniValue::VOBJ};
            o.pushKV("address", pv.address.to_string());
            o.pushKV("connected", pv.connected);
            o.pushKV("inbound", pv.inbound);
            out.push_back(o);
        }
        return out;
    };
    P["dropin.peers"] = {"list_peers"};
    H["dropin.peers.connect"] = [&m](const UniValue& p, HostEnv&) {
        std::string a{str_param(p, "address")};
        if (a.size() > 300) throw HostError(bt::appui::HOST_INVALID_REQUEST, "address: too long");
        if (a.rfind(':') == std::string::npos) a += ":" + std::to_string(DEFAULT_PORT);
        std::string err;
        if (!m.node.connect_peer(a, err)) throw HostError(err);
        return UniValue{true};
    };
    P["dropin.peers.connect"] = {"connect_peer"};
    H["dropin.peers.forget"] = [&m](const UniValue& p, HostEnv&) {
        std::string a{str_param(p, "address")};
        if (a.size() > 300) throw HostError(bt::appui::HOST_INVALID_REQUEST, "address: too long");
        if (a.rfind(':') == std::string::npos) a += ":" + std::to_string(DEFAULT_PORT);
        return UniValue{m.node.forget_peer(a)};
    };
    P["dropin.peers.forget"] = {"forget_peer"};
    // ---- the backup phrase: its words shown only in the app's trusted dialog, never to the pages.
    H["dropin.seed.status"] = [&m](const UniValue&, HostEnv&) { return m.seed_status(); };
    P["dropin.seed.status"] = {"get_wallet_addresses"};
    H["dropin.seed.prepare"] = [&m](const UniValue&, HostEnv&) {
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        UniValue o{UniValue::VOBJ};
        o.pushKV("id", m.phrases.put(bt::hd::generate_mnemonic(12)));
        o.pushKV("words", 12);
        return o;
    };
    P["dropin.seed.prepare"] = {"set_seed_from_mnemonic"};
    H["dropin.seed.create"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string id{str_param(p, "id")};
        auto phrase{m.phrases.get(id)};
        if (!phrase) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This new phrase is not known any more (5 minutes): make another");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        Question q;
        const bt::appui::WipeOnExit wipe_words{q.details};
        q.title = "Your backup phrase";
        q.message = "Write these 12 words down, in order, on paper, and keep them safe. They are the only way to get this wallet back. "
                    "Anyone who has them can take its coins. They are not shown to the app's pages.";
        q.details = Impl::words_block(*phrase);
        q.accept = "I wrote them down";
        q.level = "warning";
        q.kind = "phrase.create";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        UniValue r{m.set_phrase(*phrase, "app")};
        m.phrases.erase(id);
        return r;
    };
    P["dropin.seed.create"] = {"set_seed_from_mnemonic"};
    H["dropin.seed.restore"] = [&m](const UniValue& p, HostEnv& env) {
        std::string phrase{str_param(p, "phrase")};
        const bt::appui::WipeOnExit wipe_typed{phrase};
        auto parsed{bt::hd::parse_mnemonic(phrase)};
        if (!parsed) throw HostError(bt::appui::HOST_INVALID_REQUEST, bt::hd::mnemonic_error(phrase));
        const bt::appui::WipeOnExit wipe_parsed{*parsed};
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        auto seed{bt::hd::mnemonic_to_seed(*parsed)};
        const std::string fp{bt::appui::seed_fingerprint(seed.span())};
        sodium_memzero(seed.data(), seed.size());
        Question q;
        q.title = "Restore the wallet from this phrase";
        q.message = "The wallet takes the keys of this backup phrase. Its coins show once the node has looked at the chain.";
        q.rows = {{"Words", std::to_string(std::count(parsed->begin(), parsed->end(), ' ') + 1), "text"}, {"Fingerprint", fp, "text"}};
        q.accept = "Restore";
        q.kind = "phrase.restore";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        UniValue r{m.set_phrase(*parsed, "restored")};
        // The wallet's addresses found again from the chain (LayerTwo's gap limit of 20).
        std::set<Address> seen;
        for (auto& [op, o] : m.node.utxos()) seen.insert(o.address);
        m.wallet.recover_addresses(seen);
        m.wallet.update();
        return r;
    };
    P["dropin.seed.restore"] = {"set_seed_from_mnemonic"};
    // Shown or saved only when named in the whitelist (no privilege stands for it): the most sensitive read there is.
    H["dropin.seed.show"] = [&m](const UniValue&, HostEnv& env) {
        auto phrase{m.kept_phrase()};
        if (!phrase) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet's phrase was not set through the app: keep the backup you made then");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        Question q;
        const bt::appui::WipeOnExit wipe_words{q.details};
        q.title = "Your backup phrase";
        q.message = "Make sure nobody can see your screen. Anyone who has these words can take this wallet's coins.";
        q.details = Impl::words_block(*phrase);
        q.accept = "Done";
        q.cancel = "Close";
        q.level = "warning";
        q.kind = "phrase.show";
        env.ask(q);
        return UniValue{true};
    };
    H["dropin.seed.save"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string path{str_param(p, "path")};
        const auto file{env.file(path, true)};
        auto phrase{m.kept_phrase()};
        if (!phrase) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet's phrase was not set through the app: keep the backup you made then");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        Question q;
        q.title = "Save your backup phrase to a file";
        q.message = "Anyone who can read this file can take this wallet's coins. Keep it somewhere only you can reach, off this computer if you can.";
        q.rows = {{"File", path, "text"}};
        q.accept = "Save";
        q.kind = "phrase.save";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        std::string content{"Coinshift backup phrase (BIP-39, coinshift 0.14.8 keys)\n" + *phrase + "\n"};
        const bt::appui::WipeOnExit wipe_content{content};
        try {
            bt::appui::ChosenFiles::write(file, content);
        } catch (const std::runtime_error& e) {
            throw HostError(std::string{"The file chosen cannot be written: "} + e.what());
        }
        return UniValue{true};
    };

    // ---- Coinshift's own: the swaps of the chain (as this node sees them), the wallet's, and the app's check of an L1
    // payment.
    H["coinshift.swaps"] = [&m](const UniValue&, HostEnv& env) {
        const Impl::Mine mine{m.mine_for(env)};
        UniValue out{UniValue::VARR};
        for (const Swap& s : m.with_state([&](const State& st, const Txn& t) { return st.swaps(t); })) {
            if (out.size() >= MAX_SWAPS) break;
            out.push_back(m.swap_json(s, mine));
        }
        return out;
    };
    P["coinshift.swaps"] = {"list_swaps"};
    H["coinshift.swap"] = [&m](const UniValue& p, HostEnv& env) {
        const Hash32 id{swap_hex(param(p, "id"), "id")};
        auto s{m.swap(id)};
        if (!s) throw HostError("No such swap on this node");
        UniValue o{m.swap_json(*s, m.mine_for(env))};
        // The transactions of the swap on the active chain (its creation, reservations, its claim).
        UniValue txs{UniValue::VARR};
        auto h{m.node.height()};
        for (uint32_t at{s->created_at_height}; h && at <= *h && txs.size() < 50 && at - s->created_at_height < 10'000; ++at) {
            auto hash{m.node.block_hash_at(at)};
            if (!hash) break;
            auto b{m.node.body(*hash)};
            if (!b) continue;
            for (const auto& tx : b->transactions) {
                if (tx.data.kind == TxData::Regular || tx.data.swap_id != id) continue;
                UniValue t{UniValue::VOBJ};
                t.pushKV("txid", bt::hex_encode(tx.txid()));
                t.pushKV("kind", tx.data.kind == TxData::SwapCreate ? "create" : tx.data.kind == TxData::SwapAccept ? "accept" : "claim");
                t.pushKV("height", static_cast<uint64_t>(at));
                if (tx.data.l2_claimer_address) t.pushKV("claimer", tx.data.l2_claimer_address->to_base58());
                txs.push_back(t);
            }
        }
        o.pushKV("transactions", txs);
        return o;
    };
    P["coinshift.swap"] = {"get_swap_status", "get_block"};
    H["coinshift.wallet"] = [&m](const UniValue&, HostEnv&) {
        // What the wallet holds and has locked in swaps, and an address of it to claim with.
        UniValue o{UniValue::VOBJ};
        const auto mine{m.addresses()};
        const Impl::Balances b{m.balances(mine)};
        o.pushKV("available", n64(b.available));
        o.pushKV("inSwaps", n64(b.in_swaps));
        o.pushKV("hasSeed", m.wallet.has_seed());
        if (m.wallet.has_seed()) {
            // The address a reservation names (and the claim pays): one that holds a coin for the reservation's fee
            // (coinshift-rs takes the fee from a coin of the claimer's address).
            if (auto a{m.fee_address(1000)}) o.pushKV("claimAddress", a->to_base58());
        }
        return o;
    };
    P["coinshift.wallet"] = {"get_wallet_addresses", "balance"};
    // The L1 nodes the app checks swap payments on (url and user; the password never leaves the node).
    H["coinshift.l1"] = [&m](const UniValue&, HostEnv&) {
        UniValue out{UniValue::VARR};
        for (ParentChain c : {ParentChain::BTC, ParentChain::BCH, ParentChain::LTC, ParentChain::Signet, ParentChain::Regtest}) {
            UniValue e{UniValue::VOBJ};
            e.pushKV("chain", parent_chain_name(c));
            e.pushKV("coin", coin_name(c));
            auto cfg{m.l1_config(c)};
            e.pushKV("configured", cfg.has_value());
            if (cfg) {
                e.pushKV("url", cfg->url);
                e.pushKV("user", cfg->user);
            }
            out.push_back(e);
        }
        return out;
    };
    H["coinshift.l1.set"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string chain{str_param(p, "chain")};
        auto pc{parent_chain_from_name(chain)};
        if (!pc) throw HostError(bt::appui::HOST_INVALID_REQUEST, "chain: BTC, BCH, LTC, Signet or Regtest");
        if (param(p, "remove").isTrue()) {
            Question q;
            q.title = std::string{"Stop checking "} + coin_name(*pc) + " payments";
            q.message = "The app forgets this L1 node. Swaps of this chain cannot be claimed in the app until another is set.";
            q.accept = "Forget it";
            q.kind = "l1.remove";
            if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
            m.store->put(Impl::l1_key(*pc), "");
            return UniValue{true};
        }
        const std::string url{shown_text(param(p, "url"), "url", 200)};
        if (url.rfind("http://", 0) != 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "url: http://host:port (the L1 node's RPC)");
        const std::string user{param(p, "user").isNull() ? std::string{} : shown_text(param(p, "user"), "user", 200)};
        if (!param(p, "password").isStr() || param(p, "password").get_str().size() > 500) throw HostError(bt::appui::HOST_INVALID_REQUEST, "password: a text of at most 500 bytes");
        std::string password{param(p, "password").get_str()};
        const bt::appui::WipeOnExit wipe_password{password};
        // Asked first (the node contacts no address a page names before the user said yes), then the node must answer
        // and be of the chain named (where its reply says which it is).
        Question q;
        q.title = std::string{"Check "} + coin_name(*pc) + " payments on this node";
        q.message = "The app asks this L1 node about the payments of your swaps (it claims a swap only once this node shows its payment "
                    "confirmed). Its password is kept on this computer, readable by you only.";
        q.accept = "Use it";
        q.kind = "l1.set";
        q.rows = {{"Chain", coin_name(*pc), "text"}, {"L1 node", url, "text"}, {"User", user.empty() ? "(none)" : user, "text"}};
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        std::string err;
        auto name{l1::Client{L1RpcConfig{url, user, password}}.chain_name(err)};
        if (!name) throw HostError("This L1 node does not answer: " + err);
        const char* want{*pc == ParentChain::BTC ? "main" : *pc == ParentChain::Signet ? "signet" : *pc == ParentChain::Regtest ? "regtest" : nullptr};
        if (want && *name != want) throw HostError("This L1 node is on \"" + *name + "\", not " + coin_name(*pc));
        UniValue j{UniValue::VOBJ};
        j.pushKV("url", url);
        j.pushKV("user", user);
        j.pushKV("password", password);
        std::string text{j.write()};
        const bt::appui::WipeOnExit wipe_text{text};
        m.store->put(Impl::l1_key(*pc), text);
        return UniValue{true};
    };
    // The L1 outputs that pay a swap's L1 address its amount now (the user's L1 node scans its UTXO set): what the user
    // may name as their payment.
    H["coinshift.l1.find"] = [&m](const UniValue& p, HostEnv&) {
        const Hash32 id{swap_hex(param(p, "id"), "id")};
        auto s{m.swap(id)};
        if (!s) throw HostError("No such swap on this node");
        Impl::l1_address_usable(*s);
        UniValue sp{UniValue::VARR};
        sp.push_back("start");
        UniValue descs{UniValue::VARR};
        descs.push_back("addr(" + s->l1_recipient_address + ")");
        sp.push_back(descs);
        const UniValue r{m.l1_call(s->parent_chain, "scantxoutset", sp)};
        UniValue out{UniValue::VARR};
        const UniValue& un{r.find_value("unspents")};
        const UniValue& tip{r.find_value("height")};
        if (!un.isArray()) return out;
        for (const UniValue& u : un.getValues()) {
            if (out.size() >= 50) break;
            if (!u.isObject() || !u.find_value("txid").isStr() || !u.find_value("amount").isNum()) continue;
            auto sats{l1_amount_sats(u.find_value("amount").getValStr())};
            if (!sats || *sats < s->l1_amount) continue;
            UniValue e{UniValue::VOBJ};
            e.pushKV("txid", u.find_value("txid").get_str());
            e.pushKV("amount", n64(*sats));
            if (u.find_value("height").isNum() && tip.isNum()) {
                const int64_t c{tip.getInt<int64_t>() - u.find_value("height").getInt<int64_t>() + 1};
                e.pushKV("confirmations", c > 0 ? c : 0);
            }
            out.push_back(e);
        }
        return out;
    };
    P["coinshift.l1.find"] = {"get_swap_status"};
    // The user's L1 payment for a swap this wallet may claim: seen on the user's L1 node (to the swap's L1 address, at
    // least its amount), then given to the node as LayerTwo's update_swap_l1_txid does (node-local), with the
    // confirmations that L1 node says. Again as they grow; a claim in the app needs it.
    H["coinshift.payment"] = [&m](const UniValue& p, HostEnv&) {
        const Hash32 id{swap_hex(param(p, "id"), "id")};
        const std::string txid{str_param(p, "l1Txid")};
        auto s{m.swap(id)};
        if (!s) throw HostError("No such swap on this node");
        const auto mine{m.addresses()};
        auto claimer{m.our_claimer(*s, mine)};
        if (!claimer) throw HostError("This wallet may not claim this swap: reserve it first (or it names another recipient)");
        if (s->state.kind == SwapState::Completed || s->state.kind == SwapState::Cancelled) throw HostError(std::string{"This swap is "} + state_word(s->state.kind));
        if (auto known{m.store->get(m.paid_key(id))}; known && !known->empty() && *known != txid && !s->l1_txid.is_unset())
            throw HostError("This swap's payment is " + *known + " already");
        const Impl::L1Check c{m.l1_check(*s, txid)};
        UniValue up{UniValue::VARR};
        up.push_back(json::swap_id(id));
        up.push_back(c.txid);
        up.push_back(n64(c.confirmations));
        up.push_back(claimer->to_base58());
        m.l2l_call("update_swap_l1_txid", up);
        m.store->put(m.paid_key(id), c.txid);
        if (m.poller) m.poller->poke();
        UniValue o{UniValue::VOBJ};
        o.pushKV("txid", c.txid);
        o.pushKV("paid", n64(c.paid));
        o.pushKV("confirmations", static_cast<uint64_t>(c.confirmations));
        o.pushKV("required", static_cast<uint64_t>(s->required_confirmations));
        if (auto now{m.swap(id)}) o.pushKV("swap", m.swap_json(*now, mine));
        return o;
    };
    P["coinshift.payment"] = {"update_swap_l1_txid"};

    chain.snapshot = [&m] { return m.poller ? m.poller->snapshot() : UniValue{UniValue::VOBJ}; };
    host_ = std::make_unique<bt::appui::RemoteHost>(std::move(chain), bt::appui::HostOptions{m.options.chooser, m.options.file_dir}, *server_);
    m.poller = std::make_unique<bt::appui::Poller>(host_->events(), WALLET_NAME, [&m] { return m.look(); });
    if (!server_->listen(err)) return false;
    m.link->start();
    m.poller->start();
    LOG_INFO("app RPC on 127.0.0.1:%u (%zu methods; the LayerTwo RPC is not changed by it)", unsigned(server_->port()), server_->methods().size());
    return true;
}

void AppLayer::stop()
{
    if (!m_) return;
    if (m_->poller) m_->poller->stop();
    if (m_->link) m_->link->stop();
    if (host_) host_->stop();
    if (server_) server_->stop();
}

std::vector<std::string> AppLayer::app_methods() const
{
    std::vector<std::string> out{server_ ? server_->methods() : std::vector<std::string>{}};
    return out;
}

UniValue AppLayer::manifest() const
{
    // What a pack of Coinshift's pages uses of this node (the unified wallet names it in the app's whitelist): the
    // reads of the pages, the confirmed methods, the host methods, and the host's own methods.
    UniValue o{UniValue::VOBJ};
    o.pushKV("stem", "coinshift");
    o.pushKV("hostApi", bt::appui::HOST_API);
    UniValue reads{UniValue::VARR};
    for (const char* r : SHELL_READS) reads.push_back(r);
    for (const std::string& r : l2l_reads()) reads.push_back(r);
    o.pushKV("reads", reads);
    UniValue confirmed{UniValue::VARR};
    for (const std::string& c : confirmed_methods()) confirmed.push_back(c);
    o.pushKV("confirmed", confirmed);
    UniValue host{UniValue::VARR};
    for (const std::string& h : host_methods()) host.push_back(h);
    o.pushKV("host", host);
    UniValue protocol{UniValue::VARR};
    for (const char* p : {"webuihost", "describeconfirmation", "webuievents", "webuichosenfile", "appserverproof"}) protocol.push_back(p);
    o.pushKV("protocol", protocol);
    return o;
}

} // namespace coinshift
