#include "test.h"

#include <coinshift/net.h>
#include <coinshift/types.h>

using namespace coinshift;

TEST(coinshift_magic_bytes)
{
    // lib/net/peer/message.rs tests: Regtest 2d0cd800, Signet 2d0cd801, Betanet 2d0cd804
    auto b = magic_bytes(Network::Betanet);
    CHECK_EQ(bt::hex_encode(bt::ByteSpan(b.data(), 4)), std::string("2d0cd804"));
    auto r = magic_bytes(Network::Regtest);
    CHECK_EQ(bt::hex_encode(bt::ByteSpan(r.data(), 4)), std::string("2d0cd800"));
    auto s = magic_bytes(Network::Signet);
    CHECK_EQ(bt::hex_encode(bt::ByteSpan(s.data(), 4)), std::string("2d0cd801"));
    CHECK_EQ(DEFAULT_PORT, uint16_t(4255));
}
