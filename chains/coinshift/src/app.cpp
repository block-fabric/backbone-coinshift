// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Port of coinshift-rs v0.14.8 app/app.rs and app/rpc_server.rs.
#include <coinshift/app.h>
#include <coinshift/json.h>

#include <bt/hd.h>
#include <bt/log.h>

#include <set>

namespace coinshift {

using bt::jsonrpc::app_error;
using bt::jsonrpc::Params;
using bt::kv::Txn;

namespace {
std::string hexs(const Hash32& h) { return bt::hex_encode(h); }
/** state::Error raised under a node::Error (anyhow "{:#}" chain of app::Error::Node). */
[[noreturn]] void node_state_error(const std::exception& e) { throw app_error(std::string("node error: state error: ") + e.what()); }
} // namespace

// app.rs build_block_template: on a mainchain reorg below the tip's prev_main, mine an
// empty block on the last ancestor whose prev_main is still an ancestor.
BlockTemplate App::build_block_template(std::optional<uint64_t> fee)
{
    auto tipr = node.enforcer().get_chain_tip();
    if (!bt::enforcer::ok(tipr))
        throw app_error("Mainchain unreachable; mining requires the parentchain (mainchain) node to be up: CUSF mainchain proto error: " +
                        bt::enforcer::err(tipr).what());
    Hash32 prev_main = bt::enforcer::val(tipr).block_hash;
    auto tip_hash = node.tip();
    std::optional<Hash32> prev_side;
    if (tip_hash) {
        if (!node.request_mainchain_ancestor_infos(prev_main))
            throw app_error("Failed to request mainchain ancestor info for " + bt::hex_encode_rev(prev_main.span()));
        std::lock_guard<std::recursive_mutex> l(node.lock());
        Txn t(node.store());
        Header th = node.archive().need_header(t, *tip_hash);
        Hash32 lcma = node.archive().last_common_main_ancestor(t, prev_main, th.prev_main_hash);
        if (lcma == th.prev_main_hash) prev_side = tip_hash;
        else {
            std::optional<Hash32> cur = tip_hash;
            while (cur) {
                Header h = node.archive().need_header(t, *cur);
                if (h.prev_main_hash != lcma && node.archive().is_main_descendant(t, h.prev_main_hash, lcma)) {
                    prev_side = cur;
                    break;
                }
                cur = h.prev_side_hash;
            }
        }
    }
    BlockTemplate tpl;
    tpl.header.prev_side_hash = prev_side;
    tpl.header.prev_main_hash = prev_main;
    if (prev_side == tip_hash) {
        auto r = node.mempool_transactions(1000); // NUM_TRANSACTIONS
        std::vector<AuthorizedTransaction> ats;
        std::vector<std::vector<Output>> spent;
        uint64_t fees = 0;
        {
            std::lock_guard<std::recursive_mutex> l(node.lock());
            Txn t(node.store());
            for (auto& [tx, ft] : r.first) {
                AuthorizedTransaction a = tx;
                try {
                    node.state().regenerate_proof(t, a.transaction); // their mempool keeps proofs regenerated
                } catch (const StateError&) {
                    node.remove_from_mempool(a.transaction.txid());
                    continue;
                }
                auto f = FilledTransaction{&a.transaction, ft}.fee();
                fees += f ? *f : 0;
                ats.push_back(std::move(a));
                spent.push_back(ft);
            }
        }
        std::vector<Output> coinbase;
        if (fees) coinbase.push_back(Output{wallet.get_receive_address(), Content{Content::Value, fees}});
        tpl.body = Body::from_authorized(ats, coinbase);
        std::vector<FilledTransaction> filled;
        for (size_t i = 0; i < spent.size(); i++) filled.push_back(FilledTransaction{&tpl.body.transactions[i], spent[i]});
        auto mr = compute_merkle_root(tpl.body.coinbase, filled);
        if (!mr) throw app_error("failed to compute merkle root");
        tpl.header.merkle_root = *mr;
        std::lock_guard<std::recursive_mutex> l(node.lock());
        Txn t(node.store());
        bt::utreexo::MemForest acc;
        if (tip_hash) {
            auto a = node.state().block_accumulator(t, *tip_hash);
            if (!a) throw app_error("node error: archive error: no accumulator for block " + hexs(*tip_hash));
            acc = std::move(*a);
        }
        tpl.header.roots = State::roots_after(std::move(acc), tpl.body, *mr);
        tpl.fees = fees;
        tpl.bribe = fee ? *fee : (fees ? fees : EMPTY_BLOCK_BMM_BRIBE);
    } else {
        auto mr = compute_merkle_root({}, {});
        tpl.header.merkle_root = *mr;
        std::lock_guard<std::recursive_mutex> l(node.lock());
        Txn t(node.store());
        bt::utreexo::MemForest acc;
        if (prev_side) {
            auto a = node.state().block_accumulator(t, *prev_side);
            if (!a) throw app_error("node error: archive error: no accumulator for block " + hexs(*prev_side));
            acc = std::move(*a);
        }
        tpl.header.roots = State::roots_after(std::move(acc), tpl.body, *mr);
        tpl.bribe = EMPTY_BLOCK_BMM_BRIBE;
    }
    return tpl;
}

// app.rs mine + miner.rs attempt_bmm/confirm_bmm
void App::mine(std::optional<uint64_t> fee)
{
    auto tpl = build_block_template(fee);
    Hash32 h = tpl.header.hash();
    auto r = node.enforcer().create_bmm_critical_data_tx(tpl.bribe, 0, h, tpl.header.prev_main_hash);
    if (!bt::enforcer::ok(r)) throw app_error("miner error: CUSF mainchain proto error: " + bt::enforcer::err(r).what());
    LOG_INFO("mine: BMM transaction sent %s, waiting for confirmation", bt::hex_encode_rev(bt::enforcer::val(r).span()).c_str());
    std::optional<Hash32> main_hash;
    std::atomic<bool> cancel{false};
    node.enforcer().subscribe_events(
        [&](const bt::enforcer::SubscribeEvent& ev) {
            if (ev.kind == bt::enforcer::SubscribeEvent::Connect && ev.info.bmm_commitment == h) main_hash = ev.header.block_hash;
            return false; // only the first event of the subscription is considered
        },
        &cancel);
    if (main_hash && !node.submit_block(*main_hash, tpl.header, tpl.body)) LOG_WARN("mine: BMM not accepted as new tip");
    wallet.update();
}

bool App::connect_block(const Header& h, const Body& b, const Hash32& main_block_hash)
{
    bool ok = node.submit_block(main_block_hash, h, b);
    if (ok) wallet.update();
    return ok;
}

Hash32 App::deposit(const Address& a, uint64_t value, uint64_t fee)
{
    auto r = node.enforcer().create_deposit_tx(a.to_base58(), value, fee);
    if (!bt::enforcer::ok(r)) throw app_error("CUSF mainchain proto error: " + bt::enforcer::err(r).what());
    return bt::enforcer::val(r);
}

bool App::is_locked(const OutPoint& op)
{
    std::lock_guard<std::recursive_mutex> l(node.lock());
    Txn t(node.store());
    return node.state().locked_to(t, op).has_value();
}

Hash32 App::sign_and_send(const Transaction& tx)
{
    AuthorizedTransaction at;
    try {
        at = wallet.authorize(tx);
    } catch (const WalletError& e) {
        throw app_error(std::string("wallet error: ") + e.what());
    }
    auto submit = [&] { node.broadcast_transaction(at); };
    try {
        submit();
    } catch (const StateError& e) {
        std::string m = e.what();
        if (m.rfind("UtxoDoubleSpent", 0) == 0) throw app_error("node error: mempool error: can't add transaction, utxo double spent");
        // node/mod.rs submit_transaction: an orphaned/corrupted lock is cleaned up, then one retry
        if (m.find("orphaned lock") != std::string::npos || m.find("corrupted swap") != std::string::npos) {
            uint32_t cleaned;
            {
                std::lock_guard<std::recursive_mutex> l(node.lock());
                Txn t(node.store());
                cleaned = node.state().cleanup_orphaned_locks(t);
                t.commit();
            }
            if (cleaned == 0) node_state_error(e);
            try {
                submit();
            } catch (const StateError& e2) {
                node_state_error(e2);
            }
        } else {
            node_state_error(e);
        }
    }
    wallet.update();
    return at.transaction.txid();
}

// app.rs swap_confirmation_check_task
void App::confirmation_check_once()
{
    std::vector<Swap> todo;
    {
        std::lock_guard<std::recursive_mutex> l(node.lock());
        Txn t(node.store());
        for (auto& s : node.state().swaps(t))
            if (s.state.kind == SwapState::WaitingConfirmations && !s.l1_txid.is_unset()) todo.push_back(s);
    }
    if (todo.empty()) return;
    auto path = l1::config_path();
    for (auto& s : todo) {
        auto cfg = l1::load_config(path, s.parent_chain);
        if (!cfg) continue;
        std::string err;
        auto tx = l1::Client{*cfg}.get_transaction(s.l1_txid.to_hex(), err); // canonical-order hex (sic, theirs)
        if (!tx) continue;
        uint32_t conf = uint32_t((*tx)["confirmations"].getInt<int64_t>());
        uint32_t cur = s.state.current;
        if (conf <= cur) continue;
        std::lock_guard<std::recursive_mutex> l(node.lock());
        Txn t(node.store());
        auto tp = node.state().tip(t);
        auto ht = node.state().height(t);
        if (!tp || !ht) continue;
        try {
            node.state().update_swap_l1_txid(t, s.id, s.l1_txid, conf, std::nullopt, std::nullopt, *tp, *ht);
            t.commit();
        } catch (const StateError& e) {
            LOG_ERROR("Failed to update swap confirmations: %s", e.what());
        }
    }
}

void App::start_tasks()
{
    tasks_running_ = true;
    confirm_thread_ = std::thread([this] {
        while (tasks_running_) {
            for (int i = 0; i < 100 && tasks_running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!tasks_running_) break;
            try {
                confirmation_check_once();
            } catch (const std::exception& e) {
                LOG_DEBUG("swap confirmation check: %s", e.what());
            }
        }
    });
}

void App::stop_tasks()
{
    tasks_running_ = false;
    if (confirm_thread_.joinable()) confirm_thread_.join();
}

// ------------------------------------------------------------------ RPC (rpc_server.rs)
namespace {

UniValue arr() { return UniValue(UniValue::VARR); }

UniValue openapi_schema(const bt::jsonrpc::Server& s)
{
    // utoipa's full schema is not reproduced; the methods are listed for introspection.
    UniValue r(UniValue::VOBJ);
    r.pushKV("openapi", "3.1.0");
    UniValue info(UniValue::VOBJ);
    info.pushKV("title", "coinshift_app_rpc_api");
    info.pushKV("version", VERSION);
    r.pushKV("info", info);
    UniValue paths(UniValue::VOBJ);
    for (auto& m : s.methods()) paths.pushKV("/" + m, UniValue(UniValue::VOBJ));
    r.pushKV("paths", paths);
    return r;
}

template <class F>
auto guard(F f)
{
    return [f](const Params& p) -> UniValue {
        try {
            return f(p);
        } catch (const WalletError& e) {
            throw app_error(e.what());
        } catch (const StateError& e) {
            throw app_error(e.what());
        } catch (const bt::sidechain::ArchiveError& e) {
            throw app_error(std::string("node error: archive error: ") + e.what());
        }
    };
}

} // namespace

void register_rpc(bt::jsonrpc::Server& s, App& app)
{
    Node& n = app.node;
    Wallet& w = app.wallet;
    auto locked = [&app](const OutPoint& op) { return app.is_locked(op); };

    s.add("balance", {}, 0, guard([&w](const Params&) {
              auto b = w.balance();
              return json::balance(b.total, b.available);
          }));
    s.add("connect_block", {"block", "main_block_hash"}, 2, guard([&app](const Params& p) {
              auto [h, b] = json::parse_block(p[0]);
              Hash32 mh = json::parse_btc_hash(p[1]);
              return UniValue(app.connect_block(h, b, mh));
          }));
    s.add("connect_peer", {"addr"}, 1, guard([&n](const Params& p) {
              std::string a = json::parse_socket_addr(p[0]), err;
              if (!n.connect_peer(a, err)) throw app_error("net error: " + err);
              return UniValue();
          }));
    s.add("create_deposit", {"address", "value_sats", "fee_sats"}, 3, guard([&app](const Params& p) {
              Address a = json::parse_address(p[0]);
              uint64_t v = json::parse_u64(p[1]), f = json::parse_u64(p[2]);
              return json::btc_hash(app.deposit(a, v, f));
          }));
    s.add("format_deposit_address", {"address"}, 1, guard([](const Params& p) {
              return UniValue(json::parse_address(p[0]).format_for_deposit(THIS_SIDECHAIN));
          }));
    s.add("forget_peer", {"addr"}, 1, guard([&n](const Params& p) {
              n.forget_peer(json::parse_socket_addr(p[0]));
              return UniValue();
          }));
    s.add("generate_mnemonic", {}, 0, [](const Params&) { return UniValue(bt::hd::generate_mnemonic(12)); });
    s.add("get_block", {"block_hash"}, 1, guard([&n](const Params& p) -> UniValue {
              Hash32 h = json::parse_hash(p[0]);
              auto hd = n.header(h);
              if (!hd) return UniValue();
              auto b = n.body(h);
              if (!b) throw app_error("node error: archive error: no block body with hash " + hexs(h));
              return json::block(*hd, *b);
          }));
    s.add("get_block_hash", {"height"}, 1, guard([&n](const Params& p) -> UniValue {
              auto h = n.block_hash_at(json::parse_u32(p[0]));
              return h ? json::hash(*h) : UniValue();
          }));
    s.add("get_block_index", {"block_hash"}, 1, guard([&n](const Params& p) {
              Hash32 h = json::parse_hash(p[0]);
              auto b = n.body(h);
              if (!b) throw app_error("node error: archive error: no block body with hash " + hexs(h));
              std::optional<BlockIndexEvents> ev;
              try {
                  ev = n.block_index_events(h);
              } catch (const bt::sidechain::ArchiveError&) {
                  throw app_error("node error: archive error: no height info for block hash " + hexs(h));
              }
              if (!ev) throw app_error("block " + hexs(h) + " is not in the current chain");
              return json::block_index(*b, *ev);
          }));
    s.add("get_block_template", {}, 0, guard([&app](const Params&) {
              auto t = app.build_block_template(std::nullopt);
              UniValue o(UniValue::VOBJ);
              o.pushKV("critical_hash", json::hash(t.header.hash()));
              o.pushKV("block", json::block(t.header, t.body));
              o.pushKV("fees_sats", UniValue(t.fees));
              return o;
          }));
    s.add("get_bmm_inclusions", {"block_hash"}, 1, guard([&n](const Params& p) {
              UniValue a = arr();
              for (auto& m : n.bmm_inclusions(json::parse_hash(p[0]))) a.push_back(json::btc_hash(m));
              return a;
          }));
    s.add("get_best_mainchain_block_hash", {}, 0, guard([&n](const Params&) -> UniValue {
              auto tp = n.tip();
              if (!tp) return UniValue();
              auto m = n.best_main_verification(*tp);
              if (!m) throw app_error("node error: archive error: no BMM result with block " + hexs(*tp));
              return json::btc_hash(*m);
          }));
    s.add("get_best_sidechain_block_hash", {}, 0, guard([&n](const Params&) -> UniValue {
              auto tp = n.tip();
              return tp ? json::hash(*tp) : UniValue();
          }));
    s.add("get_new_address", {}, 0, guard([&w](const Params&) { return json::address(w.get_new_address()); }));
    s.add("get_wallet_addresses", {}, 0, guard([&w](const Params&) {
              auto as = w.addresses();
              std::vector<Address> v(as.begin(), as.end());
              std::sort(v.begin(), v.end(), [](const Address& a, const Address& b) { return a.to_base58() < b.to_base58(); });
              UniValue a = arr();
              for (auto& x : v) a.push_back(json::address(x));
              return a;
          }));
    s.add("get_wallet_utxos", {}, 0, guard([&w](const Params&) {
              UniValue a = arr();
              for (auto& [op, o] : w.utxos()) a.push_back(json::pointed_output(op, o));
              return a;
          }));
    s.add("getblockcount", {}, 0, guard([&n](const Params&) {
              auto h = n.height();
              return UniValue(uint64_t(h ? *h + 1 : 0));
          }));
    s.add("invalidate_block", {"block_hash"}, 1, guard([&n](const Params& p) {
              n.invalidate_block(json::parse_hash(p[0]));
              return UniValue();
          }));
    s.add("latest_failed_withdrawal_bundle_height", {}, 0, guard([&n](const Params&) -> UniValue {
              auto h = n.latest_failed_withdrawal_bundle_height();
              return h ? UniValue(uint64_t(*h)) : UniValue();
          }));
    s.add("list_mempool", {}, 0, guard([&n](const Params&) {
              UniValue a = arr();
              for (auto& tx : n.mempool()) a.push_back(json::mempool_tx(tx.transaction));
              return a;
          }));
    s.add("list_peers", {}, 0, [&n](const Params&) {
        UniValue a = arr();
        for (auto& pv : n.peers()) {
            UniValue o(UniValue::VOBJ);
            o.pushKV("address", pv.address.to_string());
            o.pushKV("status", pv.connected ? "Connected" : "Connecting");
            o.pushKV("name", pv.name);
            a.push_back(o);
        }
        return a;
    });
    s.add("list_utxos", {}, 0, guard([&n](const Params&) {
              UniValue a = arr();
              for (auto& [op, o] : n.utxos()) a.push_back(json::pointed_output(op, o));
              return a;
          }));
    s.add("mainchain_sync_progress", {}, 0, [&n](const Params&) {
        auto sp = n.sync_progress();
        static const char* PH[] = {"idle", "headers", "writing", "state"};
        return json::sync_progress(PH[sp.phase], sp.done, sp.total, sp.tip_height);
    });
    s.add("mine", {"fee"}, 0, guard([&app](const Params& p) {
              app.mine(json::parse_opt_u64(p[0]));
              return UniValue();
          }));
    s.add("openapi_schema", {}, 0, [&s](const Params&) { return openapi_schema(s); });
    s.add("pending_withdrawal_bundle", {}, 0, guard([&n](const Params&) {
              auto b = n.pending_withdrawal_bundle();
              return b ? json::withdrawal_bundle(*b) : UniValue();
          }));
    s.add("remove_from_mempool", {"txid"}, 1, guard([&n](const Params& p) {
              n.remove_from_mempool(json::parse_hash(p[0]));
              return UniValue();
          }));
    s.add("set_seed_from_mnemonic", {"mnemonic"}, 1, guard([&w](const Params& p) {
              std::string m = json::parse_string(p[0]);
              auto e = bt::hd::mnemonic_error(m);
              if (!e.empty()) throw app_error(e);
              w.set_seed(bt::hd::mnemonic_to_seed(*bt::hd::parse_mnemonic(m)));
              return UniValue();
          }));
    s.add("sidechain_wealth", {}, 0, guard([&n](const Params&) { return UniValue(n.sidechain_wealth()); }));
    s.add("stop", {}, 0, [&app](const Params&) {
        if (app.on_stop) app.on_stop();
        return UniValue();
    });
    s.add("transfer", {"dest", "value_sats", "fee_sats"}, 3, guard([&app, &w, locked](const Params& p) {
              Address d = json::parse_address(p[0]);
              uint64_t v = json::parse_u64(p[1]), f = json::parse_u64(p[2]);
              return json::hash(app.sign_and_send(w.create_transaction_many({{d, v}}, f, locked)));
          }));
    s.add("transfer_many", {"dests", "fee_sats"}, 2, guard([&app, &w, locked](const Params& p) {
              auto d = json::parse_transfer_dests(p[0]);
              uint64_t f = json::parse_u64(p[1]);
              return json::hash(app.sign_and_send(w.create_transaction_many(d, f, locked)));
          }));
    s.add("withdraw", {"mainchain_address", "amount_sats", "fee_sats", "mainchain_fee_sats"}, 4, guard([&app, &w, locked](const Params& p) {
              auto a = json::parse_btc_address(p[0]);
              uint64_t v = json::parse_u64(p[1]), f = json::parse_u64(p[2]), mf = json::parse_u64(p[3]);
              return json::hash(app.sign_and_send(w.create_withdrawal(a, v, mf, f, locked)));
          }));
    // ---- swaps
    s.add("create_swap",
          {"parent_chain", "l1_recipient_address", "l1_amount_sats", "l2_recipient", "l2_amount_sats", "required_confirmations", "fee_sats"}, 3,
          guard([&app, &w, locked](const Params& p) {
              ParentChain pc = json::parse_parent_chain(p[0]);
              std::string l1 = json::parse_string(p[1]);
              uint64_t l1a = json::parse_u64(p[2]);
              auto rec = json::parse_opt_address(p[3]);
              uint64_t l2a = json::parse_u64(p[4]);
              auto rc = json::parse_opt_u32(p[5]);
              uint64_t f = json::parse_u64(p[6]);
              auto [tx, id] = w.create_swap_create_tx(pc, l1, l1a, rec, l2a, rc, f, locked);
              Hash32 txid = app.sign_and_send(tx);
              UniValue r = arr();
              r.push_back(json::swap_id(id));
              r.push_back(json::hash(txid));
              return r;
          }));
    s.add("reconstruct_swaps", {}, 0, guard([&n](const Params&) {
              // state.reconstruct_swaps_from_blockchain: replays swap txs of the active chain over the
              // current tables (their RPC; node-local).
              std::lock_guard<std::recursive_mutex> l(n.lock());
              Txn t(n.store());
              auto tp = n.state().tip(t);
              if (!tp) return UniValue(uint64_t(0));
              std::vector<Hash32> chain;
              for (std::optional<Hash32> c = tp; c;) {
                  chain.push_back(*c);
                  c = n.archive().need_header(t, *c).prev_side_hash;
              }
              uint64_t count = 0;
              uint32_t height = 0;
              for (auto it = chain.rbegin(); it != chain.rend(); ++it, ++height) {
                  auto b = n.archive().body(t, *it);
                  if (!b) throw app_error("Invalid transaction: Failed to get body: no block body with hash " + hexs(*it));
                  for (auto& tx : b->transactions) {
                      Hash32 txid = tx.txid();
                      auto spent = n.state().fill_spent(t, tx);
                      auto& d = tx.data;
                      if (d.kind == TxData::SwapCreate) {
                          Swap sw;
                          sw.id = d.swap_id;
                          sw.parent_chain = d.parent_chain;
                          sw.l1_txid = SwapTxId::from_bytes(d.l1_txid_bytes);
                          sw.required_confirmations = d.required_confirmations;
                          sw.l2_recipient = d.l2_recipient;
                          sw.l2_amount = d.l2_amount;
                          sw.l1_recipient_address = d.l1_recipient_address;
                          sw.l1_amount = d.l1_amount;
                          sw.created_at_height = height;
                          if (!spent.empty()) sw.l2_creator_address = spent.front().address;
                          for (uint32_t v = 0; v < tx.outputs.size(); v++)
                              if (tx.outputs[v].content.kind == Content::SwapPending && tx.outputs[v].content.swap_id == d.swap_id) {
                                  OutPoint op{OutPoint::Regular, txid, v};
                                  if (!n.state().locked_to(t, op)) n.state().lock_output(t, op, d.swap_id);
                              }
                          n.state().save_swap(t, sw);
                          count++;
                      } else if (d.kind == TxData::SwapClaim) {
                          if (auto sw = n.state().swap(t, d.swap_id)) {
                              for (auto& [op, h] : tx.inputs)
                                  if (n.state().locked_to(t, op) == std::optional<Hash32>(d.swap_id)) n.state().unlock_output(t, op);
                              sw->state = SwapState{SwapState::Completed};
                              n.state().save_swap(t, *sw);
                          }
                      } else if (d.kind == TxData::SwapAccept) {
                          if (n.state().swap(t, d.swap_id)) n.state().reserve_swap(t, d.swap_id, *d.l2_claimer_address, height);
                      }
                  }
              }
              t.commit();
              return UniValue(count);
          }));
    s.add("update_swap_l1_txid", {"swap_id", "l1_txid_hex", "confirmations", "l2_claimer_address"}, 3, guard([&n](const Params& p) {
              Hash32 id = json::parse_swap_id(p[0]);
              std::string hx = json::parse_string(p[1]);
              uint32_t conf = json::parse_u32(p[2]);
              auto l2 = json::parse_opt_address(p[3]);
              // SwapTxId::from_hex: trimmed, exactly 64 hex chars, canonical order
              std::string tr = hx;
              tr.erase(0, tr.find_first_not_of(" \t\r\n"));
              tr.erase(tr.find_last_not_of(" \t\r\n") + 1);
              if (tr.size() != 64) throw app_error("L1 txid must be exactly 64 hex characters (32 bytes), got " + std::to_string(tr.size()));
              auto raw = bt::hex_decode(tr);
              if (!raw) throw app_error("Invalid hex: Invalid character");
              std::lock_guard<std::recursive_mutex> l(n.lock());
              Txn t(n.store());
              auto tp = n.state().tip(t);
              if (!tp) throw app_error("No tip found");
              auto ht = n.state().height(t);
              if (!ht) throw app_error("No tip height found");
              n.state().update_swap_l1_txid(t, id, SwapTxId::from_bytes(*raw), conf, std::nullopt, l2, *tp, *ht);
              t.commit();
              return UniValue();
          }));
    s.add("get_swap_status", {"swap_id"}, 1, guard([&n](const Params& p) -> UniValue {
              Hash32 id = json::parse_swap_id(p[0]);
              std::lock_guard<std::recursive_mutex> l(n.lock());
              Txn t(n.store());
              auto sw = n.state().swap(t, id);
              return sw ? json::swap(*sw) : UniValue();
          }));
    s.add("accept_swap", {"swap_id", "l2_claimer_address", "fee_sats"}, 1, guard([&app, &w, locked](const Params& p) {
              Hash32 id = json::parse_swap_id(p[0]);
              auto c = json::parse_opt_address(p[1]);
              auto f = json::parse_opt_u64(p[2]);
              return json::hash(app.sign_and_send(w.create_swap_accept_tx(id, c, f ? *f : 0, locked)));
          }));
    s.add("claim_swap", {"swap_id", "l2_claimer_address"}, 1, guard([&app, &n, &w](const Params& p) {
              Hash32 id = json::parse_swap_id(p[0]);
              auto requested = json::parse_opt_address(p[1]);
              std::optional<Swap> sw;
              std::vector<std::pair<OutPoint, Output>> locked_outputs;
              Address recipient;
              {
                  std::lock_guard<std::recursive_mutex> l(n.lock());
                  Txn t(n.store());
                  sw = n.state().swap(t, id);
                  if (!sw) throw app_error("Swap not found");
                  if (sw->state.kind != SwapState::ReadyToClaim) throw app_error("Swap is not ready to claim (state: " + debug_string(sw->state) + ")");
                  for (auto& [op, o] : n.state().utxos(t))
                      if (o.content.kind == Content::SwapPending && o.content.swap_id == id) locked_outputs.emplace_back(op, o);
                  if (locked_outputs.empty()) throw app_error("No locked outputs found for swap " + hexs(id));
                  auto ht = n.state().height(t);
                  auto rec = n.state().entitled_claimer_at(t, *sw, ht ? *ht + 1 : 0);
                  if (!rec)
                      throw app_error("Open swap has no live reservation; call accept_swap to reserve it (before paying on L1) and claim within the "
                                      "acceptance window");
                  recipient = *rec;
              }
              if (requested && *requested != recipient)
                  throw app_error("Swap is reserved for " + recipient.to_base58() + ", not " + requested->to_base58());
              // Kept fix (wallet only): only this swap's escrow outputs that are locked to it are claimed
              // (theirs also sweeps unlocked SwapPending outputs carrying the id; our policy refuses those).
              std::map<OutPoint, Output> put;
              std::vector<std::pair<OutPoint, Output>> mine;
              for (auto& [op, o] : locked_outputs)
                  if (app.is_locked(op)) {
                      put[op] = o;
                      mine.emplace_back(op, o);
                  }
              if (mine.empty()) throw app_error("No locked outputs found for swap " + hexs(id));
              w.put_utxos(put);
              std::optional<Address> claimer;
              if (!sw->l2_recipient) claimer = recipient;
              return json::hash(app.sign_and_send(w.create_swap_claim_tx(id, recipient, mine, claimer)));
          }));
    s.add("list_swaps", {}, 0, guard([&n](const Params&) {
              std::lock_guard<std::recursive_mutex> l(n.lock());
              Txn t(n.store());
              UniValue a = arr();
              for (auto& sw : n.state().swaps(t)) a.push_back(json::swap(sw));
              return a;
          }));
    s.add("list_swaps_by_recipient", {"recipient"}, 1, guard([&n](const Params& p) {
              Address r = json::parse_address(p[0]);
              std::lock_guard<std::recursive_mutex> l(n.lock());
              Txn t(n.store());
              UniValue a = arr();
              for (auto& sw : n.state().swaps_by_recipient(t, r)) a.push_back(json::swap(sw));
              return a;
          }));
    s.add("cancel_swap", {"swap_id"}, 1, guard([&n, &w](const Params& p) {
              Hash32 id = json::parse_swap_id(p[0]);
              std::lock_guard<std::recursive_mutex> l(n.lock());
              Txn t(n.store());
              auto sw = n.state().swap(t, id);
              if (!sw) throw app_error("Swap not found: " + hexs(id));
              if (!sw->l2_creator_address) throw app_error("Only the swap creator can cancel or delete this swap");
              auto mine = w.addresses();
              if (!mine.count(*sw->l2_creator_address)) throw app_error("Only the swap creator can cancel or delete this swap");
              n.state().cancel_swap(t, id, sw->l2_creator_address);
              t.commit();
              return UniValue();
          }));
}

} // namespace coinshift
