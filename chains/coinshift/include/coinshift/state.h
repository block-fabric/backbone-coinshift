// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Coinshift sidechain state: UTXO/STXO sets, the Utreexo accumulator, the two-way peg
// (lib/state/two_way_peg_data.rs, the older thunder-family variant: pending bundle stored
// beside its height, statuses Submitted/Confirmed/Failed) and the swap tables
// (lib/state/{mod.rs, block.rs, swap.rs}). Literal port of coinshift-rs v0.14.8: their branch
// order, their error texts, their panics as StateError{panic = true}.
//
// Swap state is part node-local by their design: a swap's `state` field moves with this
// node's parent-chain (L1) monitoring (the RPC update_swap_l1_txid, the L1 config file), and
// the expiry rule at each two-way-peg connect reads it. Reproduced as is; see doc/coinshift.md.
#pragma once

#include <coinshift/types.h>

#include <bt/kv.h>
#include <bt/mainchain.h>
#include <bt/sidechain/peg.h>
#include <bt/sidechain/rollback.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace coinshift {

using bt::kv::Txn;
using SpentOutput = bt::sidechain::SpentOutputT<Output>;
using WithdrawalBundle = bt::sidechain::WithdrawalBundleT<Output>;
using BlockIndexEvents = bt::sidechain::BlockIndexEventsT<Output>;

constexpr uint32_t WITHDRAWAL_BUNDLE_FAILURE_GAP = 4; // state/mod.rs

/** types/mod.rs WithdrawalBundleStatus (serde u32 index). */
enum class BundleStatus : uint8_t { Confirmed = 0, Failed = 1, Submitted = 2 };
const char* to_string(BundleStatus s);

/** state/mod.rs WithdrawalBundleInfo */
struct WithdrawalBundleInfo {
    enum Kind : uint8_t { Known = 0, Unknown = 1, UnknownConfirmed = 2 } kind = Unknown;
    WithdrawalBundle bundle;            // Known
    std::map<OutPoint, Output> spend_utxos; // UnknownConfirmed
};

/** AccumulatorDiff (types/mod.rs): ordered, insert/remove cancel out once. */
class AccumulatorDiff {
public:
    void insert(const Hash32& h);
    void remove(const Hash32& h);
    bool empty() const { return order_.empty(); }
    /** (insertions, deletions) in entry order. */
    std::pair<std::vector<bt::utreexo::NodeHash>, std::vector<bt::utreexo::NodeHash>> split() const;

private:
    std::vector<std::pair<Hash32, bool>> order_; // live entries (erased on cancel)
    std::map<Hash32, size_t> idx_;               // hash -> index in order_
    void erase(const Hash32& h);
};

/** Prevalidated block (state/mod.rs PrevalidatedBlock). */
struct Prevalidated {
    std::vector<Transaction> txs; // copies the filled txs point into
    std::vector<FilledTransaction> filled;
    Hash32 merkle_root;
    uint64_t total_fees = 0, coinbase_value = 0;
    uint32_t next_height = 0;
    AccumulatorDiff diff;
};

/** parent_chain_rpc.rs RpcConfig + the lookup of l1_rpc_configs.json. */
struct L1RpcConfig {
    std::string url, user, password;
};
using L1ConfigGetter = std::function<std::optional<L1RpcConfig>(ParentChain)>;

/** One match of find_transactions_by_address_and_amount: (sender, txid hex RPC order, confirmations, blockheight). */
struct L1Match {
    std::string sender;
    std::string txid;
    uint32_t confirmations = 0;
    std::optional<uint32_t> blockheight;
};
/** The L1 lookup (query_and_update_swap): nullopt = RPC error (logged, swap untouched). */
using L1Finder = std::function<std::optional<std::vector<L1Match>>(const L1RpcConfig&, const std::string& address, uint64_t amount_sats)>;

class State {
public:
    enum Table : uint8_t {
        T_TIP = 1, T_HEIGHT, T_UTXOS, T_STXOS, T_PENDING_BUNDLE, T_LATEST_FAILED, T_BUNDLES, T_DEPOSIT_BLOCKS,
        T_BUNDLE_EVENT_BLOCKS, T_BLOCK_INDEX_EVENTS, T_ACCUMULATOR, T_SWAPS, T_SWAPS_BY_L1TXID, T_SWAPS_BY_RECIPIENT,
        T_LOCKED, T_EXPIRED_SWAPS, T_RESERVATIONS, T_ACC_BY_BLOCK
    };

    /** L1 monitoring (node-local): unset = no L1 config (their default when the file is absent). */
    L1ConfigGetter l1_config;
    L1Finder l1_find;

    // ------------------------------------------------------------------ getters
    std::optional<Hash32> tip(const Txn& t) const;
    std::optional<uint32_t> height(const Txn& t) const;
    std::optional<Output> utxo(const Txn& t, const OutPoint& op) const;
    std::optional<SpentOutput> stxo(const Txn& t, const OutPoint& op) const;
    std::vector<std::pair<OutPoint, Output>> utxos(const Txn& t) const;
    std::vector<std::pair<OutPoint, SpentOutput>> stxos(const Txn& t) const;
    /** get_pending_withdrawal_bundle: (bundle, height it was collected at). */
    std::optional<std::pair<WithdrawalBundle, uint32_t>> pending_bundle(const Txn& t) const;
    /** get_latest_failed_withdrawal_bundle: (height, m6id); asserts its latest status is Failed. */
    std::optional<std::pair<uint32_t, Hash32>> latest_failed_bundle(const Txn& t) const;
    std::optional<std::pair<WithdrawalBundleInfo, bt::sidechain::RollBack<BundleStatus>>> bundle(const Txn& t, const Hash32& m6id) const;
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> deposit_blocks(const Txn& t) const;
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> bundle_event_blocks(const Txn& t) const;
    BlockIndexEvents block_index_events(const Txn& t, uint32_t height) const;
    uint64_t sidechain_wealth(const Txn& t) const;
    bt::utreexo::MemForest accumulator(const Txn& t) const;
    std::optional<bt::utreexo::MemForest> block_accumulator(const Txn& t, const Hash32& block) const;
    std::optional<Hash32> last_deposit_block_hash(const Txn& t) const;
    std::optional<Hash32> last_bundle_event_block_hash(const Txn& t) const;

    // ------------------------------------------------------------------ swaps
    std::optional<Swap> swap(const Txn& t, const Hash32& id) const;
    std::vector<Swap> swaps(const Txn& t) const; // load_all_swaps (key order)
    std::vector<Swap> swaps_by_recipient(const Txn& t, const Address& a) const;
    std::optional<Hash32> locked_to(const Txn& t, const OutPoint& op) const;
    std::optional<SwapReservation> reservation(const Txn& t, const Hash32& id) const;
    /** entitled_claimer_at */
    std::optional<Address> entitled_claimer_at(const Txn& t, const Swap& s, uint32_t height) const;
    void save_swap(Txn& t, const Swap& s) const;
    void delete_swap_unchecked(Txn& t, const Hash32& id) const;
    uint32_t unlock_all_outputs_for_swap(Txn& t, const Hash32& id) const;
    void lock_output(Txn& t, const OutPoint& op, const Hash32& id) const;
    void unlock_output(Txn& t, const OutPoint& op) const;
    void reserve_swap(Txn& t, const Hash32& id, const Address& claimer, uint32_t height) const;
    void clear_reservation(Txn& t, const Hash32& id) const;
    /** cancel_swap (RPC, node-local): creator check, Pending only, unlock, Cancelled. */
    void cancel_swap(Txn& t, const Hash32& id, const std::optional<Address>& creator) const;
    /** update_swap_l1_txid (RPC and the confirmation task, node-local). */
    void update_swap_l1_txid(Txn& t, const Hash32& id, const SwapTxId& l1_txid, uint32_t confirmations,
                             const std::optional<std::string>& l1_claimer, const std::optional<Address>& l2_claimer,
                             const Hash32& block_hash, uint32_t block_height) const;
    /** cleanup_orphaned_locks */
    uint32_t cleanup_orphaned_locks(Txn& t) const;

    // ------------------------------------------------------------------ transactions
    std::vector<Output> fill_spent(const Txn& t, const Transaction& tx) const; // NoUtxo
    std::vector<Output> fill_spent_from_stxos(const Txn& t, const Transaction& tx) const;
    /** validate_filled_transaction: utxo hashes, withdrawal spends, value -> fee. */
    uint64_t validate_filled(const FilledTransaction& ft) const;
    /** validate_transaction (mempool / RPC / P2P): returns the fee. */
    uint64_t validate_transaction(const Txn& t, const AuthorizedTransaction& tx) const;
    /** Our mempool/relay/template policy on top of validate_transaction (never consensus). */
    void policy_check(const Txn& t, const AuthorizedTransaction& tx) const;
    /** Node hook (P2P push / broadcast): their net task regenerates the proof before validating. */
    void prepare_tx(const Txn& t, AuthorizedTransaction& tx) const { regenerate_proof(t, tx.transaction); }
    /** Node hook: template_check runs the policy too. */
    void template_check(Txn& t, const AuthorizedTransaction& tx, uint32_t) const { policy_check(t, tx); }
    /** regenerate_proof: prove the inputs' utxo hashes against the current accumulator. */
    void regenerate_proof(const Txn& t, Transaction& tx) const;

    // ------------------------------------------------------------------ blocks (state/block.rs)
    Prevalidated prevalidate(const Txn& t, const Header& h, const Body& b) const;
    void connect_prevalidated(Txn& t, const Header& h, const Body& b, Prevalidated& pre) const;
    void disconnect_tip(Txn& t, const Header& h, const Body& b) const;
    /** Utreexo roots after a block (the template's header roots, Body::modify_memforest). */
    static std::vector<bt::utreexo::NodeHash> roots_after(bt::utreexo::MemForest acc, const Body& b, const Hash32& merkle_root);

    // ------------------------------------------------------------------ two-way peg
    void connect_2wpd(Txn& t, const bt::mainchain::TwoWayPegData& d) const;
    void disconnect_2wpd(Txn& t, const bt::mainchain::TwoWayPegData& d) const;
    /** net_task.rs: after a disconnect the state accumulator is the archive's for the new tip. */
    void restore_accumulator_for_tip(Txn& t) const;

    // ------------------------------------------------------------------ encodings
    static void enc_output(Writer& w, const Output& o) { bincode(w, o); }
    static Output dec_output(Reader& r) { return de_output(r); }
    static Bytes u32le(uint32_t v);
    void put_accumulator(Txn& t, const bt::utreexo::MemForest& f) const;

    /** Withdrawal bundles (types/mod.rs WithdrawalBundle::new, two_way_peg_data.rs collect). */
    static std::optional<WithdrawalBundle> make_withdrawal_bundle(uint32_t block_height, uint64_t fee, std::map<OutPoint, Output> spend,
                                                                  std::vector<bt::btc::TxOut> payouts);
    std::optional<WithdrawalBundle> collect_withdrawal_bundle(const Txn& t, uint32_t block_height) const;

private:
    void set_tip(Txn& t, const Hash32& h, uint32_t height) const;
    void put_utxo(Txn& t, const OutPoint& op, const Output& o) const;
    bool del_utxo(Txn& t, const OutPoint& op) const;
    void put_stxo(Txn& t, const OutPoint& op, const SpentOutput& s) const;
    bool del_stxo(Txn& t, const OutPoint& op) const;
    void put_bundle(Txn& t, const Hash32& m6id, const WithdrawalBundleInfo& i, const bt::sidechain::RollBack<BundleStatus>& s) const;
    void apply_diff(Txn& t, const AccumulatorDiff& d) const;
    void put_pending(Txn& t, const WithdrawalBundle& b, uint32_t height) const;
    std::optional<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> seq_last(const Txn& t, uint8_t table) const;
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> seq_table(const Txn& t, uint8_t table) const;

    // swap validation (state/swap.rs)
    void validate_swap_create(const Txn& t, const Transaction& tx, const FilledTransaction& ft) const;
    void validate_swap_accept(const Txn& t, const Transaction& tx, const FilledTransaction& ft) const;
    void validate_swap_claim(const Txn& t, const Transaction& tx) const; // mempool half
    void validate_swap_claim_consensus(const Txn& t, const Transaction& tx) const;
    void validate_no_locked_outputs(const Txn& t, const Transaction& tx) const;
    void validate_block_transaction(const Txn& t, const Transaction& tx, const FilledTransaction& ft) const;
    uint32_t validating_height(const Txn& t) const;

    // 2WPD pieces
    void connect_bundle_submitted(Txn& t, uint32_t height, AccumulatorDiff& d, BlockIndexEvents& ev, const Hash32& m6id) const;
    void connect_bundle_confirmed(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& event_block, const Hash32& m6id) const;
    void connect_bundle_failed(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& m6id) const;
    void disconnect_bundle_submitted(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& m6id) const;
    void disconnect_bundle_confirmed(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& m6id) const;
    void disconnect_bundle_failed(Txn& t, uint32_t height, AccumulatorDiff& d, const Hash32& m6id) const;
    void process_swaps(Txn& t, uint32_t height, const Hash32& block_hash) const;
};

/** SwapState / Swap JSON-ish display for logs and errors: their Debug of SwapState. */
std::string debug_string(const SwapState& s);

/** Bitcoin Amount Display ("0.0001 BTC"), as rust-bitcoin 0.32 prints it. */
std::string amount_display(uint64_t sats);

} // namespace coinshift
