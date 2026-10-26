#!/usr/bin/env python3
# Copyright (c) 2026 The Backbone developers
# Distributed under the MIT software license.
"""End-to-end run of coinshift-app on a local eCash regtest (doc/app.md "Tests"): slot 255, app.js in its page; the runner
is core/tests/e2e/dropin_app.py. Besides deposit, payments both ways and receipt (no withdrawal: Coinshift's bundles
follow a known upstream behaviour, reproduced, doc/coinshift.md): two swaps
with a Bitcoin regtest as their L1 (a second backboned, plain -regtest with -txindex, its wallet the users' L1 wallet).

  1. The app offers 1 ECX for 0.5 BTC (an open swap). Node B (no app) reserves it, the L1 wallet pays the app's L1
     address, node B gives its own node the payment (update_swap_l1_txid) and claims, mining its claim itself. The app
     shows the swap completed and, on its L1 node, the payment to its address.
  2. Node B offers 0.5 ECX for 0.3 BTC. The app reserves it (accept_swap), the L1 wallet pays B's L1 address, the app
     finds the payment on its L1 node, checks it (coinshift.payment) and claims the ECX (claim_swap).

    run_app.py --bin build-app/chains/coinshift --backboned <backboned> --tmp <dir> --shots <dir> [--base-port 49200]
"""

import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "core" / "tests" / "e2e"))
from dropin_app import Chain, main  # noqa: E402
from dropin_regtest import JsonRpc, bmm_block, wait_for  # noqa: E402

READS = [("getblockcount", []), ("get_best_sidechain_block_hash", []), ("get_best_mainchain_block_hash", []), ("list_utxos", []),
         ("pending_withdrawal_bundle", []), ("latest_failed_withdrawal_bundle_height", []), ("sidechain_wealth", []), ("balance", []),
         ("get_wallet_addresses", []), ("get_wallet_utxos", []), ("openapi_schema", []), ("list_peers", []), ("list_mempool", []),
         ("mainchain_sync_progress", []), ("list_swaps", []), ("get_block_hash", [0])]

L1 = {}


def rpc_flags(rpc: int, p2p: int) -> list:
    """coinshift-rs' way of giving a node its addresses: one RPC server (-r host:port), -n host:port."""
    return ["-r", f"127.0.0.1:{rpc}", "-n", f"127.0.0.1:{p2p}"]


def start_l1(run):
    """A Bitcoin regtest (backboned -regtest, -txindex): the L1 of the swaps, its wallet the users' L1 wallet."""
    port = run.port + 40
    datadir = run.base / "l1"
    datadir.mkdir(parents=True, exist_ok=True)
    run.main.procs.spawn("l1", [run.main.binary, "-regtest", f"-datadir={datadir}", f"-rpcport={port}", f"-port={port + 1}", "-rpcuser=l1",
                                "-rpcpassword=l1pass", "-listen=0", "-dnsseed=0", "-fixedseeds=0", "-txindex", "-fallbackfee=0.0002", "-printtoconsole=0"])
    rpc = JsonRpc(port, "l1", "l1pass")
    wait_for(lambda: rpc.getblockcount() >= 0, "the L1 node", 180)
    if "users" not in rpc.listwallets():
        rpc.createwallet("users")
    wallet = JsonRpc(port, "l1", "l1pass", path="/wallet/users")
    wallet.generatetoaddress(101, wallet.getnewaddress())
    L1.update(rpc=rpc, wallet=wallet, url=f"http://127.0.0.1:{port}")
    return []


def peer_setup(b):
    b.set_seed_from_mnemonic(b.generate_mnemonic())
    L1["b_address"] = b.get_new_address()
    return L1["b_address"]


def id_array(hex_id):
    return [int(hex_id[i:i + 2], 16) for i in range(0, 64, 2)]


def pay_l1(address, sats):
    txid = L1["wallet"].sendtoaddress(address, f"{sats / 1e8:.8f}")
    L1["wallet"].generatetoaddress(3, L1["wallet"].getnewaddress())
    return txid


def on_signal(msg, run):
    words = msg.split()
    failures = []
    if words[0] == "swap-offered":
        # Swap 1: node B (coinshift-rs' RPC, no app) takes the app's open swap.
        swap = id_array(words[1])
        b = run.b
        wait_for(lambda: b.get_swap_status(swap) is not None, "the app's swap on node B", 300, 1)
        # The claimer: an address of B holding a coin (the reservation's fee is paid from it, coinshift-rs' rule).
        claimer = next(u["output"]["address"] for u in b.get_wallet_utxos() if (u["output"].get("content") or {}).get("Value", 0) >= 1000)
        L1["b_claimer"] = claimer
        b.accept_swap(swap, claimer, 1000)
        wait_for(lambda: not b.list_mempool(), "the reservation in a block", 300, 1)
        status = b.get_swap_status(swap)
        txid = pay_l1(status["l1_recipient_address"], status["l1_amount"])
        print(f"  L1 wallet pays {status['l1_amount']} sats to the app's L1 address: {txid}", flush=True)
        b.update_swap_l1_txid(swap, txid, 3, L1["b_claimer"])
        claim = b.claim_swap(swap, L1["b_claimer"])
        print(f"  node B claims the app's swap: {claim}", flush=True)
        # B's own node holds the claim (only a node that saw the payment takes it in its mempool): B mines it.
        for _ in range(20):
            if (run.a.get_swap_status(swap) or {}).get("state") == "Completed":
                break
            bmm_block(b, run.main, lambda: b.getblockcount())
            time.sleep(1)
        if (run.a.get_swap_status(swap) or {}).get("state") != "Completed":
            failures.append("node B's claim of the app's swap did not reach a block")
    elif words[0] == "want-swap":
        # Swap 2: node B offers 0.5 ECX for 0.3 BTC (an open swap, 1 confirmation of its payment).
        L1["b_l1_address"] = L1["wallet"].getnewaddress()
        wait_for(lambda: run.b.balance()["available_sats"] >= 60_000_000, "B's coins", 300, 1)
        swap, txid = run.b.create_swap("Regtest", L1["b_l1_address"], 30_000_000, None, 50_000_000, 1, 1000)
        print(f"  node B offers a swap: {bytes(swap).hex()} ({txid})", flush=True)
    elif words[0] == "pay-l1":
        txid = pay_l1(words[1], int(words[2]))
        print(f"  L1 wallet pays {words[2]} sats to {words[1]}: {txid}", flush=True)
    return failures


def more_reads(a, addrs):
    reads = [("format_deposit_address", [addrs[0]])] if addrs else []
    for s in a.list_swaps()[:4]:
        reads += [("get_swap_status", [s["id"]])]
        if s.get("l2_recipient"):
            reads += [("list_swaps_by_recipient", [s["l2_recipient"]])]
    count = a.getblockcount()
    for h in range(max(0, count - 8), count):
        block = a.get_block_hash(h)
        reads += [("get_block", [block]), ("get_bmm_inclusions", [block]), ("get_block_index", [block])]
    return reads


def e2e_extra(b):
    return {"l1Url": L1["url"], "l1User": "l1", "l1Password": "l1pass", "l1Address": L1["wallet"].getnewaddress()}


if __name__ == "__main__":
    # The nodes' own L1 config (coinshift-rs' l1_rpc_configs.json) under the run's folder: none (the app keeps its own).
    tmp = next((sys.argv[i + 1] for i, x in enumerate(sys.argv) if x == "--tmp" and i + 1 < len(sys.argv)), None)
    if tmp:
        os.environ["XDG_DATA_HOME"] = str(Path(tmp).resolve() / "xdg")
    sys.exit(main(Chain(stem="coinshift", title="Coinshift", slot=255, script=HERE / "app.js", reads=READS, addresses=rpc_flags, peer_setup=peer_setup,
                        peer_pay=lambda b, to, sats: b.transfer(to, sats, 1000), peer_args=start_l1, e2e_extra=e2e_extra, on_signal=on_signal,
                        more_reads=more_reads, base_reads=lambda a, addrs: [], withdrawal=False)))
