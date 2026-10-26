// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Coinshift's app layer (doc/app.md): what our app of Coinshift and the unified wallet need of this node, on the app
// RPC (bt/appui), beside the LayerTwo JSON-RPC, which it does not touch. It reads and acts through the same Node,
// Wallet and App the LayerTwo methods use: the LayerTwo reads are served as they are (the same handlers, the same
// JSON), every spend is a confirmed method (asked in the app's trusted dialog, bound to a one-time token, run by the
// LayerTwo handler: payments, withdrawals, deposits, swaps created, accepted and claimed), and the wallet's history,
// the explorer, the peg, the swaps as the pages show them, the app's own check of L1 payments and the backup phrase
// are host methods of the pages.
#pragma once

#include <coinshift/app.h>

#include <bt/appui/dropin.h>
#include <bt/appui/host.h>
#include <bt/appui/rpcserver.h>
#include <bt/jsonrpc.h>

#include <memory>
#include <string>

namespace coinshift {

class AppLayer {
public:
    /** `network`: the LayerTwo name of the network (betanet, regtest...). */
    AppLayer(App& app, std::string network, std::string datadir, bt::appui::AppOptions options);
    ~AppLayer();
    AppLayer(const AppLayer&) = delete;
    AppLayer& operator=(const AppLayer&) = delete;

    bool start(std::string& err);
    void stop();
    uint16_t port() const;

    /** The LayerTwo methods the pages read (the same handlers and JSON as the LayerTwo server's). */
    static const std::vector<std::string>& l2l_reads();
    /** The spends of the pages (rpc.confirmed), LayerTwo's names. */
    static const std::vector<std::string>& confirmed_methods();
    /** The host methods of the pages. */
    static const std::vector<std::string>& host_methods();
    /** Every method of the app RPC: the pack's manifest is checked against it. */
    std::vector<std::string> app_methods() const;
    UniValue manifest() const;

    /** The host, for tests (webuihost without HTTP). */
    bt::appui::RemoteHost& host() { return *host_; }
    bt::appui::RpcServer& server() { return *server_; }

    /** The wallet's history (dropin.history): one row a transaction touching the wallet, newest first. */
    UniValue history(size_t limit);

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
    std::unique_ptr<bt::appui::RpcServer> server_;
    std::unique_ptr<bt::appui::RemoteHost> host_;
};

/** An L1 payment as the app checks it: the satoshis of a decimal amount of a parent chain's RPC ("0.50000000"), or
 *  nothing when it is not a plain decimal of at most 8 places within 21,000,000 coins. */
std::optional<uint64_t> l1_amount_sats(const std::string& decimal);

/** Whether an L1 address of an RPC reply is the one a swap names (exactly; for Bitcoin Cash, with or without its
 *  cashaddr prefix). */
bool l1_address_matches(const std::string& reply, const std::string& wanted);

/** What an L1 transaction (getrawtransaction's verbose reply) paid to `address`, in satoshis (the sum of its outputs
 *  to it), or nothing when its outputs cannot be read. */
std::optional<uint64_t> l1_paid_to(const UniValue& tx, const std::string& address);

} // namespace coinshift
