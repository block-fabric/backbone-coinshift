<script lang="ts">
  // Swaps: every swap this node knows of (Coinshift's swaps are of the chain; their state is partly each node's own view
  // of L1, as coinshift-rs keeps it). Open ones anyone can reserve, this wallet's own (offered, reserved, to claim),
  // and the rest; a swap opens in its own view (tab s-<id>) with where it stands and what this wallet can do next.
  import Amount from '../../components/Amount.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import '../cs/cs.css';
  import { barWidth } from '../cs/actions';
  import { blocksLeft, formatL1, nextStep, rate, stageOf, stageText, type Stage, type Swap } from '../cs/api';
  import { cs, startCoinshift } from '../cs/store.svelte';
  import SwapView from '../cs/SwapView.svelte';

  $effect(() => startCoinshift());

  const TABS = [
    { id: 'open', label: 'Open' },
    { id: 'mine', label: 'Mine' },
    { id: 'all', label: 'All' },
  ];
  const openId = $derived(nav.tab.startsWith('s-') ? nav.tab.slice(2) : '');
  const tab = $derived(nav.tab === 'mine' || nav.tab === 'all' ? nav.tab : 'open');
  let query = $state('');

  const live = (s: Swap) => !['completed', 'expired', 'cancelled'].includes(stageOf(s));
  const openSwaps = $derived(cs.swaps.filter((s) => stageOf(s) === 'open' && !s.role));
  const mine = $derived(cs.swaps.filter((s) => !!s.role));
  const toClaim = $derived(mine.filter((s) => s.canClaim));
  const locked = $derived(cs.swaps.filter((s) => s.role === 'creator' && live(s)).reduce((n, s) => n + s.escrow, 0));
  const shown = $derived.by(() => {
    const list = tab === 'open' ? openSwaps : tab === 'mine' ? mine : cs.swaps;
    const q = query.trim().toLowerCase();
    const found = q ? list.filter((s) => s.id.includes(q) || s.l1Address.toLowerCase().includes(q) || s.coin.toLowerCase().includes(q)) : list;
    return [...found].sort((a, b) => Number(live(b)) - Number(live(a)) || b.createdAt - a.createdAt);
  });

  function stageClass(st: Stage): string {
    return st === 'open' || st === 'ready' ? 'live' : st === 'reserved' || st === 'confirming' || st === 'for-recipient' ? 'vote' : st === 'completed' ? 'done' : 'bad';
  }
  function roleText(s: Swap): string {
    return s.role === 'creator' ? t('yours') : s.role === 'recipient' ? t('for you') : s.role === 'claimer' ? t('reserved by you') : '';
  }
  function nextText(s: Swap): string {
    switch (nextStep(s)) {
      case 'reserve':
        return t('Reserve it to take it');
      case 'pay':
        return t('Pay on L1, then check the payment');
      case 'claim':
        return t('Claim the ECX');
      case 'wait-taker':
        return stageOf(s) === 'open' ? t('Waiting for a taker') : t('Taken: watch your L1 address');
      case 'wait-confirmations':
        return t('Waiting for confirmations on L1');
      default:
        return '';
    }
  }
</script>

<div class="page swaps">
  {#if openId}
    <SwapView id={openId} />
  {:else}
    <header class="page-head head">
      <div>
        <h1>{t('Swaps')}</h1>
        <p class="muted sub">{t('ECX on Coinshift for coins of another chain (L1): the one who offers locks ECX, the one who takes pays on L1.')}</p>
      </div>
      <button type="button" class="btn primary" onclick={() => go('new-swap' as never)}><Icon name="plus" size={16} /> {t('Offer a swap')}</button>
    </header>

    <section class="cs-figures">
      <div class="panel cs-figure"><span class="muted">{t('Open swaps')}</span><strong class="num">{openSwaps.length}</strong><span class="faint small">{t('anyone can reserve')}</span></div>
      <div class="panel cs-figure"><span class="muted">{t('Your swaps')}</span><strong class="num">{mine.length}</strong><span class="faint small">{t('%1 to claim', toClaim.length)}</span></div>
      <div class="panel cs-figure"><span class="muted">{t('Locked in your offers')}</span><strong><Amount sats={locked} /></strong><span class="faint small">{t('back to you if they expire')}</span></div>
      <div class="panel cs-figure"><span class="muted">{t('Every swap')}</span><strong class="num">{cs.swaps.length}</strong><span class="faint small">{t('as this node sees them')}</span></div>
    </section>

    <TabRow label="Swaps" tabs={TABS} current={tab} select={(id) => (nav.tab = id === 'open' ? '' : id)} />

    <div class="search">
      <Icon name="search" size={16} />
      <input type="search" placeholder={t('Search: a swap id, an L1 address, a coin')} aria-label={t('Search the swaps')} bind:value={query} />
    </div>

    {#if !cs.loaded}
      <p class="cs-empty">{t('Asking the node…')}</p>
    {:else if shown.length === 0}
      <section class="panel">
        <p class="cs-empty">{tab === 'open' ? t('No swap is open now. Offer one: lock ECX for coins of another chain.') : tab === 'mine' ? t('This wallet has no swap yet.') : t('No swap on this chain yet.')}</p>
      </section>
    {:else}
      <ul class="cs-grid list" aria-label={t('Swaps')}>
        {#each shown as s (s.id)}
          {@const st = stageOf(s)}
          {@const left = blocksLeft(s)}
          <li>
            <button type="button" class="panel card" data-swap={s.id} onclick={() => go('swaps', `s-${s.id}`)}>
              <span class="top">
                <span class="cs-stage {stageClass(st)}">{stageText(st)}</span>
                <span class="badge">{s.coin}</span>
                {#if roleText(s)}<span class="badge accent">{roleText(s)}</span>{/if}
              </span>
              <span class="cs-sides">
                <span class="cs-side"><span class="faint small">{t('Gets on Coinshift')}</span><strong><Amount sats={s.l2Amount} /></strong></span>
                <Icon name="swap" size={18} />
                <span class="cs-side l1"><span class="faint small">{t('Pays on L1')}</span><strong translate="no">{formatL1(s.l1Amount, s.parentChain)}</strong></span>
              </span>
              {#if st === 'confirming' && s.state.required}
                <span class="conf">
                  <span class="cs-bar"><span use:barWidth={(s.state.current ?? 0) / s.state.required}></span></span>
                  <span class="faint small num">{t('%1 of %2 L1 confirmations', s.state.current ?? 0, s.state.required)}</span>
                </span>
              {/if}
              <span class="meta faint small">
                <span class="num">{t('%1 %2 per ECX', rate(s).toFixed(8), s.parentChain === 'BCH' ? 'BCH' : s.parentChain === 'LTC' ? 'LTC' : 'BTC')}</span>
                {#if left !== null && st !== 'completed'}<span>{left > 0 ? t('expires in %1 blocks', left) : t('expired')}</span>{/if}
                <span class="mono" translate="no">{s.id.slice(0, 12)}…</span>
              </span>
              {#if nextText(s)}<span class="next"><Icon name="arrow" size={14} /> <SafeText text={nextText(s)} /></span>{/if}
            </button>
          </li>
        {/each}
      </ul>
    {/if}
  {/if}
</div>

<style>
  .swaps {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .head {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    gap: 1rem;
    flex-wrap: wrap;
  }
  .sub {
    margin: 0.2rem 0 0;
    max-width: 44rem;
  }
  .search {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0 0.8rem;
    border: 1px solid var(--line);
    border-radius: var(--radius-m);
    background: var(--surface-2);
    color: var(--text-muted);
  }
  .search input {
    flex: 1;
    min-width: 0;
    border: 0;
    background: transparent;
    color: var(--text);
    font: inherit;
    padding: 0.6rem 0;
    outline: none;
  }
  .list {
    margin: 0;
    padding: 0;
    list-style: none;
  }
  .card {
    display: flex;
    flex-direction: column;
    gap: 0.65rem;
    width: 100%;
    padding: 0.95rem 1rem;
    text-align: left;
    color: var(--text);
    font: inherit;
    cursor: pointer;
  }
  .card:hover,
  .card:focus-visible {
    border-color: var(--line-strong);
  }
  .top {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.4rem;
  }
  .conf {
    display: flex;
    flex-direction: column;
    gap: 0.25rem;
  }
  .meta {
    display: flex;
    flex-wrap: wrap;
    gap: 0.3rem 0.8rem;
  }
  .next {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    font-size: 0.86rem;
    font-weight: 600;
  }
</style>
