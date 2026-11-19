/* SPDX-License-Identifier: GPL-3.0-only */
#define main maps_baseline_main
#include "sdkstatus_maps_test.c"
#undef main

int main(int argc, char **argv) {
    assert(argc == 2);
    int build = atoi(argv[1]);
    assert(azn_maps_configure(build));
    const struct azn_maps_api *api = azn_maps_service();
    Buf150 p = {0};
    Buf80 gh = {0}, ph = {0}, gc = {0}, pc = {0}, wrong = {0};
    uintptr_t pack_address = (uintptr_t) p.bytes, grid_holder = (uintptr_t) gh.bytes, phrase_holder = (uintptr_t) ph.bytes;
    uintptr_t grid = (uintptr_t) gc.bytes, phrase = (uintptr_t) pc.bytes;
    put(pack_address, profile->pack);
    put(grid_holder, profile->grid_holder);
    put(phrase_holder, profile->phrase_holder);
    put(grid, profile->grid_content);
    put(phrase, profile->phrase_content);
    put(pack_address + 0xd8, grid_holder);
    put(pack_address + 0x120, phrase_holder);
    put(grid_holder + 0x28, grid);
    put(grid_holder + 0x40, (uint32_t) 48000);
    put(phrase_holder + 0x28, phrase);
    struct {
        int64_t sample;
        double bpm;
    } beats[] = {{0, 120}, {24000, 120}};
    uint32_t sections[][5] = {{1, 1, 2, 0, 0}};
    put(grid + 0x28, (uintptr_t) beats);
    put(grid + 0x30, (int32_t) 2);
    put(phrase + 0x40, (uintptr_t) sections);
    put(phrase + 0x48, (int32_t) 1);
    assert(!api->begin_pair(138, pack_address)); /* no deck membership yet */
    assert(api->prove_pair(138, pack_address, grid_holder, grid, phrase_holder, phrase));
    playing = pack_address;
    observer(0, 5, 13, epoch);
    assert(observed_count == 1);
    assert(!api->begin_pair(0, 0) && !api->begin_pair(999, 0));
    assert(!api->begin_pair(138, (uintptr_t) wrong.bytes));
    assert(api->begin_pair(138, 0) == 1 && api->begin_pair(0, pack_address) == 1);
    shared_second = true;
    assert(api->begin_pair(138, 0) == 3 && api->begin_pair(0, pack_address) == 3);
    assert(!api->refresh_pair(0, grid_holder, grid, phrase_holder, phrase));
    assert(!api->refresh_pair(138, 0, 0, 0, 0));
    assert(!api->refresh_pair(999, grid_holder, grid, phrase_holder, phrase));
    assert(!api->refresh_pair(138, (uintptr_t) wrong.bytes, grid, phrase_holder, phrase));
    uint64_t prior = observed[0].map.grid.revision;
    put(grid_holder + 0x30, (int64_t) 480);
    assert(api->refresh_pair(138, grid_holder, grid, phrase_holder, phrase));
    assert(observed_count == 3 && observed[1].map.grid.revision == prior + 1 && observed[2].deck == 1);
    assert(azn_maps_current(0, epoch, &observed[1].map));
    unsigned before = gaps;
    Slot *slot = &cache[bucket(pack_address)];
    assert(!atomic_flag_test_and_set(&slot->busy));
    assert(!api->prove_pair(138, pack_address, grid_holder, grid, phrase_holder, phrase));
    atomic_flag_clear(&slot->busy);
    assert(gaps == before + 2 && observed_count == 3); /* both decks sharing this pack lose the busy proof */
    before = gaps;
    map_writer_original[0] = (void *) test_assign;
    uintptr_t invalid = (uintptr_t) wrong.bytes;
    assert(grid_assign(grid_holder + 0x28, (uintptr_t) &invalid) == 1);
    assert(gaps == before + 2 && observed_count == 3); /* wrong native value is never published as an owned map */
    put(grid_holder + 0x28, grid);
    assert(api->refresh_pair(138, grid_holder, grid, phrase_holder, phrase));
    assert(observed_count == 3); /* equal retained edition does not invent a new revision */
    api->seed(0);
    assert(observed_count == 4 && azn_maps_current(0, epoch, &observed[3].map));
    playing = 0;
    api->retire_pair(138);
    drain(api);
    for (size_t i = 0; i < refs_count; ++i) assert(!refs[i].count);
    puts("PASS map transactions: owner/pack masks, shared decks, retained refresh, busy proof and invalid replacement gaps, balanced custody");
    return 0;
}
