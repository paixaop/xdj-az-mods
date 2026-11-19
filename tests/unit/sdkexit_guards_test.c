/* SPDX-License-Identifier: GPL-3.0-only */
/* Production adoption and result custody; no firmware address is called. */
#include "../../src/azstem/sdk_exit.c"
#include <assert.h>
#include <stdio.h>

/* These admission-only cases must never consult a native loaded object. */
int azsdk_load_read(int deck, struct azsdk_load_sample *sample) {
    (void) deck;
    (void) sample;
    abort();
}

static void unrecognized_native_work(void) {
    uintptr_t expected_table[3] = {0}, foreign_table[3] = {0};
    uintptr_t chain[8] = {(uintptr_t) foreign_table}, node[8] = {(uintptr_t) foreign_table};
    state().profile.chain_table = (uintptr_t) expected_table;
    Slot slot = {0};
    adopt_manager_chain(&slot, 1, 0);
    assert(!slot.untracked);
    adopt_manager_chain(&slot, 1, (uintptr_t) chain);
    assert(slot.untracked); /* a foreign chain cannot prove all pending native work */
    slot.untracked = false;
    chain[0] = (uintptr_t) expected_table;
    chain[3] = (uintptr_t) node;
    adopt_manager_chain(&slot, 1, (uintptr_t) chain);
    assert(slot.untracked); /* a known chain with a foreign command is equally uncertain */
}
static void result_custody(void) {
    Slot *slot = &state().decks[0];
    slot->result.token = 77;
    atomic_store(&slot->phase, READY);
    assert(release(4, 77) == AZ_INVALID && release(0, 0) == AZ_INVALID);
    atomic_store(&slot->frame_token, 77);
    assert(release(0, 77) == AZ_BUSY && atomic_load(&slot->phase) == READY);
    atomic_store(&slot->frame_token, 0);
    assert(release(0, 76) == AZ_STALE && atomic_load(&slot->phase) == READY && slot->result.token == 77);
    assert(release(0, 77) == AZ_OK && atomic_load(&slot->phase) == IDLE);
    assert(release(0, 77) == AZ_BUSY && atomic_load(&slot->phase) == IDLE);
}
int main(void) {
    unrecognized_native_work();
    result_custody();
    puts("PASS native exit guards: unknown work remains untracked; frame and token custody protect terminal results");
    return 0;
}
