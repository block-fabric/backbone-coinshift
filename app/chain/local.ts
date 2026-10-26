// Coinshift's own pages beside the drop-in pages (chain/overrides.ts puts those in the kit's places): the swaps of the
// chain (open ones to take, the wallet's own, each one's status), and offering a swap. Their host methods are
// coinshift.* (chains/coinshift/src/appui.cpp); the L1 nodes the app checks payments on are in Settings.
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  id: string;
  title: string;
  icon: IconName;
  summary: string;
  section: 'wallet' | 'chain' | 'tools';
  wallet?: boolean;
  shortcut?: string;
  keywords?: string;
  quick?: { label: string; icon?: IconName; tab?: string; simpleLabel?: string };
  simpleTitle?: string;
  advanced?: boolean;
  simple?: boolean;
  simpleSummary?: string;
  tabOf?: { page: string; tab: string };
}

export const LOCAL_PAGES = [
  { id: 'swaps', title: 'Swaps', simpleTitle: 'Swaps', simple: true, simpleSummary: 'Trade ECX for coins of another chain', icon: 'swap', section: 'chain', shortcut: 'Alt+6', summary: 'Open swaps to take, your swaps, and where each one stands', keywords: 'swap trade exchange bitcoin btc bch ltc l1 open accept claim status', quick: { label: 'Swaps', icon: 'swap' } },
  { id: 'new-swap', title: 'Offer a swap', simpleTitle: 'Offer a swap', simple: true, simpleSummary: 'Offer ECX for coins of another chain', icon: 'offer', section: 'chain', summary: 'Lock ECX for coins paid to you on another chain (L2 to L1)', wallet: true, keywords: 'create new swap offer sell l2 l1', quick: { label: 'Offer a swap', icon: 'offer' } },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'cs.open', label: 'Open swaps', page: 'swaps', tab: 'open', tip: 'The swaps anyone can take', keywords: 'take accept reserve' },
  { id: 'cs.mine', label: 'My swaps', page: 'swaps', tab: 'mine', tip: 'The swaps you offered, reserved or may claim', keywords: 'claim status' },
  { id: 'cs.new', label: 'Offer a swap…', page: 'new-swap', tip: 'Lock ECX for coins paid to you on another chain', keywords: 'create sell' },
] as const satisfies readonly LocalCommand[];
