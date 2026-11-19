/* SPDX-License-Identifier: GPL-3.0-only */
#define main memory_baseline_main
#include "sdkstatus_memory_test.c"
#undef main
#include <sys/mman.h>
static uintptr_t targets[23];
static unsigned target_count, patches;
static int fail_site = -1, fail_mode;
static void original(void) {}
static int install_patch(uintptr_t address, void *hook, void **retained, const char *label) {
    assert(hook && label && !*retained);
    unsigned index;
    for (index = 0; index < target_count; ++index)
        if (targets[index] == address) break;
    assert(index < target_count);
    if ((int) index == fail_site) return fail_mode == 1 ? 1 : 0;
    *retained = (void *) original;
    *(uint32_t *) address = 0x14000000u;
    ++patches;
    return 1;
}
static void seed_file_sites(int build, uintptr_t bias) {
    const uint32_t words[][2] = {
        {0xa9bc7bfd, 0x910003fd}, {0xa9b87bfd, 0x910003fd}, {0xa9ba7bfd, 0xaa0003e7}, {build == 1 ? 0xa9b07bfdu : 0xa9b17bfdu, 0x910003fd}, {0xd10a83ff, 0xa9007bfd}, {0xa9aa7bfd, 0x910003fd}, {0xa9b47bfd, 0x910003fd}, {0xa9b97bfd, 0x910003fd}, {0xa9bc7bfd, 0x910003fd}, {0x39400022, 0x7103fc5f}, {0xa9bb7bfd, 0x910003fd}, {0xa9b97bfd, 0x910003fd}, {0xa9bb7bfd, 0x910003fd}};
    for (unsigned i = 0; i < 13; ++i) {
        targets[target_count++] = bias + file_profiles[build - 1].methods[i];
        memcpy((void *) targets[i], words[i], sizeof words[i]);
    }
}
static void seed_map_sites(int build, uintptr_t bias) {
    const uint32_t words[][2] = {
        {0xa9b57bfd, 0x910003fd}, {0xa9ae7bfd, 0x910003fd}, {0xa9ba7bfd, 0xaa0003e7}, {0xa9b97bfd, 0xaa0003e7}, {0xa9b67bfd, 0xd2800067}, {0xa9af7bfd, 0x910003fd}, {0xa9b57bfd, 0x910003fd}, {0xf100003f, 0x7a401864}, {0xa9b37bfd, 0x910003fd}, {0xa9b37bfd, 0x910003fd}};
    for (unsigned i = 0; i < 10; ++i) {
        uintptr_t address = bias + file_map_profiles[build - 1].method[i];
        targets[target_count++] = address;
        memcpy((void *) address, words[i], sizeof words[i]);
    }
}
int main(int argc, char **argv) {
    assert(argc == 5);
    int build = atoi(argv[1]);
    bool maps = !strcmp(argv[2], "maps");
    int index = atoi(argv[3]);
    fail_mode = atoi(argv[4]);
    void *memory = mmap(NULL, 0x3000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    uintptr_t bias = (uintptr_t) memory;
    seed_file_sites(build, bias);
    if (maps) seed_map_sites(build, bias);
    const struct azn_file_cue_api *api = azn_file_service();
    struct azn_file_cue_callbacks callbacks = {0};
    callbacks.size = sizeof callbacks;
    callbacks.version = 2;
    callbacks.begin = input_begin;
    callbacks.complete = input_complete;
    callbacks.hold = input_hold;
    callbacks.opened = input_opened;
    struct azn_file_map_callbacks map_callbacks = {sizeof map_callbacks, 1, NULL, map_test_complete, map_test_hold, NULL, common_api};
    assert(!api->install(0, install_patch, &callbacks) && !api->install(3, install_patch, &callbacks));
    assert(!api->install(build, NULL, &callbacks) && !api->install(build, install_patch, NULL));
    assert(!api->install(build, install_patch, &callbacks)); /* pack family not configured */
    profile = mem_profiles[build - 1];
    profile.pack += bias;
    recall_bias = bias;
    atomic_store(&pack_profile_ready, true);
    struct azn_file_cue_callbacks invalid = callbacks;
    invalid.version = 1;
    assert(!api->install(build, install_patch, &invalid));
    invalid = callbacks;
    invalid.size = 0;
    assert(!api->install(build, install_patch, &invalid));
    invalid = callbacks;
    invalid.opened = NULL;
    assert(!api->install(build, install_patch, &invalid));
    assert(!patches);
    if (maps) {
        assert(!api->install_maps(build, install_patch, &map_callbacks));
        assert(api->install(build, install_patch, &callbacks) && patches == 13);
        assert(!api->install_maps(build, NULL, &map_callbacks) && !api->install_maps(build, install_patch, NULL));
        struct azn_file_map_callbacks bad = map_callbacks;
        bad.version = 2;
        assert(!api->install_maps(build, install_patch, &bad));
        bad = map_callbacks;
        bad.size = 0;
        assert(!api->install_maps(build, install_patch, &bad));
        bad = map_callbacks;
        bad.inputs = NULL;
        assert(!api->install_maps(build, install_patch, &bad));
    }
    fail_site = index < 0 ? -1 : index + (maps ? 13 : 0);
    uint32_t first = fail_site < 0 ? 0 : *(uint32_t *) targets[fail_site];
    if (fail_site >= 0 && fail_mode == 2) *(uint32_t *) targets[fail_site] ^= 1;
    int result = maps ? api->install_maps(build, install_patch, &map_callbacks) : api->install(build, install_patch, &callbacks);
    assert(result == (index < 0));
    if (index >= 0) {
        assert(!(maps ? atomic_load(&file_map_callbacks) != NULL : atomic_load(&file_callbacks) != NULL));
        *(uint32_t *) targets[fail_site] = first;
        fail_site = -1;
        assert(maps ? api->install_maps(build, install_patch, &map_callbacks) : api->install(build, install_patch, &callbacks));
    }
    assert(patches == target_count);
    assert(!api->install(3 - build, install_patch, &callbacks));
    assert(api->install(build, install_patch, &callbacks) && patches == target_count);
    struct azn_file_cue_callbacks different = callbacks;
    assert(!api->install(build, install_patch, &different));
    if (maps) {
        assert(api->install_maps(build, install_patch, &map_callbacks) && patches == target_count);
        assert(!api->install_maps(3 - build, install_patch, &map_callbacks));
        struct azn_file_map_callbacks different_map = map_callbacks;
        assert(!api->install_maps(build, install_patch, &different_map));
    }
    assert(!munmap(memory, 0x3000000));
    return 0;
}
