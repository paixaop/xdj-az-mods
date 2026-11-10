/* SPDX-License-Identifier: GPL-3.0-only */
#define AZSDK_CUE_LIFECYCLE_TESTING 1
#include "../../src/azstem/sdk_cue_lifecycle.cpp"
#include <cassert>
#include <cstdio>
namespace {
unsigned registered = 0, retired = 0, destroyed = 0, retirement_stage = 0;
void *live[4] {};
uint64_t last_generation = 0;
int register_result = AZ_OK;
void stock_ctor(void *self, const uint64_t *a, const uint64_t *b, const uint64_t *, void *) {
    auto p = static_cast<unsigned char *>(self);
    uintptr_t v = state().profile.actor.manager_vptr;
    std::memcpy(p, &v, 8);
    std::memcpy(p + 0x110, a, 8);
    std::memcpy(p + 0x120, b, 8);
}
void stock_dtor(void *self) {
    for (void *p : live) assert(p != self);
    assert(retirement_stage == 0 || retirement_stage == 3);
    retirement_stage = 0;
    ++destroyed;
}
int installs = 0, fail_at = 0;
void *installer(uintptr_t, void *, void **original) {
    ++installs;
    if (installs == fail_at) return nullptr;
    *original = installs == 1 ? reinterpret_cast<void *>(stock_ctor) : reinterpret_cast<void *>(stock_dtor);
    return *original;
}
void reset() {
    auto &s = state();
    s.enabled = false;
    s.attempted = false;
    s.incarnation = 0;
    s.ctor = reinterpret_cast<void *>(stock_ctor);
    s.dtor = reinterpret_cast<void *>(stock_dtor);
    s.profile = {};
    s.profile.actor.manager_vptr = 0x12345678;
    for (unsigned i = 0; i < 4; ++i) {
        s.managers[i] = nullptr;
        live[i] = nullptr;
    }
    registered = retired = destroyed = 0;
    last_generation = 0;
    register_result = AZ_OK;
    installs = fail_at = 0;
    lifecycle_test_profile = nullptr;
}
void construct(void *self, unsigned deck, uint64_t player_high = UINT64_C(0xb9d7725fd31fc100), uint64_t renew_high = UINT64_C(0x3dc18c4946733700)) {
    uint64_t settings = UINT64_C(0x009d3addf06bd1d7), player = player_high | deck, renew = renew_high | deck;
    ctor_hook(self, &settings, &player, &renew, nullptr);
}
} // namespace
extern "C" int azsdk_cue_actor_register(unsigned deck, void *manager, uint64_t generation) noexcept {
    ++registered;
    assert(generation > last_generation);
    last_generation = generation;
    if (register_result == AZ_OK) live[deck] = manager;
    return register_result;
}
extern "C" void azsdk_cue_actor_retire(unsigned deck, void *manager) noexcept {
    assert(live[deck] == manager);
    live[deck] = nullptr;
    ++retired;
    retirement_stage = 1;
}
extern "C" void azsdk_cue_persist_retire(void *manager) noexcept {
    assert(retirement_stage == 1);
    retirement_stage = 2;
    for (void *p : live) assert(p != manager); // actor admission is already retired
}
extern "C" void azsdk_cue_recall_retire(void *manager) noexcept {
    assert(retirement_stage == 2);
    retirement_stage = 3;
    for (void *p : live) assert(p != manager);
}
int main() {
    azcue_profile p {}, q {};
    assert(!azsdk_cue_profile_get(0, 0, &p));
    assert(!azsdk_cue_profile_get(1, 0, nullptr));
    assert(azsdk_cue_profile_get(1, 0, &p));
    assert(azsdk_cue_profile_get(1, 0x10000, &q));
    assert(q.actor.manager_vptr == p.actor.manager_vptr + 0x10000);
    assert(q.store[10].address == p.store[10].address + 0x10000);
    assert(p.store[11].address == 0x8121c8);
    azcue_profile newer {};
    assert(azsdk_cue_profile_get(2, 0, &newer));
    assert(newer.store[11].address == 0x857b20);
    assert(q.store[11].address == p.store[11].address + 0x10000);
    assert(p.store[12].address == 0x1492ce0);
    assert(newer.store[12].address == 0x154e220);
    assert(q.store[12].address == p.store[12].address + 0x10000);
    assert(sizeof p.store / sizeof p.store[0] == 15);
    assert(q.persist.request == p.persist.request + 0x10000);
    assert(p.persist.memory_helper == 0x1735238);
    assert(newer.persist.memory_helper == 0x1890808);
    assert(q.persist.memory_helper == p.persist.memory_helper + 0x10000);
    assert(p.persist.content_copy == p.store[1].address);
    assert(p.persist.content_release == p.store[2].address);
    assert(p.persist.vector_append == 0x172e5d8 && newer.persist.vector_append == 0x1889c30);
    assert(p.persist.vector_destroy == 0x172e620 && newer.persist.vector_destroy == 0x1889bd0);
    assert(p.persist.content_from_vector == 0x1734b38 && newer.persist.content_from_vector == 0x1890108);
    assert(p.persist.content_publish == 0x1730710 && newer.persist.content_publish == 0x188bce0);
    assert(p.persist_hooks[4].address == p.persist.memory_helper);
    assert(q.persist_hooks[4].address == q.persist.memory_helper);
    assert(p.persist_metadata[6].address == p.persist.content_publish);
    assert(q.persist_metadata[6].address == q.persist.content_publish);
    assert(p.persist.hot_helper == 0x1734f00 && newer.persist.hot_helper == 0x18904d0);
    assert(q.persist.hot_helper == p.persist.hot_helper + 0x10000);
    assert(p.persist_hooks[5].address == p.persist.hot_helper);
    assert(q.persist_hooks[5].address == q.persist.hot_helper);
    assert(p.persist_metadata[7].address == p.persist.hot_helper);
    assert(q.persist_metadata[7].address == q.persist.hot_helper);
    assert(p.persist.hot_content_from_vector == 0x1734798 && newer.persist.hot_content_from_vector == 0x188fd68);
    assert(p.persist.hot_content_publish == 0x17303d8 && newer.persist.hot_content_publish == 0x188b9a8);
    assert(q.persist.hot_content_from_vector == p.persist.hot_content_from_vector + 0x10000);
    assert(q.persist.hot_content_publish == p.persist.hot_content_publish + 0x10000);
    assert(p.persist_metadata[8].address == p.persist.hot_content_from_vector);
    assert(p.persist_metadata[9].address == p.persist.hot_content_publish);
    assert(p.persist.cue_copy == 0x80aee0 && newer.persist.cue_copy == 0x8508c8);
    assert(p.persist.cue_destroy == 0x80b0f0 && newer.persist.cue_destroy == 0x850ad8);
    assert(q.persist.cue_copy == p.persist.cue_copy + 0x10000);
    assert(q.persist.cue_destroy == p.persist.cue_destroy + 0x10000);
    assert(p.persist_metadata[10].address == p.persist.cue_copy);
    assert(p.persist_metadata[11].address == p.persist.cue_destroy);
    assert(sizeof p.persist_metadata / sizeof p.persist_metadata[0] == 12);
    assert(!azsdk_cue_profile_get(2, UINTPTR_MAX, &q));
    alignas(8) unsigned char objects[5][0x188] {};
    reset();
    construct(objects[0], 0);
    assert(!registered); // disabled transparent stock construction
    state().enabled = true;
    for (unsigned d = 0; d < 4; ++d) construct(objects[d], d);
    assert(registered == 4);
    assert(last_generation == 4);
    construct(objects[4], 0);
    assert(registered == 5); // replacement incarnation
    dtor_hook(objects[0]);
    assert(!retired && destroyed == 1); // stale dtor cannot retire replacement
    dtor_hook(objects[4]);
    assert(retired == 1 && destroyed == 2);
    for (unsigned d = 1; d < 4; ++d) dtor_hook(objects[d]);
    assert(retired == 4);
    construct(objects[0], 4);
    construct(objects[0], 0, 0x1200);
    construct(objects[0], 0, UINT64_C(0xb9d7725fd31fc100), 0x3400);
    assert(registered == 5);
    register_result = AZ_BUSY;
    construct(objects[0], 0);
    assert(registered == 6);
    assert(!state().managers[0]);
    reset();
    azcue_profile fake {};
    fake.ctor.address = 1;
    fake.complete_dtor.address = 2;
    lifecycle_test_profile = &fake;
    fail_at = 2;
    assert(!azsdk_cue_lifecycle_configure(1, installer));
    assert(!state().enabled);
    assert(!azsdk_cue_lifecycle_configure(1, installer));
    assert(installs == 2);
    reset();
    lifecycle_test_profile = &fake;
    assert(azsdk_cue_lifecycle_configure(2, installer));
    assert(state().enabled);
    std::puts("cue lifecycle tests passed");
}
