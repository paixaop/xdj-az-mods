/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual startup caller/profile/helper, synthetic host installers/originals.
 * No firmware function, native ownership or public/source capability is tested. */
#include "sdk_cache_targets.h"
#include "sdk_cache_hooks.c"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_sdk_sources_on, g_ep147_build;
static _Atomic int g_azm_late;
static uintptr_t g_ep_img_lo, g_ep_img_hi;
typedef uint64_t (*host_fn)(uint64_t);
static unsigned patches, reads, native_calls[AZCE_KIND_COUNT], nesting;
static int fail_at, publish_failure, omit_at, invalid_admission, reenter;
static uint32_t modes;
static struct sdk_cache_target targets[SDK_CACHE_TARGET_COUNT];
static unsigned char image[SDK_CACHE_TARGET_COUNT][16];
static uint64_t forward(unsigned, uint64_t);
static uint64_t native(unsigned slot, uint64_t value) {
    ++native_calls[slot];
    if (slot == AZCE_KIND_LOADINFO_DESTROY && !nesting && azsdk_cache_entry_originals[AZCE_KIND_CACHE_DESTROY]) {
        ++nesting;
        uint64_t result = forward(AZCE_KIND_CACHE_DESTROY, value + 1);
        --nesting;
        return result + 100 + slot;
    }
    return value + 100 + slot;
}
#define HOST(n) \
    static uint64_t __attribute__((aligned(16))) original_##n(uint64_t value) { return native(n, value); } \
    void __attribute__((aligned(16))) azsdk_cache_entry_##n(void) { abort(); }
HOST(0) HOST(1) HOST(2) HOST(3) HOST(4) HOST(5) HOST(6) HOST(7) HOST(8) HOST(9) HOST(10)
HOST(11) HOST(12) HOST(13) HOST(14) HOST(15) HOST(16) HOST(17) HOST(18) HOST(19) HOST(20) HOST(21) HOST(22)
#undef HOST
static const host_fn originals[] = {original_0,original_1,original_2,original_3,original_4,original_5,
    original_6,original_7,original_8,original_9,original_10,original_11,original_12,original_13,
    original_14,original_15,original_16,original_17,original_18,original_19,original_20,original_21,original_22};
void azsdk_cache_invoke_original(struct azce_call *call) { (void) call; abort(); }
static uint64_t forward(unsigned slot, uint64_t value) {
    host_fn original = (host_fn) __atomic_load_n(&azsdk_cache_entry_originals[slot], __ATOMIC_ACQUIRE);
    assert(original && "partially exposed hook lost genuine original");
    return original(value);
}
/* The only injected boundary is raw mapped-byte copying. Range checks and
 * exact generated-byte comparison execute in the actual startup module. */
static void copy_bytes(void *out, uintptr_t address, size_t length) {
    assert(!patches && length == 16 && address >= g_ep_img_lo && address <= g_ep_img_hi &&
           g_ep_img_hi - address >= length);
    for (unsigned i = 0; i < SDK_CACHE_TARGET_COUNT; ++i) if (targets[i].address == address) {
        ++reads;
        memcpy(out, image[i], length);
        return;
    }
    assert(!"unexpected native text read");
}
static int azm_install_hook_at(uintptr_t, void *, void **, const char *);
static int azm_install_island_hook_at(uintptr_t, void *, void **, const char *);
#define AZSDK_CACHE_STARTUP_COPY_BYTES(out, address, length) copy_bytes((out), (address), (length))
#if !defined(SDK_CACHE_STARTUP_MISSING) && __has_include("../../src/azmod/sdk_cache_startup.c")
#include "../../src/azmod/sdk_cache_startup.c"
#define HAVE_STARTUP 1
#else
static struct sdk_cache_hook_state g_sdk_cache_hooks;
static void sdk_cache_at_start(void) {} // old caller leaves genuine originals untouched
#define HAVE_STARTUP 0
#endif
#undef AZSDK_CACHE_STARTUP_COPY_BYTES

static const uint32_t full_mask = ((UINT32_C(1) << AZCE_KIND_COUNT) - 2u);
static int install(uintptr_t address, void *replacement, void **original, const char *name, unsigned island) {
    assert(reads == SDK_CACHE_TARGET_COUNT && "patch started before every target proof");
    assert(patches < SDK_CACHE_TARGET_COUNT && targets[patches].address == address);
    const struct sdk_cache_target *target = &targets[patches];
    assert(target->island_only == island && replacement == azsdk_cache_entry(target->slot) &&
           original == &azsdk_cache_entry_originals[target->slot] && !*original && name && *name);
    assert(g_sdk_cache_hooks.attempted && !g_sdk_cache_hooks.ready && !azsdk_cache_capture_ready());
    unsigned ordinal = patches++;
    if (island) modes |= UINT32_C(1) << target->slot;
    if ((int) ordinal != omit_at && ((int) ordinal != fail_at || publish_failure))
        __atomic_store_n(original, invalid_admission && ordinal == SDK_CACHE_TARGET_COUNT - 1 ?
                        azsdk_cache_entry(0) : (void *) originals[target->slot], __ATOMIC_RELEASE);
    if (reenter) {
        unsigned before = patches;
        sdk_cache_at_start();
        assert(patches == before && reads == SDK_CACHE_TARGET_COUNT && !azsdk_cache_capture_ready());
    }
    if (azsdk_cache_entry_originals[18] && azsdk_cache_entry_originals[9])
        assert(forward(18, 5) == 233 && "published partial entries must already forward independently");
    return (int) ordinal == fail_at ? 0 : 1;
}
static int azm_install_hook_at(uintptr_t a, void *r, void **o, const char *n) { return install(a, r, o, n, 0); }
static int azm_install_island_hook_at(uintptr_t a, void *r, void **o, const char *n) { return install(a, r, o, n, 1); }
/* Each scenario is a fresh child of a parent that never admits. Production
 * admission/table state is never reset or replaced by a test implementation. */
static void setup(int build) {
    assert(!azsdk_cache_capture_ready() && !g_sdk_cache_hooks.attempted && !g_sdk_cache_hooks.ready);
    fail_at = omit_at = -1;
    g_sdk_sources_on = 1; atomic_store(&g_azm_late, 0); g_ep147_build = build;
    assert(ep147_addr_init(build));
    assert(sdk_cache_target_profile(build, targets, SDK_CACHE_TARGET_COUNT) == SDK_CACHE_TARGET_COUNT);
    g_ep_img_lo = UINTPTR_MAX; g_ep_img_hi = 0;
    for (unsigned i = 0; i < SDK_CACHE_TARGET_COUNT; ++i) {
        memcpy(image[i], targets[i].expected, 16);
        if (targets[i].address < g_ep_img_lo) g_ep_img_lo = targets[i].address;
        if (targets[i].address + 16 > g_ep_img_hi) g_ep_img_hi = targets[i].address + 16;
    }
}
static void untouched(void) {
    assert(!patches && !azsdk_cache_capture_ready() && !g_sdk_cache_hooks.ready && !g_sdk_cache_hooks.installed_mask);
    for (unsigned i = 0; i < AZCE_KIND_COUNT; ++i) assert(!azsdk_cache_entry_originals[i]);
}
static void no_retry(void) {
    unsigned before_patches = patches, before_reads = reads;
    int before_ready = azsdk_cache_capture_ready();
    void *before[AZCE_KIND_COUNT]; memcpy(before, azsdk_cache_entry_originals, sizeof before);
    sdk_cache_at_start();
    assert(before_patches == patches && before_reads == reads && before_ready == azsdk_cache_capture_ready());
    assert(!memcmp(before, azsdk_cache_entry_originals, sizeof before));
}
enum scenario { FULL, OFF, LATE, UNKNOWN, WRONG_PROFILE, UNRESOLVED, EMPTY_TABLE,
                BAD_BYTE, EMPTY_BOUNDS, BELOW_BOUNDS, SHORT_BOUNDS, PARTIAL, OMIT, ADMISSION_REFUSED };
static void scenario(int build, enum scenario which, unsigned parameter) {
    setup(build);
    switch (which) {
    case FULL:
        assert(original_9(3) == 112 && original_18(3) == 121);
        printf("CACHE_STARTUP_NATIVE_BASELINE_OK build=%d caller=%d\n", build, HAVE_STARTUP); fflush(stdout);
        reenter = 1; sdk_cache_at_start();
        assert(patches == SDK_CACHE_TARGET_COUNT && azsdk_cache_capture_ready() &&
               "missing actual full cache startup caller");
        assert(g_sdk_cache_hooks.ready && g_sdk_cache_hooks.installed_mask == full_mask &&
               modes == ((UINT32_C(1) << AZCE_KIND_LAYOUT) | (UINT32_C(1) << AZCE_KIND_SELECTION_CLEAR)));
        for (unsigned slot = 1; slot < AZCE_KIND_COUNT; ++slot)
            assert(azsdk_cache_entry_originals[slot] == (void *) originals[slot]);
        assert(forward(18, 5) == 233 && native_calls[9] && native_calls[18]); no_retry();
        g_sdk_sources_on = 0; sdk_cache_at_start();
        assert(azsdk_cache_capture_ready() && g_sdk_cache_hooks.ready && g_sdk_cache_hooks.installed_mask == full_mask); no_retry();
        return;
    case OFF: g_sdk_sources_on = 0; break;
    case LATE: atomic_store(&g_azm_late, 1); break;
    case UNKNOWN: g_ep147_build = EPB_UNKNOWN; break;
    case WRONG_PROFILE: g_ep147_build = build == EPB_V130 ? EPB_V200 : EPB_V130; break;
    case UNRESOLVED: ep147_addr[EPI_11fc670] = 0; break;
    case EMPTY_TABLE: memset(ep147_addr, 0, sizeof ep147_addr); break;
    case BAD_BYTE:
        image[SDK_CACHE_TARGET_COUNT - 1][parameter] ^= 1;
        sdk_cache_at_start(); untouched();
        assert(reads == SDK_CACHE_TARGET_COUNT && g_sdk_cache_hooks.attempted); no_retry(); return;
    case EMPTY_BOUNDS: g_ep_img_lo = g_ep_img_hi = 0; break;
    case BELOW_BOUNDS: g_ep_img_lo = targets[0].address + 1; break;
    case SHORT_BOUNDS: g_ep_img_hi = targets[0].address + 15; break;
    case PARTIAL: {
            int ordinal = (int) (parameter / 2), publish = parameter % 2;
            fail_at = ordinal; publish_failure = publish; reenter = 1; sdk_cache_at_start();
            uint32_t expected = 0;
            for (int i = 0; i < ordinal + publish; ++i) expected |= UINT32_C(1) << targets[i].slot;
            assert(patches == (unsigned) ordinal + 1 && !azsdk_cache_capture_ready() && !g_sdk_cache_hooks.ready &&
                   g_sdk_cache_hooks.installed_mask == expected);
            for (unsigned i = 0; i < SDK_CACHE_TARGET_COUNT; ++i)
                assert(azsdk_cache_entry_originals[targets[i].slot] ==
                       ((int) i < ordinal + publish ? (void *) originals[targets[i].slot] : NULL));
            if (expected & (UINT32_C(1) << 18)) assert(forward(18, 5) == ((expected & (UINT32_C(1) << 9)) ? 233 : 123));
            no_retry();
            return;
        }
    case OMIT:
        omit_at = 0; sdk_cache_at_start();
        assert(patches == 1 && !g_sdk_cache_hooks.ready && !azsdk_cache_capture_ready()); no_retry(); return;
    case ADMISSION_REFUSED:
        invalid_admission = 1; sdk_cache_at_start();
        assert(patches == SDK_CACHE_TARGET_COUNT && !azsdk_cache_capture_ready() && g_sdk_cache_hooks.ready); no_retry(); return;
    }
    sdk_cache_at_start(); untouched(); assert(!reads);
    if (which == OFF || which == LATE) assert(!g_sdk_cache_hooks.attempted);
    no_retry();
}
static void run(int build, enum scenario which, unsigned parameter) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) { scenario(build, which, parameter); _exit(0); }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) exit(1);
}
int main(void) {
    for (int build = EPB_V130; build <= EPB_V200; ++build) {
        for (enum scenario which = FULL; which <= ADMISSION_REFUSED; ++which) {
            unsigned count = which == BAD_BYTE ? 16 : which == PARTIAL ? SDK_CACHE_TARGET_COUNT * 2 : 1;
            for (unsigned parameter = 0; parameter < count; ++parameter) run(build, which, parameter);
        }
    }
    puts("PASS cache-startup: complete byte preflight, startup gates, per-entry modes, partial forwarding and one-shot admission");
    return 0;
}
