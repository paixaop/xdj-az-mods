/* SPDX-License-Identifier: GPL-3.0-only */
/* Named ABI members are mapped explicitly. Public structs may contain padding and
 * their receipt/value offsets need not be adjacent, including after a future append. */
#ifndef SDK_STATUS_LAYOUT_H
#define SDK_STATUS_LAYOUT_H
#include "azmod-mod.h"
#include <stddef.h>
enum ds_type {
    DS_INLINE,
    DS_STRING,
    DS_CUES,
    DS_BEATS,
    DS_PHRASES,
    DS_WAVE,
    DS_CUE_REF,
    DS_ENDPOINT
};
struct ds_layout {
    uint16_t value, receipt, width, type;
    uint32_t limit;
};
struct ds_topic_layout {
    uint16_t size, offset, fields;
    struct ds_layout field[9];
};
struct ds_time_axis {
    uint64_t units_per_second;
    const int64_t *ticks; /* grid: one tick/row; cues: START/END pairs; copied before return */
    uint32_t window_samples, window_rate;
    const int64_t *stored_cue_ms; /* optional exact native persistence keys, copied with cues */
};
struct ds_field {
    uint32_t topic, field;
    az_status_field_info_v1 info;
    const void *value;
    const struct ds_time_axis *clock;
};
enum {
    DS_BOUNDARY = 1,
    DS_MOTION = 2,
    DS_UNIT_INPUT = 4,
    DS_NATIVE_INPUT = 8,
    DS_FIELD_LOSS = 16, /* capture named only the supplied native fields */
    DS_HEAD_LOSS = 32 /* history loss from the audible motion writer */
};
struct ds_topics {
    az_status_load_v1 load;
    az_status_metadata_v1 metadata;
    az_status_key_v1 key;
    az_status_cues_v1 cues;
    az_status_loop_v1 loop;
    az_status_grid_v1 grid;
    az_status_phrases_v1 phrases;
    az_status_transport_v1 transport;
    az_status_position_v1 position;
    az_status_tempo_v1 tempo;
    az_status_sync_v1 sync;
    az_status_settings_v1 settings;
    az_status_phase_v1 phase;
    az_status_stems_v1 stems;
    az_status_overview_v1 overview;
    az_status_phrase_playback_v1 phrase_playback;
};
#define DS_F(T, F, K, L) {offsetof(az_status_##T##_v1, F), offsetof(az_status_##T##_v1, F##_info), \
                          sizeof(((az_status_##T##_v1 *) 0)->F), DS_##K, L}
#define DS_T(T, N, ...)                                                               \
    {                                                                                 \
        sizeof(az_status_##T##_v1), offsetof(struct ds_topics, T), N, { __VA_ARGS__ } \
    }
static const struct ds_topic_layout ds_layout[AZ_STATUS_TOPIC_COUNT] = {
    [AZ_STATUS_LOAD] = DS_T(load, 4,
                            DS_F(load, state, INLINE, 1),
                            DS_F(load, attempt, INLINE, 1),
                            DS_F(load, source, INLINE, 1),
                            DS_F(load, track, INLINE, 1)),
    [AZ_STATUS_METADATA] = DS_T(metadata, 8,
                                DS_F(metadata, content_id, INLINE, 1),
                                DS_F(metadata, title, STRING, 192),
                                DS_F(metadata, artist, STRING, 192),
                                DS_F(metadata, album, STRING, 192),
                                DS_F(metadata, path, STRING, 512),
                                DS_F(metadata, format, STRING, 32),
                                DS_F(metadata, duration_s, INLINE, 1),
                                DS_F(metadata, duration_precision_s, INLINE, 1)),
    [AZ_STATUS_KEY] = DS_T(key, 3,
                           DS_F(key, original, STRING, 32),
                           DS_F(key, shift_semitones, INLINE, 1),
                           DS_F(key, effective, STRING, 32)),
    [AZ_STATUS_CUES] = DS_T(cues, 5,
                            DS_F(cues, main, CUES, 1),
                            DS_F(cues, live_hot, CUES, 64),
                            DS_F(cues, live_memory, CUES, 64),
                            DS_F(cues, saved_hot, CUES, 256),
                            DS_F(cues, saved_memory, CUES, 256)),
    [AZ_STATUS_LOOP] = DS_T(loop, 9,
                            DS_F(loop, retained_in, INLINE, 1),
                            DS_F(loop, retained_out, INLINE, 1),
                            DS_F(loop, defined, INLINE, 1),
                            DS_F(loop, engaged, INLINE, 1),
                            DS_F(loop, working_in, INLINE, 1),
                            DS_F(loop, working_out, INLINE, 1),
                            DS_F(loop, reloop_available, INLINE, 1),
                            DS_F(loop, mode, INLINE, 1),
                            DS_F(loop, saved_entry, CUE_REF, 1)),
    [AZ_STATUS_GRID] = DS_T(grid, 2,
                            DS_F(grid, current, BEATS, 32768),
                            DS_F(grid, saved, BEATS, 32768)),
    [AZ_STATUS_PHRASES] = DS_T(phrases, 4,
                               DS_F(phrases, current, PHRASES, 1024),
                               DS_F(phrases, saved, PHRASES, 1024),
                               DS_F(phrases, current_grid, INLINE, 1),
                               DS_F(phrases, saved_grid, INLINE, 1)),
    [AZ_STATUS_TRANSPORT] = DS_T(transport, 6,
                                 DS_F(transport, state, INLINE, 1),
                                 DS_F(transport, direction, INLINE, 1),
                                 DS_F(transport, rate, INLINE, 1),
                                 DS_F(transport, raw_mode_is_2, INLINE, 1),
                                 DS_F(transport, raw_play_pause, INLINE, 1),
                                 DS_F(transport, raw_aux, INLINE, 1)),
    [AZ_STATUS_POSITION] = DS_T(position, 4,
                                DS_F(position, head, INLINE, 1),
                                DS_F(position, slip_head, INLINE, 1),
                                DS_F(position, remaining_s, INLINE, 1),
                                DS_F(position, estimated_s, INLINE, 1)),
    [AZ_STATUS_TEMPO] = DS_T(tempo, 4,
                             DS_F(tempo, original_bpm, INLINE, 1),
                             DS_F(tempo, current_bpm, INLINE, 1),
                             DS_F(tempo, slider_percent, INLINE, 1),
                             DS_F(tempo, slider_locked, INLINE, 1)),
    [AZ_STATUS_SYNC] = DS_T(sync, 5,
                            DS_F(sync, enabled, INLINE, 1),
                            DS_F(sync, locked, INLINE, 1),
                            DS_F(sync, following, INLINE, 1),
                            DS_F(sync, master, INLINE, 1),
                            DS_F(sync, raw_mode, INLINE, 1)),
    [AZ_STATUS_SETTINGS] = DS_T(settings, 5,
                                DS_F(settings, master_tempo, INLINE, 1),
                                DS_F(settings, key_lock, INLINE, 1),
                                DS_F(settings, quantize, INLINE, 1),
                                DS_F(settings, slip, INLINE, 1),
                                DS_F(settings, pad_mode, INLINE, 1)),
    [AZ_STATUS_PHASE] = DS_T(phase, 2,
                             DS_F(phase, beat, INLINE, 1),
                             DS_F(phase, bar, INLINE, 1)),
    [AZ_STATUS_STEMS] = DS_T(stems, 2,
                             DS_F(stems, available, INLINE, 1),
                             DS_F(stems, enabled, INLINE, 1)),
    [AZ_STATUS_OVERVIEW] = DS_T(overview, 1,
                                DS_F(overview, saved, WAVE, 1)),
    [AZ_STATUS_PHRASE_TRANSITION] = DS_T(phrase_playback, 1,
                                         DS_F(phrase_playback, active, ENDPOINT, 1)),
};
#undef DS_F
#undef DS_T
#endif
