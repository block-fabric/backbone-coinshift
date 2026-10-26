<script lang="ts">
  // Offer a swap (L2 to L1, coinshift-rs' create_swap): ECX of this wallet locked on Coinshift for coins of another chain
  // paid to your address there. Open to anyone (the first to reserve it takes it) or for one recipient. It expires
  // after a number of blocks of Coinshift set by its chain; then the ECX comes back. Asked in the app's trusted dialog.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { app, toast } from '../../lib/state.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import AddressField from '../dropin/AddressField.svelte';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { displayTextProblem } from '../dropin/text';
  import { drop, refresh } from '../dropin/store.svelte';
  import '../cs/cs.css';
  import { csSpend, defaultChain, defaults, formatL1, offerProblem, PARENT_CHAINS, parseL1, ticker, type ParentChain } from '../cs/api';
  import { cs, refreshCoinshift, startCoinshift } from '../cs/store.svelte';

  $effect(() => startCoinshift());

  const FEE = 1000n;
  const network = $derived((app.info as { network?: string } | null)?.network ?? 'betanet');
  let chain = $state<ParentChain>('BTC');
  let chainTouched = $state(false);
  $effect(() => {
    if (!chainTouched) chain = defaultChain(network);
  });
  let l1Address = $state('');
  let l1Text = $state('');
  let l2Amount = $state<bigint | null>(null);
  let forOne = $state(false);
  let recipient = $state('');
  let recipientValid = $state(false);
  let confText = $state('');
  let busy = $state(false);
  let error = $state('');

  const l1Amount = $derived(parseL1(l1Text));
  const confirmations = $derived(confText.trim() === '' ? defaults(chain).confirmations : Number(confText));
  const available = $derived(BigInt(drop.overview?.wallet.available ?? 0));
  const problem = $derived.by(() => {
    const p = offerProblem({ l1Address, l1Amount, l2Amount, available, fee: FEE, confirmations });
    if (p) return p;
    const odd = displayTextProblem(l1Address.trim());
    if (odd) return t(odd);
    if (forOne && !recipientValid) return t('Give the recipient’s Coinshift address, or open the swap to anyone.');
    return '';
  });
  const shown = $derived(!!(l1Address || l1Text || l2Amount));

  async function offer() {
    if (problem || !l1Amount || !l2Amount) return;
    busy = true;
    error = '';
    try {
      const [id] = await csSpend.create(chain, l1Address.trim(), l1Amount, forOne ? recipient.trim() : null, Number(l2Amount), confText.trim() === '' ? null : confirmations, Number(FEE));
      const hex = Array.isArray(id) ? id.map((b) => Number(b).toString(16).padStart(2, '0')).join('') : '';
      toast('success', t('Swap offered'), t('It is open from the next block of Coinshift.'));
      await refresh();
      await refreshCoinshift();
      go('swaps', /^[0-9a-f]{64}$/.test(hex) ? `s-${hex}` : 'mine');
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="page newswap">
  <header class="page-head"><h1>{t('Offer a swap')}</h1></header>
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    <p class="side-note"><Icon name="info" size={15} /> {t('You lock ECX here; the one who takes the swap pays you on the other chain, then claims the ECX. Coinshift does not check that payment on its own chain (each node looks at the other chain itself): for an amount that matters, offer it to a recipient you trust.')}</p>
    <div class="cols">
      <section class="panel">
        <div class="panel-head"><h2>{t('Your offer')}</h2></div>
        <form
          class="panel-body stack"
          onsubmit={(e) => {
            e.preventDefault();
            offer();
          }}
        >
          <label class="field">
            <span>{t('You are paid on')}</span>
            <select id="swap-chain" bind:value={chain} onchange={() => (chainTouched = true)}>
              {#each PARENT_CHAINS as c (c)}<option value={c}>{c === 'BTC' ? 'Bitcoin' : c === 'BCH' ? 'Bitcoin Cash' : c === 'LTC' ? 'Litecoin' : c === 'Signet' ? 'Bitcoin Signet' : 'Bitcoin Regtest'}</option>{/each}
            </select>
          </label>
          <AmountInput id="swap-l2-amount" label={t('ECX you offer')} bind:value={l2Amount} required max={available} />
          <span class="cs-help">{t('Available')}: <Amount sats={available} /></span>
          <label class="field"><span>{t('%1 you ask for', ticker(chain))}</span><input id="swap-l1-amount" type="text" inputmode="decimal" autocomplete="off" placeholder="0.00000000" bind:value={l1Text} /></label>
          <label class="field"><span>{t('Your address on that chain (where you are paid)')}</span><input id="swap-l1-address" type="text" autocomplete="off" spellcheck="false" bind:value={l1Address} /></label>
          <div class="cs-chips" role="radiogroup" aria-label={t('Who can take it')}>
            <button type="button" role="radio" class="cs-chip" aria-checked={!forOne} onclick={() => (forOne = false)}>{t('Anyone (the first to reserve it)')}</button>
            <button type="button" role="radio" class="cs-chip" aria-checked={forOne} onclick={() => (forOne = true)}>{t('One recipient')}</button>
          </div>
          {#if forOne}<AddressField id="swap-recipient" label={t('The recipient’s Coinshift address')} bind:value={recipient} bind:valid={recipientValid} />{/if}
          <label class="field"><span>{t('Confirmations of the payment needed')}</span><input id="swap-conf" type="text" inputmode="numeric" placeholder={String(defaults(chain).confirmations)} bind:value={confText} /></label>
          {#if shown && problem}<p class="cs-problems" role="alert"><SafeText text={problem} /></p>{/if}
          {#if error}<p class="cs-problems" role="alert"><SafeText text={error} /></p>{/if}
          <div class="row"><button type="submit" class="btn primary" disabled={busy || !!problem}>{t('Review and offer…')}</button></div>
        </form>
      </section>
      <section class="panel">
        <div class="panel-head"><h2>{t('What happens')}</h2></div>
        <div class="panel-body stack">
          <dl class="facts">
            <dt>{t('Locked now')}</dt><dd>{#if l2Amount}<Amount sats={l2Amount + FEE} />{:else}–{/if} <span class="faint small">({t('with the fee')} <Amount sats={FEE} />)</span></dd>
            <dt>{t('Paid to you')}</dt><dd translate="no">{l1Amount ? formatL1(l1Amount, chain) : '–'}</dd>
            <dt>{t('Price')}</dt><dd class="num">{l1Amount && l2Amount ? t('%1 %2 per ECX', (l1Amount / Number(l2Amount)).toFixed(8), ticker(chain)) : '–'}</dd>
            <dt>{t('Expires')}</dt><dd>{t('after %1 blocks of Coinshift, if nobody takes it', defaults(chain).expiry)}</dd>
            <dt>{t('A reservation lasts')}</dt><dd>{t('%1 blocks', defaults(chain).reserve)}</dd>
          </dl>
          <ol class="how">
            <li>{t('Your ECX is locked for the swap.')}</li>
            <li>{forOne ? t('The recipient pays you on the other chain.') : t('Someone reserves it, then pays you on the other chain.')}</li>
            <li>{t('Once the payment has its confirmations, they claim the ECX.')}</li>
          </ol>
          {#if cs.swaps.some((s) => s.role === 'creator')}<button type="button" class="linkish" onclick={() => go('swaps', 'mine')}>{t('Your swaps')}</button>{/if}
        </div>
      </section>
    </div>
  {/if}
</div>

<style>
  .newswap {
    display: flex;
    flex-direction: column;
    gap: 1rem;
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
  }
  .how {
    margin: 0;
    padding-left: 1.2rem;
    color: var(--text-muted);
    font-size: 0.9rem;
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
</style>
