<script lang="ts">
  // Settings → L1 nodes: the node of each other chain the app asks about the payments of your swaps (Bitcoin Core-style
  // RPC: getrawtransaction, scantxoutset). Kept by the app layer on this computer (its store, readable by you only),
  // never in coinshift-rs' own l1_rpc_configs.json; the password never comes back to the pages. Each change is asked in
  // the app's trusted dialog.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { toast } from '../../lib/state.svelte';
  import { csApi, type ParentChain } from './api';
  import './cs.css';
  import { cs, refreshCoinshift, startCoinshift } from './store.svelte';

  $effect(() => startCoinshift());

  let editing = $state<ParentChain | ''>('');
  let url = $state('');
  let user = $state('');
  let password = $state('');
  let busy = $state(false);
  let error = $state('');

  function edit(c: ParentChain) {
    const n = cs.l1.find((x) => x.chain === c);
    editing = c;
    url = n?.url ?? (c === 'Regtest' ? 'http://127.0.0.1:18443' : c === 'Signet' ? 'http://127.0.0.1:38332' : 'http://127.0.0.1:8332');
    user = n?.user ?? '';
    password = '';
    error = '';
  }

  async function save() {
    if (!editing) return;
    busy = true;
    error = '';
    try {
      await csApi.setL1(editing, url.trim(), user.trim(), password);
      toast('success', t('L1 node set'), t('The app checks payments of this chain on it.'));
      editing = '';
      password = '';
      await refreshCoinshift();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }

  async function remove(c: ParentChain) {
    try {
      await csApi.removeL1(c);
      await refreshCoinshift();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    }
  }
</script>

<section class="panel">
  <div class="panel-head"><h2>{t('L1 nodes')}</h2></div>
  <div class="panel-body stack">
    <p class="cs-help">{t('To take a swap, the app must see your payment on the other chain itself: name a node of that chain you run (its RPC address and login). Coinshift’s own chain does not check it.')}</p>
    <ul class="nodes">
      {#each cs.l1 as n (n.chain)}
        <li>
          <span class="grow"><strong>{n.coin}</strong> {#if n.configured}<span class="mono faint small" translate="no">{n.url}{n.user ? ` · ${n.user}` : ''}</span>{:else}<span class="faint small">{t('not set')}</span>{/if}</span>
          <button type="button" class="btn small" data-l1={n.chain} onclick={() => edit(n.chain)}>{n.configured ? t('Change') : t('Set up')}</button>
          {#if n.configured}<button type="button" class="btn small" onclick={() => remove(n.chain)}>{t('Forget')}</button>{/if}
        </li>
      {/each}
    </ul>
    {#if editing}
      <form
        class="stack"
        onsubmit={(e) => {
          e.preventDefault();
          save();
        }}
      >
        <h3>{t('The %1 node', cs.l1.find((x) => x.chain === editing)?.coin ?? editing)}</h3>
        <label class="field"><span>{t('RPC address')}</span><input id="l1-url" type="text" autocomplete="off" spellcheck="false" bind:value={url} /></label>
        <label class="field"><span>{t('User')}</span><input id="l1-user" type="text" autocomplete="off" spellcheck="false" bind:value={user} /></label>
        <label class="field"><span>{t('Password')}</span><input id="l1-password" type="password" autocomplete="off" bind:value={password} /></label>
        {#if error}<p class="cs-problems" role="alert"><SafeText text={error} /></p>{/if}
        <div class="row">
          <button type="submit" class="btn primary" disabled={busy || !/^http:\/\/\S+$/.test(url.trim())}><Icon name="check" size={15} /> {t('Check and use it…')}</button>
          <button type="button" class="btn" onclick={() => (editing = '')}>{t('Cancel')}</button>
        </div>
      </form>
    {:else if error}
      <p class="cs-problems" role="alert"><SafeText text={error} /></p>
    {/if}
  </div>
</section>

<style>
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .nodes {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .nodes li {
    display: flex;
    align-items: center;
    gap: 0.6rem;
    flex-wrap: wrap;
  }
  .grow {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  h3 {
    margin: 0.4rem 0 0;
    font-size: 1rem;
  }
</style>
