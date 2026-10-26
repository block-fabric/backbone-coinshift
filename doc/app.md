# coinshift-app: our app on the Coinshift drop-in

`coinshift-app` is Coinshift with our sidechain app: the sidechain template's window, trusted dialog and interface kit,
with pages for Coinshift's own data model (UTXOs, swaps of ECX on Coinshift for coins of a parent chain (L1) with their
escrow outputs and reservations, withdrawal bundles, eCash as the mainchain). The node is `coinshift` (this
repository's drop-in), next to the app, unchanged: the app starts it as its child and talks to its app RPC. Consensus,
P2P and the LayerTwo JSON-RPC (coinshift-rs v0.14.8, 43 methods) are the drop-in's own and stay byte for byte what
they are; the app layer is off unless the node is started with `--bt-app-rpc-port`.

The design shared by every drop-in app (why a thin client, the app RPC, the privileges, one backup phrase by BIP-85,
the pack, security, tests) is [core/doc/app.md](../core/doc/app.md). This page is Coinshift's part.

## Build and run

```
cmake -S . -B build-app -DBT_BUILD_APP=ON -DBT_KIT_DIR=<sidechain-template repo> [-DBT_DEPENDS_PREFIX=<prefix>]   # GTK 3 + WebKitGTK 4.1, npm
~/skydoge/tools/build.sh ~/skydoge/backbone-coinshift-app build-app                                              # or cmake --build build-app -j4
build-app/chains/coinshift/coinshift-app                  # betanet, the enforcer at http://localhost:50051
build-app/chains/coinshift/coinshift-app --network regtest --mainchain-grpc-url http://127.0.0.1:50051
cmake --build build-app --target coinshift-app-pack       # build-app/pack/coinshift: the pack for the unified wallet
```

The app is `coinshift-app` (the stem `coinshift`); the node it runs is `coinshift`, next to it, with LayerTwo's data
folder (`<data_dir>/coinshift`: `"datadirName"`) and its one RPC server (`--rpc-addr` only: `"addressFlags":
"rpc-addr"`, a way core's desktop app learnt for this chain). Networks: betanet, regtest, signet (the node's).

## One backup phrase

The wallet is coinshift-rs' (BIP-39, keys at m/43'/1899'/0'/255'/0'/i), all of it from the seed: the unified wallet's
BIP-85 phrase (core/doc/app.md §3, stem `coinshift`) covers it. Vectors (master "abandon ×11 about"), checked against an
independent implementation:

| stem, network | index | phrase | fingerprint |
|---|---|---|---|
| coinshift, betanet | 13881959 | fetch caught cement salon drop clarify lava marble pudding stuff omit tone | 9be806b3a06ab479 |
| coinshift, regtest | 2059020698 | occur home bulk task combine pulse boy transfer engine guard main notice | d86cb051a4b61b63 |

## The pages

The drop-in pages (`core/app/ui`) in the kit's places, with coinshift-rs' names for their spends (`transfer`,
`withdraw`): Home (with Coinshift's card: open swaps, yours under way, those to claim), Overview, Send, Receive,
Activity (a swap offered, reserved, claimed, taken as their own kinds; the swap and what it locked in the row's dialog),
Drivechains, the explorer (a swap's escrow outputs and data), the node, Settings → Node and → Wallet. Coinshift's own
(`app/chain`):

- **Swaps**: every swap this node knows (open ones to take, this wallet's own, all), each with its two sides (what the
  taker gets on Coinshift, what it pays on L1), the price, where it stands (open, reserved, for one recipient, L1
  payment confirming with its confirmations as a bar, ready to claim, completed, expired) and what this wallet can do
  next. A swap opens in its own view: the steps it went through (offered, reserved, paid on L1, confirmed, claimed),
  its details and transactions, and the next step: **Reserve** (`accept_swap [swap_id, l2_claimer_address, fee_sats]`,
  an address of this wallet holding a coin for its fee: coinshift-rs pays a reservation's fee from a coin of
  the claimer's address), **pay** on L1 from the user's own L1 wallet (the address and amount to copy), give the
  app the L1 transaction (typed, or found on the user's L1 node: `coinshift.l1.find`), **Check the payment**
  (`coinshift.payment`), **Claim** (`claim_swap [swap_id, null]`). For the swap's creator, the payments its L1 node sees
  to its L1 address.
- **Offer a swap**: ECX of this wallet for coins of another chain (`create_swap [parent_chain, l1_recipient_address,
  l1_amount_sats, l2_recipient, l2_amount_sats, required_confirmations, fee_sats]`): open to anyone (the first to
  reserve it takes it) or for one recipient; the price, when it expires, how long a reservation lasts.
- **Settings → L1 nodes**: the node of each parent chain the app checks payments on (Bitcoin Core-style RPC:
  `getrawtransaction`, `scantxoutset`, `getblockchaininfo`): asked in the trusted dialog first (the node contacts no
  address a page names before a yes), then kept only if that node answers and says it is of that chain.

### The app claims only a swap whose payment it saw on L1

Coinshift's consensus takes each node's own view of the L1 payment (its swap state is partly node-local:
`update_swap_l1_txid`, the L1 monitoring), as coinshift-rs has it (known upstream behaviour reproduced,
doc/coinshift.md). Our app never builds a transaction that relies on that: it claims a swap only once it saw the
payment itself on the user's L1 node. `coinshift.payment {id, l1Txid}`: the swap must be one this wallet may claim (its
pre-specified recipient, or reserved by it and the reservation live); the L1 transaction must pay the swap's L1 address
at least its amount, not be older than the chain lets a swap's payment be, and not be another swap's payment on this
node; then the node is told, as `update_swap_l1_txid` does, with the confirmations that L1 node reports (again as they
grow), and the app keeps which L1 transaction it checked. `claim_swap`'s describer refuses unless the swap's L1
transaction is the one the app checked, and checks it on L1 again: its confirmations at least the swap's. A swap's L1
address is text of the chain (anyone's): the app asks an L1 node about it only when it is an address of that chain's
characters (never a descriptor of its own), and shows it in a question as `display_text_shown` writes it.

The L1 nodes are the app's own (its store, `backbone-app.sqlite`, owner-only), never coinshift-rs'
`l1_rpc_configs.json` (whose rules stay LayerTwo's); their password never comes back to a page. Not offered to the
pages: `cancel_swap` and `update_swap_l1_txid` by hand (node-local only: a cancel changes nothing on other nodes, the
escrow comes back when the swap expires) and `reconstruct_swaps`.

Kept wallet fix (wallet only; consensus unchanged): an expired swap's escrow is taken back only by the swap's creator;
an escrow at this wallet's address as a swap's pre-specified recipient is never spent as a coin (`select_coins`).

Withdrawals are offered as on the other chains; their question says that Coinshift's withdrawal bundles follow a known
upstream behaviour, reproduced (a withdrawal may stay unpaid for a long time).

## What the pages use of the node (app/manifest.json)

- **Reads**: the LayerTwo reads by name (the same handlers and JSON): `balance`, `format_deposit_address`,
  `get_best_mainchain_block_hash`, `get_best_sidechain_block_hash`, `get_block`, `get_block_hash`, `get_block_index`,
  `get_bmm_inclusions`, `get_swap_status`, `get_wallet_addresses`, `get_wallet_utxos`, `getblockcount`,
  `latest_failed_withdrawal_bundle_height`, `list_mempool`, `list_peers`, `list_swaps`, `list_swaps_by_recipient`,
  `list_utxos`, `mainchain_sync_progress`, `openapi_schema`, `pending_withdrawal_bundle`, `sidechain_wealth`; and
  `getmainchaininfo`, `getbalances`, `getblockchaininfo`, `getnetworkinfo`, `uptime`.
- **Confirmed**: `transfer`, `transfer_many`, `withdraw`, `create_deposit`, `mine`, `create_swap`, `accept_swap`,
  `claim_swap`. Every describer sums with `spend_total`; `create_swap` takes a parent chain of theirs, an L1 address of
  it (Bitcoin's networks parsed and of the right network) that a dialog can show, an L1 amount within 21,000,000
  coins, a recipient that is not this wallet, 1 to 1000 confirmations; L1 addresses of the chain (someone else's swap)
  are shown in a question as `display_text_shown` writes them.
- **Host**: the shared `dropin.*` (with Coinshift's reads: `dropin.transaction`, `dropin.search` exercise `get_block`,
  `dropin.address` `list_utxos`) and `coinshift.swaps` (`list_swaps`), `coinshift.swap` (`get_swap_status`,
  `get_block`), `coinshift.wallet` (`get_wallet_addresses`, `balance`), `coinshift.l1.find` (`get_swap_status`),
  `coinshift.payment` (`update_swap_l1_txid`); `coinshift.l1` and `coinshift.l1.set` only by name (the L1 nodes' address
  and login).

Ownership: a swap's role for this wallet (creator, recipient, claimer), the payment the app checked, whether it may
claim, and what it locked are said only to a caller of `get_wallet_addresses`. The tests check this file equals what
the node serves (`AppLayer::manifest`).

## Tests

- `tests/test_coinshift_appui.cpp`: every LayerTwo method's reply byte-identical with and without the app layer (43
  methods, seven kinds of bad parameters each, the reads with good ones), the app RPC's reads equal to the LayerTwo
  results, the phrase made only in a question / refused over another, the unified wallet's phrase (the BIP-85 vectors
  above; the wallet it sets has coinshift-rs' first address of that phrase), wallet data and the L1 nodes only for
  their readers (no password ever), every spend refused before any question (sums that wrap or pass 21,000,000, a
  parent chain or an L1 address that is not right, a hidden character, a recipient of this wallet, an id that is not 32
  numbers of 0 to 255, a claimer that is not this wallet, a swap that does not exist), setting an L1 node (http only, a
  node that answers), reading an L1 payment (amounts, addresses, cashaddr prefixes), the wallet never spending an
  escrow it did not lock, the manifest.
- The interface (`build-app/coinshift-app-ui/src/webui/app`): `npx svelte-check --fail-on-warnings` and `npx vitest
  run src/chain` (`app/chain/cs.test.ts`: create_swap, accept_swap and claim_swap with coinshift-rs' parameters, the
  payment found and checked before a claim, a hidden character refused before any question, nothing outside the
  manifest, where a swap stands and what comes next; with the shared `dropin.test.ts`).
- `tests/e2e/run_app.py` (a build with `-DBT_APP_TEST_HOOKS=ON`; xvfb-run): a local eCash regtest, coinshift-app runs
  node A, a coinshift node B is the other side, a second backboned (plain `-regtest -txindex`) is the swaps' L1 with the
  users' L1 wallet: the phrase made in the trusted dialog, 10 ECX deposited, 1.5 ECX sent to B, 0.25 ECX received from
  B; the L1 node set in Settings (its password never back to the page); swap 1: the app offers 1 ECX for 0.5 BTC, node
  B reserves it, the L1 wallet pays, B claims (mining its claim itself), the app shows it completed and the 0.5 BTC on
  its L1 address; swap 2: node B offers 0.5 ECX for 0.3 BTC, the app reserves it, a claim before the payment is refused,
  the L1 wallet pays, the app finds and checks the payment and claims; the phrase shown and saved (0600); then A's
  LayerTwo replies byte-identical without and with the app layer. No withdrawal (doc/coinshift.md). Last run: 17
  checks passed, 43/43 replies byte-identical.

```
python3 tests/e2e/run_app.py --bin build-app/chains/coinshift --backboned <backboned> --tmp <dir> --shots <dir> --base-port 49200
```

## Platforms

Linux (GTK 3, WebKitGTK 4.1): built, unit-tested and run end to end. Windows: the node and the app are meant to
cross-compile as the other drop-in apps do (`-DCMAKE_TOOLCHAIN_FILE=core/cmake/mingw64.cmake -DBT_BUILD_APP=ON
-DWEBVIEW2_INCLUDE_DIR=<the WebView2 SDK headers>`); not yet built for Windows (the L1 client uses POSIX sockets).
macOS: not built.
