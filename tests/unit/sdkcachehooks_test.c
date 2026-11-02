/* SPDX-License-Identifier: GPL-3.0-only */
/* Startup installer behavior only. Host originals have a synthetic C signature;
 * this fixture establishes no firmware ABI, owner or reachable native hook. */
#include "sdk_cache_targets.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t (*host_fn)(uint64_t);
void *azsdk_cache_entry_originals[AZCE_KIND_COUNT];
static unsigned native_calls[AZCE_KIND_COUNT], nesting;
static int nested_forward, missing_entry = -1, alias_entry;
static uint64_t forward(unsigned, uint64_t);
static uint64_t native(unsigned slot, uint64_t value) {
    native_calls[slot]++;
    if (nested_forward && slot == AZCE_KIND_LOADINFO_DESTROY && !nesting) {
        nesting++;
        uint64_t result = forward(AZCE_KIND_CACHE_DESTROY, value + 1);
        nesting--;
        return result + 1000u + slot;
    }
    return value + 1000u + slot;
}
#define HOST(n) \
    static uint64_t __attribute__((aligned(16))) original_##n(uint64_t value) { return native(n, value); } \
    static uint64_t __attribute__((aligned(16))) entry_##n(uint64_t value) { return forward(n, value); }
HOST(0) HOST(1) HOST(2) HOST(3) HOST(4) HOST(5) HOST(6) HOST(7) HOST(8) HOST(9) HOST(10)
HOST(11) HOST(12) HOST(13) HOST(14) HOST(15) HOST(16) HOST(17) HOST(18) HOST(19) HOST(20) HOST(21) HOST(22)
#undef HOST
static const host_fn originals[] = {original_0,original_1,original_2,original_3,original_4,original_5,
    original_6,original_7,original_8,original_9,original_10,original_11,original_12,original_13,
    original_14,original_15,original_16,original_17,original_18,original_19,original_20,original_21,original_22};
static const host_fn entries[] = {entry_0,entry_1,entry_2,entry_3,entry_4,entry_5,
    entry_6,entry_7,entry_8,entry_9,entry_10,entry_11,entry_12,entry_13,
    entry_14,entry_15,entry_16,entry_17,entry_18,entry_19,entry_20,entry_21,entry_22};
void *azsdk_cache_entry(unsigned slot) {
    if (slot >= AZCE_KIND_COUNT || slot == (unsigned) missing_entry) return NULL;
    return (void *) entries[alias_entry && slot == AZCE_KIND_CACHE_DESTROY ? AZCE_KIND_LOADINFO_DESTROY : slot];
}
static uint64_t forward(unsigned slot, uint64_t value) {
    host_fn original = (host_fn) __atomic_load_n(&azsdk_cache_entry_originals[slot], __ATOMIC_ACQUIRE);
    assert(original && "exposed entry has no original");
    return original(value);
}

enum malformed { NORMAL, SHORT_PROFILE, DUPLICATE_SLOT, BAD_SLOT, ZERO_ADDRESS,
                 UNALIGNED_ADDRESS, DUPLICATE_ADDRESS, ZERO_PROOF, BAD_ISLAND };
static enum malformed malformed_profile;
/* Mutate a local copy of the real generated profile; generated inputs stay unchanged. */
static int fixture_profile(int build, struct sdk_cache_target *out, size_t cap) {
    int n = sdk_cache_target_profile(build, out, cap);
    if (n != SDK_CACHE_TARGET_COUNT) return n;
    unsigned a = SDK_CACHE_TARGET_COUNT, b = SDK_CACHE_TARGET_COUNT;
    for (unsigned i = 0; i < SDK_CACHE_TARGET_COUNT; ++i) {
        if (out[i].slot == AZCE_KIND_LOADINFO_DESTROY) a = i;
        if (out[i].slot == AZCE_KIND_CACHE_DESTROY) b = i;
    }
    assert(a < SDK_CACHE_TARGET_COUNT && b < SDK_CACHE_TARGET_COUNT);
    assert(out[a].slot == AZCE_KIND_LOADINFO_DESTROY && out[b].slot == AZCE_KIND_CACHE_DESTROY);
    switch (malformed_profile) {
    case SHORT_PROFILE: return n - 1;
    case DUPLICATE_SLOT: out[b].slot = out[a].slot; break;
    case BAD_SLOT: out[b].slot = AZCE_KIND_COUNT; break;
    case ZERO_ADDRESS: out[b].address = 0; break;
    case UNALIGNED_ADDRESS: out[b].address |= 1u; break;
    case DUPLICATE_ADDRESS: out[b].address = out[a].address; break;
    case ZERO_PROOF: memset(out[b].expected, 0, sizeof out[b].expected); break;
    case BAD_ISLAND: out[b].island_only = 2; break;
    default: break;
    }
    return n;
}
#define sdk_cache_target_profile fixture_profile
#if !defined(SDK_CACHE_HOOKS_MISSING) && __has_include("../../src/azmod/sdk_cache_hooks.c")
#include "../../src/azmod/sdk_cache_hooks.c"
#define HAVE_CACHE_HOOKS 1
#else
struct sdk_cache_hook_state { unsigned attempted, ready; uint32_t installed_mask; };
typedef int (*sdk_cache_verify_target_fn)(void *, const struct sdk_cache_target *);
typedef int (*sdk_cache_patch_target_fn)(void *, const struct sdk_cache_target *, void *, void **);
static int sdk_cache_hooks_install(struct sdk_cache_hook_state *state, int build, uint32_t requested,
                                   sdk_cache_verify_target_fn verify, sdk_cache_patch_target_fn patch, void *context) {
    (void) state; (void) build; (void) requested; (void) verify; (void) patch; (void) context;
    return 0; /* transparent originals exist, but there is no startup batch installer */
}
#define HAVE_CACHE_HOOKS 0
#endif
#undef sdk_cache_target_profile

struct context {
    struct sdk_cache_hook_state *state;
    int build, verify_fail, verify_code, patch_fail, patch_code, omit_original, bad_original;
    unsigned verified, patched, requested_count, reenter;
    uint32_t seen_modes;
    struct sdk_cache_target canonical[SDK_CACHE_TARGET_COUNT];
};
static uint32_t bit(unsigned slot) { return UINT32_C(1) << slot; }
static const uint32_t destructor_pair = (UINT32_C(1) << AZCE_KIND_LOADINFO_DESTROY) |
                                         (UINT32_C(1) << AZCE_KIND_CACHE_DESTROY);
static int verify(void *opaque, const struct sdk_cache_target *target) {
    struct context *c = opaque;
    assert(c->patched == 0 && "proof verification happened after first patch");
    const struct sdk_cache_target *expected = NULL;
    for (unsigned i = 0; i < SDK_CACHE_TARGET_COUNT; ++i)
        if (c->canonical[i].slot == target->slot) expected = &c->canonical[i];
    assert(expected && target->address == expected->address && target->island_only == expected->island_only);
    assert(!memcmp(target->expected, expected->expected, sizeof target->expected));
    c->verified++;
    return target->slot == (unsigned) c->verify_fail ? c->verify_code : 1;
}
static int patch(void *opaque, const struct sdk_cache_target *target, void *replacement, void **original) {
    struct context *c = opaque;
    assert(c->verified == c->requested_count && "batch was not fully preflighted");
    assert(target->slot > 0 && target->slot < AZCE_KIND_COUNT);
    assert(original == &azsdk_cache_entry_originals[target->slot] && !*original);
    assert(replacement == (void *) entries[target->slot]);
    assert(c->state->attempted && !c->state->ready);
    c->patched++;
    if (target->island_only) c->seen_modes |= bit(target->slot);
    if (target->slot != (unsigned) c->omit_original) {
        void *published = (void *) originals[target->slot];
        if (c->bad_original == 1) published = replacement;
        if (c->bad_original == 2 && target->slot == AZCE_KIND_CACHE_DESTROY)
            published = (void *) originals[AZCE_KIND_LOADINFO_DESTROY];
        if (c->bad_original == 3) published = (void *) target->address;
        __atomic_store_n(original, published, __ATOMIC_RELEASE);
    }
    if (c->reenter) {
        unsigned calls = c->patched;
        assert(!sdk_cache_hooks_install(c->state, c->build, destructor_pair, verify, patch, c));
        assert(c->patched == calls);
    }
    if (azsdk_cache_entry_originals[AZCE_KIND_CACHE_DESTROY] && azsdk_cache_entry_originals[AZCE_KIND_LOADINFO_DESTROY] && !c->bad_original) {
        nested_forward = 1;
        assert(((host_fn) azsdk_cache_entry(AZCE_KIND_LOADINFO_DESTROY))(7) == 2035);
    }
    return target->slot == (unsigned) c->patch_fail ? c->patch_code : 1;
}
/* Each setup represents a separate process startup. No reset/uninstall API is
 * provided by production code, and live original slots are never reset by it. */
static void setup(struct context *c, struct sdk_cache_hook_state *state, int build) {
    memset(state, 0, sizeof *state); memset(c, 0, sizeof *c);
    memset(azsdk_cache_entry_originals, 0, sizeof azsdk_cache_entry_originals);
    memset(native_calls, 0, sizeof native_calls);
    missing_entry = -1; alias_entry = nested_forward = nesting = 0; malformed_profile = NORMAL;
    c->state = state; c->build = build; c->requested_count = 2;
    c->verify_fail = c->patch_fail = c->omit_original = -1;
    assert(ep147_addr_init(build));
    assert(sdk_cache_target_profile(build, c->canonical, SDK_CACHE_TARGET_COUNT) == SDK_CACHE_TARGET_COUNT);
}
static void untouched(const struct context *c, const struct sdk_cache_hook_state *s) {
    assert(!c->patched && !s->ready && !s->installed_mask);
    for (unsigned i = 0; i < AZCE_KIND_COUNT; ++i) assert(!azsdk_cache_entry_originals[i]);
}
int main(void) {
    _Static_assert(AZCE_KIND_MESSAGE == 0 && AZCE_KIND_POOL_GROW == 10 && AZCE_KIND_PRIMARY_FACTORY == 11 &&
                   AZCE_KIND_COLLECTOR_DELETE == 20 && AZCE_KIND_REPLY_TICKET == 21 && AZCE_KIND_REPLY_RECEPTION == 22 && AZCE_KIND_COUNT == 23 && sizeof(struct azce_call) == AZCE_SIZE,
                   "existing entry IDs and frame ABI stay unchanged");
    for (int build = EPB_V130; build <= EPB_V200; ++build) {
        struct sdk_cache_hook_state state; struct context c;
        setup(&c, &state, build);
        assert(original_9(3) == 1012 && original_18(3) == 1021);
        printf("CACHE_NATIVE_BASELINE_OK build=%d installer=%d\n", build, HAVE_CACHE_HOOKS); fflush(stdout);
        c.reenter = 1;
        assert(sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) == 1 && "missing complete startup batch installation");
        assert(state.attempted && state.ready && state.installed_mask == destructor_pair && c.patched == 2);
        assert(azsdk_cache_entry_originals[9] == (void *) original_9 && azsdk_cache_entry_originals[18] == (void *) original_18);
        assert(azsdk_cache_entry_originals[9] != azsdk_cache_entry_originals[18]);
        assert(forward(18, 5) == 2033 && native_calls[9] && native_calls[18]);
        unsigned patched = c.patched;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && c.patched == patched && state.ready);

        setup(&c, &state, build); c.patch_fail = AZCE_KIND_CACHE_DESTROY; c.patch_code = 0; c.reenter = 1;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c));
        assert(state.attempted && !state.ready && state.installed_mask == destructor_pair && c.patched == 2);
        void *first = azsdk_cache_entry_originals[9], *second = azsdk_cache_entry_originals[18];
        assert(first && second && first != second && forward(18, 5) == 2033);
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && c.patched == 2);
        assert(azsdk_cache_entry_originals[9] == first && azsdk_cache_entry_originals[18] == second);

        setup(&c, &state, build); c.patch_fail = AZCE_KIND_LOADINFO_DESTROY; c.patch_code = 0;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c));
        assert(!state.ready && state.installed_mask == bit(18) && c.patched == 1 && forward(18, 1) == 1019);
        setup(&c, &state, build); c.verify_fail = AZCE_KIND_CACHE_DESTROY;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c)); untouched(&c, &state);
        unsigned verified = c.verified;
        c.verify_fail = -1;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && c.verified == verified);
        setup(&c, &state, build); c.verify_fail = AZCE_KIND_CACHE_DESTROY; c.verify_code = 2;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c)); untouched(&c, &state);
        for (int mode = SHORT_PROFILE; mode <= BAD_ISLAND; ++mode) {
            setup(&c, &state, build); malformed_profile = (enum malformed) mode;
            assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c)); untouched(&c, &state);
        }
        setup(&c, &state, build); missing_entry = AZCE_KIND_CACHE_DESTROY;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c)); untouched(&c, &state);
        setup(&c, &state, build); alias_entry = 1;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c)); untouched(&c, &state);
        const uint32_t bad_masks[] = {0, 1, UINT32_C(1) << 31};
        for (unsigned i = 0; i < sizeof bad_masks / sizeof bad_masks[0]; ++i) {
            setup(&c, &state, build);
            assert(!sdk_cache_hooks_install(&state, build, bad_masks[i], verify, patch, &c)); untouched(&c, &state);
        }
        setup(&c, &state, build);
        assert(!sdk_cache_hooks_install(&state, EPB_UNKNOWN, destructor_pair, verify, patch, &c)); untouched(&c, &state);
        setup(&c, &state, build);
        assert(!sdk_cache_hooks_install(&state, build == EPB_V130 ? EPB_V200 : EPB_V130, destructor_pair, verify, patch, &c)); untouched(&c, &state);
        setup(&c, &state, build); ep147_addr[EPI_11fc670] = 0;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c)); untouched(&c, &state);
        setup(&c, &state, build);
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, NULL, patch, &c)); untouched(&c, &state);
        setup(&c, &state, build);
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, NULL, &c)); untouched(&c, &state);
        assert(!sdk_cache_hooks_install(NULL, build, destructor_pair, verify, patch, &c));

        setup(&c, &state, build); c.omit_original = AZCE_KIND_LOADINFO_DESTROY;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && !state.ready && !state.installed_mask);
        setup(&c, &state, build); c.bad_original = 1;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && !state.ready);
        setup(&c, &state, build); c.bad_original = 2;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && !state.ready);
        assert(state.installed_mask == destructor_pair && azsdk_cache_entry_originals[9] == azsdk_cache_entry_originals[18]);
        setup(&c, &state, build); c.bad_original = 3;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && !state.ready);
        setup(&c, &state, build); c.patch_fail = AZCE_KIND_LOADINFO_DESTROY; c.patch_code = 2;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && !state.ready && forward(18, 1) == 1019);
        setup(&c, &state, build);
        uint32_t island_mask = bit(AZCE_KIND_LAYOUT) | bit(AZCE_KIND_SELECTION_CLEAR);
        assert(sdk_cache_hooks_install(&state, build, island_mask, verify, patch, &c));
        assert(state.ready && state.installed_mask == island_mask && c.seen_modes == island_mask);
        setup(&c, &state, build); azsdk_cache_entry_originals[9] = (void *) original_9;
        assert(!sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c) && !c.patched && !c.verified);
        assert(azsdk_cache_entry_originals[9] == (void *) original_9 && !state.ready && !state.installed_mask);
        setup(&c, &state, build); azsdk_cache_entry_originals[0] = (void *) original_0;
        assert(sdk_cache_hooks_install(&state, build, destructor_pair, verify, patch, &c));
        assert(azsdk_cache_entry_originals[0] == (void *) original_0);
        setup(&c, &state, build); c.requested_count = SDK_CACHE_TARGET_COUNT;
        uint32_t all = 0;
        for (unsigned i = 0; i < SDK_CACHE_TARGET_COUNT; ++i) all |= bit(c.canonical[i].slot);
        assert(sdk_cache_hooks_install(&state, build, all, verify, patch, &c));
        assert(state.ready && state.installed_mask == all && c.patched == SDK_CACHE_TARGET_COUNT && c.seen_modes == island_mask);
    }
    puts("cache startup hooks: distinct originals, full preflight, partial forwarding, island modes and one-shot state passed");
    return 0;
}
