/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual BpmState lease -> GUI receipt -> public snapshot, without native firmware. */
#define _POSIX_C_SOURCE 200809L
#include "azmod-mod.h"
#include "sdk_analysis.h"
#include "sdk_logic.h"
#include "../common/azdeckinfo.h"
#include "../common/azloadexport.h"
#include "../common/azcueexport.h"
#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define AZMOD_STEM_DECKS 4
#define AZMOD_PAD_DECKS 2
#define BPMSTATE_LIVE 0xa0
#include BPM_INC
static uint64_t clock_ns = 1000000000;
static int __attribute__((unused)) master_state_supported(void) { return 0; }
static int clock_ok = 1;
static int fake_clock_gettime(clockid_t clock, struct timespec *out) {
    assert(clock == CLOCK_MONOTONIC);
    out->tv_sec = (time_t) (clock_ns / 1000000000);
    out->tv_nsec = (long) (clock_ns % 1000000000);
    return clock_ok ? 0 : -1;
}
#define clock_gettime fake_clock_gettime
#include TEMPO_INC
#undef clock_gettime
static int g_st_enabled = 1;
static _Atomic(azstem_deck_snapshot_fn) g_sd_snap;
static _Atomic int g_sd_playing[4];
static int dg_slip_enum = 2;
static int pad_mode_fresh(int side, int deck) { (void) side; (void) deck; return -1; }
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
static int master_state_read(int *deck, uint64_t *revision, uint64_t *observed_ns) {
    (void) deck; (void) revision; (void) observed_ns; return 0;
}
#include DECK_DATA_INC
struct block { _Alignas(16) unsigned char bytes[0xd0]; };
static struct block *fixture_blocks[32];
static unsigned fixture_count;
static unsigned disposed;
static void dispose(void *p) { (void) p; disposed++; }
static void destroy(void *p) { (void) p; abort(); }
static void (*vtable[5])(void *) = {NULL, NULL, dispose, destroy, NULL};
static int *use(struct block *b) { return (int *) (b->bytes + BPMCTRL_USE); }
static void value(struct block *b, int offset, double v) {
    uint64_t bits; memcpy(&bits, &v, sizeof bits);
    __atomic_store_n((uint64_t *) (b->bytes + BPMCTRL_OFF + offset), bits, __ATOMIC_RELAXED);
}
static struct block *make(int deck, double bpm) {
    struct block *b = calloc(1, sizeof *b); assert(b);
    assert(fixture_count < sizeof fixture_blocks / sizeof *fixture_blocks);
    fixture_blocks[fixture_count++] = b;
    *(uintptr_t *) b->bytes = (uintptr_t) vtable;
    *use(b) = 1;
    *(int *) (b->bytes + BPMCTRL_WEAK) = 1;
    *(unsigned long *) (b->bytes + BPMCTRL_OFF + BPMSTATE_DECK) = 0xabc000u | (unsigned) deck;
    value(b, BPMSTATE_LIVE, bpm);
    value(b, BPMSTATE_TRACK, 100.0);
    assert(bpmstate_capture((unsigned long) (b->bytes + BPMCTRL_OFF), deck, (uintptr_t) vtable));
    return b;
}
static az_deck_snapshot snapshot(int deck) {
    az_deck_snapshot s = {.size = sizeof s};
    assert(sd_deck_snapshot_v1(deck, &s) == AZ_OK);
    assert(!(s.valid & (AZ_LIVE_RATE | AZ_LIVE_DIRECTION | AZ_LIVE_TRACK | AZ_LIVE_PLAYBACK)) && !s.load_generation);
    assert(!s.revision && !s.observed_ns && !s.live_unit_generation && !s.loop_unit_generation);
    return s;
}
static double legacy_tempo(int deck) {
    az_deck_info out = {.size = sizeof out};
    assert(sd_deck_info(deck, &out) == 0);
    return out.bpm;
}
static struct block *concurrent_blocks[4];
static uint64_t concurrent_base, concurrent_generation[4];
static void *publish_tempo(void *unused) {
    (void) unused;
    for (unsigned i = 1; i <= 256; ++i) {
        for (int d = 0; d < 4; ++d) value(concurrent_blocks[d], BPMSTATE_LIVE, 120.0 + i % 120 + d * 0.125);
        ++clock_ns;cm_tempo_poll();
    }
    return NULL;
}
static void *read_tempo(void *arg) {
    int deck = (int) (uintptr_t) arg;
    for (unsigned i = 0; i < 512; ++i) {
        az_deck_snapshot s = snapshot(deck);
        if (s.valid & AZ_LIVE_TEMPO) {
            assert(s.tempo_state_generation == concurrent_generation[deck] && s.tempo_observed_ns >= concurrent_base);
            double expected = 120.0 + (s.tempo_observed_ns - concurrent_base) % 120 + deck * 0.125;
            assert(s.tempo == expected);
        } else assert(!s.tempo && !s.tempo_state_generation);
        double legacy = legacy_tempo(deck);
        assert(legacy == 0.0 || (legacy >= 120.0 && legacy < 240.0));
    }
    return NULL;
}
int main(void) {
    struct block *blocks[4];
    for (int d = 0; d < 4; ++d) blocks[d] = make(d, 123.45 + d);
    cm_tempo_poll();
    assert(sd_live_fields() == AZ_LIVE_TEMPO);
    for (int d = 0; d < 4; ++d) {
        az_deck_snapshot s = snapshot(d);
        assert(s.valid == AZ_LIVE_TEMPO && s.tempo == 123.45 + d && s.tempo != 100.0);
        assert(s.tempo_revision == 1 && s.tempo_observed_ns == clock_ns && s.tempo_state_generation == (unsigned) d + 1);
        assert(*use(blocks[d]) == 1);
        assert(legacy_tempo(d) == s.tempo);
    }
    /* Public admission may inspect only retained control/generation metadata;
     * it must not wait for the native publisher or change native ownership. */
    assert(!pthread_mutex_lock(&g_bpmstate_mu));
    for (int d = 0; d < 4; ++d) assert(!(snapshot(d).valid & AZ_LIVE_TEMPO) && *use(blocks[d]) == 1);
    assert(!pthread_mutex_unlock(&g_bpmstate_mu));
    for (int d = 0; d < 4; ++d) assert((snapshot(d).valid & AZ_LIVE_TEMPO) && *use(blocks[d]) == 1 && !disposed);
    /* Malformed/native unavailable clears only this deck; no x100 conversion or track BPM. */
    const double bad[] = {NAN, INFINITY, DBL_MAX, -1, 0, 20, 400};
    for (int d = 0; d < 4; ++d) {
        for (unsigned n = 0; n < sizeof bad / sizeof bad[0]; ++n) {
            value(blocks[d], BPMSTATE_LIVE, bad[n]); ++clock_ns; cm_tempo_poll();
            az_deck_snapshot s = snapshot(d);
            assert(!(s.valid & AZ_LIVE_TEMPO) && !s.tempo && s.tempo_observed_ns == clock_ns);
        }
        value(blocks[d], BPMSTATE_LIVE, 123.45 + d); ++clock_ns; cm_tempo_poll();
        uint64_t before = snapshot(d).tempo_state_generation;
        struct block *old = blocks[d]; blocks[d] = make(d, 150.25 + d);
        value(old, BPMSTATE_LIVE, 399.0);
        puts("native BPM owner replaced before the next GUI poll");fflush(stdout);
        az_deck_snapshot between = snapshot(d);
        assert(!(between.valid & AZ_LIVE_TEMPO) && "replaced native BPM owner must invalidate its public receipt before poll");
        assert(!between.tempo && !between.tempo_state_generation && !between.tempo_observed_ns);
        assert(legacy_tempo(d) == 0.0 && "legacy deck_info must not bypass native tempo receipt admission");
        ++clock_ns; cm_tempo_poll();
        az_deck_snapshot s = snapshot(d);
        assert(s.tempo == 150.25 + d && s.tempo_state_generation > before && s.tempo_observed_ns == clock_ns);
        *use(blocks[d]) = INT_MAX; ++clock_ns; cm_tempo_poll();
        assert(!(snapshot(d).valid & AZ_LIVE_TEMPO) && *use(blocks[d]) == INT_MAX);
        *use(blocks[d]) = 1; ++clock_ns; cm_tempo_poll();
        assert(snapshot(d).tempo == 150.25 + d);
    }
    for (int d = 0; d < 4; ++d) {
        az_deck_snapshot acquired = snapshot(d);
        assert(acquired.valid & AZ_LIVE_TEMPO);
        assert(!pthread_mutex_lock(&g_cm_bpm_mu));
        *use(blocks[d]) = 0;
        double sampled_bpm = 123.0;uint64_t sampled_generation = 99;
        int sampled = bpmstate_sample(d, &sampled_bpm, NULL, &sampled_generation);
        assert(!sampled && !sampled_bpm && !sampled_generation);
        cm_tempo_publish(d, sampled, sampled_bpm, sampled_generation, clock_ns + 1);
        assert(!pthread_mutex_unlock(&g_cm_bpm_mu));
        puts("native BPM lease unavailable; GUI publication deliberately contended");fflush(stdout);
        az_deck_snapshot retired = snapshot(d);
        assert(!(retired.valid & AZ_LIVE_TEMPO) && "dropped native BPM invalidation must not resurrect the old public receipt");
        assert(!retired.tempo && !retired.tempo_state_generation && !retired.tempo_observed_ns);
        assert(legacy_tempo(d) == 0.0);
        assert((acquired.valid & AZ_LIVE_TEMPO) && acquired.tempo == 150.25 + d);
        for (int other = 0; other < 4; ++other) if (other != d) assert(snapshot(other).valid & AZ_LIVE_TEMPO);
        *use(blocks[d]) = 1;clock_ns += 2;cm_tempo_poll();assert(snapshot(d).valid & AZ_LIVE_TEMPO);
    }
    /* Public calls remain bounded and return unknown on publication contention. */
    assert(pthread_mutex_lock(&g_cm_bpm_mu) == 0);
    az_deck_snapshot busy = snapshot(0);
    assert(!(busy.valid & AZ_LIVE_TEMPO) && !busy.tempo_revision && !busy.tempo_state_generation);
    assert(!(sd_live_fields() & AZ_LIVE_TEMPO));
    double previous_bpm = g_cm_tempo[0].bpm;
    uint64_t previous_revision = g_cm_tempo[0].revision;
    puts("BPM GUI publication must not wait on a busy receipt");
    fflush(stdout);
    cm_tempo_publish(0, 1, 222.0, 123, clock_ns + 1);
    assert(g_cm_tempo[0].bpm == previous_bpm && g_cm_tempo[0].revision == previous_revision);
    pthread_mutex_unlock(&g_cm_bpm_mu);
    assert(!(snapshot(0).valid & AZ_LIVE_TEMPO) && !snapshot(0).tempo_revision);
    clock_ok = 0; cm_tempo_poll();
    assert(!(snapshot(0).valid & AZ_LIVE_TEMPO) && !snapshot(0).tempo_observed_ns);
    clock_ok = 1; --clock_ns; cm_tempo_poll();
    assert(!(snapshot(0).valid & AZ_LIVE_TEMPO));
    clock_ns += 2; cm_tempo_poll();
    assert(snapshot(0).valid & AZ_LIVE_TEMPO);
    for (int d = 0; d < 4; ++d) {
        concurrent_blocks[d] = blocks[d];
        value(blocks[d], BPMSTATE_LIVE, 120.0 + d * 0.125);
    }
    concurrent_base = ++clock_ns;cm_tempo_poll();
    for (int d = 0; d < 4; ++d) concurrent_generation[d] = snapshot(d).tempo_state_generation;
    pthread_t publisher, readers[4];
    assert(!pthread_create(&publisher, NULL, publish_tempo, NULL));
    for (int d = 0; d < 4; ++d) assert(!pthread_create(&readers[d], NULL, read_tempo, (void *) (uintptr_t) d));
    assert(!pthread_join(publisher, NULL));
    for (int d = 0; d < 4; ++d) {assert(!pthread_join(readers[d], NULL));assert(*use(blocks[d]) == 1);}
    ++clock_ns;cm_tempo_poll();
    atomic_store(&g_cm_tempo_event[0], UINT64_MAX - 1); ++clock_ns; cm_tempo_poll();
    assert(!(snapshot(0).valid & AZ_LIVE_TEMPO) && !snapshot(0).tempo_revision);
    ++clock_ns;cm_tempo_poll();assert(!(snapshot(0).valid & AZ_LIVE_TEMPO));
    g_st_enabled = 0;
    assert(!snapshot(1).valid && !snapshot(1).tempo_revision && !sd_live_fields());
    g_st_enabled = 1;
    assert(snapshot(1).valid & AZ_LIVE_TEMPO);
    /* Native retirement cannot be promoted and cannot keep an old public value. */
    *use(blocks[1]) = 0;
    assert(!(snapshot(1).valid & AZ_LIVE_TEMPO) && !snapshot(1).tempo_state_generation);
    ++clock_ns; cm_tempo_poll();
    assert(!(snapshot(1).valid & AZ_LIVE_TEMPO) && !snapshot(1).tempo_state_generation);
    puts("BPM tempo: four-deck native lease, independent GUI receipts, public opt-in and refusal guards passed");
    memset(g_bpmstate, 0, sizeof g_bpmstate);
    memset(g_bpmstate_generation, 0, sizeof g_bpmstate_generation);
    for (unsigned i = 0; i < fixture_count; ++i) free(fixture_blocks[i]);
    return 0;
}
