// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Coinshift application: node + wallet, block templates, BMM mining, deposits, swaps
// (app/app.rs) and the JSON-RPC surface (rpc-api/rpc.rs, app/rpc_server.rs): their 43 methods,
// serde_json shapes and error texts. The surface an app UI needs is listed in doc/rpc-surface.md.
#pragma once

#include <coinshift/l1rpc.h>
#include <coinshift/wallet.h>

#include <bt/jsonrpc.h>

#include <atomic>
#include <thread>

namespace coinshift {

struct BlockTemplate {
    uint64_t bribe = 0;
    Header header;
    Body body;
    uint64_t fees = 0;
};

class App {
public:
    App(Node& node, Wallet& wallet) : node(node), wallet(wallet) {}
    ~App() { stop_tasks(); }
    Node& node;
    Wallet& wallet;

    static constexpr uint64_t EMPTY_BLOCK_BMM_BRIBE = 1000; // app.rs
    BlockTemplate build_block_template(std::optional<uint64_t> fee);
    void mine(std::optional<uint64_t> fee);
    bool connect_block(const Header& h, const Body& b, const Hash32& main_block_hash);
    Hash32 deposit(const Address& a, uint64_t value, uint64_t fee);
    /** sign_and_send: authorize, submit (regenerate proof, validate, mempool, relay), update. */
    Hash32 sign_and_send(const Transaction& tx);
    bool is_locked(const OutPoint& op);
    std::function<void()> on_stop;

    /** swap_confirmation_check_task (every 10 s, node-local, needs an L1 config). */
    void start_tasks();
    void stop_tasks();

private:
    std::atomic<bool> tasks_running_{false};
    std::thread confirm_thread_;
    void confirmation_check_once();
};

void register_rpc(bt::jsonrpc::Server& s, App& app);

} // namespace coinshift
