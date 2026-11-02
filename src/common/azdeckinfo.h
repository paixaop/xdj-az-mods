/* SPDX-License-Identifier: GPL-3.0-only */
/* azdeckinfo.h: what azstem publishes about each deck for the community mod services
 * (azmod's deck_info / deck_grid / on_deck_event).
 *
 * azstem already keeps, per deck and by identity, the track EP147 loaded (copied inside
 * EP147's own update, under a sequence lock), the playhead its audio hook observes, and the
 * beat grid it reads for Drum Roll. azmod looks these two functions up with dlsym and never
 * reads EP147's memory for them. Both are safe on any thread and never block. */
#ifndef AZDECKINFO_H
#define AZDECKINFO_H
#include <stdint.h>
#include <string.h>
#include "azliveobserver.h"

static inline uint32_t azdeck_record_size(const void *record) {
    uint32_t size;
    memcpy(&size, record, sizeof size);
    return size;
}

#define AZSNAP_STR 128

struct azstem_deck_snap {
    int size;               /* = sizeof(struct azstem_deck_snap), set by the caller */
    int loaded;             /* 1 = a track is loaded, 0 = the deck is empty, -1 = unknown */
    unsigned content_id;    /* the rekordbox track id (TrackID + 0x08), 0 = unknown */
    long long playhead;     /* source samples at the deck engine rate, -1 = never observed */
    double pos_s;           /* the playhead in seconds, -1 = unknown */
    int has_stems;          /* 1 = stems are decoded for this deck's track */
    int stems_on;           /* 1 = the stems engine plays this deck */
    char title[AZSNAP_STR]; /* "" = unknown */
    char artist[AZSNAP_STR];
};

/* 0 = filled, -1 = no such deck (or a caller struct too small). */
typedef int (*azstem_deck_snapshot_fn)(int deck, struct azstem_deck_snap *out);

/* The playhead's position within the current beat, 0..1. 0 = filled, -1 = no usable grid
 * (empty deck, mid-load, a grid whose tempo and spacing disagree) or no playhead. The first
 * call asks azstem's flusher to keep every deck's grid current, so it can return -1 for a
 * moment after the first request. */
typedef int (*azstem_deck_beat_phase_fn)(int deck, double *phase);

/* Separate export: the original snapshot's exact-size ABI remains unchanged. Revision
 * counts captured LTI writes; it is not a deck-load generation or a source identity. */
struct azstem_deck_observation_v1 {
    unsigned size, version;
    int loaded, reserved;
    unsigned long long revision, observed_ns;
};
typedef int (*azstem_deck_observation_v1_fn)(int, struct azstem_deck_observation_v1 *);

/* Independent native notification stream. A generation identifies a player UNIT, not a
 * track load. observed_ns is callback receipt time; native capture time is unavailable. */
struct azstem_loop_observation_v1 {
    unsigned size, version;
    unsigned long long unit_generation, revision, observed_ns;
    int engaged, reserved;
};
typedef int (*azstem_loop_observation_v1_fn)(int, struct azstem_loop_observation_v1 *);
/* v2 certifies the corrected working-property decoder and adds independently valid
 * current bounds. v1 stays exported for existing clients. Never infer retained points or
 * a track-load identity from this notification. */
struct azstem_loop_observation_v2 {
    unsigned size, version;
    unsigned long long unit_generation, revision, observed_ns;
    int engaged, bounds_valid;
    double in_s, out_s;
};
typedef int (*azstem_loop_observation_v2_fn)(int, struct azstem_loop_observation_v2 *);
typedef unsigned (*azstem_loop_observer_mask_v1_fn)(void);

/* Independent callback receipts. Raw play context is not normalized playback; unit
 * generation is not committed track-load identity, and each stream has its own clock. */
struct azstem_live_observation_v1 {
    uint32_t size, version;
    struct azsdk_live_sample sample;
};
typedef int (*azstem_live_observation_v1_fn)(int, struct azstem_live_observation_v1 *);
typedef unsigned (*azstem_live_observer_mask_v1_fn)(void);
/* Separate Sync receipt keeps the fixed live-v1 layout intact. Native modes0
 * and2 are the verified off/on setting values; an invalid receipt retains raw
 * mode and clock while valid is0. Unit lifetime is not track-load identity. */
struct azstem_sync_observation_v1 {
    uint32_t size, version;
    uint64_t unit_generation, revision, observed_ns;
    int32_t raw_mode;
    uint32_t valid;
};
typedef int (*azstem_sync_observation_v1_fn)(int, struct azstem_sync_observation_v1 *);
struct azstem_tempo_lock_observation_v1 {
    uint32_t size, version;
    uint64_t unit_generation, revision, observed_ns;
    int32_t raw_locked;
    uint32_t valid;
};
typedef int (*azstem_tempo_lock_observation_v1_fn)(int, struct azstem_tempo_lock_observation_v1 *);
#endif
