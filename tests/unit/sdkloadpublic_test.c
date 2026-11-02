/* SPDX-License-Identifier: GPL-3.0-only */
/* Existing private load export -> actual public deck service. The native mode
 * links the real virtual-entry observer; standalone mode checks projections. */
#include "azmod-mod.h"
#include "sdk_analysis.h"
#include "../common/azdeckinfo.h"
#include "../common/azloadexport.h"
#include "../common/azcueexport.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define AZMOD_STEM_DECKS 4
static int __attribute__((unused)) master_state_supported(void) { return 0; }
static int g_st_enabled = 1;
static _Atomic(azstem_deck_snapshot_fn) g_sd_snap;
static _Atomic(azstem_deck_beat_phase_fn) g_sd_phase;
static _Atomic(azstem_deck_observation_v1_fn) g_sd_observation;
static _Atomic(azstem_loop_observation_v2_fn) g_sd_loop_observation;
static _Atomic(azstem_loop_observer_mask_v1_fn) g_sd_loop_mask;
static _Atomic(azstem_live_observation_v1_fn) g_sd_live_observation;
static _Atomic(azstem_live_observer_mask_v1_fn) g_sd_live_mask;
static _Atomic(azstem_sync_observation_v1_fn) g_sd_sync_observation;
static _Atomic(azstem_tempo_lock_observation_v1_fn) g_sd_tempo_lock_observation;
static _Atomic(azstem_load_observation_v1_fn) g_sd_load_observation;
static _Atomic(azstem_cue_observation_v1_fn) g_sd_cue_observation, g_sd_cue_take;
static _Atomic(azstem_cue_observer_mask_v1_fn) g_sd_cue_mask;
static int master_state_read(int *d, uint64_t *r, uint64_t *ns) {
    (void) d; (void) r; (void) ns; return 0;
}
static int cm_tempo_read(int d, double *bpm, uint64_t *g, uint64_t *r, uint64_t *ns) {
    (void) d; *bpm = 0; *g = *r = *ns = 0; return 0;
}
#define RTLD_DEFAULT ((void *) 0)
static void *sdk_test_dlsym(void *handle, const char *name) {
    assert(handle == RTLD_DEFAULT);
    return !strcmp(name, "azstem_load_observation_v1") ? (void *) azstem_load_observation_v1 : NULL;
}
#define dlsym sdk_test_dlsym
#include DECK_DATA_INC
#undef dlsym
void sdk_test_public_load(int deck, const struct azsdk_load_sample *expected) {
    sd_bind();
    az_deck_snapshot s = {.size = sizeof s};
    assert(sd_deck_snapshot_v1(deck, &s) == AZ_OK);
    assert(!(s.valid & (AZ_LIVE_TRACK | AZ_LIVE_PLAYBACK | AZ_LIVE_POSITION | AZ_LIVE_RATE | AZ_LIVE_DIRECTION)));
    assert(!s.track.source_id && !s.track.track_id && !s.live_unit_generation && !s.loop_unit_generation);
    /* A commit receipt alone cannot satisfy a source-qualified command target. */
    assert(!s.load_generation);
    if (expected) {
        assert(s.valid & AZ_LIVE_NATIVE_LOAD);
        assert(s.native_load_generation == expected->load_generation);
        assert(s.native_load_unit_generation == expected->unit_generation &&
               s.native_load_invalidation_epoch == expected->invalidation_epoch &&
               s.native_load_observed_ns == expected->observed_ns);
    } else assert(!(s.valid & AZ_LIVE_NATIVE_LOAD) && !s.native_load_generation &&
                  !s.native_load_unit_generation && !s.native_load_invalidation_epoch && !s.native_load_observed_ns);
    const size_t prefixes[] = {offsetof(az_deck_snapshot, loop_unit_generation),
                              offsetof(az_deck_snapshot, native_load_generation), sizeof s};
    for (unsigned i = 0; i < sizeof prefixes / sizeof prefixes[0]; ++i) {
        _Alignas(az_deck_snapshot) unsigned char bytes[sizeof s + 16];
        memset(bytes, 0xa5, sizeof bytes);
        uint32_t size = prefixes[i]; memcpy(bytes, &size, sizeof size);
        assert(sd_deck_snapshot_v1(deck, (az_deck_snapshot *) bytes) == AZ_OK);
        for (size_t p = size; p < sizeof bytes; ++p) assert(bytes[p] == 0xa5);
    }
}
void sdk_test_public_load_optout(int deck) {
    g_st_enabled = 0;
    sdk_test_public_load(deck, NULL);
    assert(!(sd_live_fields() & AZ_LIVE_NATIVE_LOAD));
    g_st_enabled = 1;
}
#ifdef SDK_LOAD_PROJECTION_STANDALONE
static struct azsdk_load_sample samples[4];
static unsigned ready;
int azsdk_load_read(int deck, struct azsdk_load_sample *out) {
    if (!(ready & (1u << deck))) return 0;
    *out = samples[deck]; return 1;
}
static int loaded(int deck, struct azstem_deck_observation_v1 *out) {
    (void) deck; out->loaded = 1; out->revision = 91; out->observed_ns = 92; return 0;
}
int main(void) {
    atomic_store(&g_sd_observation, loaded);
    for (int d = 0; d < 4; ++d) {
        sdk_test_public_load(d, NULL);
        samples[d] = (struct azsdk_load_sample) {100u + d, 200u + d, 300u + d, 400u + d, 0, 0};
        ready |= 1u << d;
        sdk_test_public_load(d, &samples[d]);
        az_deck_snapshot s = {.size = sizeof s};
        assert(sd_deck_snapshot_v1(d, &s) == AZ_OK && (s.valid & AZ_LIVE_LOADED) &&
               (s.valid & AZ_LIVE_NATIVE_LOAD) && s.loaded == 1 && s.revision == 91 && s.observed_ns == 92);
        sdk_test_public_load_optout(d);
        const struct azsdk_load_sample good = samples[d];
        for (int which = 0; which < 5; ++which) {
            samples[d] = good;
            if (which == 0) samples[d].unit_generation = 0;
            if (which == 1) samples[d].load_generation = 0;
            if (which == 2) samples[d].invalidation_epoch = 0;
            if (which == 3) samples[d].observed_ns = 0;
            if (which == 4) samples[d].reserved = 1;
            sdk_test_public_load(d, NULL);
        }
        samples[d] = good;
    }
    ready = 0;
    assert(!(sd_live_fields() & AZ_LIVE_NATIVE_LOAD));
    puts("public load: independent fields, malformed/opt-out guards and four-deck projection passed");
    return 0;
}
#endif
