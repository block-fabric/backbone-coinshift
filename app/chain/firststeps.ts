// The three first steps of Coinshift, on Home in Simple mode until dismissed.
import type { FirstStep } from '../lib/firststeps';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring ECX here', text: 'Coins come from eCash. Drivechains gives a deposit address to pay from an eCash wallet (the Backbone app: Sidechains → Coinshift). They arrive with the next block of Coinshift.', icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Offer a swap', text: 'Offer ECX for coins of another chain (Bitcoin, Bitcoin Cash, Litecoin): it is locked here until someone pays you there.', icon: 'plus', action: { label: 'Offer a swap', page: 'new-swap' as never } },
  { title: 'Take a swap', text: 'Reserve an open swap, pay its coins from your own L1 wallet, and claim the ECX once the app sees your payment confirmed.', icon: 'arrow', action: { label: 'Open swaps', page: 'swaps' as never } },
];
