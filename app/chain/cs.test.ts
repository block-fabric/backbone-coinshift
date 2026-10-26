// Coinshift's own pages over a pretend node: every spend only through rpc.confirmed with coinshift-rs' names and
// parameters in their order (create_swap [parent_chain, l1_recipient_address, l1_amount_sats, l2_recipient,
// l2_amount_sats, required_confirmations, fee_sats], accept_swap [swap_id, l2_claimer_address, fee_sats], claim_swap
// [swap_id, l2_claimer_address]), the L1 payment given to the app's check (coinshift.payment), no request outside the
// manifest; where a swap stands and what the wallet does next.
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { blocksLeft, formatL1, nextStep, offerProblem, parseL1, stageOf, steps, swapIdArray, type Swap } from './cs/api';

const manifest = JSON.parse(readFileSync(join(__dirname, 'manifest.json'), 'utf8')) as { reads: string[]; confirmed: string[]; host: string[] };
const APP_METHODS = new Set(['app.hello', 'app.info', 'app.locale', 'app.languages', 'prefs.get', 'prefs.set', 'node.state', 'rpc.confirmed', 'rpc.confirmedMethods', 'window.state', 'clipboard.write', 'app.notify']);
const ID = '0a'.repeat(32);
const ADDR = '4XrMw8bmnhchFspSyP4XRUuB5vaR';
const MINE = '3z9f38888UESrMg7XPZehHJrT6Nz';

const base: Swap = {
  id: ID,
  direction: 'L2ToL1',
  parentChain: 'Regtest',
  coin: 'Bitcoin Regtest',
  l1Address: 'bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080',
  l1Amount: 50_000_000,
  l2Amount: 100_000_000,
  l2Recipient: null,
  creator: ADDR,
  open: true,
  state: { kind: 'pending' },
  requiredConfirmations: 3,
  createdAt: 10,
  expiresAt: 60,
  l1Txid: '',
  reservation: null,
  escrow: 100_000_000,
  escrowOutputs: 1,
  nextHeight: 20,
  role: '',
  paidWith: null,
  canClaim: false,
};

let calls: BridgeRequest[] = [];
let answers: Record<string, (r: BridgeRequest) => unknown> = {};
let swap: Swap = { ...base };

beforeEach(() => {
  calls = [];
  swap = { ...base };
  answers = {
    'app.info': () => ({ name: 'Coinshift', hostApi: 1, network: 'regtest' }),
    'dropin.overview': () => ({ wallet: { hasSeed: true, available: 500_000_000, withdrawing: 0, pendingIn: 0, pendingOut: 0 }, chain: { height: 19, peers: 0, slot: 255, network: 'regtest' }, mainchain: { slot: 255, connected: true, state: 'ok' } }),
    'dropin.seed.status': () => ({ hasSeed: true, phraseKept: true, from: 'app', fingerprint: '00', derivation: 'coinshift 0.14.8' }),
    'dropin.checkAddress': (r) => ({ valid: /^[34]/.test(String((r.params as { address: string }).address)), mine: false }),
    'coinshift.swaps': () => [swap],
    'coinshift.swap': () => ({ ...swap, transactions: [] }),
    'coinshift.wallet': () => ({ available: 500_000_000, inSwaps: 0, hasSeed: true, claimAddress: MINE }),
    'coinshift.l1': () => [{ chain: 'Regtest', coin: 'Bitcoin Regtest', configured: true, url: 'http://127.0.0.1:18443', user: 'u' }],
    'coinshift.l1.find': () => [{ txid: 'ef'.repeat(32), amount: 50_000_000, confirmations: 1 }],
    'coinshift.payment': () => ({ txid: 'ef'.repeat(32), paid: 50_000_000, confirmations: 3, required: 3 }),
    'rpc.confirmed': (r) => ((r.params as { method: string }).method === 'create_swap' ? [swapIdArray(ID), 'cd'.repeat(32)] : 'cd'.repeat(32)),
  };
  setTransport(async (req) => {
    calls.push(req);
    const answer = answers[req.kind === 'host' ? (req.method ?? '') : `rpc:${req.method}`];
    return answer ? answer(req) : null;
  });
});
afterEach(() => setTransport(null));

const confirmedCalls = () => calls.filter((c) => c.kind === 'host' && c.method === 'rpc.confirmed').map((c) => c.params as { method: string; params: unknown[] });
const hostCalls = (m: string) => calls.filter((c) => c.kind === 'host' && c.method === m).map((c) => c.params);
const button = (root: HTMLElement, words: string) => [...root.querySelectorAll('button')].find((b) => b.textContent?.includes(words)) as HTMLButtonElement;
const type = async (root: HTMLElement, sel: string, value: string) => fireEvent.input(root.querySelector(sel) as HTMLInputElement, { target: { value } });

describe('Coinshift pages', () => {
  it('offer: create_swap [chain, L1 address, L1 sats, null (open), ECX sats, null (default confirmations), fee]', async () => {
    const { refresh } = await import('./dropin/store.svelte');
    await refresh();
    const NewSwap = (await import('./pages/NewSwap.svelte')).default;
    const { container } = render(NewSwap);
    await type(container, '#swap-l2-amount', '1');
    await type(container, '#swap-l1-amount', '0.5');
    await type(container, '#swap-l1-address', base.l1Address);
    await waitFor(() => expect(button(container, 'Review and offer').disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(button(container, 'Review and offer'));
    await waitFor(() => expect(confirmedCalls()).toEqual([{ method: 'create_swap', params: ['Regtest', base.l1Address, 50_000_000, null, 100_000_000, null, 1000] }]));
  });

  it('an address with a hidden character is refused before any question', async () => {
    const { refresh } = await import('./dropin/store.svelte');
    await refresh();
    const NewSwap = (await import('./pages/NewSwap.svelte')).default;
    const { container } = render(NewSwap);
    await type(container, '#swap-l2-amount', '1');
    await type(container, '#swap-l1-amount', '0.5');
    await type(container, '#swap-l1-address', 'bcrt1q​w508');
    await new Promise((r) => setTimeout(r, 300));
    expect(button(container, 'Review and offer').disabled).toBe(true);
    expect(confirmedCalls()).toEqual([]);
  });

  it('take an open swap: accept_swap [id as 32 numbers, the wallet’s address, fee]', async () => {
    const SwapView = (await import('./cs/SwapView.svelte')).default;
    const { refreshCoinshift } = await import('./cs/store.svelte');
    await refreshCoinshift();
    const { container } = render(SwapView, { id: ID });
    await waitFor(() => expect(button(container, 'Review and reserve')).toBeTruthy(), { timeout: 3000 });
    await waitFor(() => expect(button(container, 'Review and reserve').disabled).toBe(false));
    await fireEvent.click(button(container, 'Review and reserve'));
    await waitFor(() => expect(confirmedCalls()).toEqual([{ method: 'accept_swap', params: [swapIdArray(ID), MINE, 1000] }]));
  });

  it('reserved by the wallet: the payment found and checked by the app, then claim_swap [id, null]', async () => {
    swap = { ...base, role: 'claimer', reservation: { claimer: MINE, acceptedAt: 15, expiresAt: 35, live: true, mine: true } };
    const SwapView = (await import('./cs/SwapView.svelte')).default;
    const { refreshCoinshift } = await import('./cs/store.svelte');
    await refreshCoinshift();
    const { container } = render(SwapView, { id: ID });
    await waitFor(() => expect(container.querySelector('#cs-l1-txid')).toBeTruthy(), { timeout: 3000 });
    await fireEvent.click(button(container, 'Find payments'));
    await waitFor(() => expect((container.querySelector('#cs-l1-txid') as HTMLInputElement).value).toBe('ef'.repeat(32)));
    await fireEvent.click(button(container, 'Check the payment'));
    await waitFor(() => expect(hostCalls('coinshift.payment')).toEqual([{ id: ID, l1Txid: 'ef'.repeat(32) }]));
    expect(confirmedCalls()).toEqual([]);
    swap = { ...swap, state: { kind: 'ready' }, l1Txid: 'ef'.repeat(32), paidWith: 'ef'.repeat(32), canClaim: true };
    const again = render(SwapView, { id: ID });
    await waitFor(() => expect(button(again.container, 'Review and claim')).toBeTruthy(), { timeout: 3000 });
    await fireEvent.click(button(again.container, 'Review and claim'));
    await waitFor(() => expect(confirmedCalls()).toEqual([{ method: 'claim_swap', params: [swapIdArray(ID), null] }]));
  });

  it('call nothing of the node outside the manifest; every spend one of its confirmed', async () => {
    for (const page of ['Swaps', 'NewSwap']) render((await import(`./pages/${page}.svelte`)).default);
    render((await import('./cs/SwapView.svelte')).default, { id: ID });
    render((await import('./cs/L1Nodes.svelte')).default);
    render((await import('./LocalCards.svelte')).default);
    await new Promise((r) => setTimeout(r, 500));
    const allowed = new Set([...manifest.host, ...APP_METHODS]);
    const bad = calls.filter((c) => (c.kind === 'host' ? !allowed.has(c.method ?? '') : !manifest.reads.includes(c.method ?? '')));
    expect(bad.map((c) => `${c.kind}:${c.method}`)).toEqual([]);
    for (const c of confirmedCalls()) expect(manifest.confirmed).toContain(c.method);
  });

  it('where a swap stands, what comes next, amounts of L1', () => {
    expect(stageOf(base)).toBe('open');
    expect(nextStep(base)).toBe('reserve');
    expect(nextStep({ ...base, role: 'creator' })).toBe('wait-taker');
    expect(stageOf({ ...base, nextHeight: 60 })).toBe('expired');
    expect(nextStep({ ...base, nextHeight: 60 })).toBe('none');
    expect(stageOf({ ...base, reservation: { claimer: ADDR, acceptedAt: 15, expiresAt: 35, live: true } })).toBe('reserved');
    expect(nextStep({ ...base, reservation: { claimer: ADDR, acceptedAt: 15, expiresAt: 35, live: true } })).toBe('none');
    expect(stageOf({ ...base, state: { kind: 'confirming', current: 1, required: 3 } })).toBe('confirming');
    expect(nextStep({ ...base, role: 'recipient', open: false, l2Recipient: MINE })).toBe('pay');
    expect(nextStep({ ...base, role: 'claimer', reservation: { claimer: MINE, acceptedAt: 15, expiresAt: 35, live: true }, canClaim: true, state: { kind: 'ready' } })).toBe('claim');
    expect(steps({ ...base, state: { kind: 'completed' } }).every((s) => s.done)).toBe(true);
    expect(steps(base).find((s) => s.now)?.id).toBe('reserved');
    expect(blocksLeft(base)).toBe(40);
    expect(parseL1('0.5')).toBe(50_000_000);
    expect(parseL1('0,00000001')).toBe(1);
    expect(parseL1('0.000000001')).toBe(null);
    expect(parseL1('21000001')).toBe(null);
    expect(parseL1('-1')).toBe(null);
    expect(formatL1(50_000_000, 'BCH')).toBe('0.50000000 BCH');
    expect(swapIdArray('ff' + '00'.repeat(31))).toEqual([255, ...new Array(31).fill(0)]);
    expect(() => swapIdArray('xyz')).toThrow();
    const ok = { l1Address: 'bcrt1q', l1Amount: 1, l2Amount: 1000n, available: 10_000n, fee: 1000n, confirmations: 3 };
    expect(offerProblem(ok)).toBe('');
    expect(offerProblem({ ...ok, l2Amount: 9500n })).not.toBe('');
    expect(offerProblem({ ...ok, confirmations: 0 })).not.toBe('');
    expect(offerProblem({ ...ok, l1Address: 'a b' })).not.toBe('');
  });
});
