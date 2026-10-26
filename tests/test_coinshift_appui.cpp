// Coinshift's app layer (doc/app.md): the LayerTwo JSON-RPC is the same with the app layer running (same methods, same
// bytes for every reply); the app RPC's reads are the LayerTwo handlers' own JSON; the backup phrase's flows (made, its
// words only in a question, refused over another) and the unified wallet's phrase (BIP-85 vectors); wallet ownership
// (a swap's role, "mine") only for a wallet reader; every spend refused before any question when its numbers wrap,
// pass 21,000,000 coins, cannot be paid or name what the app does not take; the app's own check of an L1 payment; the
// wallet never spending an escrow it did not lock; the manifest of app/manifest.json is the node's.
#include "test.h"

#include <coinshift/app.h>
#include <coinshift/appui.h>
#include <coinshift/json.h>

#include <bt/appui/auth.h>
#include <bt/appui/phrase.h>
#include <bt/frost.h>
#include <bt/hd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

using namespace coinshift;

namespace {
const char* ABANDON = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

struct Harness {
    std::string dir;
    std::unique_ptr<Node> node;
    std::unique_ptr<Wallet> wallet;
    std::unique_ptr<App> app;
    explicit Harness(const std::string& name)
    {
        bt::frost::init();
        dir = (std::filesystem::temp_directory_path() / ("bt-coinshift-appui-" + name + "-" + bt::appui::random_hex(6))).string();
        std::filesystem::create_directories(dir);
        bt::sidechain::NodeConfig cfg;
        cfg.datadir = dir;
        cfg.network = bt::sidechain::Network::Regtest;
        cfg.p2p = false;
        cfg.use_seeds = false;
        cfg.mainchain_grpc_url = "http://127.0.0.1:1";
        node = std::make_unique<Node>(cfg);
        std::string err;
        CHECK(node->start(err));
        wallet = std::make_unique<Wallet>(dir + "/backbone-wallet.sqlite", *node);
        app = std::make_unique<App>(*node, *wallet);
    }
    ~Harness()
    {
        app.reset();
        wallet.reset();
        if (node) node->stop();
        node.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

std::string reply(bt::jsonrpc::Server& s, const std::string& method, const std::string& params)
{
    return s.handle(R"({"jsonrpc":"2.0","id":7,"method":")" + method + R"(","params":)" + params + "}");
}

UniValue host_call(AppLayer& layer, const std::string& user, const std::string& method, const std::string& params, const std::string& token = "")
{
    UniValue req{UniValue::VOBJ};
    req.pushKV("kind", "host");
    req.pushKV("method", method);
    UniValue p;
    CHECK(p.read(params));
    req.pushKV("params", p);
    return layer.host().run(req, token, "", UniValue{}, user);
}

bt::appui::AppOptions options()
{
    bt::appui::AppOptions o;
    o.enabled = true;
    o.port = 0;
    // The app's user (a reader of the chain and the wallet), a reader of the chain only, the cookie unrestricted.
    o.whitelist = {"app:webuihost,describeconfirmation,webuievents,getblockchaininfo,getbalances,get_wallet_addresses,dropin.checkAddress,dropin.block,dropin.seed.status,"
                   "dropin.seed.prepare,dropin.seed.create,dropin.seed.restore,dropin.seed.show,transfer,balance,get_block,list_swaps,get_swap_status,update_swap_l1_txid",
                   "chainonly:webuihost,getblockchaininfo,get_block,dropin.checkAddress,list_swaps,get_swap_status"};
    o.whitelist_default = false;
    return o;
}

bool refused_with(AppLayer& layer, const std::string& method, const std::string& params, const std::string& words, const std::string& user = "__cookie__")
{
    UniValue p;
    CHECK(p.read(params));
    try {
        layer.host().describe(method, p, user);
    } catch (const std::exception& e) {
        const bool ok{std::string{e.what()}.find(words) != std::string::npos};
        if (!ok) std::fprintf(stderr, "  %s %s: %s\n", method.c_str(), params.c_str(), e.what());
        return ok;
    }
    std::fprintf(stderr, "  %s %s: not refused\n", method.c_str(), params.c_str());
    return false;
}

bool host_refused(AppLayer& layer, const std::string& user, const std::string& method, const std::string& params, const std::string& words)
{
    try {
        host_call(layer, user, method, params);
    } catch (const std::exception& e) {
        const bool ok{std::string{e.what()}.find(words) != std::string::npos};
        if (!ok) std::fprintf(stderr, "  %s %s: %s\n", method.c_str(), params.c_str(), e.what());
        return ok;
    }
    std::fprintf(stderr, "  %s %s: not refused\n", method.c_str(), params.c_str());
    return false;
}

std::string id_array(const std::string& byte_hex)
{
    std::string s{"["};
    for (int i{0}; i < 32; ++i) s += (i ? "," : "") + std::to_string(std::stoi(byte_hex, nullptr, 16));
    return s + "]";
}
} // namespace

TEST(cs_appui_l2l_rpc_byte_identical_with_and_without_the_app_layer)
{
    Harness h("l2l");
    bt::jsonrpc::Server before;
    register_rpc(before, *h.app);
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    bt::jsonrpc::Server after;
    register_rpc(after, *h.app);
    CHECK(before.methods() == after.methods());
    CHECK_EQ(before.methods().size(), size_t{43});
    const std::vector<std::string> bad{"[]", "[1,2,3,4,5,6,7,8]", R"(["x"])", R"({"nope":1})", "[null]", R"([[1,2,3]])", R"([{}])"};
    static const std::set<std::string> changes{"stop", "mine", "get_new_address", "generate_mnemonic", "set_seed_from_mnemonic", "create_deposit", "transfer",
                                               "transfer_many", "withdraw", "connect_peer", "forget_peer", "invalidate_block", "remove_from_mempool", "connect_block",
                                               "get_block_template", "create_swap", "accept_swap", "claim_swap", "update_swap_l1_txid", "cancel_swap", "reconstruct_swaps"};
    size_t compared{0};
    for (const std::string& m : before.methods()) {
        for (const std::string& p : bad) {
            if (changes.contains(m) && (p == "[]" || p == "[null]")) continue;
            if (m == "generate_mnemonic") {
                UniValue a, b;
                CHECK(a.read(reply(before, m, p)));
                CHECK(b.read(reply(after, m, p)));
                CHECK(a.getKeys() == b.getKeys());
                continue;
            }
            const std::string a{reply(before, m, p)}, b{reply(after, m, p)};
            if (a != b) std::fprintf(stderr, "  %s %s:\n    %s\n    %s\n", m.c_str(), p.c_str(), a.c_str(), b.c_str());
            CHECK(a == b);
            ++compared;
        }
    }
    CHECK(compared > 43 * 4);
    const std::vector<std::pair<std::string, std::string>> reads{
        {"getblockcount", "[]"}, {"get_best_sidechain_block_hash", "[]"}, {"list_utxos", "[]"}, {"list_peers", "[]"}, {"pending_withdrawal_bundle", "[]"},
        {"sidechain_wealth", "[]"}, {"balance", "[]"}, {"get_wallet_addresses", "[]"}, {"get_wallet_utxos", "[]"}, {"openapi_schema", "[]"},
        {"latest_failed_withdrawal_bundle_height", "[]"}, {"get_block_hash", "[0]"}, {"format_deposit_address", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR"])"},
        {"list_swaps", "[]"}, {"list_swaps_by_recipient", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR"])"}, {"get_swap_status", "[" + id_array("00") + "]"},
        {"list_mempool", "[]"}, {"mainchain_sync_progress", "[]"}, {"get_best_mainchain_block_hash", "[]"}};
    for (const auto& [m, p] : reads) CHECK(reply(before, m, p) == reply(after, m, p));
    for (const auto& [m, p] : reads) {
        UniValue r;
        CHECK(r.read(reply(before, m, p)));
        bt::appui::Call c;
        c.method = m;
        CHECK(c.params.read(p));
        c.user = "__cookie__";
        if (r.exists("error")) {
            bool refused{false};
            try {
                layer.server().dispatch(c);
            } catch (const bt::appui::RpcError&) {
                refused = true;
            }
            CHECK(refused);
            continue;
        }
        CHECK_EQ(layer.server().dispatch(c).write(), r.find_value("result").write());
    }
    layer.stop();
}

TEST(cs_appui_phrase_made_in_a_question_refused_over_another)
{
    Harness h("seed");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    const UniValue prep{host_call(layer, "app", "dropin.seed.prepare", "{}")["result"]};
    const std::string id{prep["id"].get_str()};
    CHECK(!prep.exists("phrase"));
    const std::string create{"{\"id\":\"" + id + "\"}"};
    const UniValue q{host_call(layer, "app", "dropin.seed.create", create)["confirm"]};
    CHECK_EQ(q["kind"].get_str(), std::string{"phrase.create"});
    CHECK(!h.wallet->has_seed());
    const UniValue done{host_call(layer, "app", "dropin.seed.create", create, q["token"].get_str())["result"]};
    CHECK(h.wallet->has_seed());
    CHECK(!h.wallet->addresses().empty());
    CHECK_EQ(host_call(layer, "app", "dropin.seed.status", "{}")["result"]["fingerprint"].get_str(), done["fingerprint"].get_str());
    CHECK(host_call(layer, "app", "dropin.seed.status", "{}")["result"]["phraseKept"].isTrue());
    const UniValue shown{host_call(layer, "app", "dropin.seed.show", "{}")};
    CHECK(shown.exists("confirm") && !shown.exists("result"));
    CHECK(host_refused(layer, "app", "dropin.seed.restore", std::string{R"({"phrase":")"} + ABANDON + "\"}", "already"));
    bt::appui::Call c;
    c.method = "setwalletphrase";
    c.params.push_back(ABANDON);
    c.user = "__cookie__";
    bool refused{false};
    try {
        layer.server().dispatch(c);
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    layer.stop();
}

TEST(cs_appui_unified_phrase_bip85_vectors)
{
    // BIP-85 of the master "abandon ×11 about", label sidechain/dropin/v1, stem coinshift: the same as an independent
    // implementation (Python, hardened BIP-32 + BIP-85, tiny-bip39's word list).
    auto master{bt::hd::parse_mnemonic(ABANDON)};
    const auto seed{bt::hd::mnemonic_to_seed(*master)};
    CHECK_EQ(bt::appui::dropin_phrase_index("coinshift", "betanet"), uint32_t{13881959});
    CHECK_EQ(bt::appui::dropin_phrase_index("coinshift", "regtest"), uint32_t{2059020698});
    const std::string betanet{bt::appui::dropin_phrase(seed.span(), "coinshift", "betanet")};
    const std::string regtest{bt::appui::dropin_phrase(seed.span(), "coinshift", "regtest")};
    CHECK_EQ(betanet, std::string{"fetch caught cement salon drop clarify lava marble pudding stuff omit tone"});
    CHECK_EQ(regtest, std::string{"occur home bulk task combine pulse boy transfer engine guard main notice"});
    CHECK_EQ(bt::appui::seed_fingerprint(bt::hd::mnemonic_to_seed(*bt::hd::parse_mnemonic(betanet)).span()), std::string{"9be806b3a06ab479"});
    CHECK_EQ(bt::appui::seed_fingerprint(bt::hd::mnemonic_to_seed(*bt::hd::parse_mnemonic(regtest)).span()), std::string{"d86cb051a4b61b63"});

    Harness h("unified");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    bt::appui::Call c;
    c.method = "setwalletphrase";
    c.params.push_back(regtest);
    c.user = "__cookie__";
    CHECK_EQ(layer.server().dispatch(c)["fingerprint"].get_str(), std::string{"d86cb051a4b61b63"});
    bt::appui::Call info;
    info.method = "getwalletphraseinfo";
    info.user = "__cookie__";
    const UniValue i{layer.server().dispatch(info)};
    CHECK_EQ(i["fingerprint"].get_str(), std::string{"d86cb051a4b61b63"});
    CHECK_EQ(i["from"].get_str(), std::string{"unified"});
    // The wallet it set has the addresses coinshift-rs' wallet has for that phrase (the first of LayerTwo's derivation).
    const auto first{bt::sidechain::address_from_vk(Wallet::signing_key(bt::hd::mnemonic_to_seed(*bt::hd::parse_mnemonic(regtest)), 0).verifying_key())};
    CHECK(h.wallet->addresses().contains(first));
    layer.stop();
}

TEST(cs_appui_wallet_data_only_for_a_wallet_reader)
{
    Harness h("mine");
    h.wallet->set_seed_from_mnemonic(ABANDON);
    const std::string own{h.wallet->get_new_address().to_base58()};
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    const std::string p{"{\"address\":\"" + own + "\"}"};
    CHECK(host_call(layer, "app", "dropin.checkAddress", p)["result"]["mine"].isTrue());
    CHECK(!host_call(layer, "chainonly", "dropin.checkAddress", p)["result"].exists("mine"));
    // The swaps: to both; the wallet's part in them only to its reader.
    CHECK(host_call(layer, "chainonly", "coinshift.swaps", "{}")["result"].isArray());
    for (const char* m : {"coinshift.wallet", "dropin.history", "dropin.addresses", "coinshift.payment"}) {
        bool refused{false};
        try {
            host_call(layer, "chainonly", m, "{}");
        } catch (const bt::appui::RpcError&) {
            refused = true;
        }
        if (!refused) std::fprintf(stderr, "  %s reached a reader of the chain only\n", m);
        CHECK(refused);
    }
    // The L1 nodes (their URL and user) and setting one: only to a user that names them (no privilege stands for it).
    for (const char* m : {"coinshift.l1", "coinshift.l1.set", "dropin.seed.show"}) {
        bool refused{false};
        try {
            host_call(layer, "app", m, "{}");
        } catch (const bt::appui::RpcError&) {
            refused = true;
        }
        CHECK(refused);
    }
    const UniValue l1{host_call(layer, "__cookie__", "coinshift.l1", "{}")["result"]};
    CHECK_EQ(l1.size(), size_t{5});
    for (const UniValue& n : l1.getValues()) CHECK(!n.exists("password") && n["configured"].isFalse());
    layer.stop();
}

TEST(cs_appui_spends_refused_before_any_question)
{
    Harness h("refuse");
    h.wallet->set_seed_from_mnemonic(ABANDON);
    h.wallet->get_new_address();
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    const std::string a{"4XrMw8bmnhchFspSyP4XRUuB5vaR"}, b{"4TXCaVb28BDdZbpF93Gg4GDMhxd9"}, main{"bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt"};
    const std::string mine{h.wallet->addresses().begin()->to_base58()};
    // Sums that wrap past their first part, or pass 21,000,000 ECX; what the wallet cannot pay.
    CHECK(refused_with(layer, "withdraw", "[\"" + main + "\", 9223372036854775808, 9223372036854775808, 9223372036854775809]", "amount overflow"));
    CHECK(refused_with(layer, "transfer", "[\"" + a + "\", 18446744073709551615, 1]", "amount overflow"));
    CHECK(refused_with(layer, "transfer", "[\"" + a + "\", 2100000000000001, 0]", "amount overflow"));
    CHECK(refused_with(layer, "transfer_many", "[{\"" + a + "\": 9223372036854775808, \"" + b + "\": 9223372036854775808}, 1000]", "amount overflow"));
    CHECK(refused_with(layer, "create_deposit", "[\"" + a + "\", 18446744073709551615, 1000]", "amount overflow"));
    CHECK(refused_with(layer, "mine", "[2100000000000001]", "amount overflow"));
    CHECK(refused_with(layer, "transfer", "[\"" + a + "\", 1000, 10]", "Not enough"));
    CHECK(refused_with(layer, "transfer", "[\"" + a + "\", -5, 10]", "whole number"));
    // create_swap: a parent chain of theirs, an L1 address of it that a dialog can show, amounts in range, a recipient
    // that is not this wallet, confirmations 1 to 1000.
    const std::string l1{"bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"};
    CHECK(refused_with(layer, "create_swap", "[\"Doge\", \"" + l1 + "\", 1000, null, 1000, null, 1000]", "BTC, BCH, LTC, Signet or Regtest"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"bcrt1q\\u202ew508\", 1000, null, 1000, null, 1000]", "U+202E"));
    CHECK(refused_with(layer, "create_swap", "[\"Signet\", \"bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4\", 1000, null, 1000, null, 1000]", "another network"));
    CHECK(refused_with(layer, "create_swap", "[\"BTC\", \"tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx\", 1000, null, 1000, null, 1000]", "another network"));
    CHECK(refused_with(layer, "create_swap", "[\"BTC\", \"bcrt1qw508\", 1000, null, 1000, null, 1000]", "not a Bitcoin address"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"rchn1q-x\", 1000, null, 1000, null, 1000]", "not an address"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"not an address\", 1000, null, 1000, null, 1000]", "no space"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"" + l1 + "\", 0, null, 1000, null, 1000]", "Ask for an amount"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"" + l1 + "\", 2100000000000001, null, 1000, null, 1000]", "21,000,000"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"" + l1 + "\", 1000, null, 0, null, 1000]", "Offer some ECX"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"" + l1 + "\", 1000, null, 18446744073709551615, null, 1000]", "amount overflow"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"" + l1 + "\", 1000, \"" + mine + "\", 1000, null, 1000]", "this wallet"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"" + l1 + "\", 1000, null, 1000, 0, 1000]", "1 to 1000"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"" + l1 + "\", 1000, null, 1000, 4294967296, 1000]", "too large"));
    CHECK(refused_with(layer, "create_swap", "[\"Regtest\", \"" + l1 + "\", 1000, null, 1000, null, 1000]", "Not enough"));
    // accept_swap and claim_swap: an id of 32 numbers, a claimer of this wallet, a swap that exists.
    CHECK(refused_with(layer, "accept_swap", "[[1,2,3], \"" + mine + "\", 1000]", "32 numbers"));
    CHECK(refused_with(layer, "accept_swap", "[" + id_array("100") + ", \"" + mine + "\", 1000]", "0 to 255"));
    CHECK(refused_with(layer, "accept_swap", "[" + id_array("aa") + ", \"" + a + "\", 1000]", "an address of this wallet"));
    CHECK(refused_with(layer, "accept_swap", "[" + id_array("aa") + ", \"" + mine + "\", 1000]", "No such swap"));
    CHECK(refused_with(layer, "claim_swap", "[" + id_array("aa") + ", null]", "No such swap"));
    // The app's check of a payment, and finding one: a swap that exists; an L1 node set up for its chain.
    CHECK(host_refused(layer, "app", "coinshift.payment", R"({"id":")" + std::string(64, 'a') + R"(","l1Txid":")" + std::string(64, 'b') + R"("})", "No such swap"));
    CHECK(host_refused(layer, "app", "coinshift.payment", R"({"id":"zz","l1Txid":"00"})", "64 hex digits"));
    // Setting an L1 node: http only, no hidden characters, the node answers.
    CHECK(host_refused(layer, "__cookie__", "coinshift.l1.set", R"({"chain":"Regtest","url":"https://x","user":"u","password":"p"})", "http://"));
    CHECK(host_refused(layer, "__cookie__", "coinshift.l1.set", R"({"chain":"Regtest","url":"http://127.0.0.1:1\u200b","user":"u","password":"p"})", "U+200B"));
    CHECK(host_refused(layer, "__cookie__", "coinshift.l1.set", R"({"chain":"Doge","url":"http://127.0.0.1:1","user":"u","password":"p"})", "chain:"));
    // Asked first (the node contacts no address a page names before a yes, and never shows the password), then checked.
    const std::string set{R"({"chain":"Regtest","url":"http://127.0.0.1:1","user":"u","password":"secret-pw"})"};
    const UniValue asked{host_call(layer, "__cookie__", "coinshift.l1.set", set)};
    CHECK(asked.exists("confirm") && asked["confirm"]["kind"].get_str() == "l1.set");
    CHECK(asked.write().find("secret-pw") == std::string::npos);
    bool unanswered{false};
    try {
        host_call(layer, "__cookie__", "coinshift.l1.set", set, asked["confirm"]["token"].get_str());
    } catch (const std::exception& e) {
        unanswered = std::string{e.what()}.find("does not answer") != std::string::npos;
    }
    CHECK(unanswered);
    CHECK(host_call(layer, "__cookie__", "coinshift.l1", "{}")["result"][4]["configured"].isFalse());
    layer.stop();
}

TEST(cs_appui_l1_payment_read_as_the_app_checks_it)
{
    CHECK(l1_amount_sats("0.5") == std::optional<uint64_t>{50'000'000});
    CHECK(l1_amount_sats("12.00010000") == std::optional<uint64_t>{1'200'010'000});
    CHECK(l1_amount_sats("21000000") == std::optional<uint64_t>{2'100'000'000'000'000});
    CHECK(!l1_amount_sats("21000000.00000001"));
    CHECK(!l1_amount_sats("0.000000001"));
    CHECK(!l1_amount_sats("1e-05"));
    CHECK(!l1_amount_sats("-1"));
    CHECK(!l1_amount_sats("1."));
    CHECK(!l1_amount_sats(""));
    CHECK(l1_address_matches("bcrt1qabc", "bcrt1qabc"));
    CHECK(!l1_address_matches("bcrt1qabc", "bcrt1qabd"));
    CHECK(l1_address_matches("bchtest:qpabc", "qpabc"));
    CHECK(!l1_address_matches("bchtest:qpabc", "bitcoincash:qpabd"));
    CHECK(!l1_address_matches("", ""));
    UniValue tx;
    CHECK(tx.read(R"({"txid":"ab","confirmations":3,"vin":[],"vout":[
        {"value":0.25000000,"n":0,"scriptPubKey":{"address":"bcrt1qme"}},
        {"value":0.30000000,"n":1,"scriptPubKey":{"address":"bcrt1qother"}},
        {"value":0.25000000,"n":2,"scriptPubKey":{"addresses":["bcrt1qme"]}}]})"));
    CHECK(l1_paid_to(tx, "bcrt1qme") == std::optional<uint64_t>{50'000'000});
    CHECK(l1_paid_to(tx, "bcrt1qnobody") == std::optional<uint64_t>{0});
    UniValue odd;
    CHECK(odd.read(R"({"vout":[{"value":"0.5","scriptPubKey":{"address":"x"}}]})"));
    CHECK(!l1_paid_to(odd, "x"));
}

TEST(cs_appui_wallet_never_spends_an_escrow_it_did_not_lock)
{
    // An expired swap's escrow at this wallet's address (it was the swap's pre-specified recipient): not a coin of this
    // wallet. The same escrow of a swap this wallet created: taken back once expired.
    Harness h("escrow");
    h.wallet->set_seed_from_mnemonic(ABANDON);
    const Address me{h.wallet->get_new_address()};
    const Address other{*Address::from_base58("4XrMw8bmnhchFspSyP4XRUuB5vaR")};
    Swap s;
    s.id = bt::sha256(bt::ByteSpan{reinterpret_cast<const uint8_t*>("escrow"), 6});
    s.parent_chain = ParentChain::Regtest;
    s.l1_txid = SwapTxId::zero();
    s.l2_recipient = me;
    s.l2_amount = 50'000;
    s.l1_recipient_address = "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080";
    s.l1_amount = 1000;
    s.expires_at_height = 0;
    s.l2_creator_address = other;
    {
        std::lock_guard<std::recursive_mutex> l(h.node->lock());
        bt::kv::Txn t(h.node->store());
        h.node->state().save_swap(t, s);
        t.commit();
    }
    OutPoint op;
    op.kind = OutPoint::Regular;
    op.txid = bt::sha256(bt::ByteSpan{reinterpret_cast<const uint8_t*>("tx"), 2});
    Content c{Content::SwapPending, 50'000};
    c.swap_id = s.id;
    h.wallet->put_utxos({{op, Output{me, c}}});
    const auto not_locked = [](const OutPoint&) { return false; };
    // What a build says: "not enough funds" when the escrow is not a coin of it; when it is one, the build goes on to
    // prove its inputs (here an output never in the accumulator: the proof fails, which shows it was taken).
    auto build_error = [&] {
        try {
            h.wallet->create_transaction_many({{other, 10'000}}, 1000, not_locked);
        } catch (const WalletError& e) {
            return std::string{e.what()};
        }
        return std::string{};
    };
    CHECK(build_error().find("not enough funds") != std::string::npos);
    // Created by this wallet: its own escrow back, once expired.
    s.l2_creator_address = me;
    {
        std::lock_guard<std::recursive_mutex> l(h.node->lock());
        bt::kv::Txn t(h.node->store());
        h.node->state().save_swap(t, s);
        t.commit();
    }
    CHECK(build_error().find("utreexo") != std::string::npos);
}

TEST(cs_appui_manifest_is_the_nodes)
{
    Harness h("manifest");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    std::ifstream f(std::string{BT_SOURCE_DIR} + "/app/manifest.json");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    UniValue file;
    CHECK(file.read(text));
    const UniValue node{layer.manifest()};
    for (const char* group : {"reads", "confirmed", "host", "protocol"}) {
        std::set<std::string> a, b;
        for (const UniValue& v : file[group].getValues()) a.insert(v.get_str());
        for (const UniValue& v : node[group].getValues()) b.insert(v.get_str());
        if (a != b) std::fprintf(stderr, "  manifest %s differs\n", group);
        CHECK(a == b);
    }
    for (const UniValue& v : file["reads"].getValues()) CHECK(layer.server().has(v.get_str()));
    for (const UniValue& v : file["protocol"].getValues()) CHECK(layer.server().has(v.get_str()));
    bt::jsonrpc::Server l2l;
    register_rpc(l2l, *h.app);
    const auto methods{l2l.methods()};
    for (const UniValue& v : file["confirmed"].getValues()) CHECK(std::find(methods.begin(), methods.end(), v.get_str()) != methods.end());
    // Not offered to the pages: a cancel or an L1 txid set by hand (node-local only), the rebuild of the swap tables.
    for (const char* m : {"cancel_swap", "update_swap_l1_txid", "reconstruct_swaps"}) {
        for (const char* group : {"reads", "confirmed"}) {
            for (const UniValue& v : file[group].getValues()) CHECK(v.get_str() != m);
        }
    }
    CHECK_EQ(file["addressFlags"].get_str(), std::string{"rpc-addr"});
    CHECK_EQ(file["datadirName"].get_str(), std::string{"coinshift"});
    layer.stop();
}
