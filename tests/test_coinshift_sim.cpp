// Random chains built and judged by THEIR state machine (tools/oracle `sim`), replayed through
// ours: every block/tx/disconnect/local-RPC verdict and every state digest must be identical.
#include "test.h"

#include <coinshift/simcheck.h>

#include <bt/frost.h>

#include <cstdio>

TEST(oracle_random_chains)
{
    bt::frost::init();
    for (const char* f : {"sim_seed2.jsonl", "sim_seed3.jsonl"}) {
        auto r = coinshift::simcheck::run(bttest::data_dir() + "/oracle/" + f);
        std::fprintf(stderr, "  %s: %zu steps, %zu blocks, %zu txs, %zu disconnects, %zu local ops\n", f, r.steps, r.blocks_ok, r.txs, r.disconnects,
                     r.local_ops);
        if (!r.first_problem.empty()) std::fprintf(stderr, "  %s\n", r.first_problem.c_str());
        CHECK(r.steps > 500);
        CHECK_EQ(r.verdict_mismatches, size_t(0));
        CHECK_EQ(r.digest_mismatches, size_t(0));
    }
}
