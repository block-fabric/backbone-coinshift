// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Replays a `cs_oracle sim` stream (THEIR verdicts and state digests) through our State.
#pragma once

#include <map>
#include <string>

namespace coinshift::simcheck {

struct Result {
    size_t steps = 0, blocks_ok = 0, blocks_rejected = 0, txs = 0, disconnects = 0, local_ops = 0;
    size_t verdict_mismatches = 0, digest_mismatches = 0, message_mismatches = 0;
    std::string first_problem;
    std::map<std::string, size_t> message_examples; // "theirs | ours" -> count
};

Result run(const std::string& jsonl_path, bool stop_at_first = false);

} // namespace coinshift::simcheck
