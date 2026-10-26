// Wallet keys: the same mnemonic gives the same 20 addresses as THEIR wallet (tools/oracle `wallet`).
#include "test.h"

#include <coinshift/wallet.h>

#include <bt/frost.h>
#include <bt/hd.h>

#include <fstream>

TEST(wallet_addresses_equal_theirs)
{
    bt::frost::init();
    auto p = bt::hd::parse_mnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about");
    REQUIRE(p.has_value());
    auto seed = bt::hd::mnemonic_to_seed(*p);
    std::ifstream f(bttest::data_dir() + "/oracle/wallet_abandon.txt");
    std::string line;
    uint32_t i = 0;
    while (std::getline(f, line)) {
        auto k = coinshift::Wallet::signing_key(seed, i++);
        CHECK_EQ(bt::sidechain::address_from_vk(k.verifying_key()).to_base58(), line);
    }
    CHECK_EQ(i, 20u);
}
