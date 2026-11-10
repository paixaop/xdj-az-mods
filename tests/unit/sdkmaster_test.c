/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual native confirmed-master hook through the public deck projection. */
#define _POSIX_C_SOURCE 200809L
#include "azmod-mod.h"
#include "sdk_analysis.h"
#include "sdk_data_engine.h"
#include "../common/azdeckinfo.h"
#include "../common/aztimestamp.h"
#include "../common/azloadexport.h"
#include "../common/azcueexport.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define AZMOD_STEM_DECKS 4
static FILE *azlog_open(const char *path, const char *mode) {
    (void) path; (void) mode; errno = EIO; return NULL;
}
static uint64_t clock_ns = 1000000000;
static int clock_ok = 1;
static int test_clock_gettime(clockid_t id, struct timespec *out) {
    assert(id == CLOCK_MONOTONIC);
    out->tv_sec = clock_ns / 1000000000;
    out->tv_nsec = clock_ns % 1000000000;
    return clock_ok ? 0 : -1;
}
#ifdef __clang__
#define __atomic_exchange_n(p, v, order) atomic_exchange_explicit(p, v, order)
#endif
static int test_install(uintptr_t, void *, void **);
#define MASTER_EXCHANGED ((uintptr_t) 0xaabb78)
#define azm_install_hook(target,hook,out) test_install(target,hook,out)
#define clock_gettime test_clock_gettime
static void ds_master_capture(uint64_t revision,uint64_t observed,int master,int captured) {
    (void)revision; (void)observed; (void)master; (void)captured;
}
#include MASTER_INC
#undef clock_gettime
#undef azm_install_hook
static int g_st_enabled = 1;
static _Atomic(azstem_deck_observation_v1_fn) g_sd_observation;
static _Atomic(azstem_load_observation_v1_fn) g_sd_load_observation;
static _Atomic(azstem_cue_observation_v1_fn) g_sd_cue_observation;
static _Atomic(azstem_cue_observer_mask_v1_fn) g_sd_cue_mask;
static _Atomic(azstem_loop_observation_v2_fn) g_sd_loop_observation;
static _Atomic(azstem_loop_observer_mask_v1_fn) g_sd_loop_mask;
static _Atomic(azstem_live_observation_v1_fn) g_sd_live_observation;
static _Atomic(azstem_live_observer_mask_v1_fn) g_sd_live_mask;
static _Atomic(azstem_sync_observation_v1_fn) g_sd_sync_observation;
static _Atomic(azstem_tempo_lock_observation_v1_fn) g_sd_tempo_lock_observation;
static int cm_tempo_read(int d, double *bpm, uint64_t *generation, uint64_t *revision, uint64_t *ns) {
    (void) d; *bpm = 0; *generation = *revision = *ns = 0; return 0;
}
static struct { int ok; } g_sdk_sq;
static uint64_t sdk_command_capabilities(void) { return 0; }
#include DECK_DATA_INC
static unsigned calls;
static void *expected_self, *expected_id;
static int nested;
static void event(void *id);
static long original(void *self, void *id) {
    assert(self == expected_self && id == expected_id);
    ++calls;
    if (nested) {
        nested = 0;
        void *saved_id = expected_id;
        uint64_t newer = (UINT64_C(3) << 32) | 1;
        event(&newer);
        expected_id = saved_id;
    }
    errno = EDOM; return 12345;
}
static int install_result = -1, install_null;
static int test_install(uintptr_t target, void *hook, void **out) {
    assert(target == MASTER_EXCHANGED && hook == (void *) master_xm_hook);
    if (install_result > 0) *out = install_null ? NULL : (void *) original;
    return install_result;
}
static int supported(void) {
    az_data_capabilities c = {.size = sizeof c};
    assert(sdk_data_capabilities(&c) == AZ_OK && c.version == AZ_DATA_VERSION);
    return !!(c.live_fields & AZ_LIVE_MASTER);
}
static void event(void *id) {
    expected_self = &calls; expected_id = id;
    assert(master_xm_hook(expected_self, id) == 12345);
    assert(errno == EDOM);
    ++clock_ns;
}
static az_deck_snapshot snapshot(int deck) {
    az_deck_snapshot s = {.size = sizeof s};
    assert(sd_deck_snapshot_v1(deck, &s) == AZ_OK);
    assert(!(s.valid & (AZ_LIVE_PLAYBACK | AZ_LIVE_TRACK | AZ_LIVE_RATE | AZ_LIVE_DIRECTION)));
    assert(!s.load_generation && !s.live_unit_generation && !s.loop_unit_generation);
    return s;
}
static uint64_t concurrent_base;
static void *writer(void *unused) {
    (void) unused;
    for (unsigned i = 0; i < 10000; ++i) {
        uint64_t id = ((uint64_t) (i % 4) << 32) | 1;
        event(&id);
    }
    return NULL;
}
static void *reader(void *arg) {
    int deck = (int) (uintptr_t) arg;
    for (unsigned i = 0; i < 10000; ++i) {
        az_deck_snapshot s = snapshot(deck);
        if (s.valid & AZ_LIVE_MASTER) {
            assert(s.master_revision > concurrent_base && s.master_observed_ns);
            int owner = (s.master_revision - concurrent_base - 1) % 4;
            assert(s.master == (deck == owner));
        } else assert(s.master == -1 && !s.master_revision && !s.master_observed_ns);
    }
    return NULL;
}
int main(void) {
    g_orig_master_xm = original;
    assert(!(sd_live_fields() & AZ_LIVE_MASTER));
    for (int d = 0; d < 4; ++d) assert(!(snapshot(d).valid & AZ_LIVE_MASTER));
    install_master_hook();assert(!supported());
    install_result = 1;install_null = 1;install_master_hook();assert(!supported());
    install_null = 0;install_master_hook();
    puts("master native hook installed, before any receipt");fflush(stdout);
    assert(supported() && "registered master support must not require a current master receipt");
    install_result = -1;install_master_hook();assert(supported()); /* A refusal does not undo an existing hook. */
    for (int d = 0; d < 4; ++d) {
        az_deck_snapshot empty = snapshot(d);
        assert(!(empty.valid & AZ_LIVE_MASTER) && empty.master == -1 && !empty.master_revision && !empty.master_observed_ns);
    }
    for (int owner = 0; owner < 4; ++owner) {
        uint64_t id = ((uint64_t) owner << 32) | 1;
        event(&id);
        puts("native confirmed-master event must reach all four public snapshots");
        fflush(stdout);
        for (int d = 0; d < 4; ++d) {
            az_deck_snapshot s = snapshot(d);
            assert((s.valid & AZ_LIVE_MASTER) && s.master == (d == owner));
            assert(s.master_revision == (unsigned) owner + 1 && s.master_observed_ns == clock_ns - 1);
        }
        assert(sd_live_fields() & AZ_LIVE_MASTER);
    }
    const uint64_t bad[] = {0, 2, (UINT64_C(4) << 32) | 1, (UINT64_C(0xfffe) << 32) | 1};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        uint64_t id = bad[i]; event(&id);
        for (int d = 0; d < 4; ++d) {
            az_deck_snapshot s = snapshot(d);
            assert(!(s.valid & AZ_LIVE_MASTER) && s.master == -1);
        }
        assert(supported());
    }
    event(NULL);
    assert(!(snapshot(0).valid & AZ_LIVE_MASTER));
    assert(calls == 9);
    event((void *) 1); /* Refused address, never dereferenced. */
    assert(!(snapshot(0).valid & AZ_LIVE_MASTER));
    uint64_t id = 1; event(&id);
    g_st_enabled = 0;
    assert(!supported());
    assert(!(snapshot(0).valid & AZ_LIVE_MASTER));
    g_st_enabled = 1;
    assert(snapshot(0).valid & AZ_LIVE_MASTER);
    /* Public reads and the native publisher must not wait on each other. A
     * dropped event invalidates the older receipt until another confirmation. */
    int master = 99;
    uint64_t revision = 88, observed_ns = 77;
    assert(!master_state_read(NULL, &revision, &observed_ns));
    assert(master_state_read(&master, &revision, &observed_ns) && master == 0);
    pthread_mutex_lock(&g_master_state_mu);
    assert(!master_state_read(&master, &revision, &observed_ns));
    assert(master == 0 && revision == 11 && observed_ns == clock_ns - 1);
    id = (UINT64_C(3) << 32) | 1;
    event(&id);
    assert(supported());
    assert(!(snapshot(0).valid & AZ_LIVE_MASTER));
    pthread_mutex_unlock(&g_master_state_mu);
    assert(!(snapshot(0).valid & AZ_LIVE_MASTER));
    event(&id);
    assert(snapshot(3).valid & AZ_LIVE_MASTER);
    /* Failed/backwards clocks clear the public value, and the later clock must
     * advance past the last sampled boundary before the receipt can recover. */
    clock_ok = 0; event(&id);
    assert(!(snapshot(3).valid & AZ_LIVE_MASTER));
    clock_ok = 1;
    uint64_t saved_ns = clock_ns;
    clock_ns = 1; event(&id);
    assert(!(snapshot(3).valid & AZ_LIVE_MASTER));
    clock_ns = saved_ns; event(&id);
    assert(snapshot(3).valid & AZ_LIVE_MASTER);
    /* A later reentrant confirmation wins even when the older original returns
     * last. Its outer borrowed argument must not overwrite the newer master. */
    uint64_t before_nested = snapshot(3).master_revision;
    nested = 1; id = 1; event(&id);
    az_deck_snapshot newer = snapshot(3);
    assert((newer.valid & AZ_LIVE_MASTER) && newer.master == 1 &&
           newer.master_revision == before_nested + 2 && atomic_load(&g_master_deck) == 3);
    /* Older caller prefixes receive only their declared bytes. */
    const size_t prefixes[] = {offsetof(az_deck_snapshot, loop_unit_generation),
                              offsetof(az_deck_snapshot, master_revision), sizeof(az_deck_snapshot)};
    for (unsigned i = 0; i < sizeof prefixes / sizeof prefixes[0]; ++i) {
        _Alignas(az_deck_snapshot) unsigned char bytes[sizeof(az_deck_snapshot) + 32];
        memset(bytes, 0xa5, sizeof bytes);
        uint32_t size = prefixes[i]; memcpy(bytes, &size, sizeof size);
        assert(sd_deck_snapshot_v1(3, (az_deck_snapshot *) bytes) == AZ_OK);
        for (size_t p = size; p < sizeof bytes; ++p) assert(bytes[p] == 0xa5);
    }
    /* Readers may observe an entire earlier/newer event or unknown, never a
     * master from one confirmation with another confirmation's revision. */
    concurrent_base = atomic_load(&g_master_event);
    pthread_t publisher, readers[4];
    /* Invalidate the old receipt so its revision is outside this test interval. */
    g_master_state.deck = -1;
    assert(!pthread_create(&publisher, NULL, writer, NULL));
    for (int d = 0; d < 4; ++d) assert(!pthread_create(&readers[d], NULL, reader, (void *) (uintptr_t) d));
    assert(!pthread_join(publisher, NULL));
    for (int d = 0; d < 4; ++d) assert(!pthread_join(readers[d], NULL));
    atomic_store(&g_master_event, UINT64_MAX - 1);
    event(&id);
    assert(!(snapshot(3).valid & AZ_LIVE_MASTER));
    event(&id);
    assert(atomic_load(&g_master_event) == UINT64_MAX && supported());
    puts("master: confirmed event, all-deck public projection and unknown guards passed");
    return 0;
}
