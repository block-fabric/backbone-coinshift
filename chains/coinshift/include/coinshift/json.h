// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// JSON forms of Coinshift types exactly as serde_json renders them in coinshift-rs v0.14.8's
// JSON-RPC (human-readable serde; verified against their crate by tools/oracle `json`), and the
// parsers for request parameters with serde's error texts (jsonrpsee -32602 "Invalid params").
#pragma once

#include <coinshift/state.h>

#include <bt/jsonrpc.h>
#include <univalue.h>

namespace coinshift::json {

UniValue hash(const Hash32& h);     // hex (BlockHash, Txid, MerkleRoot)
UniValue btc_hash(const Hash32& h); // display order (bitcoin::BlockHash, Txid, M6id)
UniValue bytes_array(ByteSpan b);   // serde of [u8; N] / Vec<u8> without adapter: array of numbers
UniValue address(const Address& a);
UniValue outpoint(const OutPoint& o);
UniValue content(const Content& c);
UniValue output(const Output& o);
UniValue pointed_output(const OutPoint& op, const Output& o);
UniValue txdata(const TxData& d);
UniValue proof(const bt::utreexo::Proof& p);
UniValue transaction(const Transaction& t);
UniValue authorization(const Authorization& a);
UniValue body(const Body& b);
UniValue header(const Header& h);
UniValue block(const Header& h, const Body& b);
UniValue withdrawal_bundle(const WithdrawalBundle& w);
UniValue swap(const Swap& s);
UniValue swap_state(const SwapState& s);
UniValue swap_id(const Hash32& id); // SwapId([u8; 32]) newtype: array of numbers
UniValue mempool_tx(const Transaction& t);
UniValue block_index(const Body& b, const BlockIndexEvents& ev);
UniValue balance(uint64_t total, uint64_t available);
UniValue sync_progress(const char* phase, uint32_t done, uint32_t total, uint32_t tip_height);

/** A request value that does not parse: jsonrpsee's -32602 "Invalid params", serde's cause as data. */
struct Error : bt::jsonrpc::RpcError {
    explicit Error(const std::string& cause, const UniValue* at = nullptr) : bt::jsonrpc::RpcError(-32602, "Invalid params", cause, at) {}
};
std::string serde_unexpected(const UniValue& v);
Hash32 parse_hash(const UniValue& v);        // BlockHash / Txid / MerkleRoot (hex)
Hash32 parse_btc_hash(const UniValue& v);    // bitcoin::BlockHash (display hex)
Hash32 parse_swap_id(const UniValue& v);     // SwapId: [u8; 32] array
Address parse_address(const UniValue& v);
std::optional<Address> parse_opt_address(const UniValue& v);
bt::btc::Address parse_btc_address(const UniValue& v);
ParentChain parse_parent_chain(const UniValue& v);
std::map<Address, uint64_t> parse_transfer_dests(const UniValue& v); // MapPreventDuplicates
Header parse_header(const UniValue& v);
Body parse_body(const UniValue& v);
std::pair<Header, Body> parse_block(const UniValue& v);
uint64_t parse_u64(const UniValue& v);
uint32_t parse_u32(const UniValue& v);
std::optional<uint64_t> parse_opt_u64(const UniValue& v);
std::optional<uint32_t> parse_opt_u32(const UniValue& v);
std::string parse_string(const UniValue& v);
/** SocketAddr (std) from its serde string form. */
std::string parse_socket_addr(const UniValue& v);

} // namespace coinshift::json
