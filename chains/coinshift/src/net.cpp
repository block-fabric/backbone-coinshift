// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <coinshift/net.h>

namespace coinshift {

// lib/net/mod.rs:166-216 (v0.14.8)
std::vector<std::string> seed_peers(Network n)
{
    auto p = ":" + std::to_string(DEFAULT_PORT);
    switch (n) {
    case Network::Betanet: return {"seed.beta.ecash.eu.com" + p};
    case Network::Signet: return {"172.105.148.135" + p, "95.217.243.12" + p};
    case Network::Forknet: return {};
    case Network::Regtest: return {};
    }
    return {};
}

} // namespace coinshift
