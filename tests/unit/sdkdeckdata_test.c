/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual new deck-service functions, with independent loaded and native loop streams. */
#include "azmod-mod.h"
#include "sdk_analysis.h"
#include "../common/azdeckinfo.h"
#include "../common/azloadexport.h"
#include "../common/azcueexport.h"
#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#define AZMOD_STEM_DECKS 4
static int __attribute__((unused)) master_state_supported(void) { return 0; }
static int g_st_enabled = 1, loop_ready, bounds_ready = 1, engaged = 1;
static unsigned registered_mask;
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
static int tempo_lock_ready;
static struct azstem_tempo_lock_observation_v1 tempo_lock_samples[4];
static int tempo_lock_observation(int deck, struct azstem_tempo_lock_observation_v1 *out) {
    assert(deck >= 0 && deck < 4 && out->size == sizeof *out && out->version == 1);
    if (!tempo_lock_ready) return -1;
    *out = tempo_lock_samples[deck];
    return 0;
}
static int sync_ready;
static struct azstem_sync_observation_v1 sync_sample = {sizeof sync_sample, 1, 31, 70, 700, 2, 1};
static int sync_observation(int deck, struct azstem_sync_observation_v1 *out) {
    assert(deck >= 0 && deck < 4 && out->size == sizeof *out && out->version == 1);
    if (!sync_ready) return -1;
    *out = sync_sample;
    return 0;
}
static int live_ready;
static unsigned live_registered_mask;
static struct azsdk_live_sample live_sample = {
    .unit_generation = 30, .observed_fields = 7, .valid_fields = 7, .mode_is_2 = 1, .play_pause = 0, .play_aux = 1, .quantize = 0, .key_lock = 1, .revisions = {40, 50, 60}, .observed_ns = {400, 500, 600}};
static int live(int deck, struct azstem_live_observation_v1 *out) {
    (void) deck;
    if (!live_ready) return -1;
    assert(out->size == sizeof *out && out->version == 1);
    out->sample = live_sample;
    return 0;
}
static unsigned live_mask(void) { return live_registered_mask; }
static int loaded(int deck, struct azstem_deck_observation_v1 *out) {
    (void) deck;
    out->loaded = 1;
    out->revision = 10;
    out->observed_ns = 100;
    return 0;
}
static int loop(int deck, struct azstem_loop_observation_v2 *out) {
    (void) deck;
    if (!loop_ready) return -1;
    assert(out->size == sizeof *out && out->version == 2);
    out->engaged = engaged;
    out->bounds_valid = bounds_ready;
    out->in_s = 0.0;
    out->out_s = 2.5;
    out->unit_generation = 20;
    out->revision = 3;
    out->observed_ns = 200;
    return 0;
}
static unsigned mask(void) { return registered_mask; }
static int tempo_ready;
static int __attribute__((unused)) master_state_read(int *deck, uint64_t *revision, uint64_t *observed_ns) {
    (void) deck; (void) revision; (void) observed_ns; return 0;
}
static double tempo_values[4] = {123.45, 97.25, 141.75, 128.125};
static uint64_t tempo_generations[4] = {401, 402, 403, 404};
static uint64_t tempo_revisions[4] = {501, 502, 503, 504};
static uint64_t tempo_times[4] = {601, 602, 603, 604};
/* Public projection uses a copied GUI receipt; no native object is exposed. */
static int __attribute__((unused)) cm_tempo_read(int deck, double *bpm, uint64_t *generation,
                                               uint64_t *revision, uint64_t *observed_ns) {
    *bpm = tempo_ready ? tempo_values[deck] : 0.0;
    *generation = tempo_ready ? tempo_generations[deck] : 0;
    *revision = tempo_ready ? tempo_revisions[deck] : 0;
    *observed_ns = tempo_ready ? tempo_times[deck] : 0;
    return tempo_ready;
}
#include DECK_DATA_INC
static void tempo_projection(void) {
    tempo_ready = 1;
    assert(sd_live_fields() & AZ_LIVE_TEMPO);
    for (int deck = 0; deck < 4; ++deck) {
        az_deck_snapshot out = {.size = sizeof out};
        assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
        assert((out.valid & AZ_LIVE_TEMPO) && out.tempo == tempo_values[deck]);
        assert(out.tempo_state_generation == tempo_generations[deck] && out.tempo_revision == tempo_revisions[deck] && out.tempo_observed_ns == tempo_times[deck]);
        assert(!(out.valid & (AZ_LIVE_RATE | AZ_LIVE_DIRECTION | AZ_LIVE_TRACK | AZ_LIVE_PLAYBACK)));
        assert(!out.load_generation && !out.live_unit_generation);
        double original = tempo_values[deck];
        const double malformed[] = {NAN, INFINITY, DBL_MAX, 0, -1, 20, 400};
        for (unsigned n = 0; n < sizeof malformed / sizeof malformed[0]; ++n) {
            tempo_values[deck] = malformed[n];
            assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
            assert(!(out.valid & AZ_LIVE_TEMPO) && out.tempo == 0.0);
        }
        tempo_values[deck] = original;
        tempo_times[deck] = 0;
        assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK && !(out.valid & AZ_LIVE_TEMPO) && out.tempo == 0.0);
        tempo_times[deck] = 601u + (unsigned) deck;
    }
    g_st_enabled = 0;
    az_deck_snapshot disabled = {.size = sizeof disabled};
    assert(sd_deck_snapshot_v1(0, &disabled) == AZ_OK && !(disabled.valid & AZ_LIVE_TEMPO) && !disabled.tempo_revision);
    assert(!(sd_live_fields() & AZ_LIVE_TEMPO));
    g_st_enabled = 1;
    tempo_ready = 0;
}
static void master_tempo_projection(void) {
    /* Both firmware profiles' MasterTempoSetter consumes the same KeyLock
     * scalar as the existing reader. No playback inference or load join. */
    atomic_store(&g_sd_live_observation, live);
    atomic_store(&g_sd_live_mask, live_mask);
    live_registered_mask = 15;
    live_ready = 1;
    assert(sd_live_fields() & AZ_LIVE_MASTER_TEMPO);
    for (int deck = 0; deck < 4; ++deck) {
        for (int value = 0; value <= 1; ++value) {
            live_sample.valid_fields = AZSDK_LIVE_KEY_LOCK;
            live_sample.key_lock = value;
            az_deck_snapshot out = {.size = sizeof out};
            assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
            assert((out.valid & AZ_LIVE_MASTER_TEMPO) && out.master_tempo == value);
            assert((out.valid & AZ_LIVE_KEY_LOCK) && out.key_lock == value);
            assert(out.key_lock_revision == 60 && out.key_lock_observed_ns == 600 && out.live_unit_generation == 30);
            assert(!(out.valid & (AZ_LIVE_PLAYBACK | AZ_LIVE_TRACK)) && !out.load_generation);
        }
        live_sample.valid_fields = 0;
        az_deck_snapshot out = {.size = sizeof out};
        assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
        assert(!(out.valid & (AZ_LIVE_KEY_LOCK | AZ_LIVE_MASTER_TEMPO)) && out.master_tempo == -1 && out.key_lock == -1);
        live_sample.valid_fields = AZSDK_LIVE_KEY_LOCK;
        live_sample.key_lock = 2;
        assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
        assert(!(out.valid & (AZ_LIVE_KEY_LOCK | AZ_LIVE_MASTER_TEMPO)) && out.master_tempo == -1 && out.key_lock == -1);
    }
    atomic_store(&g_sd_live_observation, NULL);
    atomic_store(&g_sd_live_mask, NULL);
    live_registered_mask = 0;
    live_ready = 0;
    live_sample.valid_fields = 7;
    live_sample.key_lock = 1;
}
static void sync_projection(void) {
    atomic_store(&g_sd_sync_observation, sync_observation);
    atomic_store(&g_sd_live_mask, live_mask);
    live_registered_mask = 15;
    assert(sd_live_fields() & AZ_LIVE_SYNC);
    for (int deck = 0; deck < 4; ++deck) {
        az_deck_snapshot out = {.size = sizeof out};
        sync_ready = 0;
        assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK && !(out.valid & AZ_LIVE_SYNC) && out.sync == -1 && out.sync_raw_mode == -1);
        sync_ready = 1;
        for (int mode = 0; mode <= 2; ++mode) {
            sync_sample.raw_mode = mode;
            sync_sample.valid = mode != 1;
            assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
            assert(!!(out.valid & AZ_LIVE_SYNC) == (mode != 1));
            assert(out.sync == (mode == 0 ? 0 : mode == 2 ? 1
                                                          : -1));
            assert(out.sync_raw_mode == mode && out.sync_unit_generation == 31 && out.sync_revision == 70 && out.sync_observed_ns == 700);
            assert(!out.live_unit_generation && !out.load_generation && !(out.valid & AZ_LIVE_TRACK));
        }
        sync_sample.raw_mode = 3;
        sync_sample.valid = 1;
        assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK && !(out.valid & AZ_LIVE_SYNC) && out.sync == -1);
        sync_sample.raw_mode = 2;
        sync_sample.valid = 0;
        assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK && !(out.valid & AZ_LIVE_SYNC) && out.sync == -1);
    }
    atomic_store(&g_sd_sync_observation, NULL);
    atomic_store(&g_sd_live_mask, NULL);
    live_registered_mask = sync_ready = 0;
}
static void tempo_lock_projection(void) {
    atomic_store(&g_sd_tempo_lock_observation, tempo_lock_observation);
    atomic_store(&g_sd_live_mask, live_mask);
    live_registered_mask = 15;
    assert(sd_live_fields() & AZ_LIVE_TEMPO_SLIDER_LOCKED);
    for (int deck = 0; deck < 4; ++deck) {
        az_deck_snapshot out = {.size = sizeof out};
        tempo_lock_ready = 0;
        assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
        assert(!(out.valid & AZ_LIVE_TEMPO_SLIDER_LOCKED) && out.tempo_slider_locked == -1);
        tempo_lock_ready = 1;
        for (int value = 0; value <= 2; ++value) {
            tempo_lock_samples[deck] = (struct azstem_tempo_lock_observation_v1){sizeof out, 1,
                100u + deck, 200u + deck, 300u + deck, value, 1};
            assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
            assert(!!(out.valid & AZ_LIVE_TEMPO_SLIDER_LOCKED) == (value <= 1));
            assert(out.tempo_slider_locked == (value <= 1 ? value : -1));
            assert(out.tempo_lock_unit_generation == 100u + deck && out.tempo_lock_revision == 200u + deck && out.tempo_lock_observed_ns == 300u + deck);
            assert(!out.load_generation && !(out.valid & (AZ_LIVE_TRACK | AZ_LIVE_RATE)));
        }
    }
    atomic_store(&g_sd_tempo_lock_observation, NULL);
    atomic_store(&g_sd_live_mask, NULL);
    live_registered_mask = tempo_lock_ready = 0;
}
int main(void) {
    tempo_projection();
    tempo_lock_projection();
    sync_projection();
    master_tempo_projection();
    uint32_t *prefix = malloc(sizeof *prefix);
    assert(prefix);
    *prefix = sizeof *prefix;
    assert(sd_deck_snapshot_v1(0, (az_deck_snapshot *) prefix) == AZ_INVALID && *prefix == sizeof *prefix);
    free(prefix);


    assert(!sd_live_fields());
    atomic_store(&g_sd_observation, loaded);
    assert(sd_live_fields() == AZ_LIVE_LOADED);
    atomic_store(&g_sd_loop_observation, loop);
    atomic_store(&g_sd_loop_mask, mask);
    assert(sd_live_fields() == AZ_LIVE_LOADED);
    registered_mask = 15;
    assert(sd_live_fields() == (AZ_LIVE_LOADED | AZ_LIVE_LOOP_ENGAGED | AZ_LIVE_LOOP_BOUNDS));
    az_deck_snapshot d = {.size = sizeof d};
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK && d.loaded == 1 && d.valid == AZ_LIVE_LOADED);
    assert(!d.loop.valid && d.loop.engaged == -1 && !d.load_generation);
    loop_ready = 1;
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK);
    assert(d.valid == (AZ_LIVE_LOADED | AZ_LIVE_LOOP_ENGAGED | AZ_LIVE_LOOP_BOUNDS));
    assert(d.loop.valid == (AZ_LOOP_ENGAGED | AZ_LOOP_BOUNDS) && d.loop.engaged == 1 && d.loop.defined == -1 && d.loop.reloop_available == -1);
    assert(d.revision == 10 && d.observed_ns == 100 && d.loop_revision == 3 && d.loop_observed_ns == 200);
    assert(d.loop_unit_generation == 20 && !d.load_generation && !d.track.source_id && !d.track.track_id);
    assert(d.loop.in_s == 0.0 && d.loop.out_s == 2.5);
    /* EXIT does not establish retained/reloop state, and stale bounds must disappear. */
    engaged = bounds_ready = 0;
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK);
    assert(d.valid == (AZ_LIVE_LOADED | AZ_LIVE_LOOP_ENGAGED));
    assert(d.loop.valid == AZ_LOOP_ENGAGED && !d.loop.engaged && d.loop.defined == -1 && d.loop.reloop_available == -1);
    assert(d.loop.in_s == 0.0 && d.loop.out_s == 0.0);
    engaged = 1;
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK && d.loop.engaged == 1 && d.loop.valid == AZ_LOOP_ENGAGED);
    bounds_ready = 1;
    /* A mixed library without v2 has no trusted loop fields, even with registered units. */
    atomic_store(&g_sd_loop_observation, NULL);
    assert(sd_live_fields() == AZ_LIVE_LOADED);
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK && d.valid == AZ_LIVE_LOADED && d.loop.engaged == -1);
    atomic_store(&g_sd_loop_observation, loop);
    /* The earlier development prefix and newer caller buffers must keep their guards. */
    _Alignas(az_deck_snapshot) unsigned char bytes[sizeof d + 16];
    for (unsigned n = offsetof(az_deck_snapshot, loop_unit_generation); n <= sizeof d + 8; n++) {
        memset(bytes, 0xa5, sizeof bytes);
        az_deck_snapshot *v = (az_deck_snapshot *) bytes;
        v->size = n;
        assert(sd_deck_snapshot_v1(3, v) == AZ_OK && v->size == n && v->loop.engaged == 1);
        for (size_t i = n < sizeof d ? n : sizeof d; i < sizeof bytes; i++) assert(bytes[i] == 0xa5);
    }
    size_t earlier_size = offsetof(az_deck_snapshot, loop_unit_generation);
    void *earlier = malloc(earlier_size);
    assert(earlier);
    uint32_t declared = (uint32_t) earlier_size;
    memcpy(earlier, &declared, sizeof declared);
    assert(sd_deck_snapshot_v1(0, earlier) == AZ_OK);
    memcpy(&declared, earlier, sizeof declared);
    assert(declared == earlier_size);
    free(earlier);
    memset(bytes, 0xa5, sizeof bytes);
    ((az_deck_snapshot *) bytes)->size = offsetof(az_deck_snapshot, loop_unit_generation) - 1;
    unsigned char before[sizeof bytes];
    memcpy(before, bytes, sizeof bytes);
    assert(sd_deck_snapshot_v1(0, (az_deck_snapshot *) bytes) == AZ_INVALID && !memcmp(before, bytes, sizeof bytes));
    assert(sd_deck_snapshot_v1(-1, &d) == AZ_INVALID && sd_deck_snapshot_v1(4, &d) == AZ_INVALID);
    atomic_store(&g_sd_observation, NULL);
    atomic_store(&g_sd_loop_observation, NULL);
    atomic_store(&g_sd_live_observation, live);
    atomic_store(&g_sd_live_mask, live_mask);
    assert(!sd_live_fields());
    live_registered_mask = 1;
    const uint32_t raw = AZ_LIVE_PLAY_FLAGS | AZ_LIVE_QUANTIZE | AZ_LIVE_KEY_LOCK | AZ_LIVE_MASTER_TEMPO;
    assert(sd_live_fields() == raw && !(sd_live_fields() & AZ_LIVE_PLAYBACK));
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK && !d.valid && !d.live_unit_generation);
    assert(d.play_mode_is_2 == -1 && d.play_pause == -1 && d.play_aux == -1 && d.quantize == -1 && d.key_lock == -1);
    live_ready = 1;
    for (unsigned fields = 0; fields < 8; ++fields) {
        live_sample.valid_fields = fields;
        assert(sd_deck_snapshot_v1(0, &d) == AZ_OK);
        uint32_t expected = (fields & 1 ? AZ_LIVE_PLAY_FLAGS : 0) |
                            (fields & 2 ? AZ_LIVE_QUANTIZE : 0) | (fields & 4 ? AZ_LIVE_KEY_LOCK | AZ_LIVE_MASTER_TEMPO : 0);
        assert(d.valid == expected && !d.load_generation && !d.track.source_id && !d.track.track_id);
        assert(d.live_unit_generation == 30 && d.play_revision == 40 && d.play_observed_ns == 400);
        assert(d.quantize_revision == 50 && d.quantize_observed_ns == 500 && d.key_lock_revision == 60 && d.key_lock_observed_ns == 600);
        assert(!d.revision && !d.observed_ns && !d.loop_revision && !d.loop_observed_ns && !d.loop_unit_generation);
        assert(d.play_mode_is_2 == (fields & 1 ? 1 : -1) && d.play_pause == (fields & 1 ? 0 : -1) && d.play_aux == (fields & 1 ? 1 : -1));
        assert(d.quantize == (fields & 2 ? 0 : -1) && d.key_lock == (fields & 4 ? 1 : -1));
        assert(d.master_tempo == (fields & 4 ? 1 : -1));
        assert(d.playback == AZ_PLAYBACK_UNKNOWN && !(d.valid & AZ_LIVE_PLAYBACK));
    }
    live_sample.valid_fields = 7;
    live_sample.mode_is_2 = 0;
    live_sample.play_pause = 1;
    live_sample.play_aux = 0;
    live_sample.quantize = 1;
    live_sample.key_lock = 0;
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK && d.valid == raw && !d.play_mode_is_2 && d.play_pause == 1 && !d.play_aux && d.quantize == 1 && !d.key_lock);
    live_ready = 0;
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK && !d.valid && !d.live_unit_generation && !d.play_revision && !d.quantize_revision && !d.key_lock_revision);
    assert(d.play_mode_is_2 == -1 && d.play_pause == -1 && d.play_aux == -1 && d.quantize == -1 && d.key_lock == -1);
    atomic_store(&g_sd_live_observation, NULL);
    assert(!sd_live_fields());
    atomic_store(&g_sd_live_observation, live);
    live_ready = 1;
    g_st_enabled = 0;
    assert(sd_deck_snapshot_v1(0, &d) == AZ_OK && !d.valid && d.loaded == -1 && !d.loop.valid && d.loop.engaged == -1);
    assert(!d.live_unit_generation && !d.play_revision && !d.quantize_revision && !d.key_lock_revision && d.quantize == -1 && d.key_lock == -1);
    puts("sdk deck data: independent streams, unavailable load identity and compatible output sizes passed");
    return 0;
}
