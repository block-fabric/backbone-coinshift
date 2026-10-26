<script lang="ts">
  // Coinshift's card on Home: the open swaps, this wallet's (and those it may claim), and offering or taking one.
  import Amount from '../components/Amount.svelte';
  import Icon from '../components/Icon.svelte';
  import Card from '../views/home/Card.svelte';
  import { t } from '../lib/i18n.svelte';
  import { go } from '../lib/nav.svelte';
  import { stageOf } from './cs/api';
  import { cs, startCoinshift } from './cs/store.svelte';

  $effect(() => startCoinshift());
  const open = $derived(cs.swaps.filter((s) => stageOf(s) === 'open' && !s.role).length);
  const mine = $derived(cs.swaps.filter((s) => !!s.role && !['completed', 'expired', 'cancelled'].includes(stageOf(s))));
  const claim = $derived(mine.filter((s) => s.canClaim).length);
</script>

<Card title={t('Swaps')} icon="swap" open={() => go('swaps' as never)} openLabel={t('Open Swaps')}>
  <div class="big num">{open}<span class="unit">{open === 1 ? t('open swap') : t('open swaps')}</span></div>
  <p class="muted small">
    {mine.length ? t('%1 of yours under way', mine.length) : t('Trade ECX for coins of another chain.')}
    {#if claim}<strong> · {t('%1 to claim', claim)}</strong>{/if}
  </p>
  {#if cs.wallet && cs.wallet.inSwaps > 0}<p class="muted small">{t('Locked in your offers')}: <Amount sats={cs.wallet.inSwaps} /></p>{/if}
  {#snippet actions()}
    <button type="button" class="btn small" onclick={() => go('new-swap' as never)}><Icon name="plus" size={15} /> {t('Offer')}</button>
    <button type="button" class="btn small" onclick={() => go('swaps' as never)}><Icon name="swap" size={15} /> {t('Take one')}</button>
  {/snippet}
</Card>

<style>
  .big {
    font-size: 1.65rem;
    font-weight: 650;
    margin-bottom: 0.4rem;
  }
  .big .unit {
    font-size: 0.85rem;
    font-weight: 500;
    color: var(--text-muted);
    margin-inline-start: 0.4rem;
  }
  .small {
    font-size: 0.85rem;
    margin: 0;
  }
</style>
