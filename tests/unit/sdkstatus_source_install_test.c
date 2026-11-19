/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "sdk_status_source.c"
#include "sdk_status_map_source.c"
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static unsigned hook_count, hook_failure, hook_missing;
static void fixture_original(void) {}
static int install(uintptr_t target, uintptr_t entry, void *replacement, void **original, int slot) {
    assert(target && entry && replacement && original);
    if (slot) assert(*(uintptr_t *) target == entry);
    else assert(target == entry);
    ++hook_count;
    *original = hook_count == hook_missing ? NULL : (void *) fixture_original;
    return hook_count != hook_failure;
}
static void reset_hooks(void) {
    hook_count = hook_failure = hook_missing = 0;
    atomic_store(&js.ready, 0);
    atomic_store(&sm_ready, 0);
    memset(js.original, 0, sizeof js.original);
    memset(sm_original, 0, sizeof sm_original);
}
static void configure(unsigned version) {
    const struct js_profile *source = &js_profiles[version];
    for (unsigned i = 0; i < 7; i++) {
        memcpy((void *) source->target[i].entry, source->target[i].bytes, 16);
        if (source->target[i].slot) *(uintptr_t *) source->target[i].slot = source->target[i].entry;
    }
    const struct sm_profile *map = &sm_profiles[version];
    for (unsigned i = 0; i < 6; i++)
        memcpy((void *) map->functions[i].address, map->functions[i].words, sizeof map->functions[i].words);
    const struct sm_continuations *updates = &sm_updates[version];
    for (unsigned i = 0; i < 2; i++) *(uintptr_t *) updates->slot[i] = updates->target[i];
}
static void source_install(unsigned version) {
    const struct js_profile *profile = &js_profiles[version];
    reset_hooks();
    *(unsigned char *) profile->target[6].entry ^= 1;
    assert(!azjs_install(version + 1, install) && !hook_count && !atomic_load(&js.ready));
    *(unsigned char *) profile->target[6].entry ^= 1;
    *(uintptr_t *) profile->target[0].slot ^= 8;
    assert(!azjs_install(version + 1, install) && !hook_count);
    *(uintptr_t *) profile->target[0].slot ^= 8;
    for (unsigned failure = 1; failure <= 7; failure++) {
        reset_hooks();
        hook_failure = failure;
        assert(!azjs_install(version + 1, install) && hook_count == failure && !atomic_load(&js.ready));
    }
    reset_hooks();
    hook_missing = 3;
    assert(!azjs_install(version + 1, install) && hook_count == 3 && !atomic_load(&js.ready));
    reset_hooks();
    assert(azjs_install(version + 1, install) && hook_count == 7 && atomic_load(&js.ready));
}
static void map_install(unsigned version) {
    const struct sm_profile *profile = &sm_profiles[version];
    reset_hooks();
    *(uint32_t *) profile->functions[5].address ^= 1;
    assert(!sdk_status_map_source_install(version + 1, install) && !hook_count && !atomic_load(&sm_ready));
    *(uint32_t *) profile->functions[5].address ^= 1;
    *(uintptr_t *) sm_updates[version].slot[1] ^= 8;
    assert(!sdk_status_map_source_install(version + 1, install) && !hook_count);
    *(uintptr_t *) sm_updates[version].slot[1] ^= 8;
    for (unsigned failure = 1; failure <= 5; failure++) {
        reset_hooks();
        hook_failure = failure;
        assert(!sdk_status_map_source_install(version + 1, install) && hook_count == failure && !atomic_load(&sm_ready));
    }
    reset_hooks();
    hook_missing = 4;
    assert(!sdk_status_map_source_install(version + 1, install) && hook_count == 4 && !atomic_load(&sm_ready));
    reset_hooks();
    assert(sdk_status_map_source_install(version + 1, install) && hook_count == 5 && atomic_load(&sm_ready));
}
static const struct azstatus_native_v1 *native_service;
const struct azstatus_native_v1 *azstem_status_service_v1(void) { return native_service; }
static const struct azn_maps_api *map_service;
static unsigned map_queries, retirements, destructions, references = 1;
static const struct azn_maps_api *get_maps(void) {
    ++map_queries;
    return map_service;
}
static int prove(uintptr_t owner, uintptr_t pack, uintptr_t grid, uintptr_t grid_content, uintptr_t phrase, uintptr_t phrase_content) {
    (void) owner;
    (void) pack;
    (void) grid;
    (void) grid_content;
    (void) phrase;
    (void) phrase_content;
    return 1;
}
static void retire(uintptr_t owner) {
    assert(owner == 0x1268);
    ++retirements;
}
static unsigned char retain(uintptr_t object) {
    assert(object == 0x1234);
    ++references;
    return 1;
}
static unsigned char release(uintptr_t object) {
    assert(object == 0x1234 && references > 1);
    --references;
    return 0;
}
static uint64_t destroy(uintptr_t model) {
    assert(model == 0x1000);
    ++destructions;
    return 47;
}
static void services(void) {
    atomic_store(&sm_maps, NULL);
    assert(!sm_service());
    struct azstatus_native_v1 native = {.size = sizeof native, .version = 2, .maps = get_maps};
    native_service = &native;
    assert(!sm_service() && !map_queries);
    native.version = 1;
    native.size = offsetof(struct azstatus_native_v1, maps);
    assert(!sm_service() && !map_queries);
    native.size = sizeof native;
    assert(!sm_service() && map_queries == 1);
    struct azn_maps_api maps = {.size = sizeof maps, .version = 2, .prove_pair = prove, .retire_pair = retire};
    map_service = &maps;
    assert(!sm_service());
    maps.version = 1;
    maps.size = offsetof(struct azn_maps_api, retire_pair);
    assert(!sm_service());
    maps.size = sizeof maps;
    maps.retire_pair = NULL;
    assert(!sm_service());
    maps.retire_pair = retire;
    assert(sm_service() == &maps);
    unsigned calls = map_queries;
    native_service = NULL;
    assert(sm_service() == &maps && map_queries == calls);
    struct sm_profile profile = {0};
    profile.functions[2].address = (uintptr_t) retain;
    profile.functions[3].address = (uintptr_t) release;
    sm_profile = &profile;
    sm_original[4] = destroy;
    atomic_store(&sm_ready, 1);
    sm_binding(0x1268, 0x1234);
    assert(references == 2);
    assert(sm_destroy(0x1000) == 47 && retirements == 1 && destructions == 1 && references == 1);
    assert(!sm_bound_pack(0x1268));
    atomic_store(&sm_ready, 0);
    assert(sm_destroy(0x1000) == 47 && retirements == 1 && destructions == 2);
    atomic_store(&sm_maps, NULL);
    map_service = NULL;
}
int main(void) {
    assert(!azjs_install(0, install) && !azjs_install(1, NULL));
    assert(!sdk_status_map_source_install(0, install) && !sdk_status_map_source_install(1, NULL));
    uintptr_t low = 0x600000, high = 0x3200000;
    void *mapped = mmap((void *) low, high - low, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mapped == (void *) low);
    for (unsigned version = 0; version < 2; version++) {
        configure(version);
        source_install(version);
        map_install(version);
    }
    services();
    assert(!munmap(mapped, high - low));
    puts("PASS source/map hook installation for both firmware profiles, signature and vtable rejection, each hook failure, service ABI validation/cache and destructor retirement");
}
