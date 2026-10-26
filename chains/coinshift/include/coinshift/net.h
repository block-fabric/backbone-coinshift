// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Coinshift's wire traits for the core's P2P template, and its network constants
// (lib/net/peer/message.rs:18-36, lib/types/net.rs, lib/net/mod.rs:166-216, app/cli.rs).
#pragma once

#include <coinshift/types.h>

#include <bt/sidechain/mainchain_store.h>
#include <bt/sidechain/p2p.h>

namespace coinshift {

using bt::sidechain::BmmResult;
using bt::sidechain::Network;
using bt::sidechain::TipRef;
using bt::sidechain::U256;

/** "SHIFT" Baudot prefix (25 bits); Betanet = 2d0cd804, Regtest 00, Signet 01. */
inline std::array<uint8_t, 4> magic_bytes(Network n) { return bt::sidechain::magic_bytes({0x2d, 0x0c, 0xd8, 0x00}, n); }
constexpr uint16_t DEFAULT_PORT = 4000 + THIS_SIDECHAIN;     // P2P 4255
constexpr uint16_t DEFAULT_RPC_PORT = 6000 + THIS_SIDECHAIN; // JSON-RPC 6255
/** lib/net/mod.rs: seed peers per network (Betanet: seed.beta.ecash.eu.com; Signet: 2 IPs). */
std::vector<std::string> seed_peers(Network n);
/** CARGO_PKG_VERSION of the pinned release (PeerState.version, log dir). */
inline const char* VERSION = "0.14.8";

/** Codec traits for P2P<WireChain> (bincode on the wire). */
struct WireChain {
    using Header = coinshift::Header;
    using Body = coinshift::Body;
    using AuthorizedTransaction = coinshift::AuthorizedTransaction;
    static constexpr size_t MIN_HEADER_BYTES = 32 + 1 + 8 + 32 + 8;
    static Hash32 hash(const Header& h) { return h.hash(); }
    static void encode_header(Writer& w, const Header& h) { bincode(w, h); }
    static Header decode_header(Reader& r) { return de_header(r); }
    static void encode_body(Writer& w, const Body& b) { bincode(w, b); }
    static Body decode_body(Reader& r) { return de_body(r); }
    static void encode_authorized_tx(Writer& w, const AuthorizedTransaction& t) { bincode(w, t); }
    static AuthorizedTransaction decode_authorized_tx(Reader& r) { return de_authorized_transaction(r); }
    static std::vector<Hash32> txids(const Body& b)
    {
        std::vector<Hash32> r;
        for (auto& t : b.transactions) r.push_back(t.txid());
        return r;
    }
    static std::array<uint8_t, 4> magic(Network n) { return magic_bytes(n); }
    static std::vector<std::string> seed_peers(Network n) { return coinshift::seed_peers(n); }
    static bt::sidechain::Version p2p_version() { return bt::sidechain::Version{0, 14, 8}; }
    static size_t body_size(const Body& b) { return borsh_bytes(b).size(); }
};

using WireP2P = bt::sidechain::P2P<WireChain>;

} // namespace coinshift
