// Settings' tabs of Coinshift's app: its node (network, mainchain interface, folder), its wallet's backup phrase, and
// the L1 nodes the app checks swap payments on.
export const LOCAL_SETTINGS_TABS: readonly { id: string; label: string }[] = [
  { id: 'dropnode', label: 'Node' },
  { id: 'dropwallet', label: 'Wallet' },
  { id: 'l1nodes', label: 'L1 nodes' },
];
