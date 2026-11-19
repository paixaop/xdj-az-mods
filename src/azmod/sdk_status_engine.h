/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_STATUS_ENGINE_H
#define SDK_STATUS_ENGINE_H
#include "sdk_status_layout.h"
#define DS_PATH_PENDING 16
/* Internal producer values are copied synchronously. No reducer storage or
 * lock escapes this module. Registration/delivery/retirement belong to UI. */
enum ds_path_kind {
    DS_PATH_BASELINE,
    DS_PATH_TRAVEL,
    DS_PATH_ACTION
};
struct ds_path {
    uint64_t order, generation, phrase_provider, phrase_revision;
    az_status_grid_ref_v1 grid;
    unsigned kind, endpoints, opening, ended, began_play, defer_terminal, confirm_terminal;
    az_status_coord_v1 from, to;
    az_status_coord_v1 terminal;
    uint32_t observed_window_samples, observed_window_rate;
    uint32_t from_window_samples, from_window_rate;
    const az_status_phrase_endpoint_v1 *departure, *landing;
    uint64_t first_order; /* nonzero only for consecutive ordinary measurements */
};

typedef void (*ds_core_load_fn)(unsigned, uint64_t, uint64_t, int, const az_track_ref *);
uint64_t ds_now(void);
int ds_enabled(void);
void ds_enable(void);
int ds_publish(unsigned, uint64_t, unsigned, uint64_t, const struct ds_field *, size_t,
               const az_status_occurrence_v1 *, const az_status_phrase_transition_v1 *, uint64_t, unsigned);
int ds_publish_file(unsigned, uint64_t, const az_track_ref *, uint64_t, const struct ds_field *, size_t, uint64_t, unsigned);
int ds_publish_checked(const struct ds_path *, const az_track_ref *, int, unsigned, uint64_t, unsigned, uint64_t,
                       const struct ds_field *, size_t, const az_status_occurrence_v1 *, const az_status_phrase_transition_v1 *, uint64_t, unsigned);
int ds_retire_file_binding(unsigned, uint64_t, const az_track_ref *);
int ds_register(int, const az_deck_watch_v1 *, az_deck_status_fn, void *, az_deck_subscription *);
int ds_release(int, az_deck_subscription);
void ds_retire(int);
unsigned ds_drain(unsigned);
int sdk_status_analysis_bind(ds_core_load_fn);
int az_status_loaded_reference(unsigned, uint64_t, az_track_ref *);
struct ds_deck_stamp { uint64_t epoch, last_epoch, motion_generation; };
void ds_read_stamp(unsigned, struct ds_deck_stamp *);
/* Metadata for one coherent cut. Absent fields are zeroed. */
void ds_read_receipts(unsigned, struct ds_field *, size_t, uint64_t *);
void ds_set_stem_binding(void (*)(unsigned, uint64_t, uint64_t, const char *));
void ds_watch_stems(unsigned, int);
struct ds_phrase_span { az_status_coord_v1 start, end; };
int ds_read_phrase_span(unsigned, uint64_t, const struct ds_path *, struct ds_phrase_span *);
int ds_path_inside_span(const struct ds_phrase_span *, const struct ds_path *);
int ds_native_loop_ref(unsigned, uint64_t, uint64_t, uint64_t, unsigned, unsigned, az_status_cue_ref_v1 *);
int ds_publish_ordered(const struct ds_path *, const az_track_ref *, int, unsigned, uint64_t, unsigned, uint64_t,
                       const struct ds_field *, size_t, const az_status_occurrence_v1 *, const az_status_phrase_transition_v1 *, uint64_t, unsigned, uint64_t *);
/* Canonical equality for copied producer payloads, using their source clocks. */
int ds_payload_same(unsigned, unsigned, const void *, const void *, size_t, const struct ds_time_axis *, const struct ds_time_axis *);
#endif
