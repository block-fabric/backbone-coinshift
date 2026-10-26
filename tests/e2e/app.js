// The end-to-end run of coinshift-app on a local eCash regtest (run_app.py): the app starts and runs its node; the
// wallet's backup phrase is made (its words only in the trusted dialog); ECX is deposited from eCash through the
// mainchain interface's wallet; a payment is sent to another Coinshift node and one received from it (no withdrawal:
// Coinshift's bundles follow a known upstream behaviour, reproduced); the L1 node (a Bitcoin regtest) is set in
// Settings → L1 nodes; swap 1: the app offers 1 ECX for 0.5 BTC, node B takes it (reserves, the L1 wallet pays, B claims), the app shows it done and the payment on L1;
// swap 2: node B offers 0.5 ECX for 0.3 BTC, the app reserves it, the L1 wallet pays, the app finds and checks the
// payment on its L1 node and claims; the pages pictured, dark and light.
// window.__E2E: { peerAddress, mainAddress, saveFile, l1Url, l1User, l1Password, l1Address } from the runner.
(async () => {
  const { host, sleep, log, T, waitFor, check, snapshot, click, type, go, theme, finish, ready } = E2E;
  const overview = () => host('dropin.overview');
  const history = () => host('dropin.history', { limit: 100 });
  const swaps = () => host('coinshift.swaps');
  const swapOf = (id) => host('coinshift.swap', { id });
  const ecx = 100_000_000;
  const E = window.__E2E;
  const asked = async (kind) => (await host('test.confirmed')).filter((q) => q.kind === `node:${kind}`);
  try {
    await theme('midnight');
    await ready();
    await check('the app runs its node (phase ready, a tip)', T().app.phase === 'ready');
    await check('app.info is the node’s and the app’s', T().app.info.name === 'Coinshift' && T().app.info.hostApi === 1 && T().app.info.embedded === false);
    await go('home');
    await waitFor('the setup card', () => document.querySelector('.setup'));

    // ---- the backup phrase: made by the node, shown in the trusted dialog (answered yes by the test), the wallet set
    await host('test.confirmAnswer', { yes: true });
    await click('Make a new backup phrase');
    await waitFor('the wallet set up', async () => (await host('dropin.seed.status')).hasSeed);
    const phraseQ = (await asked('phrase.create'))[0];
    await check('the phrase was asked in the trusted dialog, its words never recorded', !!phraseQ && phraseQ.details === '(the words, not recorded)');
    await check('the page got no words', !JSON.stringify(T().app).match(/\b(abandon|zoo)\b/));

    // ---- a deposit from eCash, paid by Backbone's wallet (-grpcwallet), to the bare address
    await go('peg', 'deposit');
    await waitFor('the deposit form', () => document.querySelector('#dep-amount'));
    type('#dep-amount', '10');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and deposit');
    await waitFor('the deposit credited', async () => (await overview()).wallet.available >= 10 * ecx, 300000);
    const q1 = (await asked('deposit'))[0];
    await check('the deposit was asked with its amount', !!q1 && q1.rows.some((r) => r.label === 'Amount' && r.value === '10.00000000 ECX'));

    // ---- send to the other Coinshift node (coinshift-rs' transfer)
    await go('send');
    type('#send-to', E.peerAddress);
    await waitFor('the address checked', () => document.querySelector('.state.good'));
    type('#send-amount', '1.5');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and send');
    await waitFor('the payment in a block', async () => (await history()).some((r) => r.kind === 'send' && !r.pending), 300000);

    // ---- receive from it
    await go('receive');
    const shown = await waitFor('an address shown', () => {
      const el = document.querySelector('.addr .mono.big');
      return el ? el.textContent.trim() : null;
    });
    await log(`SIGNAL receive-address ${shown}`);
    await waitFor('the payment received', async () => (await history()).some((r) => r.kind === 'receive' && !r.pending && r.amount === 0.25 * ecx), 300000);
    await check('a payment received at the address shown', true);

    // ---- the L1 node the app checks payments on (Settings → L1 nodes), asked in the trusted dialog
    await go('settings', 'l1nodes');
    const setRegtest = await waitFor('the L1 nodes', () => document.querySelector('[data-l1="Regtest"]'));
    setRegtest.click();
    await waitFor('the L1 form', () => document.querySelector('#l1-url'));
    type('#l1-url', E.l1Url);
    type('#l1-user', E.l1User);
    type('#l1-password', E.l1Password);
    await host('test.confirmAnswer', { yes: true });
    await click('Check and use it');
    await waitFor('the L1 node set', async () => (await host('coinshift.l1')).some((n) => n.chain === 'Regtest' && n.configured), 30000);
    const ql1 = (await asked('l1.set'))[0];
    await check('the L1 node was asked with its address, never its password', !!ql1 && JSON.stringify(ql1.rows).includes(E.l1Url) && !JSON.stringify(ql1).includes(E.l1Password));
    await check('the page never gets the L1 password back', !JSON.stringify(await host('coinshift.l1')).includes(E.l1Password));
    await sleep(800);
    await snapshot('coinshift-01-l1-nodes');

    // ---- swap 1: the app offers 1 ECX for 0.5 BTC (open to anyone); node B takes it
    await go('new-swap');
    await waitFor('the offer form', () => document.querySelector('#swap-l1-address'));
    type('#swap-l2-amount', '1');
    type('#swap-l1-amount', '0.5');
    type('#swap-l1-address', E.l1Address);
    await sleep(800);
    await snapshot('coinshift-02-offer');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and offer');
    const mine = await waitFor('the offer in a block', async () => (await swaps()).find((s) => s.role === 'creator' && s.l1Address === E.l1Address), 300000);
    const qc = (await asked('swap.create'))[0];
    await check('the offer was asked with both sides and the L1 address', !!qc && JSON.stringify(qc.rows).includes('1.00000000 ECX') && JSON.stringify(qc.rows).includes('0.50000000 BTC') && JSON.stringify(qc.rows).includes(E.l1Address), JSON.stringify(qc && qc.rows));
    await check('its ECX is locked for it', mine.escrow === ecx, JSON.stringify(mine));
    await log(`SIGNAL swap-offered ${mine.id}`);
    await go('swaps', `s-${mine.id}`);
    await waitFor('the reservation by node B', async () => (await swapOf(mine.id)).reservation, 300000);
    await sleep(1200);
    await snapshot('coinshift-03-reserved');
    const done1 = await waitFor('swap 1 claimed by node B', async () => {
      const s = await swapOf(mine.id);
      return s.state.kind === 'completed' ? s : null;
    }, 600000);
    await check('swap 1 is completed in the app', !!done1);
    await go('swaps', `s-${mine.id}`);
    await click('Payments to your L1 address');
    await waitFor('the L1 payment seen', () => document.querySelector('.found.paid li'), 60000);
    await check('the app sees the 0.5 BTC paid to its L1 address', document.querySelector('.found.paid').textContent.includes('0.50000000 BTC'));
    await sleep(800);
    await snapshot('coinshift-04-done-paid');

    // ---- swap 2: node B offers 0.5 ECX for 0.3 BTC; the app takes it
    await log('SIGNAL want-swap');
    const theirs = await waitFor('node B’s offer', async () => (await swaps()).find((s) => !s.role && s.l2Amount === 50_000_000 && s.state.kind === 'pending'), 300000);
    await go('swaps', 'open');
    await sleep(1500);
    await snapshot('coinshift-05-open-swaps');
    await go('swaps', `s-${theirs.id}`);
    await host('test.confirmAnswer', { yes: true });
    await click('Review and reserve');
    await waitFor('the app’s reservation in a block', async () => {
      const s = await swapOf(theirs.id);
      return s.reservation && s.reservation.mine && s.reservation.live;
    }, 300000);
    const qa = (await asked('swap.accept'))[0];
    await check('the reservation was asked with what to pay on L1', !!qa && JSON.stringify(qa.rows).includes('0.30000000 BTC'), JSON.stringify(qa && qa.rows));
    // Claiming before any payment: refused before a question (the app claims only what it saw paid on L1).
    let early = '';
    try {
      await host('rpc.confirmed', { method: 'claim_swap', params: [theirs.id.match(/../g).map((x) => parseInt(x, 16)), null] });
    } catch (e) {
      early = String(e && e.message ? e.message : e);
    }
    await check('a claim before the L1 payment is refused', /ready|payment/i.test(early), early);
    await log(`SIGNAL pay-l1 ${theirs.l1Address} ${theirs.l1Amount}`);
    await go('swaps', `s-${theirs.id}`);
    await waitFor('the txid field', () => document.querySelector('#cs-l1-txid'), 30000);
    await waitFor('the L1 payment found', async () => {
      const b = [...document.querySelectorAll('button')].find((x) => x.textContent.includes('Find payments to this address'));
      if (b && !b.disabled) b.click();
      await sleep(1500);
      return /^[0-9a-f]{64}$/.test(document.querySelector('#cs-l1-txid')?.value ?? '');
    }, 180000);
    await click('Check the payment');
    await waitFor('the payment checked: ready to claim', async () => (await swapOf(theirs.id)).canClaim, 120000);
    await sleep(800);
    await snapshot('coinshift-06-payment-checked');
    await go('swaps', `s-${theirs.id}`);
    await host('test.confirmAnswer', { yes: true });
    await click('Review and claim');
    await waitFor('swap 2 claimed', async () => (await swapOf(theirs.id)).state.kind === 'completed', 300000);
    const qk = (await asked('swap.claim'))[0];
    await check('the claim was asked with the L1 payment the app saw', !!qk && JSON.stringify(qk.rows).includes('0.30000000 BTC'), JSON.stringify(qk && qk.rows));
    await waitFor('the claim in the activity', async () => (await history()).some((r) => r.kind === 'swap-claim' && !r.pending && r.received === 50_000_000), 120000);
    await check('the activity shows the swap claimed', true);
    await go('swaps', `s-${theirs.id}`);
    await sleep(1500);
    await snapshot('coinshift-07-claimed');

    // ---- the other pages, both themes
    for (const [mode, id] of [['dark', 'midnight'], ['light', 'light']]) {
      await theme(id);
      for (const [page, tab, name] of [['home', '', 'home'], ['swaps', 'mine', 'my-swaps'], ['swaps', 'all', 'all-swaps'], ['new-swap', '', 'offer'], ['transactions', '', 'activity'], ['peg', 'deposit', 'drivechains'], ['explorer', '', 'explorer'], ['settings', 'l1nodes', 'settings-l1'], ['settings', 'dropwallet', 'settings-wallet']]) {
        await go(page, tab);
        await sleep(1200);
        await snapshot(`coinshift-${mode}-${name}`);
      }
    }
    await theme('midnight');

    // ---- the backup phrase shown again: a question of the trusted dialog, never words in the page
    await go('settings', 'dropwallet');
    await host('test.confirmAnswer', { yes: true });
    await click('Show the phrase');
    await sleep(1000);
    await check('showing the phrase is a question of the trusted dialog', (await asked('phrase.show')).length === 1);

    // ---- saved to a file the user chose: asked, then written (the runner checks the file: owner-only, the words)
    await host('test.fileAnswer', { path: E.saveFile });
    await host('test.confirmAnswer', { yes: true });
    await click('Save it to a file');
    await waitFor('the phrase saved', async () => (await asked('phrase.save')).length > 0, 30000);
    await sleep(1500);
  } catch (e) {
    await check('the run', false, e && e.stack ? e.stack : String(e));
  }
  await finish();
})();
