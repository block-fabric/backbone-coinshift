// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Coinshift's traits for the core's archive, P2P and node templates.
#pragma once

#include <coinshift/net.h>
#include <coinshift/state.h>

#include <bt/sidechain/archive.h>
#include <bt/sidechain/node.h>

namespace coinshift {

struct Chain : WireChain {
    using Transaction = coinshift::Transaction;
    using FilledOutput = coinshift::Output;
    using Address = coinshift::Address;
    using State = coinshift::State;
    static constexpr uint8_t SLOT = THIS_SIDECHAIN;
    static constexpr uint16_t DEFAULT_PORT = coinshift::DEFAULT_PORT;
    static constexpr const char* NAME = "coinshift";
    static constexpr const char* STORE_FILE = "backbone-coinshift.sqlite";
    static constexpr size_t MAX_BODY_SIZE = bt::sidechain::BLOCK_RESPONSE_LIMIT;
    /** net_task.rs disconnect_tip_: rows "applied by an earlier block" are those with
     *  applied_height < height - 1 (u32), reproduced (doc/coinshift.md). */
    static constexpr bool DISCONNECT_WINDOW_SKIPS_PARENT = true;
};

using Archive = bt::sidechain::Archive<Chain>;
using P2P = bt::sidechain::P2P<Chain>;
using Node = bt::sidechain::Node<Chain>;

} // namespace coinshift
