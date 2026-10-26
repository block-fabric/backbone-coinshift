# JSON-RPC surface (for the app UI layer)

Same names, parameter order and serde_json shapes as coinshift-rs v0.14.8 (`rpc-api/rpc.rs`);
errors are code -1 with their anyhow chain text, bad parameters -32602 with serde's text.

Chain: getblockcount, get_block_hash(height), get_block(hash), get_block_index(hash),
get_best_sidechain_block_hash, get_best_mainchain_block_hash, get_bmm_inclusions(hash),
list_utxos, list_mempool, sidechain_wealth, pending_withdrawal_bundle,
latest_failed_withdrawal_bundle_height, mainchain_sync_progress, invalidate_block(hash).
Wallet: generate_mnemonic, set_seed_from_mnemonic(mnemonic), get_new_address, get_wallet_addresses,
get_wallet_utxos, balance, transfer(dest, value_sats, fee_sats), transfer_many(dests, fee_sats),
withdraw(mainchain_address, amount_sats, fee_sats, mainchain_fee_sats), create_deposit(address,
value_sats, fee_sats), format_deposit_address(address).
Swaps: create_swap(parent_chain, l1_recipient_address, l1_amount_sats, l2_recipient?, l2_amount_sats,
required_confirmations?, fee_sats) -> [swap_id, txid]; accept_swap(swap_id, l2_claimer_address?,
fee_sats?); claim_swap(swap_id, l2_claimer_address?); update_swap_l1_txid(swap_id, l1_txid_hex,
confirmations, l2_claimer_address?); get_swap_status(swap_id); list_swaps; list_swaps_by_recipient;
cancel_swap(swap_id); reconstruct_swaps. Swap ids are JSON arrays of 32 numbers.
Mining/peers/node: mine(fee?), get_block_template, connect_block(block, main_block_hash),
connect_peer(addr), forget_peer(addr), list_peers, remove_from_mempool(txid), openapi_schema, stop.

A UI plugs in like Thunder/BitNames: the App class owns Node + Wallet; register_rpc() is the only
binding, so libs/appui can call App methods directly or the RPC. A UI should show swap state as
node-local and warn before cancel_swap (the escrow stays locked on other nodes).
