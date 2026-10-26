<script lang="ts">
  // One swap: its two sides, where it stands (offered, reserved, paid on L1, confirmed, claimed), its transactions on
  // Coinshift, and what this wallet can do next. Taking an open swap: reserve it (accept_swap), pay its coins from your
  // own L1 wallet, give the app that payment (it checks it on your L1 node, Settings → L1 nodes, then tells the node as
  // update_swap_l1_txid does), and claim the ECX once the payment has its confirmations (claim_swap). The app claims
  // only a swap whose payment it saw on L1 itself: Coinshift's chain does not check it (each node looks at L1 itself).
  import Amount from '../../components/Amount.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import { refresh } from '../dropin/store.svelte';
  import { barWidth } from './actions';
  import { blocksLeft, csApi, defaults, csSpend, formatL1, nextStep, rate, stageOf, stageText, steps, ticker, type L1Output, type Swap } from './api';
  import './cs.css';
  import { cs, l1Ready, refreshCoinshift } from './store.svelte';

  let { id }: { id: string } = $props();

  const FEE = 1000;
  let swap = $state<Swap | null>(null);
  let error = $state('');
  let busy = $state(false);
  let txid = $state('');
  let found = $state<L1Output[] | null>(null);
  let checked = $state('');

  $effect(() => {
    void cs.version;
    csApi
      .swap(id)
      .then((s) => {
        swap = s;
        if (!txid && s.paidWith) txid = s.paidWith;
      })
      .catch((e) => (error = errorText(e)));
  });

  const st = $derived(swap ? stageOf(swap) : 'open');
  const next = $derived(swap ? nextStep(swap) : 'none');
  const ready = $derived(swap ? l1Ready(swap.parentChain) : false);
  const left = $derived(swap ? blocksLeft(swap) : null);

  async function act(f: () => Promise<unknown>, done: string) {
    busy = true;
    error = '';
    try {
      await f();
      toast('success', done, t('With the next block of Coinshift.'));
      await refresh();
      await refreshCoinshift();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }

  const reserve = () => act(() => csSpend.accept(id, cs.wallet?.claimAddress ?? '', FEE), t('Swap reserved'));
  const claim = () => act(() => csSpend.claim(id, null), t('Swap claimed'));

  async function find() {
    busy = true;
    error = '';
    try {
      found = await csApi.find(id);
      if (found.length === 1 && !txid) txid = found[0].txid;
    } catch (e) {
      error = errorText(e);
    } finally {
      busy = false;
    }
  }

  async function check() {
    busy = true;
    error = '';
    checked = '';
    try {
      const p = await csApi.payment(id, txid.trim());
      checked = p.confirmations >= p.required ? t('Seen on L1 with %1 confirmations: claim the ECX.', p.confirmations) : t('Seen on L1: %1 of %2 confirmations. Check again once it has more.', p.confirmations, p.required);
      if (p.swap) swap = { ...swap, ...p.swap } as Swap;
      await refreshCoinshift();
    } catch (e) {
      error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="view">
  <button type="button" class="btn ghost back" onclick={() => go('swaps', '')}><Icon name="receive" size={15} /> {t('All swaps')}</button>
  {#if !swap}
    {#if error}<p class="cs-problems" role="alert"><SafeText text={error} /></p>{:else}<p class="cs-empty">{t('Asking the node…')}</p>{/if}
  {:else}
    <header class="page-head head">
      <div>
        <h1>{t('Swap')} <span class="mono id" translate="no">{swap.id.slice(0, 16)}…</span> <CopyButton text={swap.id} label={t('Copy the swap id')} /></h1>
        <p class="muted sub">
          <span class="cs-stage {st === 'open' || st === 'ready' ? 'live' : st === 'completed' ? 'done' : st === 'expired' || st === 'cancelled' ? 'bad' : 'vote'}">{stageText(st)}</span>
          {swap.coin} · {swap.open ? t('open to anyone') : t('for one recipient')}
          {#if left !== null && st !== 'completed'} · {left > 0 ? t('expires in %1 blocks', left) : t('expired')}{/if}
        </p>
      </div>
    </header>

    <section class="panel sides">
      <div class="cs-sides">
        <div class="cs-side"><span class="muted small">{t('The taker gets on Coinshift')}</span><strong><Amount sats={swap.l2Amount} /></strong><span class="faint small">{t('Locked for it now:')} <Amount sats={swap.escrow} /></span></div>
        <Icon name="swap" size={22} />
        <div class="cs-side l1"><span class="muted small">{t('The taker pays on %1', swap.coin)}</span><strong translate="no">{formatL1(swap.l1Amount, swap.parentChain)}</strong><span class="faint small num">{t('%1 %2 per ECX', rate(swap).toFixed(8), ticker(swap.parentChain))}</span></div>
      </div>
      <ol class="cs-steps" aria-label={t('Where the swap stands')}>
        {#each steps(swap) as s, i (s.id)}<li class:done={s.done} class:now={s.now}><span class="dot">{s.done ? '✓' : i + 1}</span>{s.label}</li>{/each}
      </ol>
      {#if swap.state.kind === 'confirming' && swap.state.required}
        <div class="conf">
          <span class="cs-bar"><span use:barWidth={(swap.state.current ?? 0) / swap.state.required}></span></span>
          <span class="faint small num">{t('%1 of %2 L1 confirmations (as this node last saw them)', swap.state.current ?? 0, swap.state.required)}</span>
        </div>
      {/if}
    </section>

    <div class="cols">
      <section class="panel">
        <div class="panel-head"><h2>{t('Next')}</h2></div>
        <div class="panel-body stack">
          {#if error}<p class="cs-problems" role="alert"><SafeText text={error} /></p>{/if}
          {#if next === 'reserve'}
            <p class="cs-help">{t('Reserve it first: for %1 blocks only this wallet may take it. Then pay on %2 and claim here.', defaults(swap.parentChain).reserve, swap.coin)}</p>
            <div class="row"><button type="button" id="cs-reserve" class="btn primary" disabled={busy || !cs.wallet?.claimAddress} onclick={reserve}>{t('Review and reserve…')}</button></div>
          {:else if next === 'pay' || next === 'wait-confirmations'}
            <p class="cs-help">{t('From your own %1 wallet, pay exactly this to this address. Then give the app the transaction id: it checks it on your L1 node.', swap.coin)}</p>
            <dl class="facts">
              <dt>{t('Pay')}</dt><dd><strong translate="no">{formatL1(swap.l1Amount, swap.parentChain)}</strong></dd>
              <dt>{t('To')}</dt><dd class="addr"><span class="mono break" translate="no">{swap.l1Address}</span><CopyButton text={swap.l1Address} label={t('Copy the L1 address')} /></dd>
              {#if swap.reservation?.live}<dt>{t('Reserved for you until')}</dt><dd class="num">{t('block %1', swap.reservation.expiresAt)}</dd>{/if}
            </dl>
            {#if !ready}
              <p class="side-note side-warn" role="status">{t('No %1 node is set up in the app: it needs one to see your payment.', swap.coin)} <button type="button" class="linkish" onclick={() => go('settings', 'l1nodes')}>{t('Settings → L1 nodes')}</button></p>
            {:else}
              <label class="field"><span>{t('Your L1 transaction id')}</span><input id="cs-l1-txid" type="text" autocomplete="off" spellcheck="false" placeholder="64 hex digits" bind:value={txid} /></label>
              <div class="row">
                <button type="button" class="btn" disabled={busy} onclick={find}><Icon name="search" size={15} /> {t('Find payments to this address')}</button>
                <button type="button" id="cs-check" class="btn primary" disabled={busy || !/^[0-9a-fA-F]{64}$/.test(txid.trim())} onclick={check}>{t('Check the payment')}</button>
              </div>
              {#if found}
                {#if found.length === 0}<p class="cs-help">{t('Your L1 node sees no unspent payment of this amount to this address yet.')}</p>{/if}
                <ul class="found">{#each found as f (f.txid)}<li><button type="button" class="linkish mono" translate="no" onclick={() => (txid = f.txid)}>{f.txid.slice(0, 20)}…</button><span class="num">{formatL1(f.amount, swap.parentChain)}</span>{#if f.confirmations !== undefined}<span class="faint small">{t('%1 conf.', f.confirmations)}</span>{/if}</li>{/each}</ul>
              {/if}
              {#if checked}<p class="cs-help" role="status"><SafeText text={checked} /></p>{/if}
            {/if}
          {:else if next === 'claim'}
            <p class="cs-help">{t('Your L1 payment is confirmed: the ECX locked for this swap can come to this wallet.')}</p>
            <div class="row"><button type="button" id="cs-claim" class="btn primary" disabled={busy} onclick={claim}>{t('Review and claim…')}</button></div>
          {:else if next === 'wait-taker'}
            <p class="cs-help">{st === 'open' ? t('Your ECX is locked until someone takes the swap or it expires (then it comes back).') : t('Someone took it: they pay %1 to your L1 address. Watch that address in your own L1 wallet.', formatL1(swap.l1Amount, swap.parentChain))}</p>
          {:else}
            <p class="cs-help">{st === 'completed' ? (swap.role === 'creator' ? t('This swap is done: its taker claimed the ECX. Check the payment to your L1 address below.') : t('This swap is done: the ECX went to its taker.')) : st === 'expired' ? t('This swap expired: its ECX went back to the one who offered it.') : swap.role ? t('Nothing to do now.') : t('This swap is reserved by someone else, or names another recipient.')}</p>
          {/if}
          {#if swap.role === 'creator'}
            <!-- What your L1 node sees paid to your L1 address (Coinshift's chain does not say whether you were paid). -->
            {#if ready}<div class="row"><button type="button" class="btn" disabled={busy} onclick={find}><Icon name="search" size={15} /> {t('Payments to your L1 address')}</button></div>{/if}
            {#if found}
              {#if found.length === 0}<p class="cs-help">{t('Your L1 node sees no unspent payment of this amount to your address.')}</p>{/if}
              <ul class="found paid">{#each found as f (f.txid)}<li><Icon name="check" size={14} /><span class="mono" translate="no">{f.txid.slice(0, 20)}…</span><span class="num">{formatL1(f.amount, swap.parentChain)}</span>{#if f.confirmations !== undefined}<span class="faint small">{t('%1 conf.', f.confirmations)}</span>{/if}</li>{/each}</ul>
            {/if}
          {/if}
        </div>
      </section>

      <section class="panel">
        <div class="panel-head"><h2>{t('Details')}</h2></div>
        <dl class="panel-body facts">
          <dt>{t('Paid on L1 to')}</dt><dd class="mono break" translate="no">{swap.l1Address}</dd>
          <dt>{t('L1 confirmations needed')}</dt><dd class="num">{swap.requiredConfirmations}</dd>
          {#if swap.l2Recipient}<dt>{t('Recipient')}</dt><dd class="mono break" translate="no">{swap.l2Recipient}</dd>{/if}
          {#if swap.creator}<dt>{t('Offered by')}</dt><dd class="mono break" translate="no">{swap.creator}{#if swap.role === 'creator'} <span class="badge accent">{t('you')}</span>{/if}</dd>{/if}
          {#if swap.reservation}<dt>{t('Reserved by')}</dt><dd class="mono break" translate="no">{swap.reservation.claimer}{#if swap.reservation.mine} <span class="badge accent">{t('you')}</span>{/if} · {swap.reservation.live ? t('until block %1', swap.reservation.expiresAt) : t('lapsed')}</dd>{/if}
          {#if swap.l1Txid}<dt>{t('L1 payment')}</dt><dd class="mono break" translate="no">{swap.l1Txid}</dd>{/if}
          <dt>{t('Offered at')}</dt><dd class="num">{t('block %1', swap.createdAt)}</dd>
          {#if swap.expiresAt !== null}<dt>{t('Expires at')}</dt><dd class="num">{t('block %1', swap.expiresAt)}</dd>{/if}
        </dl>
        {#if swap.transactions?.length}
          <ul class="txs">
            {#each swap.transactions as x (x.txid)}
              <li><span class="badge">{x.kind === 'create' ? t('Offered') : x.kind === 'accept' ? t('Reserved') : t('Claimed')}</span><span class="mono" translate="no">{x.txid.slice(0, 16)}…</span><CopyButton text={x.txid} label={t('Copy the transaction id')} /><span class="faint small num">{t('block %1', x.height)}</span></li>
            {/each}
          </ul>
        {/if}
      </section>
    </div>
    <p class="side-note"><Icon name="info" size={15} /> {t('A swap’s state is partly this node’s own view of L1, as on every Coinshift node: the app claims only a swap whose payment it saw on your L1 node itself.')}</p>
  {/if}
</div>

<style>
  .view {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .back {
    align-self: flex-start;
    display: inline-flex;
    align-items: center;
    gap: 0.3rem;
  }
  .head h1 {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    flex-wrap: wrap;
  }
  .id {
    font-size: 0.9rem;
    font-weight: 500;
    color: var(--text-muted);
  }
  .sub {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
    margin: 0.3rem 0 0;
  }
  .sides {
    display: flex;
    flex-direction: column;
    gap: 1rem;
    padding: 1rem 1.1rem;
  }
  .conf {
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .cols {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(22rem, 100%), 1fr));
    gap: 1rem;
    align-items: start;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .facts {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: 0.45rem 1rem;
    margin: 0;
  }
  .facts dt {
    color: var(--text-muted);
    font-size: 0.86rem;
  }
  .facts dd {
    margin: 0;
    min-width: 0;
  }
  .addr {
    display: flex;
    align-items: center;
    gap: 0.4rem;
  }
  .break {
    overflow-wrap: anywhere;
  }
  .found,
  .txs {
    list-style: none;
    margin: 0;
    padding: 0 1rem 0.8rem;
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
  }
  .found {
    padding: 0;
  }
  .found li,
  .txs li {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    flex-wrap: wrap;
  }
</style>
