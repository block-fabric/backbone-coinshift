#include "test.h"

#include <coinshift/privfile.h>

#include <bt/fsperm.h>

#include <filesystem>
#include <unistd.h>

TEST(private_file_is_0600_and_refuses_symlinks)
{
    auto dir = std::filesystem::temp_directory_path() / ("cs-privfile-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::string path = (dir / "sub" / "l1_rpc_configs.json").string(), err;
    REQUIRE(coinshift::write_private_file(path, "{\"a\":1}", err));
    CHECK_EQ(bt::fsperm::mode_of(path) & 0777, 0600);
    CHECK_EQ(bt::fsperm::mode_of((dir / "sub").string()) & 0777, 0700);
    std::string link = (dir / "sub" / "link.json").string();
    std::filesystem::create_symlink(path, link);
    CHECK(!coinshift::write_private_file(link, "x", err));
    std::filesystem::remove_all(dir);
}
