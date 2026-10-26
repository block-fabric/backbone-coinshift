#!/usr/bin/env python3
"""Their Coinshift node (coinshift-rs v0.14.8 release binary) and ours on one local eCash regtest chain
(ecash-com bitcoind + electrs + bip300301_enforcer 17eb2ce). Local only; never on servers.

    tests/regtest/interop.py start [--dir D] [--base-port 49300]
    tests/regtest/interop.py run   [--dir D]
    tests/regtest/interop.py stop  [--dir D]     # stops exactly the recorded PIDs

Ports from --base-port: ecashd RPC +0, P2P +1, ZMQ +3, electrs +5..7, enforcer gRPC +10 / RPC +11, theirs P2P/RPC
+20/+21, ours +30/+31, a fresh theirs +40/+41 and a fresh ours +50/+51 (sync from the other implementation only).
Each node runs with its own XDG_DATA_HOME (their L1 config path) inside its datadir. Slot 255 is proposed (M1)
and acked by the enforcer.

Scenario: deposits (enforcer and create_deposit), blocks mined alternately (BMM through the enforcer), transfers
and transfer_many relayed both ways, swaps (pre-specified and open, SwapAccept, node-local update_swap_l1_txid
on both nodes, claims, a cancel, an expiry), withdrawals into a bundle (M6 proposed, votes, payout), a mainchain
reorg under a BMM block, a fresh node of each kind syncing only from the other with the same mnemonic, and after
every block every read RPC of both nodes compared byte for byte (HashMap-ordered lists as sets).
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

SKY = Path.home() / "skydoge"
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BINS = {
    "ecashd": SKY / "ext/ecash-bitcoin/build/bin/bitcoind",
    "electrs": SKY / "ext/electrs/target/release/electrs",
    "enforcer": SKY / "ext/enforcer/target/release/bip300301_enforcer",
    "theirs": Path(os.environ.get("THEIR_COINSHIFT", SKY / "ext/coinshift-release/coinshift-0.14.8-x86_64-unknown-linux-gnu")),
    "ours": Path(os.environ.get("OUR_COINSHIFT", REPO / "build/chains/coinshift/coinshift")),
}
ECASH_MAGIC = "eca5b134"
RPC_USER = RPC_PASS = "interop"
SLOT = 255


def ports(base):
    return {"ecash_rpc": base, "ecash_p2p": base + 1, "ecash_zmq": base + 3, "electrum": base + 5, "esplora": base + 6,
            "electrs_mon": base + 7, "enf_grpc": base + 10, "enf_rpc": base + 11,
            "theirs_p2p": base + 20, "theirs_rpc": base + 21, "ours_p2p": base + 30, "ours_rpc": base + 31,
            "theirs2_p2p": base + 40, "theirs2_rpc": base + 41, "ours2_p2p": base + 50, "ours2_rpc": base + 51}


def free(port):
    for fam, addr in ((socket.AF_INET, "127.0.0.1"), (socket.AF_INET, "0.0.0.0")):
        s = socket.socket(fam, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((addr, port))
        except OSError:
            return False
        finally:
            s.close()
    return True


# ------------------------------------------------------------------ clients
class RpcError(Exception):
    def __init__(self, err):
        super().__init__(json.dumps(err))
        self.err = err


def jsonrpc(port, method, params=None, timeout=600, raw=False):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params if params is not None else []}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
    if raw:
        return data
    v = json.loads(data)
    if "error" in v:
        raise RpcError(v["error"])
    return v["result"]


def btc(port, method, *params):
    body = json.dumps({"jsonrpc": "1.0", "id": 1, "method": method, "params": list(params)}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}", body, {"Content-Type": "application/json"})
    import base64
    req.add_header("Authorization", "Basic " + base64.b64encode(f"{RPC_USER}:{RPC_PASS}".encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            v = json.loads(r.read())
    except urllib.error.HTTPError as e:
        v = json.loads(e.read())
    if v.get("error"):
        raise RpcError(v["error"])
    return v["result"]


def grpc(port, method, req=None, timeout=300):
    if "." not in method.split("/")[0]:
        method = "cusf.mainchain.v1." + method
    r = urllib.request.Request(f"http://127.0.0.1:{port}/{method}", json.dumps(req or {}).encode(),
                               {"Content-Type": "application/json", "Connect-Protocol-Version": "1"})
    try:
        with urllib.request.urlopen(r, timeout=timeout) as x:
            d = x.read()
            return json.loads(d) if d else {}
    except urllib.error.HTTPError as e:
        raise RpcError({"grpc": method, "status": e.code, "body": e.read()[:300].decode(errors="replace")})


def wait_for(f, what, timeout=120, every=1.0):
    t = time.time()
    last = None
    while time.time() - t < timeout:
        try:
            v = f()
            if v:
                return v
        except Exception as e:  # noqa: BLE001
            last = e
        time.sleep(every)
    raise SystemExit(f"timeout waiting for {what} (last error: {last})")


# ------------------------------------------------------------------ processes
def pidfile(d):
    return d / "pids.json"


def read_pids(d):
    try:
        return json.loads(pidfile(d).read_text())
    except FileNotFoundError:
        return {}


def spawn(d, name, argv, env=None):
    log = open(d / "logs" / f"{name}.log", "ab")
    p = subprocess.Popen([str(a) for a in argv], stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True, env=env)
    pids = read_pids(d)
    pids[name] = p.pid
    pidfile(d).write_text(json.dumps(pids))
    return p.pid


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def stop(args):
    d = Path(args.dir).expanduser()
    st = json.loads((d / "state.json").read_text()) if (d / "state.json").exists() else {}
    p = st.get("ports", {})
    for name in ("ours", "theirs", "ours2", "theirs2"):
        try:
            jsonrpc(p[f"{name}_rpc"], "stop", timeout=5)
        except Exception:  # noqa: BLE001
            pass
    try:
        btc(p["ecash_rpc"], "stop")
    except Exception:  # noqa: BLE001
        pass
    pids = read_pids(d)
    for name, pid in pids.items():  # exact PIDs we started, SIGTERM then SIGKILL
        if alive(pid):
            os.kill(pid, signal.SIGTERM)
    t = time.time()
    while time.time() - t < 20 and any(alive(pid) for pid in pids.values()):
        time.sleep(0.5)
    for name, pid in pids.items():
        if alive(pid):
            os.kill(pid, signal.SIGKILL)
    print("stopped:", ", ".join(f"{n}={p}" for n, p in pids.items()))


def start(args):
    d = Path(args.dir).expanduser()
    if d.exists() and any(alive(p) for p in read_pids(d).values()):
        sys.exit(f"something runs from {d}: stop it first")
    for k, b in BINS.items():
        if not b.is_file():
            sys.exit(f"missing {k}: {b}")
    if d.exists():
        shutil.rmtree(d)
    (d / "logs").mkdir(parents=True)
    p = ports(args.base_port)
    busy = [n for n, x in p.items() if not free(x) or str(x).endswith("54") or 49660 <= x <= 49669 or 49700 <= x <= 49799]
    if busy:
        sys.exit(f"ports not usable: {busy}")
    st = {"ports": p}
    (d / "state.json").write_text(json.dumps(st))
    ed = d / "ecash"
    ed.mkdir()
    spawn(d, "ecashd", [BINS["ecashd"], "-regtest", f"-datadir={ed}", f"-rpcport={p['ecash_rpc']}", f"-port={p['ecash_p2p']}",
                        f"-bind=127.0.0.1:{p['ecash_p2p']}", "-rest", f"-zmqpubsequence=tcp://127.0.0.1:{p['ecash_zmq']}",
                        "-disablewallet", f"-rpcuser={RPC_USER}", f"-rpcpassword={RPC_PASS}", "-server",
                        "-listenonion=0", "-dnsseed=0", "-fixedseeds=0", "-discover=0", "-printtoconsole=0", "-txindex"])
    wait_for(lambda: btc(p["ecash_rpc"], "getblockcount") >= 0, "ecashd", 120)
    xd = d / "electrs"
    xd.mkdir()
    spawn(d, "electrs", [BINS["electrs"], f"--db-dir={xd}", f"--daemon-dir={ed}", f"--daemon-rpc-addr=127.0.0.1:{p['ecash_rpc']}",
                         f"--electrum-rpc-addr=127.0.0.1:{p['electrum']}", f"--http-addr=127.0.0.1:{p['esplora']}",
                         f"--monitoring-addr=127.0.0.1:{p['electrs_mon']}", "--network=regtest",
                         f"--cookie={RPC_USER}:{RPC_PASS}", "--jsonrpc-import"])
    wait_for(lambda: urllib.request.urlopen(f"http://127.0.0.1:{p['esplora']}/blocks/tip/height", timeout=5).read(), "electrs", 120)
    nd = d / "enforcer"
    nd.mkdir()
    spawn(d, "enforcer", [BINS["enforcer"], f"--data-dir={nd}", f"--node-rpc-addr=127.0.0.1:{p['ecash_rpc']}",
                          f"--node-rpc-user={RPC_USER}", f"--node-rpc-pass={RPC_PASS}",
                          f"--node-zmq-addr-sequence=tcp://127.0.0.1:{p['ecash_zmq']}", f"--network-magic={ECASH_MAGIC}",
                          "--op-drivechain=nop8", "--enable-wallet", "--wallet-auto-create", "--enable-mempool",
                          "--enable-block-template-server", f"--wallet-esplora-url=http://127.0.0.1:{p['esplora']}",
                          f"--serve-grpc-addr=127.0.0.1:{p['enf_grpc']}", f"--serve-rpc-addr=127.0.0.1:{p['enf_rpc']}",
                          f"--log-directory={nd / 'logs'}", "--log-level=info"])
    wait_for(lambda: "blockHeaderInfo" in grpc(p["enf_grpc"], "ValidatorService/GetChainTip"), "the enforcer", 180)
    addr = wait_for(lambda: grpc(p["enf_grpc"], "WalletService/CreateNewAddress")["address"], "the enforcer wallet", 120)
    st["enforcer_address"] = addr
    (d / "state.json").write_text(json.dumps(st))
    lab = Lab(d)
    lab.mine_main(101)
    grpc(p["enf_grpc"], "BlockProducerService/SetAckAllProposals", {"policy": "ACK_ALL_PROPOSALS_POLICY_ALL"})
    grpc(p["enf_grpc"], "BlockProducerService/SubmitSidechainProposal", {
        "sidechainId": SLOT, "declaration": {"v0": {"title": "Coinshift", "description": "interop test",
                                                     "hashId1": {"hex": "00" * 32}, "hashId2": {"hex": "00" * 20}}}})
    for _ in range(40):
        lab.mine_main(1)
        if lab.slot_active():
            break
    if not lab.slot_active():
        sys.exit("slot 255 did not activate")
    print(f"slot {SLOT} active at mainchain height {btc(p['ecash_rpc'], 'getblockcount')}")
    for who in ("theirs", "ours"):
        (d / who).mkdir()
        spawn_node(d, who, p)
    for who in ("theirs", "ours"):
        wait_for(lambda: jsonrpc(p[who + "_rpc"], "getblockcount", timeout=5) is not None, f"{who} RPC", 120)
        m = jsonrpc(p[who + "_rpc"], "generate_mnemonic")
        jsonrpc(p[who + "_rpc"], "set_seed_from_mnemonic", [m])
        (d / f"{who}.mnemonic").write_text(m)
    print("both nodes up; next: interop.py run")


def spawn_node(d, who, p, period_blocks=None, add_peer=True, name=None):
    name = name or who
    dd = d / name
    dd.mkdir(exist_ok=True)
    kind = "ours" if name.startswith("ours") else "theirs"
    other = "ours" if who == "theirs" else "theirs"
    argv = [BINS[kind], "--headless", "--datadir", dd, "--network", "regtest",
            "--mainchain-grpc-url", f"http://127.0.0.1:{p['enf_grpc']}",
            "--net-addr", f"127.0.0.1:{p[name + '_p2p']}", "--rpc-addr", f"127.0.0.1:{p[name + '_rpc']}",
            "--log-level", "debug" if kind == "ours" else "info"]
    if add_peer:
        argv += ["--add-peer", f"127.0.0.1:{p[other + '_p2p']}"]
    if kind == "ours":
        argv += ["--bt-no-seeds"]
    env = dict(os.environ, XDG_DATA_HOME=str(dd / "xdg"))
    spawn(d, name, argv, env=env)


def restart(args):
    """Restart one node with the same datadir and flags (e.g. ours after a rebuild)."""
    d = Path(args.dir).expanduser()
    st = json.loads((d / "state.json").read_text())
    pid = read_pids(d).get(args.node)
    if pid and alive(pid):
        try:
            jsonrpc(st["ports"][args.node + "_rpc"], "stop", timeout=5)
        except Exception:  # noqa: BLE001
            pass
        t = time.time()
        while alive(pid) and time.time() - t < 30:
            time.sleep(0.5)
        if alive(pid):
            os.kill(pid, signal.SIGTERM)
    spawn_node(d, args.node, st["ports"])
    wait_for(lambda: jsonrpc(st["ports"][args.node + "_rpc"], "getblockcount", timeout=5) is not None, "RPC", 120)
    print(f"{args.node} restarted")


# ------------------------------------------------------------------ the scenario
class Lab:
    def __init__(self, d):
        self.d = d
        self.st = json.loads((d / "state.json").read_text())
        self.p = self.st["ports"]
        self.checks = 0
        self.diffs = []
        self.swaps = []
        self.wallet_issues = []

    def node(self, who, method, params=None, **kw):
        return jsonrpc(self.p[who + "_rpc"], method, params, **kw)

    def mine_main(self, n=1):
        r = grpc(self.p["enf_grpc"], "MiningService/GenerateToAddress", {"blocks": n, "address": self.st["enforcer_address"]})
        return [h["hex"] for h in r.get("blockHashes", [])]

    def slot_active(self):
        r = grpc(self.p["enf_grpc"], "ValidatorService/GetSidechains")
        return any(s.get("sidechainNumber") == SLOT for s in r.get("sidechains", []))

    def mine_side(self, who):
        before = self.node(who, "getblockcount")
        err = []

        def run():
            try:
                self.node(who, "mine", [None])
            except Exception as e:  # noqa: BLE001
                err.append(e)
        t = threading.Thread(target=run)
        t.start()
        time.sleep(1.5)
        self.mine_main(1)
        t.join(120)
        if err:
            raise SystemExit(f"mine on {who}: {err[0]}")
        wait_for(lambda: self.node(who, "getblockcount") > before, f"a block from {who}", 60, 0.5)
        return self.synced()

    def synced(self, a="theirs", b="ours", timeout=90):
        def same():
            x = self.node(a, "get_best_sidechain_block_hash")
            y = self.node(b, "get_best_sidechain_block_hash")
            return x if x == y and x is not None else None
        return wait_for(same, f"one sidechain tip on {a} and {b}", timeout, 0.5)

    READS = [("getblockcount", []), ("get_best_sidechain_block_hash", []), ("get_best_mainchain_block_hash", []),
             ("sidechain_wealth", []), ("pending_withdrawal_bundle", []), ("latest_failed_withdrawal_bundle_height", []),
             ("list_swaps", []), ("get_block_hash", [0]), ("get_block_hash", [1]), ("get_block_hash", [100000])]
    UNORDERED = {"list_utxos", "list_mempool"}

    def compare(self, label, a="theirs", b="ours"):
        tip = self.synced(a, b)
        reads = list(self.READS)
        reads += [("get_block", [tip]), ("get_block_index", [tip]), ("get_bmm_inclusions", [tip]),
                  ("list_utxos", []), ("list_mempool", [])]
        for s in self.node(a, "list_swaps"):
            reads.append(("get_swap_status", [s["id"]]))
            if s.get("l2_recipient"):
                reads.append(("list_swaps_by_recipient", [s["l2_recipient"]]))
        for method, params in reads:
            if method in self.UNORDERED:
                ra = sorted(json.dumps(x, sort_keys=True) for x in jsonrpc(self.p[a + "_rpc"], method, params))
                rb = sorted(json.dumps(x, sort_keys=True) for x in jsonrpc(self.p[b + "_rpc"], method, params))
                self.checks += 1
                if ra != rb:
                    self.diffs.append((label, method, params, str(ra)[:500], str(rb)[:500]))
                continue
            try:
                x = jsonrpc(self.p[a + "_rpc"], method, params, raw=True)
            except Exception as e:  # noqa: BLE001
                x = f"EXC {e}".encode()
            try:
                y = jsonrpc(self.p[b + "_rpc"], method, params, raw=True)
            except Exception as e:  # noqa: BLE001
                y = f"EXC {e}".encode()
            self.checks += 1
            if x != y:
                self.diffs.append((label, method, params, x[:500], y[:500]))

    def report(self):
        print(f"{self.checks} checks, {len(self.diffs)} differences; wallet issues: {self.wallet_issues}")
        for d in self.diffs[:60]:
            print("DIFF", d)
        (self.d / "diffs.json").write_text(json.dumps([[str(x) for x in d] for d in self.diffs], indent=1))


def run(args):
    lab = Lab(Path(args.dir).expanduser())
    try:
        scenario(lab)
    finally:
        lab.report()


def other_of(w):
    return "theirs" if w == "ours" else "ours"


def relay_and_mine(lab, frm, txid, label):
    to = other_of(frm)
    wait_for(lambda: any(t["txid"] == txid for t in lab.node(to, "list_mempool")), f"{label} in {to}'s mempool", 60, 0.5)
    lab.mine_side(to)
    lab.compare(label)


def write(lab, who, method, params):
    """A wallet write; their wallet can keep a coin the chain no longer has (utreexo prove error), then the
    other node does the action (recorded)."""
    try:
        return who, lab.node(who, method, params)
    except RpcError as e:
        if "utreexo error" not in str(e.err):
            raise
        lab.wallet_issues.append((who, method, e.err))
        print(f"  {who} {method}: {e.err['message']} -> done by {other_of(who)}")
        return other_of(who), lab.node(other_of(who), method, params)


def mark_filled(lab, swap_id, conf=6):
    """The node-local L1 observation (their update_swap_l1_txid RPC), the same on both nodes."""
    import hashlib
    l1 = hashlib.sha256(bytes(swap_id)).hexdigest()
    for w in ("theirs", "ours"):
        lab.node(w, "update_swap_l1_txid", [swap_id, l1, conf, None])
    lab.compare(f"swap {bytes(swap_id).hex()[:12]} filled (node-local)")


def scenario(lab):
    p = lab.p
    for who in ("theirs", "ours"):
        a = lab.node(who, "get_new_address")
        r = grpc(p["enf_grpc"], "WalletService/CreateDepositTransaction",
                 {"sidechainId": SLOT, "address": lab.node(who, "format_deposit_address", [a]),
                  "valueSats": str(5 * 100_000_000), "feeSats": "10000"})
        print(f"deposit to {who}: {r['txid']['hex'][:16]}..")
        lab.mine_main(1)
    # a deposit through the node's own create_deposit (bare address, as theirs sends it)
    txid = lab.node("ours", "create_deposit", [lab.node("ours", "get_new_address"), 100_000_000, 10_000])
    print("create_deposit (ours):", txid[:16])
    lab.mine_main(1)
    who = "ours"
    for i in range(4):
        lab.mine_side(who)
        lab.compare(f"genesis+{i}")
        who = other_of(who)
    for w in ("theirs", "ours"):
        print(w, "balance", lab.node(w, "balance"))
    for frm in ("theirs", "ours"):
        dest = lab.node(other_of(frm), "get_new_address")
        txid = lab.node(frm, "transfer", [dest, 12_345_678, 1_000])
        relay_and_mine(lab, frm, txid, f"transfer {frm}")
    d1, d2 = lab.node("theirs", "get_new_address"), lab.node("ours", "get_new_address")
    txid = lab.node("ours", "transfer_many", [{d1: 1_000_000, d2: 2_000_000}, 500])
    relay_and_mine(lab, "ours", txid, "transfer_many")
    swaps(lab)
    withdrawals(lab)
    mainchain_reorg(lab)
    fresh_nodes(lab)
    # node-local cancel (last: it makes the two nodes' swap tables differ by design)
    for w in ("theirs", "ours"):
        try:
            lab.node(w, "cancel_swap", [lab.cancel_later])
            print("cancel_swap on", w, "ok")
        except RpcError as e:
            print("cancel_swap on", w, e.err["message"])


def swaps(lab):
    # 1. pre-specified swap created by theirs for ours' address; both nodes see the fill; ours claims
    rec = lab.node("ours", "get_new_address")
    sid, txid = lab.node("theirs", "create_swap", ["Regtest", "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080", 40_000, rec, 3_000_000, None, 1_000])
    lab.swaps.append(sid)
    relay_and_mine(lab, "theirs", txid, "swap create (pre-specified, theirs)")
    mark_filled(lab, sid)
    txid = lab.node("ours", "claim_swap", [sid, None])
    relay_and_mine(lab, "ours", txid, "swap claim (ours)")
    # 2. open swap by ours; theirs reserves (SwapAccept), both see the fill, theirs claims
    sid2, txid = lab.node("ours", "create_swap", ["Regtest", "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080", 55_000, None, 2_000_000, 3, 1_000])
    lab.swaps.append(sid2)
    relay_and_mine(lab, "ours", txid, "swap create (open, ours)")
    txid = lab.node("theirs", "accept_swap", [sid2, None, 500])
    relay_and_mine(lab, "theirs", txid, "swap accept (theirs)")
    mark_filled(lab, sid2, 2)  # WaitingConfirmations(2, 3)
    mark_conf = None
    try:
        lab.node("theirs", "claim_swap", [sid2, None])
    except RpcError as e:
        mark_conf = e.err
    print("claim before enough confirmations:", mark_conf)
    # 3. a swap the creator cancels (node-local), and one that expires (Regtest: 50 blocks)
    w3, (sid3, txid) = write(lab, "theirs", "create_swap", ["Regtest", "bcrt1qmisc", 1_000, None, 100_000, None, 1_000])
    lab.swaps.append(sid3)
    relay_and_mine(lab, w3, txid, "swap create (to expire)")
    w4, (sid4, txid) = write(lab, "ours", "create_swap", ["Signet", "tb1qcancel", 2_000, None, 200_000, None, 1_000])
    lab.swaps.append(sid4)
    relay_and_mine(lab, w4, txid, "swap create (to cancel)")
    lab.cancel_later = sid4
    who = "ours"
    for i in range(52):
        lab.mine_side(who)
        if i % 4 == 0 or i > 46:
            lab.compare(f"to expiry {i}")
        who = other_of(who)


def withdrawals(lab):
    main_addr = "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
    for w in ("theirs", "ours"):
        _, txid = write(lab, w, "withdraw", [main_addr, 1_000_000, 1_000, 5_000])
        print(f"withdrawal from {w}: {txid[:16]}..")
    who = "ours"
    for i in range(4):
        lab.mine_side(who)
        lab.compare(f"withdrawal {i}")
        who = other_of(who)
    for i in range(16):
        lab.mine_main(1)
        if i % 4 == 3:
            lab.mine_side(who)
            lab.compare(f"bundle votes {i}")
            who = other_of(who)
    print("  latest failed bundle height:", lab.node("theirs", "latest_failed_withdrawal_bundle_height"),
          "pending:", bool(lab.node("theirs", "pending_withdrawal_bundle")), "wealth", lab.node("theirs", "sidechain_wealth"))


def mainchain_reorg(lab):
    lab.mine_side("ours")
    side_tip = lab.synced()
    main_tip = btc(lab.p["ecash_rpc"], "getbestblockhash")
    btc(lab.p["ecash_rpc"], "invalidateblock", main_tip)
    btc(lab.p["ecash_rpc"], "generatetoaddress", 2, lab.st["enforcer_address"])
    best = btc(lab.p["ecash_rpc"], "getbestblockhash")
    wait_for(lambda: grpc(lab.p["enf_grpc"], "ValidatorService/GetChainTip")["blockHeaderInfo"]["blockHash"]["hex"] == best,
             "the enforcer on the reorged mainchain", 180, 2)
    lab.mine_side("theirs")
    lab.compare("after mainchain reorg")
    print(f"mainchain reorg: side tip before {side_tip[:16]}.., now {lab.node('theirs', 'get_best_sidechain_block_hash')[:16]}..")


def fresh_nodes(lab):
    """A fresh theirs syncing only from ours, a fresh ours only from theirs; same mnemonic -> same addresses."""
    d, p = lab.d, lab.p
    m = (d / "ours.mnemonic").read_text()
    for name, src in (("theirs2", "ours"), ("ours2", "theirs")):
        argv_peer = f"127.0.0.1:{p[src + '_p2p']}"
        spawn_node(d, src, p, add_peer=False, name=name)
        wait_for(lambda: jsonrpc(p[name + "_rpc"], "getblockcount", timeout=5) is not None, f"{name} RPC", 120)
        jsonrpc(p[name + "_rpc"], "connect_peer", [argv_peer])
        jsonrpc(p[name + "_rpc"], "set_seed_from_mnemonic", [m])
    for name, src in (("theirs2", "ours"), ("ours2", "theirs")):
        tip = lab.node(src, "get_best_sidechain_block_hash")
        wait_for(lambda: jsonrpc(p[name + "_rpc"], "get_best_sidechain_block_hash") == tip, f"{name} synced from {src}", 600, 2)
        print(f"{name} synced from {src} to {tip[:16]}..")
    lab.compare("fresh theirs2 vs ours", "theirs2", "ours")
    lab.compare("fresh ours2 vs theirs", "theirs", "ours2")
    for n in ("theirs2", "ours2"):
        for _ in range(3):
            jsonrpc(p[n + "_rpc"], "get_new_address")
    a = jsonrpc(p["theirs2_rpc"], "get_wallet_addresses")
    b = jsonrpc(p["ours2_rpc"], "get_wallet_addresses")
    lab.checks += 1
    if a != b:
        lab.diffs.append(("same mnemonic", "get_wallet_addresses", [], str(a), str(b)))
    print("same mnemonic, addresses equal:", a == b, a[:2])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["start", "run", "stop", "restart"])
    ap.add_argument("--node", default="ours", choices=["ours", "theirs"])
    ap.add_argument("--dir", default=os.environ.get("COINSHIFT_INTEROP_DIR", "~/.coinshift-interop"))
    ap.add_argument("--base-port", type=int, default=49300)
    args = ap.parse_args()
    {"start": start, "run": run, "stop": stop, "restart": restart}[args.cmd](args)


if __name__ == "__main__":
    main()
