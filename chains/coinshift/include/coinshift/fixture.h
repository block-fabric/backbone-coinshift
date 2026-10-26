// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Public eCash betanet data for Coinshift (slot 255) and a replay through the state machine:
//   blocks.bin    every block 0..tip as fetched over P2P (coinshift-fetch-blocks)
//   twpd255.json  the public enforcer's GetTwoWayPegData for slot 255 (BMM + deposit events)
#pragma once

#include <coinshift/net.h>
#include <coinshift/state.h>

#include <bt/kv.h>
#include <bt/mainchain.h>

#include <map>
#include <string>
#include <vector>

namespace coinshift::fixture {

struct MainBlock {
    bt::mainchain::BlockHeaderInfo header;
    bt::mainchain::BlockInfo info;
};

struct Data {
    std::vector<std::pair<uint32_t, Bytes>> blocks; // (height, bincode ResponseMessage::Block)
    std::map<uint32_t, MainBlock> main_by_height;   // sparse: blocks with a slot-255 commitment or event
};

std::optional<Data> load(const std::string& dir, std::string& err);

struct ReplayStep {
    uint32_t height = 0;
    Block block;
    Hash32 bmm_block;
    bt::mainchain::TwoWayPegData window;
};

struct ReplayResult {
    uint32_t blocks = 0;          // connected with every consensus check enabled
    uint32_t reencoded_exact = 0; // bincode re-encoding identical to the bytes received
    uint32_t bmm_verified = 0;    // commitment == header hash in a child of prev_main
    uint32_t signatures = 0, txs = 0, deposits = 0;
    std::map<uint8_t, uint32_t> data_kinds; // TxData kind -> count
    std::string error;
};

/** The replay steps (block, BMM block, 2WPD window (prev_main(N-1), prev_main(N)], genesis
 *  window = every event block up to prev_main(0)). */
std::vector<ReplayStep> steps(const Data& d, std::string& err, uint32_t max_height = UINT32_MAX);

/** Replay with all checks on a fresh store. */
ReplayResult replay(const std::vector<ReplayStep>& steps, State& st, bt::kv::Store& store);

Block decode_block_response(ByteSpan raw);

/** Digest of the whole state after a block (for the oracle comparison): one line of hex fields. */
std::string state_digest(const State& st, const bt::kv::Txn& t);

} // namespace coinshift::fixture
