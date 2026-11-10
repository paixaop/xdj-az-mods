/* SPDX-License-Identifier: GPL-3.0-only */
/* Private azstem -> azmod ABI. All records are bounded, pointer-free writer copies. */
#ifndef AZSTATUS_NATIVE_H
#define AZSTATUS_NATIVE_H
#include <stdint.h>
#include "azstatus_identity.h"
#include "azstatus_maps.h"
#include "azstatus_cue_origin.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {
    AZN_LOAD = 1, AZN_LOOP, AZN_PLAY, AZN_QUANTIZE, AZN_KEY_LOCK, AZN_SYNC,
    AZN_SLIDER_LOCK, AZN_UNIT_END, AZN_GAP, AZN_METADATA, AZN_MOTION, AZN_STEMS
};
enum { AZN_CAP_LOAD=1, AZN_CAP_LOOP=2, AZN_CAP_LTI=4, AZN_CAP_MOTION=8, AZN_CAP_PLAY_RAW=16, AZN_CAP_STEMS=1u<<12 };
enum { AZN_CAP_QUANTIZE=1u<<5, AZN_CAP_KEY_LOCK=1u<<6, AZN_CAP_SYNC=1u<<7, AZN_CAP_SLIDER_LOCK=1u<<8 };
enum { AZN_MAPS=81, AZN_CAP_MAPS=1u<<18 };
enum { AZN_HOT_CUES=64, AZN_CAP_HOT=1u<<16 };
enum { AZN_CAP_LOOP_MODE=1u<<20 };
enum { AZN_RELOOP=87, AZN_CAP_RELOOP=1u<<22 };
enum { AZN_RELOOP_DENIED=1, AZN_RELOOP_UNKNOWN, AZN_RELOOP_STALE, AZN_RELOOP_ALLOWED };
enum { AZN_CAP_HOT_LINK=1u<<26 };
enum { AZN_HOT_LINK_EMPTY=1, AZN_HOT_LINK_READY, AZN_HOT_LINK_UNKNOWN, AZN_MEMORY_LINK_READY };
struct azn_hot_link {
    uint64_t provider, stamp;
    uint32_t slot, state;
};
enum { AZN_RATE=82, AZN_CAP_RATE=1u<<19 };
enum { AZN_SYNC_ROLE=84, AZN_CAP_SYNC_ROLE=1u<<23 };
enum { AZN_TRANSPORT_STATE=85, AZN_CAP_TRANSPORT_STATE=1u<<25 };
enum { AZN_SLIP_HEAD=83, AZN_CAP_SLIP_HEAD=1u<<21 };
/* AZN_LOOP.c. Zero is the older packet without a mode observation. */
enum { AZN_MODE_TIME=1, AZN_MODE_BEAT, AZN_MODE_EMPTY, AZN_MODE_STALE, AZN_MODE_UNKNOWN };
enum { AZN_KEY_SHIFT=80, AZN_CAP_KEY_SHIFT=1u<<13 };
enum { AZN_EMPTY, AZN_LOADING, AZN_LOADED, AZN_FAILED, AZN_UNKNOWN };
enum { AZN_MEASURE=1, AZN_REPOSITION, AZN_MOTION_SEED, AZN_WRAP, AZN_JUMP, AZN_SEEK };
enum { AZN_ABSENT, AZN_COORD, AZN_INVALID };
enum { AZN_AT_OPENING=1, AZN_BEGAN_PLAY=2, AZN_TERMINAL_ARRIVAL=4, AZN_TERMINAL_CONFIRMED=8, AZN_TERMINAL_CONTINUATION=16 };
enum { AZN_END=86, AZN_CAP_END=1u<<24 };
/* Exact owned source length in points[0], independent of terminal occurrence. */
enum { AZN_DURATION=88, AZN_CAP_DURATION=1u<<28 };
enum { AZN_CAP_ORIGINAL_BPM=1u<<29 };
enum { AZN_BPM_UNKNOWN, AZN_BPM_READY, AZN_BPM_EMPTY, AZN_BPM_UNAVAILABLE };
struct azn_point {
    uint32_t state, rate;
    int64_t samples;
};
struct azn_hot_slot { struct azn_point in, out; uint32_t rgb, reserved; };
enum { AZN_HOT_COMMENT = 1, AZN_HOT_AUTO_ACTIVE = 2, AZN_HOT_AUTO_ON = 4 };
struct azstatus_native_receipt {
    uint32_t size, version, kind, deck;
    uint64_t epoch, unit, observed_ns, load_generation; /* epoch is native invalidation, not public load */
    uint64_t lost_topics;
    int32_t a, b, c, valid;
    uint64_t attempt;
    /* retained IN, retained OUT, working IN, working OUT */
    struct azn_point points[4];
    uint32_t content_id;
    union {
        struct { char title[192], artist[192]; };
        struct azn_hot_slot hot[8];
    };
    uint64_t motion_order, motion_generation; /* optional v1 position tail. Slip samples carry the generation only. */
    uint64_t revision; /* optional v1 raw notification revision */
    uint64_t occurrence_ordinal; /* optional v1 classified reposition */
    uint64_t source_pin; /* opaque core-issued token, qualified again on the worker */
    uint64_t memory_pin, memory_serial;
    struct aznm_packet map;
    uint32_t source_kind;
    uint32_t reloop;
    uint32_t history_only;
    struct azn_hot_link hot_link; /* LOOP only, copied property provenance */
    uint32_t motion_flags; /* optional v1 opening proof for MOTION and MAPS */
    uint64_t file_row_token; /* optional v1 FILE atom/edition witness for LOOP */
    /* Optional v1 endpoint observation windows. Zero means unqualified. */
    uint32_t observed_window_samples, observed_window_rate;
    uint32_t from_window_samples, from_window_rate;
    /* Matched TrackInfoToLoad metadata, independent of the grid BPM at head. */
    double original_bpm;
    uint32_t original_bpm_state;
};
/* Optional AZN_HOT_CUES tail. The slot flags occupy the existing reserved word
 * and are interpreted only with this complete tail. */
struct azn_hot_details {
    char comments[8][128];
    int64_t stored_ms[8][2]; /* firmware persistence keys in its whole-ms clock */
};
struct azn_hot_receipt {
    struct azstatus_native_receipt base;
    struct azn_hot_details details;
};
typedef int (*azn_setting_patch_fn)(uintptr_t,void *,void **,const char *);
typedef uint64_t (*azn_source_capture_fn)(unsigned deck,uint64_t unit,uint64_t native_epoch,
                                          uintptr_t wherefrom,const unsigned char track_id[12]);
struct azn_memory_api;
struct azn_file_map_callbacks;
struct azn_file_cue_api {
    uint32_t size,version;
    int (*install)(int build,azn_setting_patch_fn patch,const struct azn_file_cue_callbacks *callbacks);
    void (*transport)(unsigned stage,uintptr_t packet,unsigned peer);
    void (*observe_input)(azn_file_input_observer_fn observer);
    int (*install_maps)(int build,azn_setting_patch_fn patch,const struct azn_file_map_callbacks *callbacks);
    /* Exact pinned Content membership, returning held physical-input tokens. */
    int (*map_origins)(const struct aznm_packet *packet,uint64_t tokens[3]);
    void (*map_hold)(const uint64_t *tokens,size_t count,int acquire);
};
struct azstatus_native_v1 {
    uint32_t size, version;
    int (*wake_fd)(void); /* process-lifetime nonblocking eventfd, single core reader */
    int (*take)(unsigned deck, struct azstatus_native_receipt *out);
    int (*load_seed)(unsigned deck, struct azstatus_native_receipt *out);
    void (*interest)(unsigned deck, uint64_t topics);
    uint32_t (*capabilities)(unsigned deck); /* optional appended v1 tail */
    int (*metadata_seed)(unsigned deck, struct azstatus_native_receipt *out);
    int (*register_source_capture)(azn_source_capture_fn callback);
    void (*bind_stems)(unsigned deck, uint64_t cut, uint64_t load, const char *path);
    int (*stems_seed)(unsigned deck, struct azstatus_native_receipt *out);
    int (*install_settings)(int profile,azn_setting_patch_fn patch);
    const struct azn_memory_api *(*memory)(void);
    const struct azn_maps_api *(*maps)(void);
    int (*install_terminal)(int build,azn_setting_patch_fn patch);
    const struct azn_file_cue_api *(*file_cues)(void);
    int (*track_matches)(unsigned deck,uint64_t epoch,uint64_t load,const unsigned char source_id[12]);
};
typedef const struct azstatus_native_v1 *(*azstatus_native_fn)(void);
#ifdef AZ_STATUS_NATIVE_PRODUCER
__attribute__((visibility("hidden"))) int azn_install_settings(int,azn_setting_patch_fn);
__attribute__((visibility("hidden"))) int azn_install_terminal(int,azn_setting_patch_fn);
__attribute__((visibility("hidden"))) unsigned azn_maps_install(int,uintptr_t,azn_setting_patch_fn);
__attribute__((visibility("hidden"))) int azn_maps_current(unsigned,uint64_t,const struct aznm_packet *);
__attribute__((visibility("hidden"))) int azn_sync_seed(unsigned deck,uintptr_t unit,int *raw);
__attribute__((visibility("hidden"))) void azn_setting_gap(unsigned deck,unsigned kind);
__attribute__((visibility("hidden"))) int azn_register_source_capture(azn_source_capture_fn);
__attribute__((visibility("hidden"))) uint64_t azn_load_source_pin(unsigned deck,uint64_t epoch,uint64_t load);
__attribute__((visibility("hidden"))) unsigned azn_load_kind(unsigned deck,uint64_t epoch,uint64_t load);
__attribute__((visibility("hidden"))) unsigned azn_load_original_bpm(unsigned deck,uint64_t unit,uint64_t epoch,uint64_t load,double *out);
/* Nonzero native request identity only while this exact source is pending; source may be NULL. */
__attribute__((visibility("hidden"))) uint64_t azn_load_pending(unsigned deck,uint64_t epoch,const unsigned char source_id[12]);
__attribute__((visibility("hidden"))) int azn_load_track_matches(unsigned deck,uint64_t epoch,uint64_t load,
                                                                 const unsigned char source_id[12]);
__attribute__((visibility("hidden"))) int azn_metadata_seed(unsigned deck, struct azstatus_native_receipt *out);
__attribute__((visibility("hidden"))) void azn_raw_state(unsigned deck,uint64_t generation,int retired);
__attribute__((visibility("hidden"))) void azn_capability(unsigned deck, unsigned flags, int ready);
__attribute__((visibility("hidden"))) uint64_t azn_lifecycle_fence(unsigned deck);
__attribute__((visibility("hidden"))) int azn_load_history_revoked(unsigned deck, uint64_t epoch);
__attribute__((visibility("hidden"))) uint64_t azn_empty_fence(unsigned deck);
__attribute__((visibility("hidden"))) int azn_empty_seed(unsigned deck,struct azstatus_native_receipt *out);
__attribute__((visibility("hidden"))) void azn_empty_commit(unsigned deck, uint64_t fence);
__attribute__((visibility("hidden"))) int azn_offer(struct azstatus_native_receipt *r);
__attribute__((visibility("hidden"))) void azn_emit_hot(struct azn_hot_receipt *r);
__attribute__((visibility("hidden"))) uint64_t azn_reloop_cut(unsigned deck);
__attribute__((visibility("hidden"))) void azn_emit(struct azstatus_native_receipt *r);
__attribute__((visibility("hidden"))) uint64_t azn_map_begin_mask(unsigned mask);
__attribute__((visibility("hidden"))) void azn_map_end_mask(uint64_t ticket);
__attribute__((visibility("hidden"))) int azn_offer_map_cut(struct azstatus_native_receipt *receipt);
__attribute__((visibility("hidden"))) void azn_map_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_hot_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_reloop_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_rate_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_transport_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_end_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_duration_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_motion_gap(unsigned deck,int reposition);
__attribute__((visibility("hidden"))) void azn_key_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_sync_role_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_slip_gap(unsigned deck);
__attribute__((visibility("hidden"))) void azn_gap(unsigned deck, uint64_t topics);
__attribute__((visibility("hidden"))) uint64_t azn_interest(unsigned deck);
__attribute__((visibility("hidden"))) uint64_t azn_seed_id(unsigned deck);
__attribute__((visibility("hidden"))) uint64_t azn_cue_loss_cut(unsigned deck,int hot);
/* Called only by the built-in non-real-time discovery and control writers. The
 * probe returns an AZ_AVAIL value and fills present only for a completed read. */
__attribute__((visibility("hidden"))) void azn_stems_start(int (*probe)(const char *, int *));
__attribute__((visibility("hidden"))) void azn_stems_control(unsigned deck, int enabled);
struct azn_stem_job { uint64_t load, pin, cut, retired; };
__attribute__((visibility("hidden"))) struct azn_stem_job azn_stems_job(unsigned deck,uint64_t load,const char *path);
__attribute__((visibility("hidden"))) void azn_stems_decoded(unsigned deck,struct azn_stem_job job,const char *path,int playable);
__attribute__((visibility("hidden"))) void azn_stems_rescan(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
