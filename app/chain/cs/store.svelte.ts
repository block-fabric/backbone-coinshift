// The state of Coinshift's pages, read again when the node says something changed (a new tip, the wallet) and every
// 15 seconds while the app runs: the swaps as this node sees them, the wallet's part in them, the L1 nodes the app
// checks payments on. One refresh at a time.
import { on } from '../../lib/bridge';
import { errorText } from '../../lib/dialogs.svelte';
import { drop, follow } from '../dropin/store.svelte';
import { csApi, type CsWallet, type L1Node, type Swap } from './api';

export const cs = $state({
  swaps: [] as Swap[],
  wallet: null as CsWallet | null,
  l1: [] as L1Node[],
  loaded: false,
  error: '',
  /** Bumped by every refresh: the pages that read more read it again. */
  version: 0,
});

let running: Promise<void> | null = null;
let again = false;

export function refreshCoinshift(): Promise<void> {
  if (running) {
    again = true;
    return running;
  }
  running = (async () => {
    try {
      do {
        again = false;
        await once();
      } while (again);
    } finally {
      running = null;
    }
  })();
  return running;
}

async function once(): Promise<void> {
  try {
    const swaps = await csApi.swaps();
    cs.swaps = Array.isArray(swaps) ? swaps : [];
    cs.l1 = await csApi.l1().catch(() => [] as L1Node[]);
    cs.wallet = drop.seed?.hasSeed ? await csApi.wallet().catch(() => null) : null;
    cs.error = '';
  } catch (e) {
    cs.error = errorText(e);
  }
  cs.loaded = true;
  cs.version++;
}

let started = false;

/** Follow the chain (a new tip, the wallet's transactions and balance: the node's events); the pages and cards call it. */
export function startCoinshift(): void {
  follow();
  if (started) return;
  started = true;
  let timer: ReturnType<typeof setTimeout> | undefined;
  const soon = () => {
    if (timer) return;
    timer = setTimeout(() => {
      timer = undefined;
      void refreshCoinshift();
    }, 300);
  };
  for (const type of ['tip', 'wallet.balance', 'wallet.tx', 'wallet.loaded', 'wallet.unloaded']) on(type, soon);
  setInterval(() => void refreshCoinshift(), 15_000);
  void refreshCoinshift();
}

/** Whether the app can check payments of this chain (an L1 node set in Settings → L1 nodes). */
export function l1Ready(chain: string): boolean {
  return cs.l1.some((n) => n.chain === chain && n.configured);
}
