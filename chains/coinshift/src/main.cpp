// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// `coinshift`: drop-in replacement for coinshift-rs v0.14.8's `coinshift_app` (shipped as
// `coinshift`) on eCash slot 255. Same command-line flags (app/cli.rs), defaults (P2P
// 0.0.0.0:4255, RPC 127.0.0.1:6255, datadir <data_dir>/coinshift, logs <datadir>/logs/v0.14.8,
// mainchain http://localhost:50051, L1 config <data_dir>/coinshift/l1_rpc_configs.json) and
// JSON-RPC methods. No GUI: without --headless it runs headless. Our own options use the --bt-
// prefix; unknown flags are reported and ignored.
#include <coinshift/app.h>
#include <coinshift/appui.h>

#include <bt/frost.h>
#include <bt/fsperm.h>
#include <bt/jsonrpc.h>
#include <bt/log.h>
#include <bt/parse.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace coinshift;

static const char* OUR_VERSION = "backbone-coinshift 0.1.0 (coinshift 0.14.8 compatible)";

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

static std::string data_dir()
{
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return x;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share";
}

static bool parse_sockaddr(const std::string& s, bt::quic::SockAddr& out)
{
    auto hp = bt::split_hostport(s, std::nullopt, true);
    if (!hp) return false;
    out = {hp->first, hp->second};
    return true;
}

static void usage()
{
    std::printf(
        "Usage: coinshift [OPTIONS] [COMMAND]\n\n"
        "Commands:\n"
        "  init  Write L1 RPC config (Bitcoin Signet and/or Bitcoin Cash Testnet4) and exit\n\n"
        "Options (coinshift 0.14.8 compatible):\n"
        "      --add-peer <ADD_PEERS>                 Peer to dial at startup, as host:port or host\n"
        "  -d, --datadir <DATADIR>                    [default: %s/coinshift]\n"
        "      --headless                             Accepted; there is no GUI\n"
        "      --log-dir <LOG_DIR>                    [default: <DATADIR>/logs/v%s]; empty disables file logs\n"
        "      --log-level-file <LOG_LEVEL_FILE>      [default: WARN]\n"
        "      --log-level <LOG_LEVEL>                [default: DEBUG]\n"
        "      --mainchain-grpc-url <URL>             [default: http://localhost:50051]\n"
        "      --mnemonic-seed-phrase-path <PATH>     Path to a mnemonic seed phrase\n"
        "  -n, --net-addr <NET_ADDR>                  [default: 0.0.0.0:4255]\n"
        "      --network <NETWORK>                    betanet | regtest | signet [default: betanet]\n"
        "      --network-magic <NETWORK_MAGIC>        4 bytes as hex\n"
        "  -r, --rpc-addr <RPC_ADDR>                  [default: 127.0.0.1:6255]\n"
        "      --server-name <SERVER_NAMES>           Host name for the P2P certificate (repeatable)\n"
        "      --l1-signet                            Enable Bitcoin Signet in the L1 config before start\n"
        "      --l1-bch-testnet4                      Enable Bitcoin Cash Testnet4 in the L1 config before start\n"
        "  -h, --help / -V, --version\n\n"
        "Backbone options:\n"
        "      --bt-rpc-allowed-origin <ORIGIN>       Allow browser requests from this Origin (repeatable; \"*\" = any)\n"
        "      --bt-rpc-allowed-host <HOST>           Extra Host header name accepted by the RPC server\n"
        "      --bt-rpc-allow-external                Allow binding the unauthenticated RPC to a non-loopback address\n"
        "      --bt-no-seeds                          Do not dial the built-in seed peers\n"
        "      --bt-rebuild-side-tips                 Rebuild the side-tips index from the mainchain at start\n"
        "      --bt-no-p2p                            Disable P2P networking\n",
        data_dir().c_str(), VERSION);
    std::printf("\n%s", bt::appui::usage().c_str());
}

int main(int argc, char** argv)
{
    std::signal(SIGPIPE, SIG_IGN); // a stopped enforcer or L1 node must not kill the node
    bt::sidechain::NodeConfig cfg;
    cfg.net_addr = {"0.0.0.0", DEFAULT_PORT};
    cfg.datadir = data_dir() + "/coinshift";
    cfg.mainchain_grpc_url = "http://localhost:50051";
    std::optional<std::string> log_dir, mnemonic_path;
    bt::log::Level log_level = bt::log::Level::Debug, file_log_level = bt::log::Level::Warn;
    bt::quic::SockAddr rpc_addr{"127.0.0.1", DEFAULT_RPC_PORT};
    bool headless = false, init = false, l1_signet = false, l1_bch = false;
    std::vector<std::string> unknown;
    bt::jsonrpc::Options rpc_opts;
    bt::appui::AppOptions app_opts;
    for (int i = 1; i < argc; i++) {
        // Our app layer's options (--bt-app-*): beside the LayerTwo ones, never instead of one.
        {
            std::string app_err;
            if (bt::appui::take_option(app_opts, argc, argv, i, app_err)) {
                if (!app_err.empty()) {
                    std::fprintf(stderr, "error: %s\n", app_err.c_str());
                    return 2;
                }
                continue;
            }
        }
        std::string a = argv[i], val;
        bool has_eq = false;
        if (a.rfind("--", 0) == 0) {
            auto eq = a.find('=');
            if (eq != std::string::npos) {
                val = a.substr(eq + 1);
                a = a.substr(0, eq);
                has_eq = true;
            }
        } else if (a.size() > 2 && a[0] == '-' && a[1] != '-') {
            val = a.substr(2);
            a = a.substr(0, 2);
            has_eq = true;
        }
        auto next = [&](std::string& out) -> bool {
            if (has_eq) {
                out = val;
                return true;
            }
            if (i + 1 < argc) {
                out = argv[++i];
                return true;
            }
            std::fprintf(stderr, "error: a value is required for '%s'\n", a.c_str());
            return false;
        };
        std::string v;
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "-V" || a == "--version") {
            std::printf("coinshift_app %s\n%s\n", VERSION, OUR_VERSION);
            return 0;
        } else if (a == "init") {
            init = true;
        } else if (a == "--add-peer") {
            if (!next(v)) return 2;
            if (!bt::split_hostport(v, DEFAULT_PORT)) {
                std::fprintf(stderr, "error: invalid value '%s' for '--add-peer <ADD_PEERS>': cannot parse the seed address\n", v.c_str());
                return 2;
            }
            cfg.add_peers.push_back(v);
        } else if (a == "-d" || a == "--datadir") {
            if (!next(v)) return 2;
            cfg.datadir = v;
        } else if (a == "--headless") {
            headless = true;
        } else if (a == "--log-dir") {
            if (!next(v)) return 2;
            log_dir = v;
        } else if (a == "--log-level" || a == "--log-level-file") {
            if (!next(v)) return 2;
            auto l = bt::log::parse_level(v);
            if (!l) {
                std::fprintf(stderr, "error: invalid value '%s' for '%s'\n", v.c_str(), a.c_str());
                return 2;
            }
            (a == "--log-level" ? log_level : file_log_level) = *l;
        } else if (a == "--mainchain-grpc-url") {
            if (!next(v)) return 2;
            cfg.mainchain_grpc_url = v;
        } else if (a == "--mnemonic-seed-phrase-path") {
            if (!next(v)) return 2;
            mnemonic_path = v;
        } else if (a == "-n" || a == "--net-addr") {
            if (!next(v) || !parse_sockaddr(v, cfg.net_addr)) {
                std::fprintf(stderr, "error: invalid value for '--net-addr <NET_ADDR>'\n");
                return 2;
            }
        } else if (a == "--network") {
            if (!next(v)) return 2;
            if (v == "betanet") cfg.network = Network::Betanet;
            else if (v == "regtest") cfg.network = Network::Regtest;
            else if (v == "signet") cfg.network = Network::Signet;
            else {
                std::fprintf(stderr, "error: invalid value '%s' for '--network <NETWORK>'\n  [possible values: betanet, regtest, signet]\n", v.c_str());
                return 2;
            }
        } else if (a == "--network-magic") {
            if (!next(v)) return 2;
            auto m = bt::hex_decode(v);
            if (!m || m->size() != 4) {
                std::fprintf(stderr, "error: invalid value '%s' for '--network-magic <NETWORK_MAGIC>'\n", v.c_str());
                return 2;
            }
            cfg.magic_override = std::array<uint8_t, 4>{(*m)[0], (*m)[1], (*m)[2], (*m)[3]};
        } else if (a == "-r" || a == "--rpc-addr") {
            if (!next(v) || !parse_sockaddr(v, rpc_addr)) {
                std::fprintf(stderr, "error: invalid value for '--rpc-addr <RPC_ADDR>'\n");
                return 2;
            }
        } else if (a == "--server-name") {
            if (!next(v)) return 2;
            cfg.server_names.push_back(v);
        } else if (a == "--l1-signet") {
            l1_signet = true;
        } else if (a == "--l1-bch-testnet4") {
            l1_bch = true;
        } else if (a == "--bt-rpc-allowed-origin") {
            if (!next(v)) return 2;
            rpc_opts.allowed_origins.push_back(v);
        } else if (a == "--bt-rpc-allowed-host") {
            if (!next(v)) return 2;
            rpc_opts.allowed_hosts.push_back(v);
        } else if (a == "--bt-rpc-allow-external") {
            rpc_opts.allow_external = true;
        } else if (a == "--bt-no-seeds") {
            cfg.use_seeds = false;
        } else if (a == "--bt-rebuild-side-tips") {
            cfg.rebuild_side_tips = true;
        } else if (a == "--bt-no-p2p") {
            cfg.p2p = false;
        } else {
            unknown.push_back(argv[i]);
        }
    }
    std::string l1_path = l1::config_path();
    if (init || l1_signet || l1_bch) {
        std::vector<ParentChain> chains;
        if (l1_signet) chains.push_back(ParentChain::Signet);
        if (l1_bch) chains.push_back(ParentChain::BCH);
        if (!l1::write_config_file(l1_path, chains)) {
            std::fprintf(stderr, "Error: cannot write %s\n", l1_path.c_str());
            return 1;
        }
        if (init) {
            std::printf("L1 config written to %s (Signet: %s, BCH Testnet4: %s)\n", l1_path.c_str(), l1_signet ? "true" : "false", l1_bch ? "true" : "false");
            return 0;
        }
    }
    // cli.rs get_config: without --headless the console level drops one step (GUI default)
    if (!headless && log_level != bt::log::Level::Error) log_level = bt::log::Level(int(log_level) - 1);
    std::string logs = log_dir ? *log_dir : cfg.datadir + "/logs/v" + VERSION;
    bt::log::set_console_level(log_level);
    if (!logs.empty()) bt::log::set_file(logs, file_log_level);
    for (auto& u : unknown) LOG_WARN("ignoring unknown argument '%s'", u.c_str());
    if (!headless) LOG_INFO("no GUI in this build; running headless");
    LOG_INFO("%s starting: datadir %s, mainchain %s", OUR_VERSION, cfg.datadir.c_str(), cfg.mainchain_grpc_url.c_str());

    // app.rs App::new: validate the L1 config file before start (exact predefined configs, chain check)
    if (auto e = l1::validate_config_file(l1_path); !e.empty()) {
        LOG_ERROR("L1 config validation failed: %s", e.c_str());
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    bt::frost::init();
    if (!bt::fsperm::private_dir(cfg.datadir)) {
        LOG_ERROR("cannot create %s", cfg.datadir.c_str());
        return 1;
    }
    Node node(cfg);
    // net_task.rs: the L1 config is read at each two-way-peg connect (node-local swap monitoring)
    node.state().l1_config = [l1_path](ParentChain c) { return l1::load_config(l1_path, c); };
    node.state().l1_find = [](const L1RpcConfig& c, const std::string& addr, uint64_t amount) {
        std::string err;
        return l1::Client{c}.find(addr, amount, err);
    };
    std::unique_ptr<Wallet> wallet;
    try {
        wallet = std::make_unique<Wallet>(cfg.datadir + "/backbone-wallet.sqlite", node);
        if (mnemonic_path) {
            std::ifstream f(*mnemonic_path);
            std::string m((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            wallet->set_seed_from_mnemonic(m);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("wallet: %s", e.what());
        return 1;
    }
    std::string err;
    if (!node.start(err)) {
        LOG_ERROR("startup failed: %s", err.c_str());
        return 1;
    }
    Wallet* wp = wallet.get();
    node.on_tip_changed = [wp, &node] {
        try {
            if (wp->addresses().empty() && wp->has_seed()) {
                std::set<Address> as;
                for (auto& [op, o] : node.utxos()) as.insert(o.address);
                wp->recover_addresses(as);
            }
            wp->update();
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to update wallet: %s", e.what());
        }
    };
    try {
        std::set<Address> as;
        for (auto& [op, o] : node.utxos()) as.insert(o.address);
        if (wallet->has_seed()) wallet->recover_addresses(as);
        wallet->update();
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to perform initial wallet update: %s", e.what());
    }
    App app(node, *wallet);
    app.on_stop = [] { g_stop = true; };
    app.start_tasks();
    bt::jsonrpc::Server rpc;
    rpc.options = rpc_opts;
    register_rpc(rpc, app);
    if (!rpc.listen(rpc_addr.ip, rpc_addr.port, err)) {
        LOG_ERROR("%s", err.c_str());
        app.stop_tasks();
        node.stop();
        return 1;
    }
    LOG_INFO("starting RPC server at `%s` (%zu methods)", rpc_addr.to_string().c_str(), rpc.methods().size());
    // Our app layer (--bt-app-rpc-port): a server of its own; the LayerTwo server above is the same with or without it.
    std::unique_ptr<AppLayer> app_layer;
    if (app_opts.enabled) {
        const char* names[] = {"betanet", "forknet", "regtest", "signet"};
        app_layer = std::make_unique<AppLayer>(app, names[static_cast<int>(cfg.network)], cfg.datadir, app_opts);
        if (!app_layer->start(err)) {
            LOG_ERROR("app RPC: %s", err.c_str());
            rpc.stop();
            app.stop_tasks();
            node.stop();
            return 1;
        }
    }
    if (app_opts.exit_with_stdin) {
        // The app that started this node holds our standard input: when it goes (even killed), we stop.
        std::thread([] {
            char buf[64];
            while (std::fread(buf, 1, sizeof(buf), stdin) > 0) {
            }
            LOG_INFO("standard input closed: stopping");
            g_stop = true;
        }).detach();
    }
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    LOG_INFO("shutting down");
    if (app_layer) app_layer->stop();
    rpc.stop();
    app.stop_tasks();
    node.stop();
    return 0;
}
