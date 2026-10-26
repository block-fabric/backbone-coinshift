// What the drop-in pages (chain/dropin) need to know of Coinshift.
// (No import of the drop-in pages' modules here: they read this file when they load.)
import type { HistoryRow } from './dropin/api';

/** The swap a row is about (the start of its id). */
const swapLine = (r: HistoryRow) => {
  const id = (r as HistoryRow & { swap?: string }).swap;
  return id ? `swap ${id.slice(0, 12)}…` : '';
};

const escrow = (r: HistoryRow) => {
  const x = r as HistoryRow & { swap?: string; escrowIn?: number; escrowOut?: number };
  const out: { label: string; value: string }[] = [];
  if (x.swap) out.push({ label: 'Swap', value: x.swap });
  if (x.escrowIn) out.push({ label: 'Locked for the swap', value: `${(x.escrowIn / 1e8).toFixed(8)} ECX` });
  if (x.escrowOut) out.push({ label: 'Left the swap', value: `${(x.escrowOut / 1e8).toFixed(8)} ECX` });
  return out;
};

export const DROPIN = {
  /** Its slot on eCash. */
  slot: 255,
  /** The fee a payment proposes (satoshis). */
  defaultFee: 1000,
  /** The fee on eCash a deposit or a withdrawal proposes. */
  defaultMainFee: 1000,
  /** What an address looks like, for the placeholder. */
  addressExample: '4XrMw8bmnhchFspSyP4XRUuB5vaR',
  /** coinshift-rs' names of the spends of the shared pages (the same parameters as Thunder's). */
  spend: { transfer: 'transfer', withdraw: 'withdraw' },
  /** Under the deposit address of Drivechains. */
  depositNote: 'This is an address of this wallet, as it is: give exactly this. Coinshift credits a deposit to the plain address (or its own s255_ form); one made to anything else is lost.',
  /** Coinshift's own rows of the activity (dropin.history's kinds): their words, icon, line and type filter. */
  kinds: {
    'swap-create': { text: 'Swap offered', icon: 'swap', filter: 'Swaps', line: swapLine },
    'swap-accept': { text: 'Swap reserved', icon: 'lock', line: swapLine },
    'swap-claim': { text: 'Swap claimed', icon: 'receive', line: swapLine },
    'swap-done': { text: 'Swap taken', icon: 'swap', line: swapLine },
  },
  /** The swap of a row and what it locked, in its transaction dialog. */
  rowFacts: escrow,
};
