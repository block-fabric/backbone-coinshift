// coinshift-simcheck <sim.jsonl>...: replay `cs_oracle sim` streams through our State.
#include <coinshift/simcheck.h>

#include <bt/frost.h>

#include <cstdio>

int main(int argc, char** argv)
{
    bt::frost::init();
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        auto r = coinshift::simcheck::run(argv[i]);
        std::printf("%s: %zu steps, %zu blocks ok, %zu rejected, %zu txs, %zu disconnects, %zu local; verdict mismatches %zu, digest mismatches %zu, "
                    "message mismatches %zu%s%s\n",
                    argv[i], r.steps, r.blocks_ok, r.blocks_rejected, r.txs, r.disconnects, r.local_ops, r.verdict_mismatches, r.digest_mismatches,
                    r.message_mismatches, r.first_problem.empty() ? "" : "\n  FIRST: ", r.first_problem.c_str());
        for (auto& [m, n] : r.message_examples) std::printf("  MSG %zu x %s\n", n, m.c_str());
        if (r.verdict_mismatches || r.digest_mismatches) bad++;
    }
    return bad ? 1 : 0;
}
