// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Parent-chain (L1) JSON-RPC client and the l1_rpc_configs.json file: port of coinshift-rs
// v0.14.8 lib/parent_chain_rpc.rs (Bitcoin Core-style JSON-RPC 1.0 over HTTP, basic auth, 10 s
// timeout). Node-local by their design (see doc/coinshift.md): it moves swaps' local state.
#include <coinshift/l1rpc.h>
#include <coinshift/privfile.h>

#include <bt/crypto.h>
#include <bt/log.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace coinshift::l1 {

std::string config_path()
{
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/coinshift/l1_rpc_configs.json";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/coinshift/l1_rpc_configs.json";
}

std::map<ParentChain, L1RpcConfig> read_configs(const std::string& path, bool* parsed)
{
    std::map<ParentChain, L1RpcConfig> r;
    if (parsed) *parsed = false;
    std::ifstream f(path);
    if (!f) return r;
    std::stringstream ss;
    ss << f.rdbuf();
    UniValue v;
    if (!v.read(ss.str()) || !v.isObject()) return r;
    for (auto& k : v.getKeys()) {
        auto pc = parent_chain_from_name(k);
        const UniValue& e = v[k];
        if (!pc || !e.isObject() || !e["url"].isStr() || !e["user"].isStr() || !e["password"].isStr()) return {};
        r[*pc] = L1RpcConfig{e["url"].get_str(), e["user"].get_str(), e["password"].get_str()};
    }
    if (parsed) *parsed = true;
    return r;
}

std::optional<L1RpcConfig> load_config(const std::string& path, ParentChain c)
{
    auto m = read_configs(path, nullptr);
    auto it = m.find(c);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<ParentChain, L1RpcConfig>> supported_configs()
{
    return {{ParentChain::Signet, {"http://localhost:38332", "user", "password"}},
            {ParentChain::BCH, {"http://173.230.135.236:28332", "user", "password"}}};
}

bool is_supported(ParentChain c, const L1RpcConfig& cfg)
{
    for (auto& [pc, s] : supported_configs())
        if (pc == c && s.url == cfg.url && s.user == cfg.user && s.password == cfg.password) return true;
    return false;
}

UniValue configs_json(const std::map<ParentChain, L1RpcConfig>& m)
{
    UniValue o(UniValue::VOBJ);
    for (auto& [pc, c] : m) {
        UniValue e(UniValue::VOBJ);
        e.pushKV("url", c.url);
        e.pushKV("user", c.user);
        e.pushKV("password", c.password);
        o.pushKV(parent_chain_name(pc), e);
    }
    return o;
}

bool write_config_file(const std::string& path, const std::vector<ParentChain>& enable)
{
    auto m = read_configs(path, nullptr);
    for (auto it = m.begin(); it != m.end();) it = is_supported(it->first, it->second) ? std::next(it) : m.erase(it);
    for (auto c : enable)
        for (auto& [pc, s] : supported_configs())
            if (pc == c) m[c] = s;
    std::string err; // owner-only: the file holds L1 RPC credentials
    return write_private_file(path, configs_json(m).write(2), err); // serde_json::to_string_pretty
}

// ---------------------------------------------------------------- HTTP
namespace {
struct Url {
    std::string host, path = "/";
    uint16_t port = 80;
};
std::optional<Url> parse_url(const std::string& s)
{
    if (s.rfind("http://", 0) != 0) return std::nullopt; // https: not supported by this client
    std::string rest = s.substr(7);
    Url u;
    auto slash = rest.find('/');
    std::string hp = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (slash != std::string::npos) u.path = rest.substr(slash);
    auto colon = hp.rfind(':');
    if (colon != std::string::npos && hp.find(']') == std::string::npos) {
        u.host = hp.substr(0, colon);
        u.port = uint16_t(std::stoi(hp.substr(colon + 1)));
    } else {
        u.host = hp;
    }
    if (u.host.size() > 2 && u.host.front() == '[') u.host = u.host.substr(1, u.host.size() - 2);
    return u;
}

std::string b64(const std::string& in)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = uint8_t(in[i]) << 16 | uint8_t(in[i + 1]) << 8 | uint8_t(in[i + 2]);
        o += t[v >> 18];
        o += t[(v >> 12) & 63];
        o += t[(v >> 6) & 63];
        o += t[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        o += t[v >> 18];
        o += t[(v >> 12) & 63];
        o += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = uint8_t(in[i]) << 16 | uint8_t(in[i + 1]) << 8;
        o += t[v >> 18];
        o += t[(v >> 12) & 63];
        o += t[(v >> 6) & 63];
        o += '=';
    }
    return o;
}

/** POST with a 10 s overall timeout; nullopt on transport errors. */
std::optional<std::string> http_post(const L1RpcConfig& cfg, const std::string& body, std::string& err)
{
    auto u = parse_url(cfg.url);
    if (!u) {
        err = "unsupported URL " + cfg.url;
        return std::nullopt;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u->host.c_str(), std::to_string(u->port).c_str(), &hints, &res) != 0 || !res) {
        err = "dns error";
        return std::nullopt;
    }
    int fd = -1;
    for (auto* a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK, 0);
        if (fd < 0) continue;
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc == 0 || errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 10000) == 1) {
                int e = 0;
                socklen_t l = sizeof e;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
                if (e == 0) break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        err = "error sending request";
        return std::nullopt;
    }
    std::string req = "POST " + u->path + " HTTP/1.1\r\nHost: " + u->host + ":" + std::to_string(u->port) +
                      "\r\ncontent-type: application/json\r\naccept: */*\r\nconnection: close\r\n";
    if (!cfg.user.empty()) req += "authorization: Basic " + b64(cfg.user + ":" + cfg.password) + "\r\n";
    req += "content-length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t off = 0;
    while (off < req.size()) {
        pollfd p{fd, POLLOUT, 0};
        int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (ms <= 0 || poll(&p, 1, ms) != 1) break;
        ssize_t n = send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += size_t(n);
    }
    std::string resp;
    char buf[8192];
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (ms <= 0 || poll(&p, 1, ms) != 1) break;
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        resp.append(buf, size_t(n));
    }
    close(fd);
    auto hdr_end = resp.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        err = "error decoding response body";
        return std::nullopt;
    }
    std::string headers = resp.substr(0, hdr_end), b = resp.substr(hdr_end + 4);
    std::string lower = headers;
    for (auto& c : lower) c = char(std::tolower(c));
    if (lower.find("transfer-encoding: chunked") != std::string::npos) { // de-chunk
        std::string out;
        size_t p = 0;
        while (p < b.size()) {
            auto e = b.find("\r\n", p);
            if (e == std::string::npos) break;
            size_t n = std::stoul(b.substr(p, e - p), nullptr, 16);
            if (n == 0) break;
            out += b.substr(e + 2, n);
            p = e + 2 + n + 2;
        }
        b = out;
    }
    return b;
}
} // namespace

std::optional<UniValue> Client::call(const std::string& method, const UniValue& params, std::string& err) const
{
    UniValue req(UniValue::VOBJ);
    req.pushKV("jsonrpc", "1.0");
    req.pushKV("id", "coinshift");
    req.pushKV("method", method);
    req.pushKV("params", params);
    auto body = http_post(cfg, req.write(), err);
    if (!body) return std::nullopt;
    UniValue v;
    if (!v.read(*body) || !v.isObject()) {
        err = "JSON parsing error";
        return std::nullopt;
    }
    if (v.exists("error") && v["error"].isObject()) {
        err = "RPC error: " + std::to_string(v["error"]["code"].getInt<int64_t>()) + ": " + v["error"]["message"].get_str();
        return std::nullopt;
    }
    if (!v.exists("result") || v["result"].isNull()) {
        err = "Invalid response format";
        return std::nullopt;
    }
    return v["result"];
}

namespace {
std::optional<uint64_t> to_sats(double v)
{
    if (!std::isfinite(v) || v < 0) return std::nullopt;
    double s = std::round(v * 100000000.0);
    if (s < 0 || s > 18446744073709551615.0) return std::nullopt;
    return uint64_t(s);
}
std::optional<std::string> vout_address(const UniValue& spk, bool* matches = nullptr, const std::string& want = "")
{
    std::optional<std::string> a;
    if (spk["address"].isStr()) a = spk["address"].get_str();
    if (matches) {
        *matches = a && *a == want;
        if (!*matches && spk["addresses"].isArray())
            for (auto& x : spk["addresses"].getValues())
                if (x.isStr() && x.get_str() == want) *matches = true;
    }
    if (!a && spk["addresses"].isArray() && !spk["addresses"].empty() && spk["addresses"][0].isStr()) a = spk["addresses"][0].get_str();
    return a;
}
} // namespace

std::optional<UniValue> Client::get_transaction(const std::string& txid, std::string& err) const
{
    UniValue p(UniValue::VARR);
    p.push_back(txid);
    p.push_back(true);
    auto r = call("getrawtransaction", p, err);
    if (!r || !r->isObject() || !(*r)["txid"].isStr() || !(*r)["confirmations"].isNum() || !(*r)["vout"].isArray() || !(*r)["vin"].isArray()) {
        if (r) err = "JSON parsing error";
        return std::nullopt;
    }
    return r;
}

std::optional<std::vector<L1Match>> Client::find(const std::string& address, uint64_t amount, std::string& err) const
{
    UniValue lp(UniValue::VARR);
    lp.push_back(0);
    lp.push_back(999999);
    UniValue addrs(UniValue::VARR);
    addrs.push_back(address);
    lp.push_back(addrs);
    auto unspent = call("listunspent", lp, err);
    if (!unspent || !unspent->isArray()) return std::nullopt;
    std::set<std::string> txids;
    for (auto& u : unspent->getValues())
        if (u["txid"].isStr()) txids.insert(u["txid"].get_str());
    {
        UniValue gp(UniValue::VARR);
        gp.push_back(address);
        gp.push_back(0);
        std::string ignored;
        call("getreceivedbyaddress", gp, ignored);
    }
    if (!call("getblockchaininfo", UniValue(UniValue::VARR), err)) return std::nullopt;
    std::vector<L1Match> r;
    for (auto& txid : txids) { // HashSet iteration order in theirs: unspecified
        std::string terr;
        auto tx = get_transaction(txid, terr);
        if (!tx) continue;
        for (auto& vout : (*tx)["vout"].getValues()) {
            auto sats = vout["value"].isNum() ? to_sats(vout["value"].get_real()) : std::nullopt;
            if (!sats) continue;
            bool match = false;
            vout_address(vout["scriptPubKey"], &match, address);
            if (match && *sats == amount) {
                std::string sender = "unknown";
                auto& vin = (*tx)["vin"];
                if (!vin.empty() && vin[0]["txid"].isStr() && vin[0]["vout"].isNum()) {
                    std::string serr;
                    if (auto in = get_transaction(vin[0]["txid"].get_str(), serr)) {
                        size_t i = size_t(vin[0]["vout"].getInt<int64_t>());
                        if (i < (*in)["vout"].size())
                            if (auto a = vout_address((*in)["vout"][i]["scriptPubKey"])) sender = *a;
                    }
                }
                L1Match m;
                m.sender = sender;
                m.txid = (*tx)["txid"].get_str();
                m.confirmations = uint32_t((*tx)["confirmations"].getInt<int64_t>());
                if ((*tx)["blockheight"].isNum()) m.blockheight = uint32_t((*tx)["blockheight"].getInt<int64_t>());
                r.push_back(m);
                break;
            }
        }
    }
    return r;
}

std::optional<std::string> Client::chain_name(std::string& err) const
{
    auto r = call("getblockchaininfo", UniValue(UniValue::VARR), err);
    if (!r || !(*r)["chain"].isStr()) {
        if (r) err = "Invalid response format";
        return std::nullopt;
    }
    std::string c = (*r)["chain"].get_str();
    for (auto& ch : c) ch = char(std::tolower(ch));
    return c;
}

std::string validate_config_file(const std::string& path)
{
    bool parsed = false;
    auto m = read_configs(path, &parsed);
    if (!parsed) return {}; // no file / invalid JSON: nothing to validate
    for (auto& [pc, cfg] : m) {
        if (!is_supported(pc, cfg)) return "L1 config is not supported: only predefined networks are allowed";
        std::string err;
        auto c = Client{cfg}.chain_name(err);
        if (!c) return err;
        std::optional<ParentChain> detected;
        if (*c == "signet") detected = ParentChain::Signet;
        else if (*c == "testnet4" || *c == "test4") detected = ParentChain::BCH;
        if (!detected) return "Node chain mismatch: expected Signet, node reported chain \"" + *c + "\"";
        if (*detected != pc) return std::string("Node chain mismatch: expected ") + parent_chain_name(pc) + ", node reported chain \"" + *c + "\"";
    }
    return {};
}

} // namespace coinshift::l1
