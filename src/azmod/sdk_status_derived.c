/* SPDX-License-Identifier: GPL-3.0-only */
/* Included inside the reducer. Only combine READY fields from the current cut.
 * The caller already holds ds.mu and has applied the transaction's whole delta. */
#include "camelot_text.h"
static const struct ds_cell *ds_ready(const struct ds_image *im, unsigned topic, unsigned field) {
    const struct ds_cell *c = im->field[topic][field];
    if (!c || c->info.info.availability != AZ_AVAIL_READY ||
        (c->info.scope == AZ_STATUS_LOAD_SCOPE && (!im->epoch || c->info.scope_id != im->epoch))) return NULL;
    return c;
}
static const void *ds_value(const struct ds_image *im, unsigned topic, unsigned field) {
    const struct ds_cell *c = ds_ready(im, topic, field);
    return c ? c->data : NULL;
}
static int ds_integer_is(const struct ds_image *im, unsigned t, unsigned f, int value) {
    const int32_t *n = ds_value(im, t, f);
    return n && *n == value;
}
/* Only an old derived value is withdrawn when a dependency is missing. The
 * absence of a head provider by itself never claims support for remaining. */
static int ds_derived(struct ds_deck *deck, unsigned t, unsigned f, uint64_t now,
                      const void *v, size_t bytes, uint64_t *loss) {
    struct ds_image *im = &deck->current;
    const struct ds_cell *old = im->field[t][f];
    if (!v && (!old || old->info.info.provenance != AZ_PROV_DERIVED)) return 0;
    struct ds_field p = {t, f, {.info = {v ? AZ_AVAIL_READY : AZ_AVAIL_STALE, AZ_PROV_DERIVED, old ? old->info.info.revision : 0, now}, .scope = AZ_STATUS_LOAD_SCOPE, .count = v ? 1u : 0u, .scope_id = im->epoch, .provider_id = AZSP_DERIVED + (deck - ds.deck) + 1}, v, NULL};
    struct ds_time_axis clock = {.units_per_second = 1, .window_samples = deck->head_window_samples, .window_rate = deck->head_window_rate};
    if (t == AZ_STATUS_POSITION && f == AZ_STATUS_POSITION_REMAINING_S) p.clock = &clock;
    if (v && ds_layout[t].field[f].type == DS_STRING) p.info.count = (unsigned) bytes - 1;
    if (ds_same(old, &p, bytes)) return 0;
    int exhausted = p.info.info.revision == UINT64_MAX;
    if (!exhausted) {
        p.info.info.revision++;
        exhausted = ds_set(im, &p, bytes) == AZ_LIMIT;
    }
    if (exhausted) {
        ds_clear_field(&im->field[t][f]);
        *loss |= AZ_STATUS_TOPIC(t);
    }
    return 1;
}
/* EP147's native Camelot formatter uses the relative major pitch class for
 * minor keys. Preserve the stored spelling in KEY.original. */
static int ds_pitch(const char *text, int *pitch, int *major) {
    if (!text || !*text) return 0;
    for (int n = 0; n < 12; ++n)
        for (int m = 0; m < 2; ++m) {
            char encoded[4];
            ct_camelot(n, m, encoded);
            if (!strcmp(text, encoded)) {
                *pitch = n;
                *major = m;
                return 1;
            }
        }
    static const unsigned note[] = {9, 11, 0, 2, 4, 5, 7}; /* A..G */
    if (*text < 'A' || *text > 'G') return 0;
    int n = note[*text++ - 'A'];
    if (*text == '#') {
        ++n;
        ++text;
    } else if (*text == 'b') {
        --n;
        ++text;
    } else if (!strncmp(text, "♯", 3)) {
        ++n;
        text += 3;
    } else if (!strncmp(text, "♭", 3)) {
        --n;
        text += 3;
    }
    while (*text == ' ') ++text;
    if (!*text || !strcmp(text, "maj") || !strcmp(text, "major") || !strcmp(text, "Major")) *major = 1;
    else if (!strcmp(text, "m") || !strcmp(text, "min") || !strcmp(text, "minor") || !strcmp(text, "Minor")) *major = 0;
    else return 0;
    *pitch = (n + (*major ? 12 : 15)) % 12;
    return 1;
}
static uint64_t ds_effective_key(struct ds_deck *d, uint64_t now, uint64_t *loss) {
    const struct ds_image *im = &d->current;
    const struct ds_cell *actual = im->field[AZ_STATUS_KEY][AZ_STATUS_KEY_EFFECTIVE];
    if (actual && actual->info.info.provenance == AZ_PROV_NATIVE &&
        actual->info.scope_id == im->epoch && actual->info.info.availability != AZ_AVAIL_LOADING &&
        actual->info.info.availability != AZ_AVAIL_UNSUPPORTED) return 0;
    const char *original = ds_value(im, AZ_STATUS_KEY, AZ_STATUS_KEY_ORIGINAL);
    const int32_t *shift = ds_value(im, AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES);
    const char *value = NULL;
    char key[4];
    int pitch, major;
    if (shift && *shift >= -12 && *shift <= 12 && ds_integer_is(im, AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_MASTER_TEMPO, 1) &&
        ds_pitch(original, &pitch, &major)) {
        ct_camelot((pitch + *shift + 12) % 12, major, key);
        value = key;
    }
    return ds_derived(d, AZ_STATUS_KEY, AZ_STATUS_KEY_EFFECTIVE, now, value, value ? strlen(value) + 1 : 0, loss)
               ? AZ_STATUS_TOPIC(AZ_STATUS_KEY)
               : 0;
}
static uint64_t ds_times(struct ds_deck *d, uint64_t now, uint64_t *loss) {
    const struct ds_image *im = &d->current;
    const double *duration = ds_value(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S);
    const double *precision = ds_value(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_PRECISION_S);
    const az_status_coord_v1 *head = ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    const double *rate = ds_value(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE);
    double remaining = NAN, estimated = NAN;
    if (duration && head && d->head_window_samples && d->head_window_rate) {
        const struct ds_cell *source = ds_ready(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S);
        const struct ds_time_axis *clock = &source->clock;
        int exact = clock->ticks && head->rate_hz;
        int duration_zero = exact ? ds_compare_source_times(clock->ticks[0], clock->units_per_second, 0, 1,
                                                            d->head_window_rate, d->head_window_samples)
                                  : compare_timestamps(*duration, 0., 1, d->head_window_rate, d->head_window_samples);
        int head_zero = head->rate_hz ? compare_timestamps(head->samples, 0, head->rate_hz, d->head_window_rate, d->head_window_samples)
                                      : compare_timestamps(head->seconds, 0., 1, d->head_window_rate, d->head_window_samples);
        if (duration_zero != AZ_TIME_INVALID && head_zero != AZ_TIME_INVALID &&
            (duration_zero == AZ_TIME_EQUAL || duration_zero == AZ_TIME_AFTER) &&
            (head_zero == AZ_TIME_EQUAL || head_zero == AZ_TIME_AFTER)) {
            int order = exact ? ds_compare_source_times(clock->ticks[0], clock->units_per_second, head->samples, head->rate_hz,
                                                        d->head_window_rate, d->head_window_samples)
                              : compare_timestamps(*duration, head->seconds, 1, d->head_window_rate, d->head_window_samples);
            long double difference = (long double) *duration - head->seconds;
            if (exact) {
                __int128_t ticks = (__int128_t) clock->ticks[0] * head->rate_hz - (__int128_t) head->samples * clock->units_per_second;
                difference = (long double) ticks / (clock->units_per_second * head->rate_hz);
            }
            if (order == AZ_TIME_EQUAL) remaining = 0;
            else if (order == AZ_TIME_AFTER) remaining = (double) difference;
            else if (order == AZ_TIME_BEFORE) {
                int ended = ds_integer_is(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, AZ_PLAYBACK_ENDED);
                double resolution = 0;
                if (precision) {
                    int precision_order = compare_timestamps(*precision, 0., 1, d->head_window_rate, d->head_window_samples);
                    if (precision_order != AZ_TIME_INVALID && precision_order == AZ_TIME_AFTER) resolution = *precision;
                }
                if (ended && head->rate_hz) resolution += 1.0 / head->rate_hz;
                int extent = compare_timestamps(-difference, resolution, 1,
                                                d->head_window_rate, d->head_window_samples);
                if (extent != AZ_TIME_INVALID && (extent == AZ_TIME_EQUAL || extent == AZ_TIME_BEFORE)) remaining = 0;
            }
            /* INVALID leaves remaining unavailable rather than guessing a clock. */
        }
    }
    if (isfinite(remaining) && rate && isfinite(*rate) && *rate > 0 &&
        ds_integer_is(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, AZ_PLAYBACK_PLAYING) &&
        ds_integer_is(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_DIRECTION, AZ_STATUS_DIRECTION_FORWARD) &&
        ds_integer_is(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED, 0) &&
        ds_integer_is(im, AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_SLIP, 0)) estimated = remaining / *rate;
    int r = ds_derived(d, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S, now,
                       isfinite(remaining) ? &remaining : NULL, isfinite(remaining) ? sizeof remaining : 0, loss);
    const double *old_estimated = ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_ESTIMATED_S);
    int same_estimate = 0;
    if (old_estimated && isfinite(estimated)) {
        /* Convert both wall-time estimates back to the measured source clock.
         * Its current playback ratio determines this observation's window. */
        int order = compare_timestamps((long double) *old_estimated * *rate, (long double) estimated * *rate,
                                       1, d->head_window_rate, d->head_window_samples);
        same_estimate = order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
    }
    int e = same_estimate ? 0 : ds_derived(d, AZ_STATUS_POSITION, AZ_STATUS_POSITION_ESTIMATED_S, now, isfinite(estimated) ? &estimated : NULL, isfinite(estimated) ? sizeof estimated : 0, loss);
    return (r || e) ? AZ_STATUS_TOPIC(AZ_STATUS_POSITION) : 0;
}
static double ds_bar_fraction(struct ds_deck *d, const struct ds_cell *grid, unsigned index, double beat) {
    const az_beat *rows = (const void *) grid->data;
    unsigned ordinal = rows[index].beat_in_bar;
    if (!ordinal || ordinal - 1 > index) return NAN;
    unsigned first = index - (ordinal - 1);
    if (rows[first].beat_in_bar != 1) return NAN;
    if (d->bar_provider != grid->info.provider_id || d->bar_revision != grid->info.info.revision ||
        d->bar_epoch != grid->info.scope_id || d->bar_begin != first || !d->bar_end) {
        unsigned end = first + 1;
        while (end < grid->info.count && rows[end].beat_in_bar == end - first + 1) end++;
        d->bar_provider = grid->info.provider_id;
        d->bar_revision = grid->info.info.revision;
        d->bar_epoch = grid->info.scope_id;
        d->bar_begin = first;
        d->bar_end = end < grid->info.count && rows[end].beat_in_bar == 1 ? end : UINT_MAX;
    }
    if (d->bar_end == UINT_MAX || index >= d->bar_end) return NAN;
    return (index - first + beat) / (d->bar_end - first);
}
static uint64_t ds_phase(struct ds_deck *d, uint64_t now, uint64_t *loss) {
    const struct ds_image *im = &d->current;
    const struct ds_cell *grid = ds_ready(im, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    const az_status_coord_v1 *head = ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    double phase = NAN, bar = NAN;
    if (grid && grid->clock.ticks && head && head->rate_hz && grid->info.count >= 2 &&
        d->head_window_samples && d->head_window_rate) {
        const int64_t *ticks = ds_clock_ticks(grid);
        uint64_t units = grid->clock.units_per_second;
        unsigned rate = d->head_window_rate, window = d->head_window_samples;
        unsigned lo = 0, hi = grid->info.count - 1;
        int first = ds_compare_source_times(head->samples, head->rate_hz, ticks[lo], units, rate, window);
        int last = ds_compare_source_times(head->samples, head->rate_hz, ticks[hi], units, rate, window);
        if (first != AZ_TIME_INVALID && last != AZ_TIME_INVALID &&
            (first == AZ_TIME_EQUAL || first == AZ_TIME_AFTER) && (last == AZ_TIME_EQUAL || last == AZ_TIME_BEFORE)) {
            while (hi - lo > 1) {
                unsigned mid = lo + (hi - lo) / 2;
                int order = ds_compare_source_times(head->samples, head->rate_hz, ticks[mid], units, rate, window);
                if (order == AZ_TIME_INVALID) break;
                if (order == AZ_TIME_BEFORE) hi = mid;
                else lo = mid;
            }
            int span = ds_compare_source_times(ticks[hi], units, ticks[lo], units, rate, window);
            if (span != AZ_TIME_INVALID && span == AZ_TIME_AFTER) {
                unsigned at = lo;
                int lower = ds_compare_source_times(head->samples, head->rate_hz, ticks[lo], units, rate, window);
                int upper = ds_compare_source_times(head->samples, head->rate_hz, ticks[hi], units, rate, window);
                if (lower != AZ_TIME_INVALID && upper != AZ_TIME_INVALID) {
                    if (upper == AZ_TIME_EQUAL) {
                        phase = 0;
                        at = hi;
                    } else if (lower == AZ_TIME_EQUAL) phase = 0;
                    else if (lower == AZ_TIME_AFTER && upper == AZ_TIME_BEFORE) {
                        __int128_t delta = (__int128_t) head->samples * units - (__int128_t) ticks[lo] * head->rate_hz;
                        __int128_t length = ((__int128_t) ticks[hi] - ticks[lo]) * head->rate_hz;
                        phase = (double) ((long double) delta / (long double) length);
                    }
                    if (isfinite(phase) && phase >= 0 && phase < 1) bar = ds_bar_fraction(d, grid, at, phase);
                }
            }
        }
    }
    int beat_changed = ds_derived(d, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BEAT, now, isfinite(phase) ? &phase : NULL,
                                  isfinite(phase) ? sizeof phase : 0, loss);
    int bar_changed = ds_derived(d, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BAR, now, isfinite(bar) ? &bar : NULL,
                                 isfinite(bar) ? sizeof bar : 0, loss);
    return beat_changed || bar_changed ? AZ_STATUS_TOPIC(AZ_STATUS_PHASE) : 0;
}
static int ds_grid_matches(const struct ds_image *im, const az_status_grid_ref_v1 *ref, unsigned field) {
    const struct ds_cell *grid = ds_ready(im, AZ_STATUS_GRID, field);
    return grid && ref->grid_field == field && ref->load_epoch && ref->load_epoch == im->epoch &&
           grid->info.scope == AZ_STATUS_LOAD_SCOPE && grid->info.scope_id == im->epoch && ref->provider_id &&
           ref->provider_id == grid->info.provider_id && ref->revision == grid->info.info.revision &&
           ref->provenance == grid->info.info.provenance;
}
static uint64_t ds_withdraw(struct ds_image *im, unsigned topic, unsigned field, uint64_t now, uint64_t *loss) {
    struct ds_cell **c = &im->field[topic][field];
    if (!*c || (*c)->info.info.availability != AZ_AVAIL_READY) return 0;
    struct ds_field p = {topic, field, (*c)->info, NULL, NULL};
    p.info.info.availability = AZ_AVAIL_STALE;
    p.info.info.observed_ns = now;
    p.info.count = 0;
    if (ds_set(im, &p, 0) == AZ_LIMIT) {
        ds_clear_field(c);
        *loss |= AZ_STATUS_TOPIC(topic);
    }
    return AZ_STATUS_TOPIC(topic);
}
/* The repository's memory-cue lookup compares the two whole-ms keys within
 * a TrackID. Hot identity is the slot. Neither lookup is a recall witness. */
static int ds_cue_key_same(const az_cue_entry *a, const int64_t *ak,
                           const az_cue_entry *b, const int64_t *bk) {
    if (a->type != b->type) return 0;
    unsigned points = a->type == AZ_CUE_LOOP ? 2 : 1;
    for (unsigned i = 0; i < points; ++i) {
        int order = compare_timestamps(ak[i], bk[i], 1000, 1000, 1);
        if (order == AZ_TIME_INVALID) return -1;
        if (order != AZ_TIME_EQUAL) return 0;
    }
    return 1;
}
static int ds_cue_modified(const az_cue_entry *live, const az_cue_entry *saved, int same_key, int hot) {
    if (same_key < 0) return -1;
    if (!same_key) return 1;
    unsigned color = hot ? AZ_CUE_COLOR : AZ_CUE_COLOR_ID;
    /* Legacy rows may omit the comment. A decoded comment or the extended
     * row's color encoding qualifies the storage representation here. */
    int has_comment = !!(saved->valid & (AZ_CUE_COMMENT | color));
    if (has_comment && strncmp(live->comment, saved->comment, sizeof live->comment)) return 1;
    int complete = has_comment && strnlen(live->comment, sizeof live->comment) < sizeof live->comment - 4;
    if ((live->valid & saved->valid & color) && (!hot || saved->rgb)) {
        if (hot ? live->rgb != saved->rgb : live->color_id != saved->color_id) return 1;
    } else complete = 0;
    if (live->type == AZ_CUE_LOOP) {
        if (live->valid & saved->valid & AZ_CUE_AUTO_ACTIVE) {
            if (live->auto_active != saved->auto_active) return 1;
        } else complete = 0;
    }
    return complete ? 0 : -1;
}
static int ds_cue_saved_row(const struct ds_cell *live, unsigned row, const struct ds_cell *saved, int hot) {
    const az_cue_entry *rows = (const void *) live->data, *stored = (const void *) saved->data;
    const int64_t *key = live->clock.stored_cue_ms + 2 * row;
    if (!hot) {
        for (unsigned i = 0; i < live->info.count; ++i)
            if (i != row && ds_cue_key_same(&rows[row], key, &rows[i], live->clock.stored_cue_ms + 2 * i))
                return -2;
    }
    int found = -1;
    for (unsigned i = 0; i < saved->info.count; ++i) {
        int match = hot ? rows[row].slot == stored[i].slot
                        : ds_cue_key_same(&rows[row], key, &stored[i], saved->clock.ticks + 2 * i);
        if (match < 0 || (match && found >= 0)) return -2;
        if (match) found = (int) i;
    }
    return found;
}
static uint64_t ds_cue_storage_flags(struct ds_deck *d, unsigned field, uint64_t now, uint64_t *loss) {
    struct ds_image *im = &d->current;
    const struct ds_cell *live = ds_ready(im, AZ_STATUS_CUES, field);
    if (!live || !live->native_cue_revision || !live->clock.stored_cue_ms || live->info.count > AZ_NATIVE_CUE_LIMIT)
        return 0;
    int hot = field == AZ_STATUS_CUES_LIVE_HOT;
    const struct ds_cell *saved = im->field[AZ_STATUS_CUES][hot ? AZ_STATUS_CUES_SAVED_HOT : AZ_STATUS_CUES_SAVED_MEMORY];
    int qualified = ds_ready(im, AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK) && saved && saved->info.provider_id &&
        saved->info.info.revision && saved->info.info.provenance == AZ_PROV_FILE && saved->info.scope == AZ_STATUS_LOAD_SCOPE &&
        saved->info.scope_id == im->epoch && (saved->info.info.availability == AZ_AVAIL_EMPTY ||
        (saved->info.info.availability == AZ_AVAIL_READY && saved->clock.ticks && saved->clock.units_per_second == 1000));
    az_cue_entry rows[AZ_NATIVE_CUE_LIMIT];
    memcpy(rows, live->data, live->bytes);
    const az_cue_entry *previous = (const void *) live->data;
    int changed = 0;
    for (unsigned i = 0; i < live->info.count; ++i) {
        az_cue_entry *row = &rows[i];
        row->saved = row->modified = -1;
        row->valid &= ~AZ_CUE_SAVED;
        int found = qualified ? ds_cue_saved_row(live, i, saved, hot) : -2;
        if (found >= -1) {
            row->valid |= AZ_CUE_SAVED;
            row->saved = found >= 0;
            row->modified = 1;
            if (found >= 0) {
                const az_cue_entry *stored = (const void *) saved->data;
                int same_key = ds_cue_key_same(row, live->clock.stored_cue_ms + 2 * i, &stored[found], saved->clock.ticks + 2 * found);
                row->modified = ds_cue_modified(row, &stored[found], same_key, hot);
            }
        }
        changed |= row->valid != previous[i].valid || row->saved != previous[i].saved || row->modified != previous[i].modified;
    }
    if (!changed) return 0;
    struct ds_field next = {AZ_STATUS_CUES, field, live->info, rows, &live->clock};
    next.info.info.revision = live->native_cue_revision;
    next.info.info.observed_ns = now;
    if (ds_set_live_cues(d, &next, live->bytes, 1) == AZ_LIMIT) {
        ds_clear_field(&im->field[AZ_STATUS_CUES][field]);
        *loss |= AZ_STATUS_TOPIC(AZ_STATUS_CUES);
    }
    return AZ_STATUS_TOPIC(AZ_STATUS_CUES);
}
static const az_cue_entry *ds_cue_reference_row(const struct ds_image *im, const az_status_cue_ref_v1 *ref) {
    const struct ds_cell *collection = ref && ref->cue_field >= AZ_STATUS_CUES_LIVE_HOT &&
        ref->cue_field <= AZ_STATUS_CUES_SAVED_MEMORY ? ds_ready(im, AZ_STATUS_CUES, ref->cue_field) : NULL;
    if (!collection || ref->entry.type != AZ_CUE_LOOP || !ref->load_epoch || ref->load_epoch != im->epoch ||
        !ref->provider_id || collection->info.scope != AZ_STATUS_LOAD_SCOPE || collection->info.scope_id != im->epoch ||
        collection->info.info.revision != ref->collection_revision || collection->info.provider_id != ref->provider_id ||
        collection->info.info.provenance != ref->provenance) return NULL;
    const az_cue_entry *rows = (const void *) collection->data, *found = NULL;
    az_cue_entry expected = ref->entry;
    expected.start_s = expected.end_s = 0;
    for (unsigned i = 0; i < collection->info.count; ++i) {
        az_cue_entry row = rows[i];
        row.start_s = row.end_s = 0;
        if (memcmp(&row, &expected, sizeof row)) continue;
        if (found) return NULL;
        found = &rows[i];
    }
    /* The producer's witnessed association names this exact immutable
     * edition and row. Bounds neither prove identity nor resolve ambiguity.
     * Publish the canonical row, including its coordinates, from this cut. */
    return found;
}
static uint64_t ds_associations(struct ds_deck *d, uint64_t now, uint64_t *loss) {
    struct ds_image *im = &d->current;
    uint64_t changed = 0;
    for (unsigned i = 0; i < 2; i++) {
        unsigned field = i ? AZ_STATUS_PHRASES_SAVED_GRID : AZ_STATUS_PHRASES_CURRENT_GRID;
        const az_status_grid_ref_v1 *ref = ds_value(im, AZ_STATUS_PHRASES, field);
        const struct ds_cell *phrases = ds_ready(im, AZ_STATUS_PHRASES, i);
        if (!ref) continue;
        if (!phrases || !ds_grid_matches(im, ref, i)) {
            changed |= ds_withdraw(im, AZ_STATUS_PHRASES, field, now, loss);
            continue;
        }
        const az_phrase *rows = (const void *) phrases->data;
        const struct ds_cell *grid = ds_ready(im, AZ_STATUS_GRID, i);
        for (unsigned j = 0; j < phrases->info.count; j++) {
            if (rows[j].end_beat >= grid->info.count || rows[j].start_beat >= rows[j].end_beat ||
                (j && rows[j].start_beat < rows[j - 1].end_beat)) {
                changed |= ds_withdraw(im, AZ_STATUS_PHRASES, field, now, loss);
                break;
            }
        }
    }
    const az_status_cue_ref_v1 *ref = ds_value(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY);
    if (ref && !ds_cue_reference_row(im, ref))
        changed |= ds_withdraw(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY, now, loss);
    return changed;
}
/* A native copy witness names a bank edition and one hot slot. The copied
 * row survives the lock, and ds_associations verifies it again in the actual
 * publication in case a newer bank revision arrived in between. */
static int ds_native_loop_ref(unsigned deck, uint64_t epoch, uint64_t provider, uint64_t revision,
                              unsigned field, unsigned entry_id, az_status_cue_ref_v1 *out) {
    if (deck >= 4 || !epoch || !provider || !revision || !entry_id || !out ||
        (field != AZ_STATUS_CUES_LIVE_HOT && field != AZ_STATUS_CUES_LIVE_MEMORY) ||
        (field == AZ_STATUS_CUES_LIVE_HOT && entry_id > 8)) return 0;
    pthread_mutex_lock(&ds.mu);
    struct ds_image *im = &ds.deck[deck].current;
    const struct ds_cell *c = ds_ready(im, AZ_STATUS_CUES, field);
    int found = 0;
    if (im->epoch == epoch && c && c->info.scope == AZ_STATUS_LOAD_SCOPE && c->info.scope_id == epoch &&
        c->info.provider_id == provider && c->native_cue_revision == revision && c->info.info.provenance == AZ_PROV_NATIVE) {
        const az_cue_entry *rows = (const void *) c->data;
        for (unsigned i = 0; i < c->info.count; i++)
            if ((field == AZ_STATUS_CUES_LIVE_HOT ? rows[i].slot : rows[i].entry_id) == entry_id && rows[i].type == AZ_CUE_LOOP) {
                *out = (az_status_cue_ref_v1) {.load_epoch = epoch, .provider_id = provider, .collection_revision = c->info.info.revision, .provenance = AZ_PROV_NATIVE, .cue_field = field, .entry = rows[i]};
                found = 1;
                break;
            }
    }
    pthread_mutex_unlock(&ds.mu);
    return found;
}
