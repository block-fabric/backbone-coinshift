// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Coinshift wallet, key-compatible with coinshift-rs v0.14.8 (lib/wallet/{mod.rs, bip32.rs}):
//   seed = BIP-39 (tiny-bip39, empty passphrase); "bip32ish" over ristretto255 on the path
//   m/43'/1899'/0'/255'/0'/index; address = BLAKE3-XOF(compressed VK)[..20]. Addresses start at
//   index 0, change goes to a fresh address. Like theirs the wallet keeps its own UTXO/STXO
//   tables, refreshed from the node at every tip change (update_wallet).
// Their LMDB wallet is not read: import by mnemonic restores the same keys and addresses.
#pragma once

#include <coinshift/chain.h>

#include <bt/kv.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace coinshift {

struct WalletError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Balance {
    uint64_t total = 0, available = 0;
};

class Wallet {
public:
    Wallet(const std::string& path, Node& node);
    bool has_seed();
    /** set_seed: error if a different seed exists ("seed has already been set"). */
    void set_seed(const bt::ByteArray<64>& seed);
    void set_seed_from_mnemonic(const std::string& mnemonic);

    Address get_new_address();
    Address get_receive_address();
    std::set<Address> addresses();
    bool is_mine(const Address& a);
    std::map<OutPoint, Output> utxos();
    Balance balance();
    void put_utxos(const std::map<OutPoint, Output>& u);
    /** update_wallet (app.rs): node UTXOs at our addresses in, our spent outpoints out. */
    void update();
    /** recover_addresses_from_utxo_set (gap limit 20). */
    size_t recover_addresses(const std::set<Address>& utxo_addresses);

    using IsLocked = std::function<bool(const OutPoint&)>;
    Transaction create_transaction_many(const std::map<Address, uint64_t>& dests, uint64_t fee, const IsLocked& locked);
    Transaction create_withdrawal(const bt::btc::Address& main_address, uint64_t value, uint64_t main_fee, uint64_t fee, const IsLocked& locked);
    std::pair<Transaction, Hash32> create_swap_create_tx(ParentChain pc, const std::string& l1_recipient, uint64_t l1_amount,
                                                         const std::optional<Address>& l2_recipient, uint64_t l2_amount,
                                                         std::optional<uint32_t> required_confirmations, uint64_t fee, const IsLocked& locked);
    Transaction create_swap_accept_tx(const Hash32& swap_id, const std::optional<Address>& claimer, uint64_t fee, const IsLocked& locked);
    Transaction create_swap_claim_tx(const Hash32& swap_id, const Address& recipient, const std::vector<std::pair<OutPoint, Output>>& locked_outputs,
                                     const std::optional<Address>& l2_claimer);
    AuthorizedTransaction authorize(const Transaction& tx);

    static bt::frost::SigningKey signing_key(const bt::ByteArray<64>& seed, uint32_t index);

private:
    std::unique_ptr<bt::kv::Store> store_;
    Node& node_;
    std::recursive_mutex mu_;
    std::optional<bt::ByteArray<64>> seed();
    bt::ByteArray<64> need_seed();
    std::pair<uint64_t, std::vector<std::pair<OutPoint, Output>>> select_coins(uint64_t value, const IsLocked& locked);
    std::vector<std::pair<OutPoint, Hash32>> inputs_of(const std::vector<std::pair<OutPoint, Output>>& coins);
    bt::utreexo::Proof prove(const std::vector<std::pair<OutPoint, Hash32>>& inputs);
    void ensure_address_indexed(const Address& a);
};

} // namespace coinshift
