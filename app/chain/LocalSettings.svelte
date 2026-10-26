<script lang="ts">
  // Settings' tabs of the drop-in: the node the app runs, the wallet's backup phrase, and Coinshift's L1 nodes.
  import NodeSettings from './dropin/NodeSettings.svelte';
  import WalletSettings from './dropin/WalletSettings.svelte';
  import L1Nodes from './cs/L1Nodes.svelte';
  let { tab }: { tab: string } = $props();
</script>

{#if tab === 'dropnode'}<NodeSettings />{:else if tab === 'dropwallet'}<WalletSettings />{:else if tab === 'l1nodes'}<L1Nodes />{/if}
