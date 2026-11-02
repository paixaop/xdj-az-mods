/* SPDX-License-Identifier: GPL-3.0-only */
/* sdk_export.c: per-deck state for the community mod services (see ../common/azdeckinfo.h).
 * #included last by azstem.c: it reads the track copy (deck_track_map.c), the observed
 * playhead (key_shift.c), the stems state (g_dk) and the beat grid (drumroll.c). Everything
 * here is a few atomic loads or one sequence-locked copy: any thread, never blocking. */
#include "../common/azdeckinfo.h"
#include "sdk_load_export.c" /* private versioned native load receipt, no public capability */
#include "sdk_cue_export.c" /* GUI-collected native cue-set changes, no persisted-data claim */

int azstem_deck_snapshot(int deck, struct azstem_deck_snap *out);
int azstem_deck_snapshot(int deck, struct azstem_deck_snap *out) {
    if (!out || out->size < (int) sizeof *out || deck < 0 || deck >= NDECK)
        return -1;
    int size = out->size;
    memset(out, 0, sizeof *out);
    out->size = size;
    char st = atomic_load_explicit(&g_deck_state[deck], memory_order_relaxed);
    out->loaded = st == 'L' ? 1 : st == 'E' ? 0
                                            : -1;
    if (out->loaded == 1) {
        unsigned char id[16];
        char ti[LTI_STR], ar[LTI_STR];
        if (g_lti_on && lti_read(deck, id, ti, sizeof ti, ar, sizeof ar) && !lti_id_zero(id)) {
            memcpy(&out->content_id, id + 8, 4); /* trackid::TrackID + 0x08 */
            snprintf(out->title, sizeof out->title, "%s", ti);
            snprintf(out->artist, sizeof out->artist, "%s", ar);
        }
    }
    long long S = atomic_load_explicit(&g_play_obs[deck], memory_order_relaxed);
    out->playhead = S >= 0 ? S : -1;
    out->pos_s = S >= 0 ? (double) S / (double) AZ_EP147_RATE : -1.0;
    long frames = g_dk[deck].stem_frames;
    out->has_stems = frames > 0;
    out->stems_on = frames > 0 && (atomic_load_explicit(&g_dk[deck].ctrl, memory_order_relaxed) & 0x80u);
    return 0;
}

int azstem_deck_beat_phase(int deck, double *phase);
int azstem_deck_beat_phase(int deck, double *phase) {
    if (!phase || deck < 0 || deck >= NDECK || deck >= DRUM_SIDES)
        return -1;
    if (!atomic_load_explicit(&g_grid_demand, memory_order_relaxed))
        atomic_store_explicit(&g_grid_demand, 1, memory_order_relaxed);
    drum_grid *g = &g_drum_grid[deck];
    unsigned s1 = atomic_load_explicit(&g->seq, memory_order_acquire);
    if (s1 & 1u)
        return -1;
    long long anchor = atomic_load_explicit(&g->anchor_src, memory_order_relaxed);
    double beat = atomic_load_explicit(&g->beat_src, memory_order_relaxed);
    int valid = atomic_load_explicit(&g->valid, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire); /* the fields before the second look at seq */
    if (atomic_load_explicit(&g->seq, memory_order_relaxed) != s1 || !valid || beat < 1.0)
        return -1;
    long long S = atomic_load_explicit(&g_play_obs[deck], memory_order_relaxed);
    if (S < 0)
        return -1;
    double ph = fmod((double) (S - anchor), beat);
    if (ph < 0)
        ph += beat;
    *phase = ph / beat;
    return 0;
}

/* One attempt at the atomic LTI copy, with its capture time. No retry, yield or native
 * dereference. A concurrent update means unavailable until the caller's next observation. */
int azstem_deck_observation_v1(int deck, struct azstem_deck_observation_v1 *out);
int azstem_deck_observation_v1(int deck, struct azstem_deck_observation_v1 *out) {
    if (!out || azdeck_record_size(out) < sizeof *out || out->version != 1 || deck < 0 || deck >= NDECK || !g_lti_on)
        return -1;
    struct lti_snap *sn = &g_lti_snap[deck];
    unsigned seq = atomic_load_explicit(&sn->seq, memory_order_acquire);
    if (seq & 1u) return -1;
    int valid = atomic_load_explicit(&sn->valid, memory_order_relaxed);
    uintptr_t owner = atomic_load_explicit(&sn->owner, memory_order_relaxed);
    unsigned char id[16];
    lti_snap_get(id, sn->id, 2);
    /* Both native writers assign/clear twelve ID bytes. The copied tail is storage
     * padding, not loaded identity; it must not turn an empty ID into a loaded deck. */
    memset(id + 12, 0, 4);
    unsigned long long observed = atomic_load_explicit(&sn->observed_ns, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&sn->seq, memory_order_relaxed) != seq || !valid || !owner ||
        atomic_load_explicit(&g_lti_tok[deck], memory_order_acquire) != owner) return -1;
    struct azstem_deck_observation_v1 v = {out->size, 1, !lti_id_zero(id), 0, seq, observed};
    memcpy(out, &v, sizeof v);
    return 0;
}

unsigned azstem_loop_observer_mask_v1(void);
unsigned azstem_loop_observer_mask_v1(void) {
    return g_sdk_loop_on ? azsdk_loop_capabilities() : 0;
}
int azstem_loop_observation_v1(int deck, struct azstem_loop_observation_v1 *out);
int azstem_loop_observation_v1(int deck, struct azstem_loop_observation_v1 *out) {
    if (!g_sdk_loop_on || !out || azdeck_record_size(out) < sizeof *out || out->version != 1 ||
        deck < 0 || deck >= NDECK) return -1;
    struct azsdk_loop_sample sample;
    if (!azsdk_loop_read(deck, &sample)) return -1;
    struct azstem_loop_observation_v1 v = {out->size, 1, sample.unit_generation,
                                           sample.revision, sample.observed_ns, sample.engaged, 0};
    memcpy(out, &v, sizeof v);
    return 0;
}

/* A new export keeps the original exact-size v1 contract intact and prevents a mixed
 * new azmod / old experimental azstem pair from trusting the superseded byte-zero reader. */
int azstem_loop_observation_v2(int deck, struct azstem_loop_observation_v2 *out);
int azstem_loop_observation_v2(int deck, struct azstem_loop_observation_v2 *out) {
    if (!g_sdk_loop_on || !out || azdeck_record_size(out) < sizeof *out || out->version != 2 ||
        deck < 0 || deck >= NDECK) return -1;
    struct azsdk_loop_sample sample;
    if (!azsdk_loop_read(deck, &sample)) return -1;
    struct azstem_loop_observation_v2 v = {out->size, 2, sample.unit_generation,
                                           sample.revision, sample.observed_ns, sample.engaged, sample.bounds_valid,
                                           sample.in_s, sample.out_s};
    memcpy(out, &v, sizeof v);
    return 0;
}

unsigned azstem_live_observer_mask_v1(void);
unsigned azstem_live_observer_mask_v1(void) {
    return g_sdk_live_on ? azsdk_live_capabilities() : 0;
}
int azstem_live_observation_v1(int deck, struct azstem_live_observation_v1 *out);
int azstem_live_observation_v1(int deck, struct azstem_live_observation_v1 *out) {
    if (!g_sdk_live_on || !out || azdeck_record_size(out) < sizeof *out || out->version != 1 ||
        deck < 0 || deck >= NDECK) return -1;
    struct azsdk_live_sample sample;
    if (!azsdk_live_read(deck, &sample)) return -1;
    struct azstem_live_observation_v1 v = {out->size, 1, sample};
    memcpy(out, &v, sizeof v);
    return 0;
}

/* Independent Sync export; the existing fixed-size live-v1 ABI is preserved. */
int azstem_sync_observation_v1(int deck, struct azstem_sync_observation_v1 *out);
int azstem_sync_observation_v1(int deck, struct azstem_sync_observation_v1 *out) {
    if (!g_sdk_live_on || !out || azdeck_record_size(out) < sizeof *out || out->version != 1 ||
        deck < 0 || deck >= NDECK) return -1;
    struct azsdk_sync_sample sample;
    if (!azsdk_sync_read(deck, &sample)) return -1;
    struct azstem_sync_observation_v1 value = {out->size, 1, sample.unit_generation,
                                               sample.revision, sample.observed_ns, sample.raw_mode, sample.valid};
    memcpy(out, &value, sizeof value);
    return 0;
}
int azstem_tempo_lock_observation_v1(int deck, struct azstem_tempo_lock_observation_v1 *out);
int azstem_tempo_lock_observation_v1(int deck, struct azstem_tempo_lock_observation_v1 *out) {
    if (!g_sdk_live_on || !out || azdeck_record_size(out) < sizeof *out || out->version != 1 ||
        deck < 0 || deck >= NDECK) return -1;
    struct azsdk_tempo_lock_sample sample;
    if (!azsdk_tempo_lock_read(deck, &sample)) return -1;
    struct azstem_tempo_lock_observation_v1 value = {out->size, 1, sample.unit_generation,
        sample.revision, sample.observed_ns, sample.raw_locked, sample.valid};
    memcpy(out, &value, sizeof value);
    return 0;
}
