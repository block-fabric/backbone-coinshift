// Swap-claim / swap-accept: upstream consensus reproduced, our mempool policy stricter.
#include "test.h"

#include <coinshift/state.h>

using namespace coinshift;

namespace {
bt::frost::SigningKey key(uint8_t b)
{
    bt::frost::SigningKey k;
    k.s[0] = b; // small canonical scalar
    return k;
}
Address addr(const bt::frost::SigningKey& k) { return bt::sidechain::address_from_vk(k.verifying_key()); }
OutPoint op(uint8_t b) { return OutPoint{OutPoint::Regular, Hash32{}, b}; }
Output pending(const Address& a, uint64_t v, const Hash32& id)
{
    Output o;
    o.address = a;
    o.content.kind = Content::SwapPending;
    o.content.value = v;
    o.content.swap_id = id;
    return o;
}
} // namespace

TEST(policy_claim_spending_foreign_escrow)
{
    bt::frost::init();
    bt::kv::MemStore store;
    State st;
    auto attacker = key(7), victim = key(9);
    Hash32 own{}, other{};
    own[0] = 1;
    other[0] = 2;
    Swap s;
    s.id = own;
    s.parent_chain = ParentChain::Regtest;
    s.l1_txid = SwapTxId::zero();
    s.state = SwapState{SwapState::ReadyToClaim};
    s.l2_recipient = addr(attacker);
    s.l2_amount = 100;
    s.l1_recipient_address = "x";
    {
        bt::kv::Txn t(store);
        Writer w;
        w.u32(5);
        t.put(State::T_HEIGHT, {}, w.buf);
        st.save_swap(t, s);
        Output a = pending(addr(attacker), 100, own), b = pending(addr(victim), 5000, other);
        Writer wa, wb;
        bincode(wa, a);
        bincode(wb, b);
        t.put(State::T_UTXOS, ByteSpan(op(1).key().data(), 37), wa.buf);
        t.put(State::T_UTXOS, ByteSpan(op(2).key().data(), 37), wb.buf); // escrow not locked (expired)
        st.lock_output(t, op(1), own);
        t.commit();
    }
    AuthorizedTransaction atx;
    auto& tx = atx.transaction;
    tx.inputs = {{op(1), pointed_output_hash(op(1), pending(addr(attacker), 100, own))},
                 {op(2), pointed_output_hash(op(2), pending(addr(victim), 5000, other))}};
    Output pay;
    pay.address = addr(attacker);
    pay.content.value = 5100;
    tx.outputs = {pay};
    tx.data.kind = TxData::SwapClaim;
    tx.data.swap_id = own;
    auto msg = tx.canonical_bytes();
    for (int i = 0; i < 2; i++) atx.authorizations.push_back({attacker.verifying_key(), attacker.sign(msg)});
    bt::kv::Txn t(store);
    // upstream validate_transaction (state/mod.rs:519-531) accepts it: reproduced
    bool ok = true;
    try {
        st.validate_transaction(t, atx);
    } catch (const StateError&) {
        ok = false;
    }
    CHECK(ok);
    // our policy refuses it
    bool refused = false;
    try {
        st.policy_check(t, atx);
    } catch (const StateError&) {
        refused = true;
    }
    CHECK(refused);
}
