/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__linux__)
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static unsigned save_hook_calls, save_hook_failure, save_hook_missing;
static void save_original(void) {}
static int save_installer(uintptr_t target, uintptr_t expected, void *hook, void **original, int slot) {
    assert(target == expected && !slot && hook && original);
    ++save_hook_calls;
    *original = save_hook_calls == save_hook_missing ? NULL : (void *) save_original;
    return save_hook_calls != save_hook_failure;
}
static void reset_save_hooks(void) {
    save_hook_calls = save_hook_failure = save_hook_missing = 0;
    atomic_store(&sw_ready, 0);
    memset(sw_original, 0, sizeof sw_original);
}
static int test_save_hooks(void) {
    const uintptr_t low = 0x1800000, high = 0x1a00000;
    void *mapped = mmap((void *) low, high - low, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mapped == (void *) low);
    assert(!sdk_status_save_install(0, save_installer) && !sdk_status_save_install(1, NULL));
    for (unsigned version = 0; version < 2; version++) {
        const struct sw_target *targets = sw_targets[version];
        for (unsigned i = 0; i < 4; i++) memcpy((void *) targets[i].address, targets[i].bytes, sizeof targets[i].bytes);
        reset_save_hooks();
        *(uint32_t *) targets[3].address ^= 1;
        assert(!sdk_status_save_install(version + 1, save_installer) && !save_hook_calls && !atomic_load(&sw_ready));
        *(uint32_t *) targets[3].address ^= 1;
        for (unsigned failure = 1; failure <= 4; failure++) {
            reset_save_hooks();
            save_hook_failure = failure;
            assert(!sdk_status_save_install(version + 1, save_installer));
            assert(save_hook_calls == failure && !atomic_load(&sw_ready));
        }
        reset_save_hooks();
        save_hook_missing = 2;
        assert(!sdk_status_save_install(version + 1, save_installer) && save_hook_calls == 2 && !atomic_load(&sw_ready));
        reset_save_hooks();
        assert(sdk_status_save_install(version + 1, save_installer) && save_hook_calls == 4 && atomic_load(&sw_ready));
    }
    assert(!munmap(mapped, high - low));
    puts("PASS save observation hooks validate both firmware profiles and publish readiness only after all four original writers are retained");
    return 0;
}
#endif
