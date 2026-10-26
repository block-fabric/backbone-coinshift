# Coinshift drop-in: status

Target: coinshift-rs v0.14.8 (ef8e914, latest release = main HEAD on 2026-10-22; v0.14.7 differs
only by the betanet seed). Licence: "All rights reserved" (licensing for the L2L chains cleared by
the user). Pinned in `~/skydoge/ext/PINNED.txt`.

## Verification

| Check | Result |
|---|---|
| Betanet blocks | 2,262 blocks (0..2261) from seed.beta.ecash.eu.com:4255 decode and re-encode byte-identically |
| Full replay | every block with all checks (BMM vs the enforcer's slot-255 data, fee/size CBMT merkle roots, Utreexo proofs and roots, FROST signatures, swap rules, 2WPD). Tip 2261 `b191f390…`: 12 UTXOs, 3 STXOs, 1 swap (open, BTC, 10,000 sat, expires at 3152), 1 locked output, pending bundle `4e20f32b…`@1526, wealth 670,000 |
| Their State as oracle (betanet) | per-block digest (tip, accumulator roots, UTXOs, STXOs, swaps (bincode), locks, reservations, pending bundle, wealth): 2,262/2,262 identical |
| Their State as oracle (random chains) | 100 seeds x 300 blocks: blocks, mempool verdicts, disconnects, node-local RPCs (update_swap_l1_txid, cancel_swap): 0 verdict and 0 digest differences; error texts identical except internal Utreexo/panic texts |
| JSON | 54 serde_json shapes and 28 parse-error cases identical to their crate |
| Wallet | the same mnemonic gives the same 20 addresses |
| Live sync | empty datadir -> public enforcer + public seed -> tip 2262 `24bf2a2d…`, wealth 670,000 |
| Regtest pair (their release binary) | 629 checks, 0 differences: deposits (enforcer and create_deposit), transfers and transfer_many both ways, pre-specified swap claimed, open swap reserved (SwapAccept) and claimed only after confirmations, swap expiry, withdrawals; then both implementations stopped at the same block when the bundle's M3 arrived (known upstream behaviour reproduced) |

Not done (low-usage mode): mainchain reorg and fresh-node cross sync on regtest (scenario stopped at the
bundle step above), a run against backboned -grpc, a payout of a withdrawal bundle.

## Known upstream behaviour reproduced (consensus)

Several upstream consensus behaviours around swaps, swap expiry and withdrawal-bundle events are
reproduced as they are, because a different rule would split us from the live chain. Details are held
for coordinated disclosure. Swap `state` is partly node-local by their design (L1 monitoring, the RPCs
`update_swap_l1_txid` and `cancel_swap`) and feeds the expiry rule; this is reproduced too.

Other reproduced quirks: canonical size = borsh length / 8; SwapPending's swap_id travels as a hex
string in bincode but as a number array in JSON (serde_with IfIsHumanReadable order); the
disconnect 2WPD window uses `applied_height < height - 1`; pending bundles never expire.

## Kept fixes (non-consensus)

* Mempool/relay/template policy on top of their validate_transaction: no repeated inputs, no replays
  of confirmed txs, swap-claim and swap-accept policy, no spend of an unexpired swap's escrow; the
  template regenerates proofs and drops what no longer proves.
* Wallet: escrows of unexpired swaps are not selected; a claim only sweeps outputs locked to its swap; an expired
  swap's escrow is taken back only by its creator (an escrow at the wallet's address as a pre-specified recipient is
  never spent as a coin).
* App layer (doc/app.md): a swap is claimed through the app only once the app saw its L1 payment itself, on an L1 node
  the user set up; `cancel_swap` and a hand-set `update_swap_l1_txid` are not offered to its pages.
* P2P hardening from core (bans at connect/request/dial, fail-closed, per-IP GetHeaders budget,
  bounded body fetch); SIGPIPE ignored; L1 config written owner-only (0600, no symlinks).
* No panics: their asserts are rejections (the block is refused, the node keeps running).

## Proposals (not coded; need a coordinated flag day)

Bind the swap rules to consensus-visible state only (no node-local fields), verify the L1 payment
in consensus (their own unmerged work), make bundle-event handling tolerant of late M3s, fix the
disconnect window, and expire pending bundles.

## Open questions

* Whether to make the policy refusal of node-locally unlocked escrows also a UI warning.
* Coordinate the held findings with LayerTwo Labs before betanet's pending bundle gets an M3.
