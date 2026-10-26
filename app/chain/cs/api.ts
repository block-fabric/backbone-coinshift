// Coinshift in the app: what the node's app layer says of the swaps (chains/coinshift/src/appui.cpp, coinshift.*), its
// spends (rpc.confirmed with coinshift-rs' own names and parameters: create_swap, accept_swap, claim_swap), and what the
// pages work out of it: where a swap stands, what this wallet can do with it next, amounts of the other chain. Pure
// functions besides the calls: cs.test.ts tries them. Amounts are satoshis (of ECX on Coinshift, of the L1 coin there).
import { request } from '../../lib/bridge';
import { t } from '../../lib/i18n.svelte';
import { confirmed } from '../dropin/api';

export type ParentChain = 'BTC' | 'BCH' | 'LTC' | 'Signet' | 'Regtest';
export const PARENT_CHAINS: readonly ParentChain[] = ['BTC', 'BCH', 'LTC', 'Signet', 'Regtest'];

export interface SwapState {
  kind: 'pending' | 'confirming' | 'ready' | 'completed' | 'cancelled';
  current?: number;
  required?: number;
}

export interface Reservation {
  claimer: string;
  acceptedAt: number;
  expiresAt: number;
  live: boolean;
  mine?: boolean;
}

export interface Swap {
  id: string;
  direction: 'L2ToL1' | 'L1ToL2';
  parentChain: ParentChain;
  coin: string;
  l1Address: string;
  l1Amount: number;
  l2Amount: number;
  l2Recipient: string | null;
  creator: string | null;
  open: boolean;
  state: SwapState;
  requiredConfirmations: number;
  createdAt: number;
  expiresAt: number | null;
  l1Txid: string;
  reservation: Reservation | null;
  escrow: number;
  escrowOutputs: number;
  nextHeight: number;
  /** The wallet's part (only to a reader of the wallet). */
  role?: '' | 'creator' | 'recipient' | 'claimer';
  /** The L1 payment the app checked for it (the wallet's own). */
  paidWith?: string | null;
  canClaim?: boolean;
  transactions?: { txid: string; kind: 'create' | 'accept' | 'claim'; height: number; claimer?: string }[];
}

export interface L1Node {
  chain: ParentChain;
  coin: string;
  configured: boolean;
  url?: string;
  user?: string;
}

export interface L1Output {
  txid: string;
  amount: number;
  confirmations?: number;
}

export interface Payment {
  txid: string;
  paid: number;
  confirmations: number;
  required: number;
  swap?: Swap;
}

export interface CsWallet {
  available: number;
  inSwaps: number;
  hasSeed: boolean;
  claimAddress?: string;
}

const host = <T>(method: string, params: Record<string, unknown> = {}) => request<T>('host', method, params);

export const csApi = {
  swaps: () => host<Swap[]>('coinshift.swaps'),
  swap: (id: string) => host<Swap>('coinshift.swap', { id }),
  wallet: () => host<CsWallet>('coinshift.wallet'),
  l1: () => host<L1Node[]>('coinshift.l1'),
  setL1: (chain: ParentChain, url: string, user: string, password: string) => host<boolean>('coinshift.l1.set', { chain, url, user, password }),
  removeL1: (chain: ParentChain) => host<boolean>('coinshift.l1.set', { chain, remove: true }),
  find: (id: string) => host<L1Output[]>('coinshift.l1.find', { id }),
  payment: (id: string, l1Txid: string) => host<Payment>('coinshift.payment', { id, l1Txid }),
};

/** A swap's id as coinshift-rs' RPC takes it: its 32 bytes as an array of numbers. */
export function swapIdArray(hex: string): number[] {
  if (!/^[0-9a-f]{64}$/i.test(hex)) throw new Error('not a swap id');
  const out: number[] = [];
  for (let i = 0; i < 64; i += 2) out.push(parseInt(hex.slice(i, i + 2), 16));
  return out;
}

/** The spends, coinshift-rs' names and parameters (in their order). */
export const csSpend = {
  create: (chain: ParentChain, l1Address: string, l1Amount: number, l2Recipient: string | null, l2Amount: number, confirmations: number | null, fee: number) =>
    confirmed<[number[], string]>('create_swap', [chain, l1Address, l1Amount, l2Recipient, l2Amount, confirmations, fee]),
  accept: (id: string, claimer: string, fee: number) => confirmed<string>('accept_swap', [swapIdArray(id), claimer, fee]),
  claim: (id: string, claimer: string | null) => confirmed<string>('claim_swap', [swapIdArray(id), claimer]),
};

/** The L1 coin's ticker. */
export function ticker(c: ParentChain): string {
  return c === 'BCH' ? 'BCH' : c === 'LTC' ? 'LTC' : 'BTC';
}

/** What the chain asks by default: confirmations of the payment, and blocks of Coinshift before a swap expires. */
export function defaults(c: ParentChain): { confirmations: number; expiry: number; reserve: number } {
  return {
    confirmations: c === 'BTC' ? 6 : 3,
    expiry: c === 'BTC' ? 1008 : c === 'Regtest' ? 50 : 432,
    reserve: c === 'BTC' ? 144 : c === 'Regtest' ? 20 : 72,
  };
}

/** The parent chain a swap most likely is on for this node's network (the creator may choose another). */
export function defaultChain(network: string): ParentChain {
  return network === 'regtest' ? 'Regtest' : network === 'signet' ? 'Signet' : 'BTC';
}

/** An L1 amount the user typed ("0.5", "0,25"), as satoshis; null when it is not one (at most 8 decimals). */
export function parseL1(text: string): number | null {
  const s = text.trim().replace(',', '.');
  if (!/^\d{1,8}(\.\d{1,8})?$/.test(s)) return null;
  const [w, f = ''] = s.split('.');
  const v = Number(w) * 1e8 + Number(f.padEnd(8, '0'));
  return Number.isSafeInteger(v) && v > 0 && v <= 21_000_000 * 1e8 ? v : null;
}

/** Satoshis of the L1 coin, with its ticker. */
export function formatL1(sats: number, c: ParentChain): string {
  const neg = sats < 0;
  const a = Math.abs(Math.trunc(sats));
  return `${neg ? '−' : ''}${Math.floor(a / 1e8)}.${String(a % 1e8).padStart(8, '0')} ${ticker(c)}`;
}

/** The price: coins of L1 for one ECX. */
export function rate(s: Pick<Swap, 'l1Amount' | 'l2Amount'>): number {
  return s.l2Amount > 0 ? s.l1Amount / s.l2Amount : 0;
}

export type Stage = 'open' | 'reserved' | 'for-recipient' | 'confirming' | 'ready' | 'completed' | 'expired' | 'cancelled';

/** Where a swap stands, in one word. */
export function stageOf(s: Swap): Stage {
  const expired = s.expiresAt !== null && s.nextHeight >= s.expiresAt;
  switch (s.state.kind) {
    case 'completed':
      return 'completed';
    case 'cancelled':
      return expired ? 'expired' : 'cancelled';
    case 'ready':
      return 'ready';
    case 'confirming':
      return 'confirming';
    default:
      if (expired) return 'expired';
      if (!s.open) return 'for-recipient';
      return s.reservation?.live ? 'reserved' : 'open';
  }
}

export function stageText(st: Stage): string {
  switch (st) {
    case 'open':
      return t('Open');
    case 'reserved':
      return t('Reserved');
    case 'for-recipient':
      return t('For one recipient');
    case 'confirming':
      return t('L1 payment confirming');
    case 'ready':
      return t('Ready to claim');
    case 'completed':
      return t('Completed');
    case 'expired':
      return t('Expired');
    case 'cancelled':
      return t('Cancelled');
  }
}

/** The steps of a swap and how far it went: offered, reserved (open swaps), paid on L1, confirmed, claimed. */
export function steps(s: Swap): { id: string; label: string; done: boolean; now: boolean }[] {
  const st = stageOf(s);
  const paid = !!s.l1Txid || st === 'confirming' || st === 'ready' || st === 'completed';
  const confirmed = st === 'ready' || st === 'completed';
  const list = [
    { id: 'offered', label: t('Offered'), done: true },
    ...(s.open ? [{ id: 'reserved', label: t('Reserved'), done: !!s.reservation || paid }] : []),
    { id: 'paid', label: t('Paid on L1'), done: paid },
    { id: 'confirmed', label: t('Confirmed'), done: confirmed },
    { id: 'claimed', label: t('Claimed'), done: st === 'completed' },
  ];
  const firstOpen = list.findIndex((x) => !x.done);
  const over = st === 'expired' || st === 'cancelled';
  return list.map((x, i) => ({ ...x, now: !over && i === firstOpen }));
}

export type NextStep = 'reserve' | 'pay' | 'claim' | 'wait-taker' | 'wait-confirmations' | 'none';

/** What this wallet can do with a swap now. */
export function nextStep(s: Swap): NextStep {
  const st = stageOf(s);
  if (st === 'completed' || st === 'expired' || st === 'cancelled') return 'none';
  if (s.role === 'creator') return 'wait-taker';
  if (s.canClaim) return 'claim';
  const entitled = s.role === 'recipient' || (s.role === 'claimer' && !!s.reservation?.live);
  if (entitled) return st === 'confirming' && s.paidWith ? 'wait-confirmations' : 'pay';
  if (st === 'open' && !s.role) return 'reserve';
  return 'none';
}

/** Blocks left before the swap expires (null: it does not). */
export function blocksLeft(s: Pick<Swap, 'expiresAt' | 'nextHeight'>): number | null {
  if (s.expiresAt === null) return null;
  return Math.max(0, s.expiresAt - s.nextHeight);
}

/** Why a new swap cannot be offered as typed ('' when it can). */
export function offerProblem(o: { l1Address: string; l1Amount: number | null; l2Amount: bigint | null; available: bigint; fee: bigint; confirmations: number }): string {
  if (!o.l1Address.trim()) return t('Give your address on the other chain: you are paid there.');
  if (/\s/.test(o.l1Address.trim())) return t('An address has no space.');
  if (!o.l1Amount) return t('Ask for an amount of the other chain’s coin.');
  if (!o.l2Amount || o.l2Amount <= 0n) return t('Offer some ECX.');
  if (o.l2Amount + o.fee > o.available) return t('Not enough available for this offer and its fee.');
  if (!Number.isInteger(o.confirmations) || o.confirmations < 1 || o.confirmations > 1000) return t('Confirmations: 1 to 1000.');
  return '';
}
