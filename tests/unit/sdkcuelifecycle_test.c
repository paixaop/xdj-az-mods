/* SPDX-License-Identifier: GPL-3.0-only */
#define AZSDK_CUE_LIFECYCLE_TESTING 1
#include "../../src/azstem/sdk_cue_lifecycle.c"
#include <assert.h>
#include <stdio.h>
unsigned registered = 0, retired = 0, destroyed = 0, retirement_stage = 0;
void *live[4] = {0};
uint64_t last_generation = 0;
int register_result = AZ_OK;
void stock_ctor(void *self, const uint64_t *a, const uint64_t *b, const uint64_t *unused, void *unused2) {
    (void) unused;
    (void) unused2;
    unsigned char *p = self;
    uintptr_t v = state()->profile.actor.manager_vptr;
    memcpy(p, &v, 8);
    memcpy(p + 0x110, a, 8);
    memcpy(p + 0x120, b, 8);
}
void stock_dtor(void *self) {
    for (unsigned i = 0; i < 4; ++i) assert(live[i] != self);
    assert(retirement_stage == 0 || retirement_stage == 3);
    retirement_stage = 0;
    ++destroyed;
}
int installs = 0, fail_at = 0;
void *installer(uintptr_t unused, void *unused2, void **original) {
    (void) unused;
    (void) unused2;
    ++installs;
    if (installs == fail_at) return NULL;
    *original = installs == 1 ? (void *) stock_ctor : (void *) stock_dtor;
    return *original;
}
void reset() {
    State *s = state();
    s->enabled = false;
    s->attempted = false;
    s->incarnation = 0;
    s->ctor = (void *) stock_ctor;
    s->dtor = (void *) stock_dtor;
    s->profile = (struct azcue_profile) {0};
    s->profile.actor.manager_vptr = 0x12345678;
    for (unsigned i = 0; i < 4; ++i) {
        s->managers[i] = NULL;
        live[i] = NULL;
    }
    registered = retired = destroyed = 0;
    last_generation = 0;
    register_result = AZ_OK;
    installs = fail_at = 0;
    lifecycle_test_profile = NULL;
}
void construct_with(void *self, unsigned deck, uint64_t player_high, uint64_t renew_high) {
    uint64_t settings = UINT64_C(0x009d3addf06bd1d7), player = player_high | deck, renew = renew_high | deck;
    ctor_hook(self, &settings, &player, &renew, NULL);
}
void construct(void *self, unsigned deck) {
    construct_with(self, deck, UINT64_C(0xb9d7725fd31fc100), UINT64_C(0x3dc18c4946733700));
}
int azsdk_cue_actor_register(unsigned deck, void *manager, uint64_t generation) {
    ++registered;
    assert(generation > last_generation);
    last_generation = generation;
    if (register_result == AZ_OK) live[deck] = manager;
    return register_result;
}
void azsdk_cue_actor_retire(unsigned deck, void *manager) {
    assert(live[deck] == manager);
    live[deck] = NULL;
    ++retired;
    retirement_stage = 1;
}
void azsdk_cue_persist_retire(void *manager) {
    assert(retirement_stage == 1);
    retirement_stage = 2;
    for (unsigned i = 0; i < 4; ++i) assert(live[i] != manager); // actor admission is already retired
}
void azsdk_cue_recall_retire(void *manager) {
    assert(retirement_stage == 2);
    retirement_stage = 3;
    for (unsigned i = 0; i < 4; ++i) assert(live[i] != manager);
}
static void test_native_profile_validation(void) {
#if defined(__linux__) && defined(__aarch64__)
    struct {
        _Alignas(8) unsigned char code[16];
        uintptr_t tables[6][8];
    } native = {0};
    ElfW(Phdr) segment = {.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = (uintptr_t) &native, .p_memsz = sizeof native};
    Main image = {.found = true, .headers = &segment, .count = 1};
    struct azcue_profile profile = {0};
    struct azcue_entry entry = {.address = (uintptr_t) native.code};
    profile.ctor = profile.complete_dtor = profile.deleting_dtor = entry;
    struct azcue_entry *groups[] = {profile.store, profile.persist_hooks, profile.persist_metadata, profile.actor_entries};
    const unsigned counts[] = {sizeof profile.store / sizeof *profile.store, sizeof profile.persist_hooks / sizeof *profile.persist_hooks,
                               sizeof profile.persist_metadata / sizeof *profile.persist_metadata, sizeof profile.actor_entries / sizeof *profile.actor_entries};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < counts[i]; ++j) groups[i][j] = entry;
    profile.actor.manager_vptr = (uintptr_t) native.tables[0];
    profile.actor.box_vptr = (uintptr_t) native.tables[1];
    profile.actor.owner_vptr = (uintptr_t) native.tables[2];
    profile.actor.task_vptr = (uintptr_t) native.tables[3];
    profile.persist.handler_table = (uintptr_t) native.tables[4];
    profile.persist.reception_table = (uintptr_t) native.tables[5];
    profile.actor.memory_producer = profile.actor.post = profile.actor.complete_dtor = profile.actor.execute = entry.address;
    native.tables[0][0] = native.tables[0][1] = native.tables[0][2] = entry.address;
    native.tables[1][6] = native.tables[3][0] = native.tables[3][2] = entry.address;
    assert(profile_verified(&image, &profile));
    native.code[3] = 1;
    assert(!profile_verified(&image, &profile)); // prologue mismatch blocks all hooking
    native.code[3] = 0;
    segment.p_flags = PF_R;
    assert(!profile_verified(&image, &profile)); // readable data cannot be called as code
    assert(profile_tables_readable(&image, &profile));
    segment.p_flags = PF_X;
    assert(!profile_tables_readable(&image, &profile));
    segment.p_flags = PF_R | PF_X;
    native.tables[3][2] = 0;
    assert(!profile_verified(&image, &profile)); // execute vtable must match the verified profile
    native.tables[3][2] = entry.address;
    profile.persist.reception_table = (uintptr_t) &native + sizeof native - 8;
    assert(!profile_verified(&image, &profile)); // full table must lie in a readable segment
    assert(!main_contains(&image, UINTPTR_MAX - 7, 16, false));
    Main found = {0};
    struct dl_phdr_info info = {.dlpi_name = "library.so", .dlpi_addr = 0x2000, .dlpi_phdr = &segment, .dlpi_phnum = 1};
    assert(!main_image(&info, sizeof info, &found) && !found.found);
    info.dlpi_name = "";
    assert(main_image(&info, sizeof info, &found) && found.found && found.bias == 0x2000 && found.headers == &segment && found.count == 1);
#endif
}
int main() {
    test_native_profile_validation();
    struct azcue_profile p = {0}, q = {0};
    assert(!azsdk_cue_profile_get(0, 0, &p));
    assert(!azsdk_cue_profile_get(1, 0, NULL));
    assert(azsdk_cue_profile_get(1, 0, &p));
    assert(azsdk_cue_profile_get(1, 0x10000, &q));
    assert(q.actor.manager_vptr == p.actor.manager_vptr + 0x10000);
    assert(q.store[10].address == p.store[10].address + 0x10000);
    assert(p.store[11].address == 0x8121c8);
    struct azcue_profile newer = {0};
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
    _Alignas(8) unsigned char objects[5][0x188] = {0};
    reset();
    construct(objects[0], 0);
    assert(!registered); // disabled transparent stock construction
    state()->enabled = true;
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
    construct_with(objects[0], 0, 0x1200, UINT64_C(0x3dc18c4946733700));
    construct_with(objects[0], 0, UINT64_C(0xb9d7725fd31fc100), 0x3400);
    assert(registered == 5);
    register_result = AZ_BUSY;
    construct(objects[0], 0);
    assert(registered == 6);
    assert(!state()->managers[0]);
    reset();
    struct azcue_profile fake = {0};
    fake.ctor.address = 1;
    fake.complete_dtor.address = 2;
    lifecycle_test_profile = &fake;
    fail_at = 2;
    assert(!azsdk_cue_lifecycle_configure(1, installer));
    assert(!state()->enabled);
    assert(!azsdk_cue_lifecycle_configure(1, installer));
    assert(installs == 2);
    reset();
    lifecycle_test_profile = &fake;
    assert(azsdk_cue_lifecycle_configure(2, installer));
    assert(state()->enabled);
    puts("cue lifecycle tests passed");
}
