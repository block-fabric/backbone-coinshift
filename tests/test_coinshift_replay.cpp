// Known answers against public eCash betanet data (slot 255): every block fetched from the
// public seed peer (coinshift-fetch-blocks) and the enforcer's BMM/deposit events
// (GetTwoWayPegData), replayed through the state machine with every check enabled.
#include "test.h"

#include <coinshift/fixture.h>

#include <cstdio>

using namespace coinshift;

static const fixture::Data& data()
{
    static std::optional<fixture::Data> d;
    if (!d) {
        std::string err;
        d = fixture::load(bttest::data_dir() + "/betanet", err);
        if (!d) std::fprintf(stderr, "fixture load: %s\n", err.c_str());
    }
    return *d;
}

TEST(betanet_blocks_codec_and_hashes)
{
    auto& d = data();
    REQUIRE(d.blocks.size() >= 2262);
    std::optional<Hash32> prev;
    uint32_t exact = 0;
    for (auto& [h, raw] : d.blocks) {
        Block b = fixture::decode_block_response(raw);
        Writer w;
        w.u32(0);
        bincode(w, b.header);
        bincode(w, b.body);
        if (w.buf == raw) exact++;
        CHECK(b.header.prev_side_hash == prev);
        prev = b.header.hash();
    }
    std::fprintf(stderr, "  %zu blocks, %u byte-exact\n", d.blocks.size(), exact);
    CHECK_EQ(exact, uint32_t(d.blocks.size()));
    CHECK_EQ(bt::hex_encode(*prev), std::string("b191f39008b8c012a9e2f31f6f50b943fd6578cd35652bad9bceb9ce79e90aad")); // tip 2261
}

TEST(betanet_replay_all_checks)
{
    bt::frost::init();
    std::string err;
    auto st = fixture::steps(data(), err);
    if (!err.empty()) std::fprintf(stderr, "steps: %s\n", err.c_str());
    REQUIRE(err.empty());
    bt::kv::MemStore store;
    State state;
    auto res = fixture::replay(st, state, store);
    if (!res.error.empty()) std::fprintf(stderr, "replay: %s\n", res.error.c_str());
    CHECK(res.error.empty());
    bt::kv::Txn t(store);
    std::fprintf(stderr, "  %u blocks, %u txs, %u signatures, %u deposits\n  %s\n", res.blocks, res.txs, res.signatures, res.deposits,
                 fixture::state_digest(state, t).c_str());
    CHECK_EQ(res.blocks, uint32_t(2262));
}
