// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Coinshift (eCash betanet slot 255) consensus types, byte-exact with LayerTwo Labs'
// coinshift-rs v0.14.8 (ef8e914, lib/types/). Every hash is BLAKE3(borsh(x)); the P2P wire and
// their LMDB tables use bincode 1.3 over serde. Citations are coinshift-rs paths at v0.14.8.
//
// Family: Thunder-style Utreexo (inputs carry the utxo hash, the header commits to the
// accumulator roots, txs carry a serde-only proof), FROST/ristretto255 signatures over
// borsh(tx), the fee- and size-annotated CBMT of the octobocto forks, plus Coinshift's swap
// data (TxData) and the SwapPending output content.
#pragma once

#include <bt/bitcoin.h>
#include <bt/bytes.h>
#include <bt/codec.h>
#include <bt/crypto.h>
#include <bt/frost.h>
#include <bt/sidechain/types.h>
#include <bt/utreexo.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace coinshift {

using bt::append;
using bt::Bytes;
using bt::ByteSpan;
using bt::DecodeError;
using bt::Hash32;
using bt::Reader;
using bt::Writer;
using bt::sidechain::Address;
using bt::sidechain::InPoint;
using bt::sidechain::OutPoint;
using bt::sidechain::StateError;

constexpr uint8_t THIS_SIDECHAIN = 255; // types/mod.rs:44

/** types/swap.rs ParentChainType (derived borsh u8 / serde u32 variant index). */
enum class ParentChain : uint8_t { BTC = 0, BCH = 1, LTC = 2, Signet = 3, Regtest = 4 };
const char* parent_chain_name(ParentChain c); // serde variant name ("BTC", "Signet", ...)
std::optional<ParentChain> parent_chain_from_name(std::string_view s);
uint32_t default_confirmations(ParentChain c);
uint32_t default_swap_expiration_blocks(ParentChain c);
uint32_t accept_expiration_blocks(ParentChain c);
uint32_t max_l1_tx_age_blocks(ParentChain c);
const char* coin_name(ParentChain c);

/** transaction.rs content::Content */
struct Content {
    enum Kind : uint8_t { Value = 0, Withdrawal = 1, SwapPending = 2 };
    Kind kind = Value;
    uint64_t value = 0;
    uint64_t main_fee = 0;          // Withdrawal
    bt::btc::Address main_address;  // Withdrawal
    Hash32 swap_id;                 // SwapPending
    /** GetValue: Withdrawal = value + main_fee saturating at u64::MAX. */
    uint64_t get_value() const;
    bool is_value() const { return kind == Value; }
    bool is_withdrawal() const { return kind == Withdrawal; }
    bool is_swap_pending() const { return kind == SwapPending; }
    bool operator==(const Content& o) const;
};

struct Output {
    Address address;
    Content content;
    uint64_t get_value() const { return content.get_value(); }
    bool operator==(const Output&) const = default;
};

/** transaction.rs TxData. Variant order is consensus (borsh/serde tags). */
struct TxData {
    enum Kind : uint8_t { Regular = 0, SwapCreate = 1, SwapClaim = 2, SwapAccept = 3 };
    Kind kind = Regular;
    Hash32 swap_id;                            // all swap kinds
    // SwapCreate
    ParentChain parent_chain = ParentChain::BTC;
    Bytes l1_txid_bytes;
    uint32_t required_confirmations = 0;
    std::optional<Address> l2_recipient;
    uint64_t l2_amount = 0;
    std::string l1_recipient_address;
    uint64_t l1_amount = 0;
    // SwapClaim: optional claimer; SwapAccept: the claimer (always set)
    std::optional<Address> l2_claimer_address;
    std::optional<Bytes> proof_data; // SwapClaim
    bool operator==(const TxData&) const = default;
};

struct Transaction {
    std::vector<std::pair<OutPoint, Hash32>> inputs; // (outpoint, utxo_hash)
    bt::utreexo::Proof proof;                        // #[borsh(skip)], serde only
    std::vector<Output> outputs;
    TxData data;

    Bytes canonical_bytes() const; // borsh
    /** transaction.rs canonical_size: borsh::object_length / 8 (sic). */
    uint64_t canonical_size() const { return canonical_bytes().size() / 8; }
    Hash32 txid() const;
};

struct Authorization {
    bt::frost::Point verifying_key;
    bt::frost::Signature signature;
};

struct AuthorizedTransaction {
    Transaction transaction;
    std::vector<Authorization> authorizations;
};

struct Body {
    std::vector<Output> coinbase;
    std::vector<Transaction> transactions;
    std::vector<Authorization> authorizations;
    size_t inputs_len() const;
    std::vector<AuthorizedTransaction> authorized_transactions() const;
    static Body from_authorized(std::vector<AuthorizedTransaction> txs, std::vector<Output> coinbase);
};

/** types/mod.rs Header */
struct Header {
    Hash32 merkle_root;
    std::optional<Hash32> prev_side_hash;
    Hash32 prev_main_hash; // bitcoin block hash, internal byte order
    std::vector<bt::utreexo::NodeHash> roots;

    Hash32 hash() const;
    bool operator==(const Header& o) const
    {
        return merkle_root == o.merkle_root && prev_side_hash == o.prev_side_hash && prev_main_hash == o.prev_main_hash &&
               roots == o.roots;
    }
};

struct Block {
    Header header;
    Body body;
};

/** FilledTransaction: a transaction with the outputs it spends. */
struct FilledTransaction {
    const Transaction* tx = nullptr;
    std::vector<Output> spent_utxos;
    std::optional<uint64_t> value_in() const;
    std::optional<uint64_t> value_out() const;
    /** get_fee: nullopt on overflow or underfunding (ComputeFeeError). */
    std::optional<uint64_t> fee() const;
};

// ---------------------------------------------------------------- swaps (types/swap.rs)
/** SwapId::from_l2_to_l1 */
Hash32 swap_id_l2_to_l1(const std::string& l1_recipient_address, uint64_t l1_amount_sats, const Address& l2_sender,
                        const std::optional<Address>& l2_recipient);

/** SwapTxId = Hash32([u8;32]) | Hash(Vec<u8>) */
struct SwapTxId {
    enum Kind : uint8_t { Hash32 = 0, Hash = 1 };
    Kind kind = Hash32;
    Bytes bytes; // 32 bytes for Hash32
    static SwapTxId from_bytes(ByteSpan b);
    static SwapTxId zero() { return from_bytes(Bytes(32, 0)); }
    /** "is_new" / "has_l1_tx" helpers: Hash32 all-zero, or Hash empty / all-zero. */
    bool is_unset() const;
    std::string to_hex() const; // canonical order
    bool operator==(const SwapTxId&) const = default;
    auto operator<=>(const SwapTxId&) const = default;
};

/** SwapState */
struct SwapState {
    enum Kind : uint8_t { Pending = 0, WaitingConfirmations = 1, ReadyToClaim = 2, Completed = 3, Cancelled = 4 };
    Kind kind = Pending;
    uint32_t current = 0, required = 0; // WaitingConfirmations
    bool operator==(const SwapState&) const = default;
};

/** types/swap.rs Swap (stored as SerdeBincode<Swap>). */
struct Swap {
    Hash32 id;
    uint8_t direction = 1; // SwapDirection: L1ToL2 = 0, L2ToL1 = 1
    ParentChain parent_chain = ParentChain::BTC;
    SwapTxId l1_txid;
    uint32_t required_confirmations = 0;
    SwapState state;
    std::optional<Address> l2_recipient;
    uint64_t l2_amount = 0;
    std::string l1_recipient_address;
    uint64_t l1_amount = 0;
    std::optional<std::string> l1_claimer_address;
    std::optional<Address> l2_claimer_address;
    uint32_t created_at_height = 0;
    std::optional<uint32_t> expires_at_height;
    std::optional<Hash32> l1_txid_validated_at_block_hash;
    std::optional<uint32_t> l1_txid_validated_at_height;
    std::optional<Address> l2_creator_address;
    bool operator==(const Swap&) const = default;
};

/** SwapReservation */
struct SwapReservation {
    Address claimer;
    uint32_t accepted_at_height = 0;
    uint32_t expires_at(ParentChain c) const;
    bool is_live_at(ParentChain c, uint32_t height) const { return height < expires_at(c); }
};

// ---------------------------------------------------------------- borsh
void borsh(Writer& w, const Address& a);
void borsh(Writer& w, const Content& c);
void borsh(Writer& w, const Output& o);
void borsh(Writer& w, const TxData& d);
void borsh(Writer& w, const Transaction& t);
void borsh(Writer& w, const Authorization& a);
void borsh(Writer& w, const Body& b);
void borsh(Writer& w, const Header& h);
template <class T>
Bytes borsh_bytes(const T& x)
{
    Writer w;
    borsh(w, x);
    return w.buf;
}
template <class T>
Hash32 hash(const T& x)
{
    return bt::blake3(borsh_bytes(x));
}
/** utxo_hash / Utreexo leaf = blake3(borsh(PointedOutput{outpoint, output})). */
Hash32 pointed_output_hash(const OutPoint& op, const Output& out);

// ---------------------------------------------------------------- bincode (serde)
void bincode(Writer& w, const Address& a);
void bincode(Writer& w, const Content& c);
void bincode(Writer& w, const Output& o);
void bincode(Writer& w, const TxData& d);
void bincode(Writer& w, const bt::utreexo::NodeHash& h);
void bincode(Writer& w, const bt::utreexo::Proof& p);
void bincode(Writer& w, const Transaction& t);
void bincode(Writer& w, const Authorization& a);
void bincode(Writer& w, const AuthorizedTransaction& t);
void bincode(Writer& w, const Body& b);
void bincode(Writer& w, const Header& h);
void bincode(Writer& w, const Block& b);
void bincode(Writer& w, const Swap& s);
void bincode(Writer& w, const SwapReservation& r);

Address de_address(Reader& r);
Content de_content(Reader& r);
Output de_output(Reader& r);
TxData de_txdata(Reader& r);
bt::utreexo::NodeHash de_nodehash(Reader& r);
bt::utreexo::Proof de_proof(Reader& r);
Transaction de_transaction(Reader& r);
Authorization de_authorization(Reader& r);
AuthorizedTransaction de_authorized_transaction(Reader& r);
Body de_body(Reader& r);
Header de_header(Reader& r);
Swap de_swap(Reader& r);
SwapReservation de_reservation(Reader& r);

/** borsh decoders (RPC raw hex, tools). */
Output borsh_de_output(Reader& r);
Transaction borsh_de_transaction(Reader& r);

// ---------------------------------------------------------------- merkle (types/mod.rs:900-945)
/** Body::compute_merkle_root: hash((hash(coinbase), txs_root)) where txs_root is the
 *  fee/size-annotated CBMT over the transactions. nullopt where theirs errors (a tx fee
 *  that cannot be computed); `panic` set where theirs panics (Amount addition overflow). */
std::optional<Hash32> compute_merkle_root(const std::vector<Output>& coinbase, const std::vector<FilledTransaction>& txs,
                                          bool* panic = nullptr);

// ---------------------------------------------------------------- limits (state/mod.rs)
uint64_t body_sigops_limit(uint32_t height);
uint64_t body_size_limit(uint32_t height);
double powi(double a, int b);

// ---------------------------------------------------------------- authorizations (authorization.rs)
/** verify_authorizations: exact count, then every signature over borsh(tx) of its tx. */
bool verify_authorizations(const Body& body);
/** verify_authorized_transaction: exact count; no inputs = Ok. */
bool verify_authorized_transaction(const AuthorizedTransaction& tx);

// ---------------------------------------------------------------- withdrawal bundles
/** OP_DRIVECHAIN_SCRIPT etc. are not needed; bundles use bt::sidechain helpers. */

} // namespace coinshift
