// JSON shapes and parameter errors against serde_json of THEIR types (coinshift-rs v0.14.8):
// tests/data/oracle/json_cases.txt is `cs_oracle json`; the same sample values are rebuilt here.
#include "test.h"

#include <coinshift/json.h>

#include <fstream>
#include <functional>
#include <map>
#include <sstream>

using namespace coinshift;

namespace {
Hash32 h(uint8_t b)
{
    Hash32 x;
    for (int i = 0; i < 32; i++) x[i] = uint8_t(b + i);
    return x;
}
Address addr(uint8_t b)
{
    Address a;
    for (auto& x : a.b.v) x = b;
    return a;
}
bt::btc::Address btc(const char* s) { return *bt::btc::Address::parse(s); }
Output value_out(uint8_t a, uint64_t v)
{
    Output o;
    o.address = addr(a);
    o.content.kind = Content::Value;
    o.content.value = v;
    return o;
}
Output wd_out(uint8_t a, uint64_t v, uint64_t f, const char* s)
{
    Output o;
    o.address = addr(a);
    o.content.kind = Content::Withdrawal;
    o.content.value = v;
    o.content.main_fee = f;
    o.content.main_address = btc(s);
    return o;
}
std::vector<std::pair<std::string, Output>> outputs()
{
    Output sp;
    sp.address = addr(5);
    sp.content.kind = Content::SwapPending;
    sp.content.value = 10000;
    sp.content.swap_id = h(0x58);
    return {{"output_value", value_out(1, 12345)},
            {"output_withdrawal_main", wd_out(2, 89000, 10000, "bc1qm38x97c87wv26xmmxdf2lcmaaq7ukqz5npmslx")},
            {"output_withdrawal_p2pkh", wd_out(3, 10000, 10000, "1A7pUaYFaXHq1JTTJfLfgy2FwFcjNsVXMT")},
            {"output_withdrawal_test", wd_out(4, 1, 0, "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx")},
            {"output_swap_pending", sp}};
}
std::vector<OutPoint> outpoints()
{
    return {OutPoint{OutPoint::Regular, h(1), 3}, OutPoint{OutPoint::Coinbase, h(2), 0},
            OutPoint{OutPoint::Deposit, *bt::hash_from_rev_hex("ea76bdc8e596b594304a33670c9100b4576c1a0f6038a39ac9e8097e0bcca9d2"), 7}};
}
std::vector<std::pair<std::string, TxData>> txdatas()
{
    std::vector<std::pair<std::string, TxData>> r;
    r.push_back({"txdata_regular", TxData{}});
    TxData c;
    c.kind = TxData::SwapCreate;
    c.swap_id = h(0x58);
    c.parent_chain = ParentChain::BTC;
    c.l1_txid_bytes = Bytes(32, 0);
    c.required_confirmations = 6;
    c.l2_amount = 10000;
    c.l1_recipient_address = "bc1qax4x32ydk562snyyx2rnl3gleyuhjg5w2536m9";
    c.l1_amount = 10000;
    r.push_back({"txdata_create_open", c});
    TxData t = c;
    t.swap_id = h(9);
    t.parent_chain = ParentChain::Regtest;
    t.l1_txid_bytes = {1, 2, 3};
    t.required_confirmations = 3;
    t.l2_recipient = addr(7);
    t.l2_amount = 5;
    t.l1_recipient_address = "x\"y";
    t.l1_amount = UINT64_MAX;
    r.push_back({"txdata_create_to", t});
    TxData cl;
    cl.kind = TxData::SwapClaim;
    cl.swap_id = h(3);
    cl.l2_claimer_address = addr(8);
    cl.proof_data = Bytes{9, 8};
    r.push_back({"txdata_claim", cl});
    TxData cn;
    cn.kind = TxData::SwapClaim;
    cn.swap_id = h(3);
    r.push_back({"txdata_claim_none", cn});
    TxData ac;
    ac.kind = TxData::SwapAccept;
    ac.swap_id = h(4);
    ac.l2_claimer_address = addr(6);
    r.push_back({"txdata_accept", ac});
    return r;
}
bt::utreexo::Proof proof()
{
    return bt::utreexo::Proof{{5, 9}, {bt::utreexo::NodeHash::some(h(0x20)), bt::utreexo::NodeHash::empty(), bt::utreexo::NodeHash{bt::utreexo::NodeHash::Placeholder, {}}}};
}
Transaction sample_tx(const TxData& d)
{
    auto ops = outpoints();
    Transaction t;
    t.inputs = {{ops[0], h(0x40)}, {ops[2], h(0x41)}};
    t.proof = proof();
    for (auto& [n, o] : outputs()) t.outputs.push_back(o);
    t.data = d;
    return t;
}
Authorization authorization()
{
    auto bp = *bt::hex_to_array<32>("e2f2ae0a6abc4e71a884a961c500515f58e30b6aa582dd8db6a65945e08d2d76");
    Authorization a;
    a.verifying_key = bp;
    a.signature.R = bp;
    a.signature.z = Hash32{};
    a.signature.z[0] = 1;
    return a;
}
Header header(bool roots, bool prev)
{
    Header hd;
    hd.merkle_root = h(0x66);
    if (prev) hd.prev_side_hash = h(0x5e);
    hd.prev_main_hash = *bt::hash_from_rev_hex("00000000000000004a8ba51316f643532cf6654b5bdcb59db428e98b86f50d83");
    if (roots) hd.roots = {bt::utreexo::NodeHash::some(h(0x20)), bt::utreexo::NodeHash::empty()};
    return hd;
}
Body body()
{
    Body b;
    b.coinbase = {value_out(9, 1000)};
    b.transactions = {sample_tx(TxData{})};
    b.authorizations = {authorization(), authorization()};
    return b;
}
std::vector<std::pair<std::string, Swap>> swaps()
{
    Swap base;
    base.id = h(0x58);
    base.parent_chain = ParentChain::BTC;
    base.l1_txid = SwapTxId::zero();
    base.required_confirmations = 6;
    base.l2_amount = 10000;
    base.l1_recipient_address = "bc1qax4x32ydk562snyyx2rnl3gleyuhjg5w2536m9";
    base.l1_amount = 10000;
    base.created_at_height = 2144;
    base.expires_at_height = 3152;
    base.l2_creator_address = addr(0x1a);
    Swap waiting = base;
    waiting.state = SwapState{SwapState::WaitingConfirmations, 1, 6};
    waiting.l1_txid = SwapTxId::from_bytes(Bytes{1, 2, 3});
    waiting.l2_recipient = addr(3);
    waiting.l1_claimer_address = std::string("tb1qclaimer");
    waiting.l2_claimer_address = addr(4);
    waiting.l1_txid_validated_at_block_hash = h(0x11);
    waiting.l1_txid_validated_at_height = 17;
    Swap ready = base;
    ready.state = SwapState{SwapState::ReadyToClaim};
    ready.parent_chain = ParentChain::Signet;
    ready.expires_at_height.reset();
    ready.l2_creator_address.reset();
    Swap completed = base;
    completed.state = SwapState{SwapState::Completed};
    completed.parent_chain = ParentChain::LTC;
    Swap cancelled = base;
    cancelled.state = SwapState{SwapState::Cancelled};
    cancelled.parent_chain = ParentChain::BCH;
    return {{"swap_pending", base}, {"swap_waiting", waiting}, {"swap_ready", ready}, {"swap_completed", completed}, {"swap_cancelled", cancelled}};
}

std::map<std::string, std::string> golden()
{
    std::ifstream f(bttest::data_dir() + "/oracle/json_cases.txt");
    std::map<std::string, std::string> m;
    std::string line;
    while (std::getline(f, line)) {
        auto sp = line.find(' ');
        if (sp != std::string::npos) m[line.substr(0, sp)] = line.substr(sp + 1);
    }
    return m;
}
} // namespace

TEST(json_shapes_equal_their_serde)
{
    bt::frost::init();
    auto g = golden();
    REQUIRE(g.size() > 50);
    std::map<std::string, UniValue> ours;
    for (auto& [n, o] : outputs()) ours[n] = json::output(o);
    auto ops = outpoints();
    auto outs = outputs();
    for (size_t i = 0; i < ops.size(); i++) {
        ours["outpoint_" + std::to_string(i)] = json::outpoint(ops[i]);
        ours["pointed_" + std::to_string(i)] = json::pointed_output(ops[i], outs[i].second);
    }
    for (auto& [n, d] : txdatas()) {
        ours[n] = json::txdata(d);
        ours["tx_" + n] = json::transaction(sample_tx(d));
    }
    ours["proof"] = json::proof(proof());
    ours["authorization"] = json::authorization(authorization());
    ours["header_full"] = json::header(header(true, true));
    ours["header_genesis"] = json::header(header(false, false));
    ours["body"] = json::body(body());
    ours["block"] = json::block(header(true, true), body());
    {
        std::map<OutPoint, Output> spend;
        for (size_t i = 0; i < ops.size(); i++) spend[ops[i]] = outs[i + 1].second;
        std::vector<bt::btc::TxOut> payouts{{89000, btc("bc1qm38x97c87wv26xmmxdf2lcmaaq7ukqz5npmslx").script_pubkey()}};
        auto b = State::make_withdrawal_bundle(1526, 10000, spend, payouts);
        REQUIRE(b.has_value());
        ours["withdrawal_bundle"] = json::withdrawal_bundle(*b);
    }
    for (auto& [n, s] : swaps()) {
        ours[n] = json::swap(s);
        ours[n + "_state"] = json::swap_state(s.state);
    }
    ours["swap_id"] = json::swap_id(h(0x58));
    {
        TxData ac;
        ac.kind = TxData::SwapAccept;
        ac.swap_id = h(4);
        ac.l2_claimer_address = addr(6);
        auto tx = sample_tx(ac);
        ours["mempool_tx"] = json::mempool_tx(tx);
        Body b;
        b.transactions = {tx};
        BlockIndexEvents ev;
        ev.deposits.emplace_back(ops[2], outs[0].second);
        ev.bundle_spends.emplace_back(ops[0], *bt::hash_from_rev_hex("4e20f32b53eadfe696573226badea5ffc535eb537e1b9b72e82dccbd1b3fa098"));
        ours["block_index"] = json::block_index(b, ev);
        ours["block_index_empty"] = json::block_index(Body{}, BlockIndexEvents{});
    }
    ours["balance"] = json::balance(670000, 570000);
    ours["sync_idle"] = json::sync_progress("idle", 3, 9, 973923);
    ours["sync_headers"] = json::sync_progress("headers", 3, 9, 973923);
    ours["sync_writing"] = json::sync_progress("writing", 3, 9, 973923);
    ours["sync_state"] = json::sync_progress("state", 3, 9, 973923);
    for (int i = 0; i < 5; i++) ours["parent_chain_" + std::to_string(i)] = UniValue(parent_chain_name(ParentChain(i)));
    size_t n = 0;
    for (auto& [name, v] : ours) {
        auto it = g.find(name);
        REQUIRE(it != g.end());
        std::string w = v.write();
        if (w != it->second) std::fprintf(stderr, "  %s\n    ours:   %s\n    theirs: %s\n", name.c_str(), w.c_str(), it->second.c_str());
        CHECK_EQ(w, it->second);
        n++;
    }
    std::fprintf(stderr, "  %zu JSON shapes identical\n", n);
}

TEST(json_parse_errors_equal_serde)
{
    auto g = golden();
    using P = std::function<void(const UniValue&)>;
    std::map<std::string, P> parsers;
    auto A = [](const UniValue& v) { json::parse_address(v); };
    auto S = [](const UniValue& v) { json::parse_swap_id(v); };
    auto PC = [](const UniValue& v) { json::parse_parent_chain(v); };
    auto D = [](const UniValue& v) { json::parse_transfer_dests(v); };
    auto H = [](const UniValue& v) { json::parse_hash(v); };
    for (auto n : {"addr_ok", "addr_bs58", "addr_len", "addr_num"}) parsers[n] = A;
    for (auto n : {"swapid_short", "swapid_str", "swapid_big"}) parsers[n] = S;
    for (auto n : {"pc_bad", "pc_num"}) parsers[n] = PC;
    for (auto n : {"dests_dup", "dests_ok", "dests_neg"}) parsers[n] = D;
    for (auto n : {"blockhash_short", "blockhash_odd", "blockhash_char", "txid_num"}) parsers[n] = H;
    parsers["u64_neg"] = parsers["u64_float"] = parsers["u64_str"] = [](const UniValue& v) { json::parse_u64(v); };
    parsers["u32_big"] = [](const UniValue& v) { json::parse_u32(v); };
    parsers["optaddr_null"] = [](const UniValue& v) { json::parse_opt_address(v); };
    parsers["btcaddr_bad"] = parsers["btcaddr_b58"] = [](const UniValue& v) { json::parse_btc_address(v); };
    parsers["btcblockhash_bad"] = [](const UniValue& v) { json::parse_btc_hash(v); };
    parsers["sockaddr_bad"] = parsers["sockaddr_ok"] = [](const UniValue& v) { json::parse_socket_addr(v); };
    parsers["block_missing"] = parsers["block_ok_roundtrip"] = [](const UniValue& v) { json::parse_block(v); };
    // inputs exactly as the oracle parsed them
    std::map<std::string, std::string> inputs = {
        {"addr_ok", "\"11111111111111111111\""}, {"addr_bs58", "\"0OIl\""}, {"addr_len", "\"abc\""}, {"addr_num", "5"},
        {"swapid_short", "[1,2]"}, {"swapid_str", "\"58a9\""}, {"pc_bad", "\"Doge\""}, {"pc_num", "3"},
        {"dests_dup", "{\"11111111111111111111\":1,\"11111111111111111111\":2}"}, {"dests_ok", "{\"11111111111111111111\":1}"},
        {"dests_neg", "{\"11111111111111111111\":-1}"}, {"blockhash_short", "\"abcd\""}, {"blockhash_odd", "\"abc\""},
        {"blockhash_char", "\"" + std::string(62, '0') + "zz\""}, {"txid_num", "1"}, {"u64_neg", "-1"}, {"u64_float", "1.5"},
        {"u64_str", "\"1\""}, {"u32_big", "4294967296"}, {"optaddr_null", "null"}, {"btcaddr_bad", "\"bc1qxyz\""},
        {"btcaddr_b58", "\"1A7pUaYFaXHq1JTTJfLfgy2FwFcjNsVXMx\""}, {"btcblockhash_bad", "\"xyz\""}, {"sockaddr_bad", "\"1.2.3.4\""},
        {"sockaddr_ok", "\"[::1]:4255\""}, {"block_missing", "{\"header\":{}}"}};
    {
        std::string big = "[";
        for (int i = 0; i < 31; i++) big += "0,";
        inputs["swapid_big"] = big + "256]";
        inputs["block_ok_roundtrip"] = json::block(header(true, true), body()).write();
    }
    size_t n = 0;
    for (auto& [name, in] : inputs) {
        auto it = g.find(name);
        REQUIRE(it != g.end());
        UniValue v;
        REQUIRE(v.read(in));
        std::string got = "OK";
        try {
            parsers.at(name)(v);
        } catch (const bt::jsonrpc::RpcError& e) {
            got = "ERR " + (e.data ? *e.data : std::string(e.what()));
        }
        std::string want = it->second;
        if (auto p = want.find(" at line "); p != std::string::npos) want = want.substr(0, p); // the server adds the position
        if (got != want) std::fprintf(stderr, "  %s: ours %s / theirs %s\n", name.c_str(), got.c_str(), want.c_str());
        CHECK_EQ(got, want);
        n++;
    }
    std::fprintf(stderr, "  %zu parse cases identical\n", n);
}
