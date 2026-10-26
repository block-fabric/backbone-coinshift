// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Parent-chain (L1) RPC (lib/parent_chain_rpc.rs) and l1_rpc_configs.json
// (<data_dir>/coinshift/l1_rpc_configs.json, independent of --datadir, as theirs).
#pragma once

#include <coinshift/state.h>

#include <univalue.h>

#include <filesystem>
#include <map>
#include <set>

namespace coinshift::l1 {

std::string config_path();
std::map<ParentChain, L1RpcConfig> read_configs(const std::string& path, bool* parsed);
std::optional<L1RpcConfig> load_config(const std::string& path, ParentChain c);
std::vector<std::pair<ParentChain, L1RpcConfig>> supported_configs();
bool is_supported(ParentChain c, const L1RpcConfig& cfg);
UniValue configs_json(const std::map<ParentChain, L1RpcConfig>& m);
/** write_l1_config_file: keep supported entries, add/overwrite the predefined ones for `enable`. */
bool write_config_file(const std::string& path, const std::vector<ParentChain>& enable);
/** validate_l1_config_file: "" = ok, else their error text (startup fails). */
std::string validate_config_file(const std::string& path);

struct Client {
    L1RpcConfig cfg;
    std::optional<UniValue> call(const std::string& method, const UniValue& params, std::string& err) const;
    std::optional<UniValue> get_transaction(const std::string& txid, std::string& err) const;
    /** find_transactions_by_address_and_amount */
    std::optional<std::vector<L1Match>> find(const std::string& address, uint64_t amount, std::string& err) const;
    std::optional<std::string> chain_name(std::string& err) const;
};

} // namespace coinshift::l1
