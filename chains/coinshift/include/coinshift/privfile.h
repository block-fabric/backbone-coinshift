// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#pragma once

#include <string>

namespace coinshift {

/** Write a file that holds credentials (l1_rpc_configs.json): the parent directory is made
 *  owner-only (0700), the data goes to a temp file created owner-only from the start (0600,
 *  O_EXCL|O_NOFOLLOW), is fsynced and renamed over the target. A symlink at the target is
 *  refused. On Windows the temp file gets an owner-only DACL before any data is written. */
bool write_private_file(const std::string& path, const std::string& content, std::string& err);

} // namespace coinshift
