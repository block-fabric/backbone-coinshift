// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// `coinshift-cli`: the subcommands, flags and output of coinshift-rs v0.14.8's
// coinshift_app_cli (cli/lib.rs) over the node's JSON-RPC. Results print as their
// `println!("{res}")`: JSON like serde_json::to_string_pretty, scalars like Rust's Display, the
// swap commands' sentences, and an empty line for unit results. get-l1-config / set-l1-config
// act on <data_dir>/coinshift/l1_rpc_configs.json locally, as theirs.
#include <coinshift/net.h>
#include <coinshift/privfile.h>
#include <coinshift/types.h>

#include <bt/crypto.h>
#include <bt/jsonrpc.h>
#include <bt/parse.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using coinshift::ParentChain;

enum class Kind { Str, Json, SwapId, ParentChainArg };
enum class Out { Pretty, Display, Empty, Custom };

struct Param {
    std::string name;          // RPC parameter name (positional order = vector order)
    std::string flag;          // "--flag", or "" for a positional argument
    Kind kind = Kind::Str;
    bool required = true;
};

struct Spec {
    std::string rpc;
    std::vector<Param> params;
    Out out = Out::Pretty;
};

Param pos(const std::string& n, Kind k = Kind::Str) { return Param{n, "", k, true}; }
Param flag(const std::string& f, const std::string& n, Kind k = Kind::Str, bool req = true) { return Param{n, f, k, req}; }

std::map<std::string, Spec> specs()
{
    std::map<std::string, Spec> m;
    auto J = Kind::Json;
    m["balance"] = {"balance", {}, Out::Pretty};
    m["connect-block"] = {"connect_block", {pos("block", J), pos("main_block_hash")}, Out::Display};
    m["connect-peer"] = {"connect_peer", {pos("addr")}, Out::Empty};
    m["create-swap"] = {"create_swap",
                        {flag("--parent-chain", "parent_chain", Kind::ParentChainArg), flag("--l1-recipient-address", "l1_recipient_address"),
                         flag("--l1-amount-sats", "l1_amount_sats", J), flag("--l2-recipient", "l2_recipient", Kind::Str, false),
                         flag("--l2-amount-sats", "l2_amount_sats", J), flag("--required-confirmations", "required_confirmations", J, false),
                         flag("--fee-sats", "fee_sats", J)},
                        Out::Custom};
    m["create-deposit"] = {"create_deposit", {pos("address"), flag("--value-sats", "value_sats", J), flag("--fee-sats", "fee_sats", J)}, Out::Display};
    m["format-deposit-address"] = {"format_deposit_address", {pos("address")}, Out::Display};
    m["forget-peer"] = {"forget_peer", {pos("addr")}, Out::Empty};
    m["generate-mnemonic"] = {"generate_mnemonic", {}, Out::Display};
    m["get-best-mainchain-block-hash"] = {"get_best_mainchain_block_hash", {}, Out::Pretty};
    m["get-best-sidechain-block-hash"] = {"get_best_sidechain_block_hash", {}, Out::Pretty};
    m["get-block"] = {"get_block", {pos("block_hash")}, Out::Pretty};
    m["get-block-hash"] = {"get_block_hash", {pos("height", J)}, Out::Pretty};
    m["get-block-index"] = {"get_block_index", {pos("block_hash")}, Out::Pretty};
    m["get-block-template"] = {"get_block_template", {}, Out::Pretty};
    m["get-bmm-inclusions"] = {"get_bmm_inclusions", {pos("block_hash")}, Out::Pretty};
    m["get-new-address"] = {"get_new_address", {}, Out::Display};
    m["get-wallet-addresses"] = {"get_wallet_addresses", {}, Out::Pretty};
    m["get-wallet-utxos"] = {"get_wallet_utxos", {}, Out::Pretty};
    m["get-blockcount"] = {"getblockcount", {}, Out::Display};
    m["invalidate-block"] = {"invalidate_block", {pos("block_hash")}, Out::Empty};
    m["accept-swap"] = {"accept_swap",
                        {flag("--swap-id", "swap_id", Kind::SwapId), flag("--l2-claimer-address", "l2_claimer_address", Kind::Str, false),
                         flag("--fee-sats", "fee_sats", J, false)},
                        Out::Custom};
    m["claim-swap"] = {"claim_swap", {flag("--swap-id", "swap_id", Kind::SwapId), flag("--l2-claimer-address", "l2_claimer_address", Kind::Str, false)},
                       Out::Custom};
    m["get-swap-status"] = {"get_swap_status", {flag("--swap-id", "swap_id", Kind::SwapId)}, Out::Pretty};
    m["latest-failed-withdrawal-bundle-height"] = {"latest_failed_withdrawal_bundle_height", {}, Out::Pretty};
    m["list-mempool"] = {"list_mempool", {}, Out::Pretty};
    m["list-peers"] = {"list_peers", {}, Out::Pretty};
    m["list-utxos"] = {"list_utxos", {}, Out::Pretty};
    m["list-swaps"] = {"list_swaps", {}, Out::Pretty};
    m["list-swaps-by-recipient"] = {"list_swaps_by_recipient", {pos("recipient")}, Out::Pretty};
    m["recover-from-mnemonic"] = {"set_seed_from_mnemonic", {pos("mnemonic")}, Out::Custom};
    m["reconstruct-swaps"] = {"reconstruct_swaps", {}, Out::Custom};
    m["cancel-swap"] = {"cancel_swap", {pos("swap_id", Kind::SwapId)}, Out::Custom};
    m["mainchain-sync-progress"] = {"mainchain_sync_progress", {}, Out::Pretty};
    m["mine"] = {"mine", {flag("--fee-sats", "fee", J, false)}, Out::Empty};
    m["pending-withdrawal-bundle"] = {"pending_withdrawal_bundle", {}, Out::Pretty};
    m["openapi-schema"] = {"openapi_schema", {}, Out::Pretty};
    m["remove-from-mempool"] = {"remove_from_mempool", {pos("txid")}, Out::Empty};
    m["set-seed-from-mnemonic"] = {"set_seed_from_mnemonic", {pos("mnemonic")}, Out::Empty};
    m["sidechain-wealth"] = {"sidechain_wealth", {}, Out::Display};
    m["stop"] = {"stop", {}, Out::Empty};
    m["transfer"] = {"transfer", {pos("dest"), flag("--value-sats", "value_sats", J), flag("--fee-sats", "fee_sats", J)}, Out::Display};
    m["transfer-many"] = {"transfer_many", {pos("dests", J), flag("--fee-sats", "fee_sats", J)}, Out::Display};
    m["update-swap-l1-txid"] = {"update_swap_l1_txid",
                                {flag("--swap-id", "swap_id", Kind::SwapId), flag("--l1-txid-hex", "l1_txid_hex"), flag("--confirmations", "confirmations", J),
                                 flag("--l2-claimer-address", "l2_claimer_address", Kind::Str, false)},
                                Out::Custom};
    m["withdraw"] = {"withdraw",
                     {pos("mainchain_address"), flag("--amount-sats", "amount_sats", J), flag("--fee-sats", "fee_sats", J),
                      flag("--mainchain-fee-sats", "mainchain_fee_sats", J)},
                     Out::Display};
    return m;
}

void json_str(std::string& o, const std::string& s)
{
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    o += '"';
}

// serde_json::to_string_pretty: two-space indent, "key": value, empty containers inline.
void pretty(std::string& o, const UniValue& v, int indent)
{
    auto pad = [&](int n) { o.append(size_t(n) * 2, ' '); };
    if (v.isObject()) {
        if (v.empty()) {
            o += "{}";
            return;
        }
        o += "{\n";
        for (size_t i = 0; i < v.size(); i++) {
            pad(indent + 1);
            json_str(o, v.getKeys()[i]);
            o += ": ";
            pretty(o, v.getValues()[i], indent + 1);
            o += i + 1 < v.size() ? ",\n" : "\n";
        }
        pad(indent);
        o += "}";
    } else if (v.isArray()) {
        if (v.empty()) {
            o += "[]";
            return;
        }
        o += "[\n";
        for (size_t i = 0; i < v.size(); i++) {
            pad(indent + 1);
            pretty(o, v[i], indent + 1);
            o += i + 1 < v.size() ? ",\n" : "\n";
        }
        pad(indent);
        o += "]";
    } else if (v.isStr()) {
        json_str(o, v.get_str());
    } else {
        o += v.write();
    }
}

std::string pretty(const UniValue& v)
{
    std::string o;
    pretty(o, v, 0);
    return o;
}

bool parse_url(const std::string& url, std::string& host, uint16_t& port)
{
    std::string r = url;
    if (r.rfind("http://", 0) == 0) r = r.substr(7);
    else if (r.find("://") != std::string::npos) return false;
    auto slash = r.find('/');
    if (slash != std::string::npos) r = r.substr(0, slash);
    auto hp = bt::split_hostport(r, uint16_t(80));
    if (!hp) return false;
    host = hp->first;
    port = hp->second;
    return true;
}

int fail(const std::string& m)
{
    std::fprintf(stderr, "Error: %s\n", m.c_str());
    return 1;
}

int usage_error(const std::string& m)
{
    std::fprintf(stderr, "error: %s\n\nFor more information, try '--help'.\n", m.c_str());
    return 2;
}

UniValue rpc(const std::string& host, uint16_t port, const std::string& method, const UniValue& params, std::string& err)
{
    UniValue resp = bt::jsonrpc::call(host, port, method, params, err);
    if (!err.empty()) return UniValue();
    if (!resp["error"].isNull()) {
        err = resp["error"]["message"].getValStr();
        return UniValue();
    }
    return resp["result"];
}

// cli/lib.rs parse_swap_id: hex, 32 bytes
std::optional<UniValue> swap_id_arg(const std::string& s, std::string& err)
{
    // hex::decode error texts: invalid characters first, then the odd length
    for (size_t i = 0; i < s.size(); i++)
        if (!std::isxdigit((unsigned char)s[i])) {
            err = std::string("invalid swap_id hex: Invalid character '") + s[i] + "' at position " + std::to_string(i);
            return std::nullopt;
        }
    auto b = bt::hex_decode(s);
    if (!b) {
        err = "invalid swap_id hex: Odd number of digits";
        return std::nullopt;
    }
    if (b->size() != 32) {
        err = "swap_id must be 32 bytes (64 hex chars)";
        return std::nullopt;
    }
    UniValue a(UniValue::VARR);
    for (auto x : *b) a.push_back(int(x));
    return a;
}

// cli/lib.rs parse_parent_chain (case-insensitive)
std::optional<ParentChain> parent_chain_arg(std::string s)
{
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    if (s == "btc") return ParentChain::BTC;
    if (s == "bch") return ParentChain::BCH;
    if (s == "ltc") return ParentChain::LTC;
    if (s == "signet") return ParentChain::Signet;
    if (s == "regtest") return ParentChain::Regtest;
    return std::nullopt;
}

std::string data_dir()
{
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return x;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share";
}
std::string l1_config_path() { return data_dir() + "/coinshift/l1_rpc_configs.json"; }

std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

// HashMap<ParentChainType, RpcConfig> from the file; serde_json errors -> empty (unwrap_or_default)
UniValue read_l1_configs(const std::string& path)
{
    UniValue v(UniValue::VOBJ), r(UniValue::VOBJ);
    if (!v.read(slurp(path)) || !v.isObject()) return r;
    for (size_t i = 0; i < v.size(); i++) {
        auto& k = v.getKeys()[i];
        auto& c = v.getValues()[i];
        if (!coinshift::parent_chain_from_name(k) || !c.isObject() || !c["url"].isStr() || !c["user"].isStr() || !c["password"].isStr())
            return UniValue(UniValue::VOBJ);
        UniValue x(UniValue::VOBJ);
        x.pushKV("url", c["url"]);
        x.pushKV("user", c["user"]);
        x.pushKV("password", c["password"]);
        r.pushKV(k, x);
    }
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    std::string url = "http://localhost:" + std::to_string(coinshift::DEFAULT_RPC_PORT);
    int i = 1;
    for (; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--rpc-url" && i + 1 < argc) url = argv[++i];
        else if (a.rfind("--rpc-url=", 0) == 0) url = a.substr(10);
        else if ((a == "--timeout" || a == "--log-level") && i + 1 < argc) ++i;
        else if (a.rfind("--timeout=", 0) == 0 || a.rfind("--log-level=", 0) == 0) {
        } else if (a == "-v" || a == "--verbose") {
        } else break;
    }
    auto all = specs();
    if (i >= argc || std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h" || std::string(argv[i]) == "help") {
        std::printf("Usage: coinshift-cli [OPTIONS] <COMMAND>\n\nCommands:\n");
        for (auto& [k, _] : all) std::printf("  %s\n", k.c_str());
        std::printf("  get-l1-config\n  set-l1-config\n\nOptions:\n      --rpc-url <RPC_URL>      [default: http://localhost:6255]\n"
                    "      --timeout <TIMEOUT>      Timeout for RPC requests in seconds (default: 300)\n"
                    "  -v, --verbose                Enable verbose HTTP output\n      --log-level <LOG_LEVEL>  [default: INFO]\n");
        return i >= argc ? 2 : 0;
    }
    if (std::string(argv[i]) == "-V" || std::string(argv[i]) == "--version") {
        std::printf("coinshift_app_cli %s\n", coinshift::VERSION);
        return 0;
    }
    std::string cmd = argv[i++];

    // local L1 config commands (no RPC)
    if (cmd == "get-l1-config" || cmd == "set-l1-config") {
        std::map<std::string, std::string> f;
        for (; i < argc; i++) {
            std::string a = argv[i];
            auto eq = a.find('=');
            std::string k = eq == std::string::npos ? a : a.substr(0, eq);
            std::string v;
            if (eq != std::string::npos) v = a.substr(eq + 1);
            else if (i + 1 < argc) v = argv[++i];
            else return usage_error("a value is required for '" + k + "' but none was supplied");
            f[k] = v;
        }
        std::string path = l1_config_path();
        if (cmd == "get-l1-config") {
            UniValue cfg = read_l1_configs(path);
            UniValue out(UniValue::VOBJ);
            if (f.count("--chain")) {
                auto pc = parent_chain_arg(f["--chain"]);
                if (!pc) return usage_error("invalid value '" + f["--chain"] + "' for '--chain <CHAIN>': unknown parent_chain '" + f["--chain"] +
                                            "', use: btc, bch, ltc, signet, regtest");
                if (cfg.exists(coinshift::parent_chain_name(*pc))) out.pushKV(coinshift::parent_chain_name(*pc), cfg[coinshift::parent_chain_name(*pc)]);
            } else {
                out = cfg;
            }
            std::printf("%s\n", pretty(out).c_str());
            return 0;
        }
        if (!f.count("--parent-chain") || !f.count("--url"))
            return usage_error("the following required arguments were not provided:\n  --parent-chain <PARENT_CHAIN>\n  --url <URL>");
        auto pc = parent_chain_arg(f["--parent-chain"]);
        if (!pc) return usage_error("invalid value '" + f["--parent-chain"] + "' for '--parent-chain <PARENT_CHAIN>': unknown parent_chain '" +
                                    f["--parent-chain"] + "', use: btc, bch, ltc, signet, regtest");
        UniValue cfg = std::filesystem::exists(path) ? read_l1_configs(path) : UniValue(UniValue::VOBJ);
        UniValue x(UniValue::VOBJ);
        x.pushKV("url", f["--url"]);
        x.pushKV("user", f.count("--user") ? f["--user"] : "");
        x.pushKV("password", f.count("--password") ? f["--password"] : "");
        UniValue nc(UniValue::VOBJ);
        bool done = false;
        for (size_t k = 0; k < cfg.size(); k++) {
            if (cfg.getKeys()[k] == coinshift::parent_chain_name(*pc)) {
                nc.pushKV(cfg.getKeys()[k], x);
                done = true;
            } else {
                nc.pushKV(cfg.getKeys()[k], cfg.getValues()[k]);
            }
        }
        if (!done) nc.pushKV(coinshift::parent_chain_name(*pc), x);
        std::string werr; // owner-only (the file holds L1 RPC credentials)
        if (!coinshift::write_private_file(path, pretty(nc), werr)) return fail("write config: " + path + ": " + werr);
        std::printf("L1 RPC config saved for %s at %s\n", coinshift::coin_name(*pc), path.c_str());
        return 0;
    }

    auto it = all.find(cmd);
    if (it == all.end()) return usage_error("unrecognized subcommand '" + cmd + "'");
    const Spec& spec = it->second;
    std::map<std::string, UniValue> given;
    size_t npos = 0;
    std::vector<const Param*> positional;
    for (auto& p : spec.params)
        if (p.flag.empty()) positional.push_back(&p);
    auto convert = [&](const Param& p, const std::string& value, UniValue& out) -> int {
        std::string e;
        switch (p.kind) {
        case Kind::Str: out = UniValue(value); return 0;
        case Kind::Json:
            if (!out.read(value)) return usage_error("invalid value '" + value + "' for '<" + p.name + ">'");
            return 0;
        case Kind::SwapId: {
            auto a = swap_id_arg(value, e);
            if (!a) return usage_error("invalid value '" + value + "' for '--swap-id <SWAP_ID>': " + e);
            out = *a;
            return 0;
        }
        case Kind::ParentChainArg: {
            auto pc = parent_chain_arg(value);
            if (!pc) return usage_error("invalid value '" + value + "' for '--parent-chain <PARENT_CHAIN>': unknown parent_chain '" + value +
                                        "', use: btc, bch, ltc, signet, regtest");
            out = UniValue(coinshift::parent_chain_name(*pc));
            return 0;
        }
        }
        return 0;
    };
    for (; i < argc; i++) {
        std::string a = argv[i];
        const Param* p = nullptr;
        std::string value;
        if (a.rfind("--", 0) == 0) {
            auto eq = a.find('=');
            std::string k = eq == std::string::npos ? a : a.substr(0, eq);
            for (auto& q : spec.params)
                if (q.flag == k) p = &q;
            if (!p) return usage_error("unexpected argument '" + k + "' found");
            if (eq != std::string::npos) value = a.substr(eq + 1);
            else if (i + 1 < argc) value = argv[++i];
            else return usage_error("a value is required for '" + k + "' but none was supplied");
        } else {
            if (npos >= positional.size()) return usage_error("unexpected argument '" + a + "' found");
            p = positional[npos++];
            value = a;
        }
        UniValue v;
        if (int rc = convert(*p, value, v)) return rc;
        given[p->name] = v;
    }
    std::string missing;
    for (auto& p : spec.params)
        if (p.required && !given.count(p.name)) missing += "\n  " + (p.flag.empty() ? "<" + p.name + ">" : p.flag + " <" + p.name + ">");
    if (!missing.empty()) return usage_error("the following required arguments were not provided:" + missing);
    // jsonrpsee's proc-macro client sends positional (array) params, None as null
    UniValue params(UniValue::VARR);
    for (auto& p : spec.params) params.push_back(given.count(p.name) ? given[p.name] : UniValue());

    std::string host;
    uint16_t port;
    if (!parse_url(url, host, port)) return fail("invalid --rpc-url");
    std::string err;
    UniValue r = rpc(host, port, spec.rpc, params, err);
    if (!err.empty()) return fail(err);
    switch (spec.out) {
    case Out::Pretty: std::printf("%s\n", pretty(r).c_str()); break;
    case Out::Display: std::printf("%s\n", r.isStr() ? r.get_str().c_str() : r.write().c_str()); break;
    case Out::Empty: std::printf("\n"); break;
    case Out::Custom:
        if (cmd == "create-swap") {
            std::string id;
            for (auto& b : r[0].getValues()) {
                char x[3];
                std::snprintf(x, sizeof x, "%02x", int(b.getInt<int>()));
                id += x;
            }
            std::printf("Swap created: id=%s txid=%s\n", id.c_str(), r[1].get_str().c_str());
        } else if (cmd == "accept-swap") {
            std::printf("Swap reserved: txid=%s\n", r.get_str().c_str());
        } else if (cmd == "claim-swap") {
            std::printf("Swap claimed: txid=%s\n", r.get_str().c_str());
        } else if (cmd == "reconstruct-swaps") {
            std::printf("Reconstructed %s swaps from blockchain\n", r.write().c_str());
        } else if (cmd == "cancel-swap") {
            std::printf("Swap cancelled\n");
        } else if (cmd == "update-swap-l1-txid") {
            std::printf("Swap L1 txid updated\n");
        } else if (cmd == "recover-from-mnemonic") {
            UniValue addrs = rpc(host, port, "get_wallet_addresses", UniValue(UniValue::VARR), err);
            if (!err.empty()) return fail(err);
            UniValue bal = rpc(host, port, "balance", UniValue(UniValue::VARR), err);
            if (!err.empty()) return fail(err);
            std::printf("Recovery complete.\nAddresses:\n%s\nBalance: total %s sats, available %s sats\n", pretty(addrs).c_str(),
                        bal["total_sats"].write().c_str(), bal["available_sats"].write().c_str());
        }
        break;
    }
    return 0;
}
