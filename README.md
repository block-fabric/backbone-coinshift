# backbone-coinshift

Drop-in replacement client for **Coinshift on eCash (slot 255)**: consensus-, P2P-, mainchain-
and RPC-compatible with LayerTwo Labs' coinshift-rs v0.14.8 (ef8e914), which is what betanet runs
(seed peers report version 0.14.8). C++20 on the shared engine `backbone-core` (submodule `core`,
branch `dropin-coinshift` = 7d780ae + one commit).

* `doc/coinshift.md`: status, reproduced behaviour, kept fixes, proposals, open questions.
* `doc/rpc-surface.md`: the JSON-RPC surface (for the app UI layer).

Inspired by Coinshift by LayerTwo Labs.

## Layout

```
core/                 git submodule ../backbone-core (engine, generic sidechain Node<C>, archive, side tips)
chains/coinshift/     libcoinshift_core (types, codecs, merkle, state machine incl. swaps, JSON, fixtures)
                      libcoinshift_node (wallet, app/RPC, L1 RPC client); binaries coinshift, coinshift-cli,
                      coinshift-fetch-blocks, coinshift-replay, coinshift-simcheck
tests/                betanet known answers (tests/data/betanet), oracle cases (tests/data/oracle)
tests/regtest/        interop.py: their release binary and ours on one local eCash regtest chain
tools/oracle/         Rust oracle linking their lib crate (ext/coinshift-rs, pinned)
```

## Build and run

```
cmake -S . -B build -DBT_DEPENDS_PREFIX=<backbone-core>/depends/built/linux && cmake --build build -j4
build/tests/bt_tests
coinshift --headless --mainchain-grpc-url https://enforcer.beta.ecash.ninja   # public enforcer (TLS)
coinshift --headless --mainchain-grpc-url http://127.0.0.1:50051              # backboned -grpc or a local enforcer
coinshift-cli getblockcount                                                    # --rpc-url http://localhost:6255
```

Defaults as theirs: P2P 0.0.0.0:4255 (magic 2d 0c d8 04), RPC 127.0.0.1:6255, datadir
`<data_dir>/coinshift`, L1 config `<data_dir>/coinshift/l1_rpc_configs.json`.
