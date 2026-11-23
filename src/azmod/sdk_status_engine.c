/* SPDX-License-Identifier: GPL-3.0-only */
/* All state below belongs to reducer/UI, never to a real-time writer. Each
 * retained cut shares immutable field revisions. The separate motion slot
 * cannot evict a discrete transition. Callback pins outlive cursor removal. */
#include "sdk_status_engine.h"
#include "sdk_status_host.h"
#include "sdk_status_diagnostics.h"
#include "../common/azstatus_identity.h"
#include "../common/aztimestamp.h"
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
_Thread_local struct ds_proof_context ds_proof_context;
_Atomic unsigned ds_proof_enabled;
#define DS_HISTORY 256u
#define DS_ACTION_ROUTES (AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION))
#define DS_PHRASE_ROUTE AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION)
#define DS_MOTION_TOPICS (AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE))
/* Internal journal flag, added only after the reducer commits the cut. */
#define DS_FLUSH_MOTION 64u
uint64_t ds_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

struct ds_cell {
    unsigned refs;
    size_t bytes, clock_bytes;
    uint64_t native_cue_revision;
    az_status_field_info_v1 info;
    struct ds_time_axis clock;
    unsigned char data[]; /* aligned for every public element */
};
struct ds_image {
    struct ds_cell *field[AZ_STATUS_TOPIC_COUNT][9];
    uint64_t epoch, sequence, observed, published;
};
/* A built-in consumer receives a copied reducer cut after ds.mu is released.
 * The cut sequence lets the consumer reject notifications reordered by threads. */
static _Atomic(ds_core_load_fn) ds_core_load;
struct ds_analysis_cut {
    uint64_t order, epoch;
    az_track_ref track;
    int loaded;
};
static int ds_analysis_cell_ready(const struct ds_cell *cell, uint64_t epoch) {
    if (!cell || cell->info.info.availability != AZ_AVAIL_READY) return 0;
    return cell->info.scope == AZ_STATUS_UNIT ||
           (cell->info.scope == AZ_STATUS_LOAD_SCOPE && cell->info.scope_id == epoch);
}
static int ds_analysis_loaded(const struct ds_image *image) {
    const struct ds_cell *cell = image->field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_STATE];
    if (!image->epoch || !ds_analysis_cell_ready(cell, image->epoch)) return 0;
    if (cell->info.info.provenance != AZ_PROV_NATIVE || cell->bytes != sizeof(uint32_t)) return 0;
    uint32_t state;
    memcpy(&state, cell->data, sizeof state);
    return state == AZ_STATUS_LOAD_LOADED;
}
static struct ds_analysis_cut ds_analysis_copy(const struct ds_image *image) {
    struct ds_analysis_cut copy = {.order = image->sequence, .epoch = image->epoch,
                                   .loaded = ds_analysis_loaded(image)};
    const struct ds_cell *track = image->field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
    if (!copy.loaded || !ds_analysis_cell_ready(track, image->epoch)) return copy;
    if (track->info.scope == AZ_STATUS_LOAD_SCOPE && track->bytes == sizeof copy.track)
        memcpy(&copy.track, track->data, sizeof copy.track);
    return copy;
}
static void ds_analysis_deliver(unsigned deck, const struct ds_analysis_cut *copy) {
    ds_core_load_fn observe = atomic_load_explicit(&ds_core_load, memory_order_acquire);
    if (observe && copy->order) observe(deck, copy->order, copy->epoch, copy->loaded, &copy->track);
}
struct ds_record {
    struct ds_image image;
    uint64_t routes, lost;
    unsigned reason, motion_barrier;
    az_status_occurrence_v1 action;
    az_status_phrase_transition_v1 pair;
};
struct ds_deck {
    struct ds_image current;
    struct ds_record history[DS_HISTORY], motion;
    size_t next, length;
    uint64_t evicted[AZ_STATUS_TOPIC_COUNT], loss[4][AZ_STATUS_TOPIC_COUNT];
    uint64_t last_epoch, last_ordinal, path_order, motion_generation, stem_binding;
    uint64_t cue_revisions[2];
    uint64_t bar_provider, bar_revision, bar_epoch;
    unsigned bar_begin, bar_end;
    uint32_t head_window_samples, head_window_rate;
    int watch_stems;
};
struct ds_cursor {
    struct ds_image last;
    int initial;
    uint64_t covered, examined, last_motion_ns, evicted_motion;
};
struct ds_subscription {
    az_deck_subscription handle;
    az_deck_watch_v1 watch;
    az_deck_status_fn fn;
    void *user;
    int owner;
    struct ds_cursor cursor[4];
};
static struct {
    pthread_mutex_t mu;
    struct ds_deck deck[4];
    struct ds_subscription sub[DS_SUBS];
    uint64_t handles, stem_bindings;
    unsigned rotation;
    size_t bytes, peak;
    struct ds_metrics metrics[4];
    uint64_t slow_callbacks[DS_SUBS], max_callback_ns[DS_SUBS];
    unsigned enabled;
} ds = {.mu = PTHREAD_MUTEX_INITIALIZER};

/* Append-only evidence is enabled with the existing metrics switch. Producers
 * copy bounded scalars under ds.mu; only the metrics thread performs file I/O.
 * Exhaustion is sticky and never wraps into apparently complete evidence. */
static struct {
    unsigned enabled, count, overflow;
    uint64_t capture_base[4], revision_base[4], resync_base[4], sequence_base[4];
    struct {
        uint64_t interval, epoch, pending;
        az_track_ref source;
    } active[4];
    struct ds_proof_record record[DS_PROOF_CAPACITY];
} ds_proofs;
static struct ds_proof_record *ds_proof_new(unsigned kind, unsigned deck, uint64_t epoch, uint64_t sequence) {
    if (!ds_proofs.enabled) return NULL;
    if (ds_proofs.count == DS_PROOF_CAPACITY) {
        ds_proofs.overflow = 1;
        return NULL;
    }
    struct ds_proof_record *r = &ds_proofs.record[ds_proofs.count++];
    r->id = ds_proofs.count;
    r->kind = kind;
    r->deck = deck;
    r->epoch = epoch;
    r->sequence = sequence;
    return r;
}
static void ds_proof_context_copy(struct ds_proof_record *r, int file_only, unsigned kind) {
    if (file_only || (kind == DS_PROOF_RETIRE && ds_proof_context.kind == DS_PROOF_RETIRE))
        r->context = ds_proof_context;
}

static void ds_proof_interval(struct ds_proof_record *r, const az_track_ref *source) {
    if (source) r->source = *source;
    if (r->kind == DS_PROOF_GAP && r->file_only) {
        if (!ds_proofs.active[r->deck].interval) {
            ds_proofs.active[r->deck].interval = r->id;
            ds_proofs.active[r->deck].epoch = r->epoch;
            if (source) ds_proofs.active[r->deck].source = *source;
        }
        ds_proofs.active[r->deck].pending |= r->input_lost;
    }
    if (r->kind != DS_PROOF_GAP || r->file_only) {
        r->interval = ds_proofs.active[r->deck].interval;
        if (!source) r->source = ds_proofs.active[r->deck].source;
    }
}

static void ds_proof_settle(struct ds_proof_record *r, const struct ds_field *fields, size_t count) {
    if (r->kind == DS_PROOF_BASELINE && r->file_only && r->epoch == ds_proofs.active[r->deck].epoch) {
        uint64_t loading = 0;
        for (size_t i = 0; i < count; i++) {
            uint64_t bit = AZ_STATUS_TOPIC(fields[i].topic);
            unsigned availability = fields[i].info.info.availability;
            r->settled |= bit;
            if (availability == AZ_AVAIL_LOADING) loading |= bit;
            if (availability != AZ_AVAIL_READY && availability != AZ_AVAIL_EMPTY) r->unavailable |= bit;
        }
        r->settled &= ds_proofs.active[r->deck].pending & ~loading;
        r->unavailable &= r->settled;
        r->ready = r->settled & ~r->unavailable;
        ds_proofs.active[r->deck].pending &= ~r->settled;
    }
}

static void ds_proof_close(struct ds_proof_record *r, int retire) {
    if (r->kind == DS_PROOF_RETIRE) {
        r->context.cause |= retire ? DS_PROOF_BOUNDARY : 0;
        r->settled = r->unavailable = ds_proofs.active[r->deck].pending;
        ds_proofs.active[r->deck].pending = 0;
    }
    r->pending = ds_proofs.active[r->deck].pending;
    if (!r->pending) memset(&ds_proofs.active[r->deck], 0, sizeof ds_proofs.active[r->deck]);
}

static int ds_proof_allowed(unsigned kind, unsigned deck) {
    if (!kind || (kind != DS_PROOF_GAP && !ds_proofs.active[deck].interval)) return 0;
    if (kind == DS_PROOF_BASELINE && !(ds_proof_context.topics & ds_proofs.active[deck].pending)) return 0;
    return 1;
}

static unsigned ds_proof_cut_kind(uint64_t lost, int retire, int file_only) {
    return lost ? DS_PROOF_GAP : retire  ? DS_PROOF_RETIRE
                             : file_only ? ds_proof_context.kind
                                         : 0;
}
static void ds_proof_cut(unsigned deck, uint64_t epoch, const az_track_ref *source, int file_only,
                         unsigned flags, uint64_t input_lost, unsigned input_reason,
                         uint64_t lost, unsigned reason, uint64_t routes, const struct ds_field *fields,
                         size_t count, int retire, unsigned committed) {
    if (!ds_proofs.enabled) return;
    if (retire && lost && ds_proofs.active[deck].interval)
        ds_proof_cut(deck, epoch, NULL, 0, flags, 0, 0, 0, 0, routes, NULL, 0, 1, committed);
    unsigned kind = ds_proof_cut_kind(lost, retire, file_only);
    if (!ds_proof_allowed(kind, deck)) return;
    struct ds_proof_record *r = ds_proof_new(kind, deck, epoch, ds.deck[deck].current.sequence);
    if (!r) return;
    ds_proof_context_copy(r, file_only, kind);
    r->topics = r->context.topics;
    r->file_only = file_only;
    r->flags = flags;
    r->committed = committed;
    r->input_lost = input_lost;
    r->input_reason = input_reason;
    r->lost = lost;
    r->reason = reason;
    r->routes = routes;
    ds_proof_interval(r, source);
    ds_proof_settle(r, fields, count);
    ds_proof_close(r, retire);
}

static void ds_drop(struct ds_cell *c) {
    if (c && !--c->refs) {
        ds.bytes -= sizeof *c + c->bytes + c->clock_bytes;
        free(c);
    }
}
static const int64_t *ds_clock_ticks(const struct ds_cell *c) {
    return c ? c->clock.ticks : NULL;
}
/* Convert two exact signed source clocks to a common unsigned clock. The sign
 * bias preserves differences, including legitimate negative grid offsets. */
static int ds_compare_source_times(int64_t first, uint64_t first_rate, int64_t second,
                                   uint64_t second_rate, unsigned rate, unsigned window) {
    if (!first_rate || first_rate > UINT32_MAX || !second_rate || second_rate > UINT32_MAX)
        return AZ_TIME_INVALID;
    const __uint128_t bias = (__uint128_t) 1 << 127;
    __uint128_t a = (__uint128_t) ((__int128_t) first * second_rate) ^ bias;
    __uint128_t b = (__uint128_t) ((__int128_t) second * first_rate) ^ bias;
    return compare_timestamps(a, b, first_rate * second_rate, rate, window);
}
static int ds_axis_equal(const struct ds_time_axis *a, const struct ds_time_axis *b, unsigned index) {
    if (!a || !b || !a->ticks || !b->ticks) return 0;
    int order = ds_compare_source_times(a->ticks[index], a->units_per_second,
                                        b->ticks[index], b->units_per_second, b->window_rate, b->window_samples);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}
static int ds_beats_same(const void *before, const void *after, size_t bytes, const struct ds_time_axis *a, const struct ds_time_axis *b) {
    const az_beat *old = before, *next = after;
    for (unsigned i = 0; i < bytes / sizeof *old; i++) {
        if (!ds_axis_equal(a, b, i) || old[i].bpm != next[i].bpm ||
            old[i].beat_in_bar != next[i].beat_in_bar || old[i].reserved != next[i].reserved) return 0;
    }
    return 1;
}

static int ds_cues_same(const void *before, const void *after, size_t bytes, const struct ds_time_axis *a, const struct ds_time_axis *b) {
    const az_cue_entry *old = before, *next = after;
    for (unsigned i = 0; i < bytes / sizeof *old; i++) {
        az_cue_entry left = old[i], right = next[i];
        left.start_s = left.end_s = right.start_s = right.end_s = 0;
        if (memcmp(&left, &right, sizeof left)) return 0;
        if ((next[i].valid & AZ_CUE_START) && !ds_axis_equal(a, b, i * 2)) return 0;
        if ((next[i].valid & AZ_CUE_END) && !ds_axis_equal(a, b, i * 2 + 1)) return 0;
    }
    return 1;
}

static int ds_wave_same(const void *before, const void *after, size_t bytes, const struct ds_time_axis *a, const struct ds_time_axis *b) {
    (void) bytes;
    const az_waveform *old = before, *next = after;
    return old->sample_count == next->sample_count && ds_axis_equal(a, b, 0) &&
           !memcmp(old->samples, next->samples, next->sample_count);
}

static int ds_cue_ref_same(const void *before, const void *after, size_t bytes, const struct ds_time_axis *a, const struct ds_time_axis *b) {
    (void) bytes;
    (void) a;
    (void) b;
    const az_status_cue_ref_v1 *old = before, *next = after;
    return old->load_epoch == next->load_epoch && old->provider_id == next->provider_id &&
           old->collection_revision == next->collection_revision && old->provenance == next->provenance &&
           old->cue_field == next->cue_field && old->entry.entry_id == next->entry.entry_id &&
           old->entry.slot == next->entry.slot && old->entry.classification == next->entry.classification;
}

static int ds_coord_same(const az_status_coord_v1 *old, const az_status_coord_v1 *next, const struct ds_time_axis *b) {
    if (!b || old->rate_hz != next->rate_hz || old->reserved != next->reserved) return 0;
    int order = next->rate_hz
                    ? ds_compare_source_times(old->samples, old->rate_hz, next->samples, next->rate_hz,
                                              b->window_rate, b->window_samples)
                    : compare_timestamps(old->seconds, next->seconds, 1, b->window_rate, b->window_samples);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}

static int ds_duration_same(const void *before, const void *after, const struct ds_time_axis *a, const struct ds_time_axis *b) {
    if (!b) return 0;
    if (a && a->ticks && b->ticks) return ds_axis_equal(a, b, 0);
    int order = compare_timestamps(*(const double *) before, *(const double *) after, 1,
                                   b->window_rate, b->window_samples);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}

static int ds_coordinate_field(unsigned topic, unsigned field) {
    return (topic == AZ_STATUS_POSITION && field <= AZ_STATUS_POSITION_SLIP_HEAD) ||
           (topic == AZ_STATUS_LOOP && (field == AZ_STATUS_LOOP_RETAINED_IN || field == AZ_STATUS_LOOP_RETAINED_OUT ||
                                        field == AZ_STATUS_LOOP_WORKING_IN || field == AZ_STATUS_LOOP_WORKING_OUT));
}

static int ds_duration_field(unsigned topic, unsigned field) {
    return (topic == AZ_STATUS_METADATA && field >= AZ_STATUS_METADATA_DURATION_S) ||
           (topic == AZ_STATUS_POSITION && field >= AZ_STATUS_POSITION_REMAINING_S);
}
int ds_payload_same(unsigned topic, unsigned field, const void *before, const void *after,
                           size_t bytes, const struct ds_time_axis *a, const struct ds_time_axis *b) {
    if (!before || !after) return 0;
    unsigned type = ds_layout[topic].field[field].type;
    if (ds_coordinate_field(topic, field)) {
        return ds_coord_same(before, after, b);
    }
    if (type == DS_BEATS) return ds_beats_same(before, after, bytes, a, b);
    if (type == DS_CUES) return ds_cues_same(before, after, bytes, a, b);
    if (type == DS_WAVE) return ds_wave_same(before, after, bytes, a, b);
    if (type == DS_CUE_REF) return ds_cue_ref_same(before, after, bytes, a, b);
    if (ds_duration_field(topic, field)) {
        return ds_duration_same(before, after, a, b);
    }
    return !memcmp(before, after, bytes);
}
static void ds_select_cell(struct ds_cell **slot, struct ds_cell *next) {
    if (next == *slot) return;
    if (next) next->refs++;
    ds_drop(*slot);
    *slot = next;
}

static void ds_image_select(struct ds_image *dst, const struct ds_image *src, uint64_t topics) {
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
        if (!(topics & AZ_STATUS_TOPIC(t))) continue;
        for (unsigned f = 0; f < ds_layout[t].fields; f++) {
            ds_select_cell(&dst->field[t][f], src ? src->field[t][f] : NULL);
        }
    }
    dst->epoch = src ? src->epoch : 0;
    dst->sequence = src ? src->sequence : 0;
    dst->observed = src ? src->observed : 0;
    dst->published = src ? src->published : 0;
}
static void ds_image_copy(struct ds_image *dst, const struct ds_image *src) {
    ds_image_select(dst, src, AZ_STATUS_ALL_V1);
}

/* Only the installed audio provider receives the path positively qualified by the
 * file worker for this load. bind_stems copies it and wakes a non-audio worker.
 * It performs no I/O or callbacks into core; the reducer protects this cut. */
static void (*ds_bind_stems)(unsigned, uint64_t, uint64_t, const char *);
static int ds_stem_track_ready(const struct ds_deck *d, const struct ds_cell *ref) {
    return d->current.epoch && ref && ref->info.info.availability == AZ_AVAIL_READY;
}

static const char *ds_stem_source(const struct ds_deck *d) {
    const struct ds_cell *path = d->current.field[AZ_STATUS_METADATA][AZ_STATUS_METADATA_PATH];
    const struct ds_cell *ref = d->current.field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
    const char *src = NULL;
    if (ds_stem_track_ready(d, ref) && path &&
        path->info.scope_id == d->current.epoch && path->info.info.provenance == AZ_PROV_FILE &&
        path->info.info.availability == AZ_AVAIL_READY && path->bytes && path->data[0] == '/')
        src = (const char *) path->data;
    return src;
}

static void ds_send_stem_binding(unsigned deck) {
    struct ds_deck *d = &ds.deck[deck];
    if (!ds_bind_stems || !d->watch_stems || !d->stem_binding) return;
    const char *src = ds_stem_source(d);
    ds_bind_stems(deck, d->stem_binding, d->current.epoch, src);
}

static int ds_field_scope_valid(const struct ds_field *p, uint64_t epoch) {
    const az_status_field_info_v1 *info = &p->info;
    return p->value && info->scope_id && (info->scope == AZ_STATUS_UNIT || info->scope == AZ_STATUS_LOAD_SCOPE) &&
           (info->scope != AZ_STATUS_LOAD_SCOPE || info->scope_id == epoch);
}
static int ds_field_clock_valid(const struct ds_field *p, const struct ds_layout *layout) {
    if (!p->clock) return 1;
    return p->clock->units_per_second && p->clock->units_per_second <= UINT32_MAX &&
           (layout->type != DS_BEATS || p->clock->ticks);
}

static int ds_wave_descriptor_valid(unsigned count, const az_waveform *w) {
    if (count != 1 || w->size < sizeof *w || w->version != AZ_DATA_VERSION ||
        w->representation != AZ_WAVE_PREVIEW || !w->samples || !w->sample_count ||
        w->sample_count > 4096) return 0;
    return 1;
}

static int ds_wave_valid(const struct ds_field *p, const az_waveform *w) {
    if (!ds_wave_descriptor_valid(p->info.count, w)) return 0;
    if (!p->clock || !p->clock->ticks || !isfinite(w->seconds_per_sample)) return 0;
    return 1;
}

static size_t ds_wave_extent(const struct ds_field *p) {
    const az_waveform *w = p->value;
    if (!ds_wave_valid(p, w)) return SIZE_MAX;
    int start = compare_timestamps(w->start_s, 0., 1, p->clock->window_rate, p->clock->window_samples);
    int extent = ds_compare_source_times(p->clock->ticks[0], p->clock->units_per_second, 0, 1,
                                         p->clock->window_rate, p->clock->window_samples);
    long double period = (long double) p->clock->ticks[0] / p->clock->units_per_second / w->sample_count;
    int spacing = compare_timestamps(w->seconds_per_sample, period, 1,
                                     (uint64_t) p->clock->window_rate * w->sample_count, p->clock->window_samples);
    if (start == AZ_TIME_INVALID || extent == AZ_TIME_INVALID || spacing == AZ_TIME_INVALID ||
        start != AZ_TIME_EQUAL || extent != AZ_TIME_AFTER || spacing != AZ_TIME_EQUAL)
        return SIZE_MAX;
    return sizeof *w + w->sample_count;
}

static size_t ds_collection_extent(unsigned count, unsigned type, const size_t *sizes, size_t types) {
    if (!count || type >= types || !sizes[type]) return SIZE_MAX;
    return count * sizes[type];
}
static size_t ds_string_extent(const struct ds_field *p, const struct ds_layout *layout) {
    if (p->info.count >= layout->limit || strnlen(p->value, layout->limit) != p->info.count) return SIZE_MAX;
    return p->info.count + 1;
}

static size_t ds_value_extent(const struct ds_field *p, const struct ds_layout *layout) {
    const az_status_field_info_v1 *info = &p->info;
    static const size_t row_sizes[] = {
        [DS_CUES] = sizeof(az_cue_entry),
        [DS_BEATS] = sizeof(az_beat),
        [DS_PHRASES] = sizeof(az_phrase),
    };
    if (layout->type == DS_INLINE) return info->count == 1 ? layout->width : SIZE_MAX;
    if (layout->type == DS_CUE_REF) return info->count == 1 ? sizeof(az_status_cue_ref_v1) : SIZE_MAX;
    if (layout->type == DS_ENDPOINT) return info->count == 1 ? sizeof(az_status_phrase_endpoint_v1) : SIZE_MAX;
    if (layout->type == DS_STRING) return ds_string_extent(p, layout);
    if (layout->type == DS_WAVE) return ds_wave_extent(p);
    return ds_collection_extent(info->count, layout->type, row_sizes, sizeof row_sizes / sizeof row_sizes[0]);
}
static size_t ds_extent(const struct ds_field *p, uint64_t epoch) {
    const struct ds_layout *layout = &ds_layout[p->topic].field[p->field];
    const az_status_field_info_v1 *info = &p->info;
    if (info->info.availability > AZ_AVAIL_STALE ||
        info->info.provenance > AZ_PROV_DERIVED) return SIZE_MAX;
    if (info->info.availability != AZ_AVAIL_READY)
        return info->count ? SIZE_MAX : 0;
    if (!ds_field_scope_valid(p, epoch) || info->count > layout->limit || !ds_field_clock_valid(p, layout)) return SIZE_MAX;
    return ds_value_extent(p, layout);
}
static int ds_field_absent(const struct ds_field *b) {
    return b->info.info.availability == AZ_AVAIL_UNSUPPORTED && !b->info.scope_id &&
           !b->info.provider_id && !b->info.info.provenance && !b->info.info.revision;
}

static int ds_cell_info_same(const struct ds_cell *a, const struct ds_field *b, size_t bytes) {
    if (a->bytes != bytes || a->info.scope != b->info.scope || a->info.scope_id != b->info.scope_id ||
        a->info.provider_id != b->info.provider_id || a->info.count != b->info.count ||
        a->info.info.availability != b->info.info.availability ||
        a->info.info.provenance != b->info.info.provenance ||
        a->info.info.revision != b->info.info.revision) return 0;
    return 1;
}

static int ds_native_grid_edition_same(const struct ds_cell *a, const struct ds_field *b) {
    /* Older private map providers retain exact immutable editions but cannot
     * expose source ticks. Keep their raw lists without deriving boundaries. */
    if (b->topic == AZ_STATUS_GRID && b->info.info.provenance == AZ_PROV_NATIVE && b->info.info.revision &&
        !a->clock.ticks && (!b->clock || !b->clock->ticks)) return 1;
    return 0;
}

static int ds_native_edition_same(const struct ds_cell *a, const struct ds_field *b) {
    /* Native immutable banks carry their writer's semantic edition. Their
     * identity is independent of the public seconds representation. */
    if (b->topic == AZ_STATUS_CUES && (b->field == AZ_STATUS_CUES_LIVE_HOT || b->field == AZ_STATUS_CUES_LIVE_MEMORY) &&
        b->info.info.provenance == AZ_PROV_NATIVE && b->info.info.revision) return 1;
    return ds_native_grid_edition_same(a, b);
}

static int ds_same(const struct ds_cell *a, const struct ds_field *b, size_t bytes) {
    if (!a) return ds_field_absent(b);
    if (!ds_cell_info_same(a, b, bytes)) return 0;
    if (!bytes) return 1;
    if (ds_native_edition_same(a, b)) return 1;
    return ds_payload_same(b->topic, b->field, a->data, b->value, bytes, &a->clock, b->clock);
}
static uint64_t ds_motion_changes(const struct ds_image *before, const struct ds_image *after, uint64_t topics) {
    uint64_t changed = 0;
    const unsigned moving[] = {AZ_STATUS_POSITION, AZ_STATUS_PHASE};
    for (unsigned i = 0; i < 2; i++) {
        unsigned t = moving[i];
        if (!(topics & AZ_STATUS_TOPIC(t))) continue;
        for (unsigned f = 0; f < ds_layout[t].fields; f++)
            if (before->field[t][f] != after->field[t][f]) {
                changed |= AZ_STATUS_TOPIC(t);
                break;
            }
    }
    return changed;
}
static uint32_t ds_motion_availability(const struct ds_image *im) {
    uint32_t value = 0;
    const unsigned moving[] = {AZ_STATUS_POSITION, AZ_STATUS_PHASE};
    for (unsigned i = 0; i < 2; i++) {
        unsigned t = moving[i];
        for (unsigned f = 0; f < ds_layout[t].fields; f++) {
            const struct ds_cell *c = im->field[t][f];
            value = (value << 3) | (c ? c->info.info.availability : AZ_AVAIL_UNSUPPORTED);
        }
    }
    return value;
}
static void ds_evict_motion(struct ds_deck *d, const struct ds_record *r) {
    if (r->motion_barrier) {
        unsigned deck = (unsigned) (d - ds.deck);
        for (unsigned i = 0; i < DS_SUBS; i++) {
            struct ds_subscription *s = &ds.sub[i];
            struct ds_cursor *c = &s->cursor[deck];
            if (s->handle && (s->watch.deck_mask & (1u << deck)) && c->covered < r->image.sequence)
                c->evicted_motion |= ds_motion_changes(&c->last, &r->image, s->watch.topics);
        }
    }
}

static void ds_evict(struct ds_deck *d, struct ds_record *r) {
    ds.metrics[d - ds.deck].evictions++;
    ds_evict_motion(d, r);
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
        uint64_t bit = AZ_STATUS_TOPIC(t);
        if ((r->routes | r->lost) & bit) d->evicted[t] = r->image.sequence;
        if (r->lost & bit)
            for (unsigned b = 0; b < 4; b++)
                if (r->reason & (1u << b)) d->loss[b][t] = r->image.sequence;
    }
    ds_image_copy(&r->image, NULL);
    r->routes = r->lost = r->reason = 0;
}
static int ds_make_room(size_t bytes) {
    while (bytes > DS_BYTES - ds.bytes) {
        struct ds_record *oldest = NULL;
        struct ds_deck *owner = NULL;
        for (unsigned i = 0; i < 4; i++) {
            struct ds_deck *d = &ds.deck[i];
            if (!d->length) continue;
            struct ds_record *r = &d->history[(d->next + DS_HISTORY - d->length) % DS_HISTORY];
            int order = oldest ? compare_timestamps(r->image.published, oldest->image.published, 1000000000, 1000000000, 1) : AZ_TIME_BEFORE;
            if (order == AZ_TIME_INVALID) return 0;
            if (order == AZ_TIME_BEFORE) {
                oldest = r;
                owner = d;
            }
        }
        if (!owner) return 0;
        ds_evict(owner, oldest);
        owner->length--;
    }
    return 1;
}
struct ds_cell_extent {
    size_t padding, ticks, keys, clocks, allocation;
};
static struct ds_cell_extent ds_cell_allocation(const struct ds_field *p, size_t bytes) {
    unsigned ticks_per_row = ds_layout[p->topic].field[p->field].type == DS_CUES ? 2 : 1;
    size_t tick_bytes = bytes && p->clock && p->clock->ticks ? p->info.count * ticks_per_row * sizeof(int64_t) : 0;
    size_t padding = tick_bytes ? (sizeof(int64_t) - bytes % sizeof(int64_t)) % sizeof(int64_t) : 0;
    size_t key_bytes = tick_bytes && ticks_per_row == 2 && p->clock->stored_cue_ms ? tick_bytes : 0;
    size_t clock_bytes = padding + tick_bytes + key_bytes;
    size_t allocation = sizeof(struct ds_cell) + bytes + clock_bytes;
    return (struct ds_cell_extent) {padding, tick_bytes, key_bytes, clock_bytes, allocation};
}

static void ds_cell_initialize(struct ds_cell *c, const struct ds_field *p, size_t bytes, struct ds_cell_extent extent) {
    c->refs = 1;
    c->bytes = bytes;
    c->clock_bytes = extent.clocks;
    c->native_cue_revision = 0;
    c->info = p->info;
    c->clock = bytes && p->clock ? *p->clock : (struct ds_time_axis) {0};
    c->clock.ticks = extent.ticks ? (const void *) (c->data + bytes + extent.padding) : NULL;
    c->clock.stored_cue_ms = extent.keys ? c->clock.ticks + extent.ticks / sizeof(int64_t) : NULL;
    if (extent.ticks) memcpy(c->data + bytes + extent.padding, p->clock->ticks, extent.ticks);
    if (extent.keys) memcpy(c->data + bytes + extent.padding + extent.ticks, p->clock->stored_cue_ms, extent.keys);
}

static void ds_cell_copy_payload(struct ds_cell *c, const struct ds_field *p, size_t bytes) {
    if (bytes) {
        if (ds_layout[p->topic].field[p->field].type == DS_WAVE) {
            az_waveform *w = (void *) c->data;
            memcpy(w, p->value, sizeof *w);
            memcpy(c->data + sizeof *w, w->samples, w->sample_count);
            w->samples = c->data + sizeof *w;
        } else memcpy(c->data, p->value, bytes);
    }
}

static int ds_set(struct ds_image *im, const struct ds_field *p, size_t bytes) {
    struct ds_cell **slot = &im->field[p->topic][p->field];
    if (ds_same(*slot, p, bytes)) return 0;
    struct ds_cell_extent extent = ds_cell_allocation(p, bytes);
    size_t allocation = extent.allocation;
    if (!ds_make_room(allocation)) return AZ_LIMIT;
    struct ds_cell *c = malloc(allocation);
    if (!c) return AZ_LIMIT;
    ds_cell_initialize(c, p, bytes, extent);
    ds_cell_copy_payload(c, p, bytes);
    ds.bytes += allocation;
    if (ds.bytes > ds.peak) ds.peak = ds.bytes;
    ds_drop(*slot);
    *slot = c;
    return 1;
}

/* The public edition also changes when a native row's storage evidence
 * changes. Preserve the reducer's separate writer edition for recall proof. */
static int ds_native_cues_same(const struct ds_cell *old, const struct ds_field *p, uint64_t native) {
    if (old && old->native_cue_revision == native &&
        old->info.scope_id == p->info.scope_id && old->info.provider_id == p->info.provider_id &&
        old->info.info.availability == p->info.info.availability && old->info.count == p->info.count &&
        old->info.info.provenance == p->info.info.provenance) return 1;
    return 0;
}

static int ds_set_live_cues(struct ds_deck *d, const struct ds_field *p, size_t bytes, int annotations) {
    const struct ds_cell *old = d->current.field[AZ_STATUS_CUES][p->field];
    uint64_t native = p->info.info.revision;
    if (!annotations && ds_native_cues_same(old, p, native)) return 0;
    uint64_t *revision = &d->cue_revisions[p->field - AZ_STATUS_CUES_LIVE_HOT];
    uint64_t prior = old && old->info.info.revision > *revision ? old->info.info.revision : *revision;
    if (prior == UINT64_MAX) return AZ_LIMIT;
    struct ds_field next = *p;
    next.info.info.revision = prior + 1;
    int rc = ds_set(&d->current, &next, bytes);
    if (rc == 1) {
        *revision = next.info.info.revision;
        d->current.field[AZ_STATUS_CUES][p->field]->native_cue_revision = native;
    }
    return rc;
}
static void ds_clear_field(struct ds_cell **cell) {
    ds_drop(*cell);
    *cell = NULL;
}
static int ds_stale_eligible(const struct ds_cell *cell, unsigned t, unsigned f, int boundary, unsigned provenance) {
    int gui_tempo = t == AZ_STATUS_TEMPO && f == AZ_STATUS_TEMPO_CURRENT_BPM;
    if (!cell || (provenance && cell->info.info.provenance != provenance) || (boundary && cell->info.scope != AZ_STATUS_LOAD_SCOPE && !gui_tempo)) return 0;
    return 1;
}

static int ds_stale_field(struct ds_image *im, unsigned t, unsigned f, int boundary, uint64_t observed) {
    struct ds_cell **slot = &im->field[t][f];
    struct ds_field stale = {t, f, (*slot)->info, NULL, NULL};
    if (stale.info.info.availability != AZ_AVAIL_UNSUPPORTED)
        stale.info.info.availability = AZ_AVAIL_STALE;
    stale.info.info.observed_ns = observed;
    stale.info.count = 0;
    if (boundary && stale.info.scope == AZ_STATUS_LOAD_SCOPE) stale.info.scope_id = im->epoch;
    int rc = ds_set(im, &stale, 0);
    if (rc == AZ_LIMIT) ds_clear_field(slot);
    return rc;
}

static uint64_t ds_stale(struct ds_image *im, uint64_t mask, int boundary, unsigned provenance, uint64_t observed) {
    uint64_t changed = 0;
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
        if (!(mask & AZ_STATUS_TOPIC(t))) continue;
        for (unsigned f = 0; f < ds_layout[t].fields; f++) {
            struct ds_cell **slot = &im->field[t][f];
            if (!ds_stale_eligible(*slot, t, f, boundary, provenance)) continue;
            int rc = ds_stale_field(im, t, f, boundary, observed);
            if (rc) changed |= AZ_STATUS_TOPIC(t);
        }
    }
    return changed;
}

static struct ds_record *ds_append_slot(struct ds_deck *d, unsigned flags, uint64_t routes, const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair, uint64_t lost) {
    struct ds_record *r = &d->motion;
    if (!(flags & DS_MOTION) || (flags & DS_FLUSH_MOTION) || (routes & ~DS_MOTION_TOPICS) || lost || action || pair) {
        r = &d->history[d->next];
        if (d->length == DS_HISTORY) ds_evict(d, r);
        d->next = (d->next + 1) % DS_HISTORY;
        if (d->length < DS_HISTORY) d->length++;
    }
    return r;
}

static void ds_append(struct ds_deck *d, unsigned flags, uint64_t routes,
                      const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair,
                      uint64_t lost, unsigned reason) {
    struct ds_record *r = ds_append_slot(d, flags, routes, action, pair, lost);
    struct ds_metrics *metric = &ds.metrics[d - ds.deck];
    if (metric->high_water < d->length) metric->high_water = d->length;
    if (reason & AZ_STATUS_CAPTURE_GAP) metric->capture_gaps++;
    if (reason & AZ_STATUS_REVISION_LOST) metric->revision_losses++;
    ds_image_copy(&r->image, &d->current);
    r->routes = routes;
    r->lost = lost;
    r->reason = reason;
    r->motion_barrier = !!(flags & DS_FLUSH_MOTION);
    r->action = action ? *action : (az_status_occurrence_v1) {0};
    r->pair = pair ? *pair : (az_status_phrase_transition_v1) {0};
}

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
static struct ds_field ds_derived_field(struct ds_deck *deck, unsigned t, unsigned f, uint64_t now, const void *v, const struct ds_cell *old) {
    return (struct ds_field) {t, f, {.info = {v ? AZ_AVAIL_READY : AZ_AVAIL_STALE, AZ_PROV_DERIVED, old ? old->info.info.revision : 0, now}, .scope = AZ_STATUS_LOAD_SCOPE, .count = v ? 1u : 0u, .scope_id = deck->current.epoch, .provider_id = AZSP_DERIVED + (deck - ds.deck) + 1}, v, NULL};
}

static void ds_commit_derived(struct ds_image *im, struct ds_field *p, size_t bytes, uint64_t *loss) {
    int exhausted = p->info.info.revision == UINT64_MAX;
    if (!exhausted) {
        p->info.info.revision++;
        exhausted = ds_set(im, p, bytes) == AZ_LIMIT;
    }
    if (exhausted) {
        ds_clear_field(&im->field[p->topic][p->field]);
        *loss |= AZ_STATUS_TOPIC(p->topic);
    }
}

static int ds_derived(struct ds_deck *deck, unsigned t, unsigned f, uint64_t now,
                      const void *v, size_t bytes, uint64_t *loss) {
    struct ds_image *im = &deck->current;
    const struct ds_cell *old = im->field[t][f];
    if (!v && (!old || old->info.info.provenance != AZ_PROV_DERIVED)) return 0;
    struct ds_field p = ds_derived_field(deck, t, f, now, v, old);
    struct ds_time_axis clock = {.units_per_second = 1, .window_samples = deck->head_window_samples, .window_rate = deck->head_window_rate};
    if (t == AZ_STATUS_POSITION && f == AZ_STATUS_POSITION_REMAINING_S) p.clock = &clock;
    if (v && ds_layout[t].field[f].type == DS_STRING) p.info.count = (unsigned) bytes - 1;
    if (ds_same(old, &p, bytes)) return 0;
    ds_commit_derived(im, &p, bytes, loss);
    return 1;
}
/* EP147's native Camelot formatter uses the relative major pitch class for
 * minor keys. Preserve the stored spelling in KEY.original. */
static int ds_camelot_pitch(const char *text, int *pitch, int *major) {
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
    return 0;
}

static const char *ds_accidental(const char *text, int *note) {
    if (*text == '#') {
        ++*note;
        ++text;
    } else if (*text == 'b') {
        --*note;
        ++text;
    } else if (!strncmp(text, "♯", 3)) {
        ++*note;
        text += 3;
    } else if (!strncmp(text, "♭", 3)) {
        --*note;
        text += 3;
    }
    return text;
}

static int ds_key_mode(const char *text, int *major) {
    static const struct {
        const char *text;
        int major;
    } modes[] = {
        {"", 1},
        {"maj", 1},
        {"major", 1},
        {"Major", 1},
        {"m", 0},
        {"min", 0},
        {"minor", 0},
        {"Minor", 0},
    };
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i)
        if (!strcmp(text, modes[i].text)) {
            *major = modes[i].major;
            return 1;
        }
    return 0;
}

static int ds_pitch(const char *text, int *pitch, int *major) {
    if (!text || !*text) return 0;
    if (ds_camelot_pitch(text, pitch, major)) return 1;
    static const unsigned note[] = {9, 11, 0, 2, 4, 5, 7}; /* A..G */
    if (*text < 'A' || *text > 'G') return 0;
    int n = note[*text++ - 'A'];
    text = ds_accidental(text, &n);
    while (*text == ' ') ++text;
    if (!ds_key_mode(text, major)) return 0;
    *pitch = (n + (*major ? 12 : 15)) % 12;
    return 1;
}
static int ds_native_key_present(const struct ds_image *im) {
    const struct ds_cell *actual = im->field[AZ_STATUS_KEY][AZ_STATUS_KEY_EFFECTIVE];
    if (actual && actual->info.info.provenance == AZ_PROV_NATIVE &&
        actual->info.scope_id == im->epoch && actual->info.info.availability != AZ_AVAIL_LOADING &&
        actual->info.info.availability != AZ_AVAIL_UNSUPPORTED) return 1;
    return 0;
}

static uint64_t ds_effective_key(struct ds_deck *d, uint64_t now, uint64_t *loss) {
    const struct ds_image *im = &d->current;
    if (ds_native_key_present(im)) return 0;
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
static double ds_remaining_overshoot(const struct ds_deck *d, const az_status_coord_v1 *head, const double *precision, long double difference) {
    int ended = ds_integer_is(&d->current, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, AZ_PLAYBACK_ENDED);
    double resolution = 0;
    if (precision) {
        int precision_order = compare_timestamps(*precision, 0., 1, d->head_window_rate, d->head_window_samples);
        if (precision_order != AZ_TIME_INVALID && precision_order == AZ_TIME_AFTER) resolution = *precision;
    }
    if (ended && head->rate_hz) resolution += 1.0 / head->rate_hz;
    int extent = compare_timestamps(-difference, resolution, 1,
                                    d->head_window_rate, d->head_window_samples);
    if (extent != AZ_TIME_INVALID && (extent == AZ_TIME_EQUAL || extent == AZ_TIME_BEFORE)) return 0;
    return NAN;
}
static int ds_time_nonnegative(int order) {
    return order != AZ_TIME_INVALID && (order == AZ_TIME_EQUAL || order == AZ_TIME_AFTER);
}
static double ds_remaining_value(const struct ds_deck *d, const double *duration, const double *precision, const az_status_coord_v1 *head, const struct ds_time_axis *clock, int exact) {
    double remaining = NAN;
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
        remaining = ds_remaining_overshoot(d, head, precision, difference);
    }
    return remaining;
}
static int ds_duration_origin(const struct ds_deck *d, const double *duration, const struct ds_time_axis *clock, int exact) {
    return exact ? ds_compare_source_times(clock->ticks[0], clock->units_per_second, 0, 1,
                                           d->head_window_rate, d->head_window_samples)
                 : compare_timestamps(*duration, 0., 1, d->head_window_rate, d->head_window_samples);
}
static double ds_remaining_time(const struct ds_deck *d, const double *duration, const double *precision, const az_status_coord_v1 *head) {
    if (!duration || !head || !d->head_window_samples || !d->head_window_rate) return NAN;
    const struct ds_cell *source = ds_ready(&d->current, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S);
    const struct ds_time_axis *clock = &source->clock;
    int exact = clock->ticks && head->rate_hz;
    int duration_zero = ds_duration_origin(d, duration, clock, exact);
    int head_zero = head->rate_hz ? compare_timestamps(head->samples, 0, head->rate_hz, d->head_window_rate, d->head_window_samples)
                                  : compare_timestamps(head->seconds, 0., 1, d->head_window_rate, d->head_window_samples);
    if (!ds_time_nonnegative(duration_zero) || !ds_time_nonnegative(head_zero)) return NAN;
    /* INVALID leaves remaining unavailable rather than guessing a clock. */
    return ds_remaining_value(d, duration, precision, head, clock, exact);
}
static double ds_estimated_time(const struct ds_image *im, double remaining, const double *rate) {
    if (isfinite(remaining) && rate && isfinite(*rate) && *rate > 0 &&
        ds_integer_is(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, AZ_PLAYBACK_PLAYING) &&
        ds_integer_is(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_DIRECTION, AZ_STATUS_DIRECTION_FORWARD) &&
        ds_integer_is(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED, 0) &&
        ds_integer_is(im, AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_SLIP, 0)) return remaining / *rate;
    return NAN;
}
static int ds_estimate_same(const struct ds_deck *d, const double *rate, double estimated) {
    const struct ds_image *im = &d->current;
    const double *old_estimated = ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_ESTIMATED_S);
    int same_estimate = 0;
    if (old_estimated && isfinite(estimated)) {
        /* Convert both wall-time estimates back to the measured source clock.
         * Its current playback ratio determines this observation's window. */
        int order = compare_timestamps((long double) *old_estimated * *rate, (long double) estimated * *rate,
                                       1, d->head_window_rate, d->head_window_samples);
        same_estimate = order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
    }
    return same_estimate;
}
static uint64_t ds_times(struct ds_deck *d, uint64_t now, uint64_t *loss) {
    const struct ds_image *im = &d->current;
    const double *duration = ds_value(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S);
    const double *precision = ds_value(im, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_PRECISION_S);
    const az_status_coord_v1 *head = ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    const double *rate = ds_value(im, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE);
    double remaining = ds_remaining_time(d, duration, precision, head);
    double estimated = ds_estimated_time(im, remaining, rate);
    int r = ds_derived(d, AZ_STATUS_POSITION, AZ_STATUS_POSITION_REMAINING_S, now,
                       isfinite(remaining) ? &remaining : NULL, isfinite(remaining) ? sizeof remaining : 0, loss);
    int same_estimate = ds_estimate_same(d, rate, estimated);
    int e = same_estimate ? 0 : ds_derived(d, AZ_STATUS_POSITION, AZ_STATUS_POSITION_ESTIMATED_S, now, isfinite(estimated) ? &estimated : NULL, isfinite(estimated) ? sizeof estimated : 0, loss);
    return (r || e) ? AZ_STATUS_TOPIC(AZ_STATUS_POSITION) : 0;
}
static int ds_bar_cached(const struct ds_deck *d, const struct ds_cell *grid, unsigned first) {
    return d->bar_provider == grid->info.provider_id && d->bar_revision == grid->info.info.revision &&
           d->bar_epoch == grid->info.scope_id && d->bar_begin == first && d->bar_end;
}
static void ds_find_bar(struct ds_deck *d, const struct ds_cell *grid, const az_beat *rows, unsigned first) {
    if (ds_bar_cached(d, grid, first)) return;
    unsigned end = first + 1;
    while (end < grid->info.count && rows[end].beat_in_bar == end - first + 1) end++;
    d->bar_provider = grid->info.provider_id;
    d->bar_revision = grid->info.info.revision;
    d->bar_epoch = grid->info.scope_id;
    d->bar_begin = first;
    d->bar_end = end < grid->info.count && rows[end].beat_in_bar == 1 ? end : UINT_MAX;
}

static double ds_bar_fraction(struct ds_deck *d, const struct ds_cell *grid, unsigned index, double beat) {
    const az_beat *rows = (const void *) grid->data;
    unsigned ordinal = rows[index].beat_in_bar;
    if (!ordinal || ordinal - 1 > index) return NAN;
    unsigned first = index - (ordinal - 1);
    if (rows[first].beat_in_bar != 1) return NAN;
    ds_find_bar(d, grid, rows, first);
    if (d->bar_end == UINT_MAX || index >= d->bar_end) return NAN;
    return (index - first + beat) / (d->bar_end - first);
}
static void ds_phase_bracket(const az_status_coord_v1 *head, const int64_t *ticks, uint64_t units, unsigned rate, unsigned window, unsigned *lo, unsigned *hi) {
    while (*hi - *lo > 1) {
        unsigned mid = *lo + (*hi - *lo) / 2;
        int order = ds_compare_source_times(head->samples, head->rate_hz, ticks[mid], units, rate, window);
        if (order == AZ_TIME_INVALID) break;
        if (order == AZ_TIME_BEFORE) *hi = mid;
        else *lo = mid;
    }
}
static double ds_phase_interpolate(const az_status_coord_v1 *head, const int64_t *ticks, uint64_t units, unsigned lo, unsigned hi, int lower, int upper, unsigned *at) {
    if (upper == AZ_TIME_EQUAL) {
        *at = hi;
        return 0;
    } else if (lower == AZ_TIME_EQUAL) return 0;
    else if (lower == AZ_TIME_AFTER && upper == AZ_TIME_BEFORE) {
        __int128_t delta = (__int128_t) head->samples * units - (__int128_t) ticks[lo] * head->rate_hz;
        __int128_t length = ((__int128_t) ticks[hi] - ticks[lo]) * head->rate_hz;
        return (double) ((long double) delta / (long double) length);
    }
    return NAN;
}
static int ds_phase_within(int first, int last) {
    return first != AZ_TIME_INVALID && last != AZ_TIME_INVALID &&
           (first == AZ_TIME_EQUAL || first == AZ_TIME_AFTER) && (last == AZ_TIME_EQUAL || last == AZ_TIME_BEFORE);
}
static void ds_phase_from_grid(struct ds_deck *d, const struct ds_cell *grid, const az_status_coord_v1 *head, double *phase, double *bar) {
    const int64_t *ticks = ds_clock_ticks(grid);
    uint64_t units = grid->clock.units_per_second;
    unsigned rate = d->head_window_rate, window = d->head_window_samples;
    unsigned lo = 0, hi = grid->info.count - 1;
    int first = ds_compare_source_times(head->samples, head->rate_hz, ticks[lo], units, rate, window);
    int last = ds_compare_source_times(head->samples, head->rate_hz, ticks[hi], units, rate, window);
    if (!ds_phase_within(first, last)) return;
    ds_phase_bracket(head, ticks, units, rate, window, &lo, &hi);
    int span = ds_compare_source_times(ticks[hi], units, ticks[lo], units, rate, window);
    if (span == AZ_TIME_INVALID || span != AZ_TIME_AFTER) return;
    unsigned at = lo;
    int lower = ds_compare_source_times(head->samples, head->rate_hz, ticks[lo], units, rate, window);
    int upper = ds_compare_source_times(head->samples, head->rate_hz, ticks[hi], units, rate, window);
    if (lower == AZ_TIME_INVALID || upper == AZ_TIME_INVALID) return;
    *phase = ds_phase_interpolate(head, ticks, units, lo, hi, lower, upper, &at);
    if (isfinite(*phase) && *phase >= 0 && *phase < 1) *bar = ds_bar_fraction(d, grid, at, *phase);
}
static int ds_phase_inputs(const struct ds_deck *d, const struct ds_cell *grid, const az_status_coord_v1 *head) {
    return grid && grid->clock.ticks && head && head->rate_hz && grid->info.count >= 2 &&
           d->head_window_samples && d->head_window_rate;
}
static uint64_t ds_phase(struct ds_deck *d, uint64_t now, uint64_t *loss) {
    const struct ds_image *im = &d->current;
    const struct ds_cell *grid = ds_ready(im, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    const az_status_coord_v1 *head = ds_value(im, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD);
    double phase = NAN, bar = NAN;
    if (ds_phase_inputs(d, grid, head)) ds_phase_from_grid(d, grid, head, &phase, &bar);
    int beat_changed = ds_derived(d, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BEAT, now, isfinite(phase) ? &phase : NULL,
                                  isfinite(phase) ? sizeof phase : 0, loss);
    int bar_changed = ds_derived(d, AZ_STATUS_PHASE, AZ_STATUS_PHASE_BAR, now, isfinite(bar) ? &bar : NULL,
                                 isfinite(bar) ? sizeof bar : 0, loss);
    return beat_changed || bar_changed ? AZ_STATUS_TOPIC(AZ_STATUS_PHASE) : 0;
}
static int ds_reference_edition(const struct ds_cell *cell, uint64_t epoch, uint64_t provider, uint64_t revision, unsigned provenance) {
    return cell && cell->info.scope == AZ_STATUS_LOAD_SCOPE && cell->info.scope_id == epoch &&
           provider && provider == cell->info.provider_id && revision == cell->info.info.revision &&
           provenance == cell->info.info.provenance;
}
static int ds_grid_matches(const struct ds_image *im, const az_status_grid_ref_v1 *ref, unsigned field) {
    if (ref->grid_field != field || !ref->load_epoch || ref->load_epoch != im->epoch) return 0;
    return ds_reference_edition(ds_ready(im, AZ_STATUS_GRID, field), im->epoch, ref->provider_id, ref->revision, ref->provenance);
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
static int ds_cue_comment_modified(const az_cue_entry *live, const az_cue_entry *saved, unsigned color) {
    /* Legacy rows may omit the comment. A decoded comment or the extended
     * row's color encoding qualifies the storage representation here. */
    int has_comment = !!(saved->valid & (AZ_CUE_COMMENT | color));
    if (!has_comment) return -1;
    if (strncmp(live->comment, saved->comment, sizeof live->comment)) return 1;
    return strnlen(live->comment, sizeof live->comment) < sizeof live->comment - 4 ? 0 : -1;
}
static int ds_cue_color_modified(const az_cue_entry *live, const az_cue_entry *saved, unsigned color, int hot) {
    if (!(live->valid & saved->valid & color) || (hot && !saved->rgb)) return -1;
    return hot ? live->rgb != saved->rgb : live->color_id != saved->color_id;
}
static int ds_cue_auto_modified(const az_cue_entry *live, const az_cue_entry *saved) {
    if (live->type != AZ_CUE_LOOP) return 0;
    if (!(live->valid & saved->valid & AZ_CUE_AUTO_ACTIVE)) return -1;
    return live->auto_active != saved->auto_active;
}
static int ds_cue_representation_modified(const az_cue_entry *live, const az_cue_entry *saved, int hot) {
    unsigned color = hot ? AZ_CUE_COLOR : AZ_CUE_COLOR_ID;
    int comment = ds_cue_comment_modified(live, saved, color);
    int encoding = ds_cue_color_modified(live, saved, color, hot);
    int automatic = ds_cue_auto_modified(live, saved);
    if (comment > 0 || encoding > 0 || automatic > 0) return 1;
    return comment < 0 || encoding < 0 || automatic < 0 ? -1 : 0;
}
static int ds_cue_modified(const az_cue_entry *live, const az_cue_entry *saved, int same_key, int hot) {
    if (same_key < 0) return -1;
    if (!same_key) return 1;
    return ds_cue_representation_modified(live, saved, hot);
}
static int ds_cue_live_duplicate(const struct ds_cell *live, unsigned row) {
    const az_cue_entry *rows = (const void *) live->data;
    const int64_t *key = live->clock.stored_cue_ms + 2 * row;
    for (unsigned i = 0; i < live->info.count; ++i)
        if (i != row && ds_cue_key_same(&rows[row], key, &rows[i], live->clock.stored_cue_ms + 2 * i)) return 1;
    return 0;
}
static int ds_cue_saved_row(const struct ds_cell *live, unsigned row, const struct ds_cell *saved, int hot) {
    const az_cue_entry *rows = (const void *) live->data, *stored = (const void *) saved->data;
    const int64_t *key = live->clock.stored_cue_ms + 2 * row;
    if (!hot && ds_cue_live_duplicate(live, row)) return -2;
    int found = -1;
    for (unsigned i = 0; i < saved->info.count; ++i) {
        int match = hot ? rows[row].slot == stored[i].slot
                        : ds_cue_key_same(&rows[row], key, &stored[i], saved->clock.ticks + 2 * i);
        if (match < 0 || (match && found >= 0)) return -2;
        if (match) found = (int) i;
    }
    return found;
}
static int ds_saved_cues_available(const struct ds_cell *saved) {
    if (saved->info.info.availability == AZ_AVAIL_EMPTY) return 1;
    return saved->info.info.availability == AZ_AVAIL_READY && saved->clock.ticks && saved->clock.units_per_second == 1000;
}
static int ds_saved_cues_qualified(const struct ds_image *im, const struct ds_cell *saved) {
    return ds_ready(im, AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK) && saved && saved->info.provider_id &&
           saved->info.info.revision && saved->info.info.provenance == AZ_PROV_FILE && saved->info.scope == AZ_STATUS_LOAD_SCOPE &&
           saved->info.scope_id == im->epoch && ds_saved_cues_available(saved);
}
static void ds_cue_annotate(az_cue_entry *row, const struct ds_cell *live, unsigned i, const struct ds_cell *saved, int hot, int qualified) {
    row->saved = row->modified = -1;
    row->valid &= ~AZ_CUE_SAVED;
    int found = qualified ? ds_cue_saved_row(live, i, saved, hot) : -2;
    if (found < -1) return;
    row->valid |= AZ_CUE_SAVED;
    row->saved = found >= 0;
    row->modified = 1;
    if (found >= 0) {
        const az_cue_entry *stored = (const void *) saved->data;
        int same_key = ds_cue_key_same(row, live->clock.stored_cue_ms + 2 * i, &stored[found], saved->clock.ticks + 2 * found);
        row->modified = ds_cue_modified(row, &stored[found], same_key, hot);
    }
}
static int ds_cue_storage_rows(const struct ds_cell *live, const struct ds_cell *saved, int hot, int qualified, az_cue_entry *rows) {
    const az_cue_entry *previous = (const void *) live->data;
    memcpy(rows, live->data, live->bytes);
    int changed = 0;
    for (unsigned i = 0; i < live->info.count; ++i) {
        ds_cue_annotate(&rows[i], live, i, saved, hot, qualified);
        changed |= rows[i].valid != previous[i].valid || rows[i].saved != previous[i].saved || rows[i].modified != previous[i].modified;
    }
    return changed;
}
static uint64_t ds_cue_storage_flags(struct ds_deck *d, unsigned field, uint64_t now, uint64_t *loss) {
    struct ds_image *im = &d->current;
    const struct ds_cell *live = ds_ready(im, AZ_STATUS_CUES, field);
    if (!live || !live->native_cue_revision || !live->clock.stored_cue_ms || live->info.count > AZ_NATIVE_CUE_LIMIT) return 0;
    int hot = field == AZ_STATUS_CUES_LIVE_HOT;
    const struct ds_cell *saved = im->field[AZ_STATUS_CUES][hot ? AZ_STATUS_CUES_SAVED_HOT : AZ_STATUS_CUES_SAVED_MEMORY];
    az_cue_entry rows[AZ_NATIVE_CUE_LIMIT];
    if (!ds_cue_storage_rows(live, saved, hot, ds_saved_cues_qualified(im, saved), rows)) return 0;
    struct ds_field next = {AZ_STATUS_CUES, field, live->info, rows, &live->clock};
    next.info.info.revision = live->native_cue_revision;
    next.info.info.observed_ns = now;
    if (ds_set_live_cues(d, &next, live->bytes, 1) == AZ_LIMIT) {
        ds_clear_field(&im->field[AZ_STATUS_CUES][field]);
        *loss |= AZ_STATUS_TOPIC(AZ_STATUS_CUES);
    }
    return AZ_STATUS_TOPIC(AZ_STATUS_CUES);
}
static const az_cue_entry *ds_cue_canonical_row(const struct ds_cell *collection, const az_cue_entry *entry) {
    const az_cue_entry *rows = (const void *) collection->data, *found = NULL;
    az_cue_entry expected = *entry;
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
static const az_cue_entry *ds_cue_reference_row(const struct ds_image *im, const az_status_cue_ref_v1 *ref) {
    if (!ref || ref->cue_field < AZ_STATUS_CUES_LIVE_HOT || ref->cue_field > AZ_STATUS_CUES_SAVED_MEMORY) return NULL;
    if (ref->entry.type != AZ_CUE_LOOP || !ref->load_epoch || ref->load_epoch != im->epoch) return NULL;
    const struct ds_cell *collection = ds_ready(im, AZ_STATUS_CUES, ref->cue_field);
    if (!ds_reference_edition(collection, im->epoch, ref->provider_id, ref->collection_revision, ref->provenance)) return NULL;
    return ds_cue_canonical_row(collection, &ref->entry);
}
static int ds_phrase_bounds(const struct ds_cell *phrases, const struct ds_cell *grid) {
    const az_phrase *rows = (const void *) phrases->data;
    for (unsigned j = 0; j < phrases->info.count; j++)
        if (rows[j].end_beat >= grid->info.count || rows[j].start_beat >= rows[j].end_beat ||
            (j && rows[j].start_beat < rows[j - 1].end_beat)) return 0;
    return 1;
}
static int ds_phrase_grid_valid(const struct ds_image *im, const az_status_grid_ref_v1 *ref, unsigned field) {
    const struct ds_cell *phrases = ds_ready(im, AZ_STATUS_PHRASES, field);
    if (!phrases || !ds_grid_matches(im, ref, field)) return 0;
    return ds_phrase_bounds(phrases, ds_ready(im, AZ_STATUS_GRID, field));
}
static uint64_t ds_associations(struct ds_deck *d, uint64_t now, uint64_t *loss) {
    struct ds_image *im = &d->current;
    uint64_t changed = 0;
    for (unsigned i = 0; i < 2; i++) {
        unsigned field = i ? AZ_STATUS_PHRASES_SAVED_GRID : AZ_STATUS_PHRASES_CURRENT_GRID;
        const az_status_grid_ref_v1 *ref = ds_value(im, AZ_STATUS_PHRASES, field);
        if (ref && !ds_phrase_grid_valid(im, ref, i)) changed |= ds_withdraw(im, AZ_STATUS_PHRASES, field, now, loss);
    }
    const az_status_cue_ref_v1 *ref = ds_value(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY);
    if (ref && !ds_cue_reference_row(im, ref)) changed |= ds_withdraw(im, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY, now, loss);
    return changed;
}
/* A native copy witness names a bank edition and one hot slot. The copied
 * row survives the lock, and ds_associations verifies it again in the actual
 * publication in case a newer bank revision arrived in between. */
static int ds_native_loop_identity(unsigned deck, uint64_t epoch, uint64_t provider, uint64_t revision, unsigned field, unsigned entry_id, const az_status_cue_ref_v1 *out) {
    if (deck >= 4 || !epoch || !provider || !revision || !entry_id || !out) return 0;
    if (field == AZ_STATUS_CUES_LIVE_HOT) return entry_id <= 8;
    return field == AZ_STATUS_CUES_LIVE_MEMORY;
}
static int ds_native_cue_edition(const struct ds_image *im, const struct ds_cell *cell, uint64_t epoch, uint64_t provider, uint64_t revision) {
    return im->epoch == epoch && cell && cell->info.scope == AZ_STATUS_LOAD_SCOPE && cell->info.scope_id == epoch &&
           cell->info.provider_id == provider && cell->native_cue_revision == revision && cell->info.info.provenance == AZ_PROV_NATIVE;
}
static int ds_native_cue_reference(const struct ds_cell *cell, uint64_t epoch, uint64_t provider, unsigned field, unsigned entry_id, az_status_cue_ref_v1 *out) {
    const az_cue_entry *rows = (const void *) cell->data;
    for (unsigned i = 0; i < cell->info.count; i++)
        if ((field == AZ_STATUS_CUES_LIVE_HOT ? rows[i].slot : rows[i].entry_id) == entry_id && rows[i].type == AZ_CUE_LOOP) {
            *out = (az_status_cue_ref_v1) {.load_epoch = epoch, .provider_id = provider, .collection_revision = cell->info.info.revision, .provenance = AZ_PROV_NATIVE, .cue_field = field, .entry = rows[i]};
            return 1;
        }
    return 0;
}
int ds_native_loop_ref(unsigned deck, uint64_t epoch, uint64_t provider, uint64_t revision,
                              unsigned field, unsigned entry_id, az_status_cue_ref_v1 *out) {
    if (!ds_native_loop_identity(deck, epoch, provider, revision, field, entry_id, out)) return 0;
    pthread_mutex_lock(&ds.mu);
    struct ds_image *im = &ds.deck[deck].current;
    const struct ds_cell *c = ds_ready(im, AZ_STATUS_CUES, field);
    int found = 0;
    if (ds_native_cue_edition(im, c, epoch, provider, revision)) found = ds_native_cue_reference(c, epoch, provider, field, entry_id, out);
    pthread_mutex_unlock(&ds.mu);
    return found;
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* This private input is for a common writer fence. A current-value getter, a
 * change in sign or a newer receipt timestamp cannot construct a ds_path. */
struct ds_map {
    const az_phrase *rows;
    const struct ds_time_axis *clock;
    unsigned count;
    uint64_t provider, revision;
    az_status_grid_ref_v1 grid;
};
static int ds_path_map_edition(const struct ds_cell *phrases, const az_status_grid_ref_v1 *grid, const struct ds_path *p) {
    return phrases->info.provider_id == p->phrase_provider && phrases->info.info.revision == p->phrase_revision &&
           p->phrase_provider && p->phrase_revision && !memcmp(grid, &p->grid, sizeof *grid);
}
static int ds_map_at(const struct ds_image *im, const struct ds_path *p, struct ds_map *map) {
    const struct ds_cell *phrases = ds_ready(im, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT);
    const struct ds_cell *beats = ds_ready(im, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    const az_status_grid_ref_v1 *grid = ds_value(im, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID);
    if (!phrases || !beats || !beats->clock.ticks || !grid || !ds_grid_matches(im, grid, AZ_STATUS_GRID_CURRENT) ||
        !ds_path_map_edition(phrases, grid, p)) return 0;
    *map = (struct ds_map) {(const void *) phrases->data, &beats->clock, phrases->info.count,
                            p->phrase_provider, p->phrase_revision, *grid};
    return 1;
}
static az_status_phrase_endpoint_v1 ds_endpoint(const struct ds_map *m, unsigned kind, unsigned index) {
    az_status_phrase_endpoint_v1 e = {.kind = kind, .phrase_provider_id = m->provider, .phrase_revision = m->revision, .grid = m->grid};
    if (kind == AZ_PHRASE_ITEM) {
        e.index = index;
        e.phrase = m->rows[index];
    }
    return e;
}
static az_status_coord_v1 ds_grid_point(const struct ds_map *m, unsigned index) {
    return (az_status_coord_v1) {.samples = m->clock->ticks[index], .rate_hz = m->clock->units_per_second};
}
static int ds_coord_order(const az_status_coord_v1 *first, const az_status_coord_v1 *second,
                          unsigned rate, unsigned window) {
    return ds_compare_source_times(first->samples, first->rate_hz, second->samples, second->rate_hz, rate, window);
}
static int ds_opening_at(const struct ds_map *m, const az_status_coord_v1 *point, unsigned rate, unsigned window) {
    az_status_coord_v1 zero = {.rate_hz = point->rate_hz}, first = ds_grid_point(m, m->rows[0].start_beat);
    int at_zero = ds_coord_order(point, &zero, rate, window);
    int at_first = ds_coord_order(point, &first, rate, window);
    if (at_zero == AZ_TIME_INVALID || at_first == AZ_TIME_INVALID) return 0;
    return at_zero == AZ_TIME_EQUAL || at_first == AZ_TIME_EQUAL;
}
static unsigned ds_phrase_at(const struct ds_map *m, const az_status_coord_v1 *point, unsigned rate, unsigned window) {
    unsigned lo = 0, hi = m->count;
    while (lo + 1 < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        az_status_coord_v1 edge = ds_grid_point(m, m->rows[mid].start_beat);
        int order = ds_coord_order(point, &edge, rate, window);
        if (order == AZ_TIME_INVALID) return UINT_MAX;
        if (order == AZ_TIME_BEFORE) hi = mid;
        else lo = mid;
    }
    return lo;
}
static az_status_phrase_endpoint_v1 ds_at(const struct ds_map *m, const az_status_coord_v1 *point,
                                          int opening, int ended, unsigned rate, unsigned window) {
    az_status_coord_v1 edge = ds_grid_point(m, m->rows[0].start_beat);
    int first = ds_coord_order(point, &edge, rate, window);
    if (first == AZ_TIME_INVALID) return (az_status_phrase_endpoint_v1) {0};
    if (ended) return ds_endpoint(m, AZ_PHRASE_END, 0);
    if ((opening && ds_opening_at(m, point, rate, window)) || first == AZ_TIME_BEFORE) return ds_endpoint(m, AZ_PHRASE_START, 0);
    unsigned lo = ds_phrase_at(m, point, rate, window);
    if (lo == UINT_MAX) return (az_status_phrase_endpoint_v1) {0};
    edge = ds_grid_point(m, m->rows[lo].end_beat);
    int end = ds_coord_order(point, &edge, rate, window);
    if (end == AZ_TIME_INVALID) return (az_status_phrase_endpoint_v1) {0};
    if (end == AZ_TIME_BEFORE) return ds_endpoint(m, AZ_PHRASE_ITEM, lo);
    return ds_endpoint(m, AZ_PHRASE_GAP, 0);
}
static int ds_terminal_edge(const struct ds_map *m, const struct ds_path *p, unsigned row) {
    if (!p->defer_terminal || row + 1 != m->count) return 0;
    az_status_coord_v1 edge = ds_grid_point(m, m->rows[row].end_beat);
    int order = ds_coord_order(&edge, &p->terminal, p->observed_window_rate, p->observed_window_samples);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}
static int ds_endpoint_known(const struct ds_map *m, const az_status_phrase_endpoint_v1 *e) {
    if (!e || e->kind < AZ_PHRASE_START || e->kind > AZ_PHRASE_GAP ||
        (e->kind == AZ_PHRASE_ITEM && e->index >= m->count)) return 0;
    az_status_phrase_endpoint_v1 expected = ds_endpoint(m, e->kind, e->index);
    return !memcmp(&expected, e, sizeof expected);
}
static int ds_point_inside(int start, int end) {
    return ds_time_nonnegative(start) && end != AZ_TIME_INVALID && end == AZ_TIME_BEFORE;
}
static int ds_inside_item(const struct ds_map *m, const az_status_phrase_endpoint_v1 *active, const struct ds_path *p) {
    if (!active || active->kind != AZ_PHRASE_ITEM || active->index >= m->count || p->ended) return 0;
    const az_phrase *row = &m->rows[active->index];
    az_status_coord_v1 start = ds_grid_point(m, row->start_beat), end = ds_grid_point(m, row->end_beat);
    int from_start = ds_coord_order(&p->from, &start, p->from_window_rate, p->from_window_samples);
    int to_start = ds_coord_order(&p->to, &start, p->observed_window_rate, p->observed_window_samples);
    int from_end = ds_coord_order(&p->from, &end, p->from_window_rate, p->from_window_samples);
    int to_end = ds_coord_order(&p->to, &end, p->observed_window_rate, p->observed_window_samples);
    return ds_point_inside(from_start, from_end) && ds_point_inside(to_start, to_end);
}
static void ds_classify_terminal(const struct ds_map *map, const struct ds_path *p, int from,
                                 const az_status_coord_v1 *point, unsigned rate, unsigned window,
                                 az_status_phrase_endpoint_v1 *out) {
    if (out->kind != AZ_PHRASE_GAP || !ds_terminal_edge(map, p, map->count - 1)) return;
    /* A terminal arrival can span consecutive native measurements.
     * Its prior landing remains the last ITEM while the source reader
     * refines/clamps the end. Classifying that same departure as GAP
     * would invent a missing crossing inside the prior sample window. */
    int terminal = ds_coord_order(point, &p->terminal, rate, window);
    az_status_coord_v1 start = ds_grid_point(map, map->rows[map->count - 1].start_beat);
    int final_start = ds_coord_order(point, &start, rate, window);
    if (!from || (terminal != AZ_TIME_INVALID && terminal <= AZ_TIME_EQUAL &&
                  final_start != AZ_TIME_INVALID && final_start >= AZ_TIME_EQUAL))
        *out = ds_endpoint(map, AZ_PHRASE_ITEM, map->count - 1);
}
static int ds_path_point_observed(const struct ds_path *p, uint32_t bit, const az_status_coord_v1 *point, unsigned rate, unsigned window) {
    az_status_coord_v1 origin = {.rate_hz = point->rate_hz};
    int zero = ds_coord_order(point, &origin, rate, window);
    return (p->endpoints & bit) && zero != AZ_TIME_INVALID && zero != AZ_TIME_BEFORE;
}
static int ds_classify_point(const struct ds_map *map, const struct ds_path *p, int from, az_status_phrase_endpoint_v1 *out) {
    uint32_t bit = from ? AZ_STATUS_OCCURRENCE_FROM : AZ_STATUS_OCCURRENCE_TO;
    const az_status_coord_v1 *point = from ? &p->from : &p->to;
    unsigned rate = from ? p->from_window_rate : p->observed_window_rate;
    unsigned window = from ? p->from_window_samples : p->observed_window_samples;
    if (!ds_path_point_observed(p, bit, point, rate, window)) return 0;
    *out = ds_at(map, point, !from && p->opening, !from && p->ended, rate, window);
    ds_classify_terminal(map, p, from, point, rate, window, out);
    return out->kind != 0;
}
static int ds_classify(const struct ds_map *map, const struct ds_path *p, int from, az_status_phrase_endpoint_v1 *out) {
    const az_status_phrase_endpoint_v1 *known = from ? p->departure : p->landing;
    if (known && ds_endpoint_known(map, known)) {
        *out = *known;
        return 1;
    }
    return ds_classify_point(map, p, from, out);
}
static void ds_cross(struct ds_deck *d, az_status_phrase_endpoint_v1 *active,
                     const az_status_phrase_endpoint_v1 *to, uint64_t now, uint64_t *loss) {
    if (!memcmp(active, to, sizeof *active)) return;
    az_status_phrase_transition_v1 tr = {.size = sizeof tr, .version = AZ_DATA_VERSION, .from = *active, .to = *to};
    ds_derived(d, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, now, to, sizeof *to, loss);
    if (!*loss) {
        d->current.sequence++;
        d->current.observed = now;
        d->current.published = ds_now();
        ds_append(d, 0, DS_PHRASE_ROUTE, NULL, &tr, 0, 0);
    }
    *active = *to;
}
/* Cross every proven boundary on a monotone leg, before applying the measured
 * landing to POSITION. A half-open interval belongs to the later item. */
struct ds_crossing_bounds {
    az_status_coord_v1 start, end;
    int start_from, start_to, end_from, end_to;
};
static int ds_crossing_bounds(const struct ds_map *m, const struct ds_path *p, unsigned i, struct ds_crossing_bounds *b) {
    b->start = ds_grid_point(m, m->rows[i].start_beat);
    b->end = ds_grid_point(m, m->rows[i].end_beat);
    b->start_from = ds_coord_order(&b->start, &p->from, p->from_window_rate, p->from_window_samples);
    b->start_to = ds_coord_order(&b->start, &p->to, p->observed_window_rate, p->observed_window_samples);
    b->end_from = ds_coord_order(&b->end, &p->from, p->from_window_rate, p->from_window_samples);
    b->end_to = ds_coord_order(&b->end, &p->to, p->observed_window_rate, p->observed_window_samples);
    return b->start_from != AZ_TIME_INVALID && b->start_to != AZ_TIME_INVALID && b->end_from != AZ_TIME_INVALID && b->end_to != AZ_TIME_INVALID;
}
static int ds_forward_entry(const struct ds_crossing_bounds *b) {
    return (b->start_from == AZ_TIME_EQUAL || b->start_from == AZ_TIME_AFTER) &&
           (b->start_to == AZ_TIME_EQUAL || b->start_to == AZ_TIME_BEFORE);
}
static int ds_forward_exit(const struct ds_map *m, const struct ds_path *p, unsigned i, const struct ds_crossing_bounds *b) {
    return b->end_from == AZ_TIME_AFTER && (b->end_to == AZ_TIME_EQUAL || b->end_to == AZ_TIME_BEFORE) && !ds_terminal_edge(m, p, i);
}
static void ds_cross_forward(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p, unsigned i,
                             const struct ds_crossing_bounds *b, az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    if (ds_forward_entry(b)) {
        az_status_phrase_endpoint_v1 dest = ds_endpoint(m, AZ_PHRASE_ITEM, i);
        ds_cross(d, active, &dest, now, loss);
    }
    if (ds_forward_exit(m, p, i, b)) {
        az_status_phrase_endpoint_v1 dest = ds_at(m, &b->end, 0, p->ended && b->end_to == AZ_TIME_EQUAL,
                                                  p->observed_window_rate, p->observed_window_samples);
        if (dest.kind) ds_cross(d, active, &dest, now, loss);
    }
}
static int ds_backward_exit(const struct ds_map *m, const struct ds_path *p, unsigned i,
                            const struct ds_crossing_bounds *b, az_status_phrase_endpoint_v1 *dest) {
    unsigned kind = AZ_PHRASE_START;
    if (i) {
        az_status_coord_v1 previous_end = ds_grid_point(m, m->rows[i - 1].end_beat);
        int adjacent = ds_coord_order(&previous_end, &b->start, p->observed_window_rate, p->observed_window_samples);
        if (adjacent == AZ_TIME_INVALID) return 0;
        kind = adjacent == AZ_TIME_EQUAL ? AZ_PHRASE_ITEM : AZ_PHRASE_GAP;
    }
    *dest = ds_endpoint(m, kind, i ? i - 1 : 0);
    return 1;
}
static int ds_cross_backward(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p, unsigned i,
                             const struct ds_crossing_bounds *b, az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    if ((b->end_from == AZ_TIME_BEFORE || b->end_from == AZ_TIME_EQUAL) && b->end_to == AZ_TIME_AFTER) {
        az_status_phrase_endpoint_v1 dest = ds_endpoint(m, AZ_PHRASE_ITEM, i);
        ds_cross(d, active, &dest, now, loss);
    }
    if ((b->start_from == AZ_TIME_BEFORE || b->start_from == AZ_TIME_EQUAL) && b->start_to == AZ_TIME_AFTER) {
        az_status_phrase_endpoint_v1 dest;
        if (!ds_backward_exit(m, p, i, b, &dest)) return 0;
        ds_cross(d, active, &dest, now, loss);
    }
    return 1;
}
static int ds_cross_leg(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p, int forward,
                        az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    for (unsigned n = 0; n < m->count; ++n) {
        unsigned i = forward ? n : m->count - n - 1;
        struct ds_crossing_bounds b;
        if (!ds_crossing_bounds(m, p, i, &b)) return 0;
        if (forward) ds_cross_forward(d, m, p, i, &b, active, now, loss);
        else if (!ds_cross_backward(d, m, p, i, &b, active, now, loss)) return 0;
    }
    return 1;
}
static void ds_crossings(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p,
                         az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    if (ds_inside_item(m, active, p)) return;
    int direction = ds_coord_order(&p->to, &p->from, p->observed_window_rate, p->observed_window_samples);
    if (direction == AZ_TIME_INVALID) return;
    int forward = direction == AZ_TIME_EQUAL || direction == AZ_TIME_AFTER;
    if (!ds_cross_leg(d, m, p, forward, active, now, loss)) return;
    az_status_phrase_endpoint_v1 dest;
    if (ds_classify(m, p, 0, &dest)) ds_cross(d, active, &dest, now, loss);
}
/* Discrete actions classify each endpoint independently. A qualified landing
 * can seed active even without proof of departure. Only the phrase route loses
 * continuity in that case. The action remains deliverable to its own routes. */
struct ds_path_reduction {
    const struct ds_path *path;
    struct ds_map map;
    const az_status_phrase_endpoint_v1 *previous;
    az_status_phrase_endpoint_v1 from, to;
    int mapped, has_from, has_to, endpoint_order;
};
static void ds_path_prepare(struct ds_deck *d, const struct ds_path *p, struct ds_path_reduction *r) {
    r->path = p;
    r->previous = ds_value(&d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    r->mapped = r->has_from = r->has_to = 0;
    r->endpoint_order = AZ_TIME_INVALID;
    if (!p) return;
    r->mapped = ds_map_at(&d->current, p, &r->map);
    r->has_from = r->mapped && ds_classify(&r->map, p, 1, &r->from);
    r->has_to = r->mapped && ds_classify(&r->map, p, 0, &r->to);
    r->endpoint_order = ds_coord_order(&p->from, &p->to, p->observed_window_rate, p->observed_window_samples);
}
static int ds_path_travel_ready(const struct ds_path_reduction *r) {
    return r->path && r->path->kind == DS_PATH_TRAVEL && r->has_from && r->has_to && r->previous &&
           ds_endpoint_known(&r->map, r->previous);
}
static int ds_path_still_at_opening(const struct ds_path_reduction *r) {
    const struct ds_path *p = r->path;
    return !p->ended && r->previous->kind == AZ_PHRASE_START &&
           ds_opening_at(&r->map, &p->to, p->observed_window_rate, p->observed_window_samples);
}
static void ds_path_confirm_terminal(struct ds_deck *d, const struct ds_path_reduction *r, uint64_t now,
                                     az_status_phrase_transition_v1 *pair, uint64_t *routes, uint64_t *fail) {
    if (!memcmp(r->previous, &r->to, sizeof r->to)) return;
    *pair = (az_status_phrase_transition_v1) {.size = sizeof *pair, .version = AZ_DATA_VERSION, .from = *r->previous, .to = r->to};
    *routes |= DS_PHRASE_ROUTE;
    if (ds_derived(d, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, now, &r->to, sizeof r->to, fail))
        *routes |= DS_PHRASE_ROUTE;
}
static int ds_path_stationary(struct ds_deck *d, const struct ds_path_reduction *r, uint64_t now,
                              az_status_phrase_transition_v1 *pair, uint64_t *routes, uint64_t *fail) {
    if (!ds_path_travel_ready(r) || r->endpoint_order == AZ_TIME_INVALID || r->endpoint_order != AZ_TIME_EQUAL) return 0;
    if (r->path->confirm_terminal) {
        ds_path_confirm_terminal(d, r, now, pair, routes, fail);
        return 1;
    }
    return !r->path->began_play && (!memcmp(r->previous, &r->to, sizeof r->to) || ds_path_still_at_opening(r));
}
static int ds_path_contiguous(const struct ds_path_reduction *r) {
    if (!ds_path_travel_ready(r)) return 0;
    const struct ds_path *p = r->path;
    return !memcmp(r->previous, &r->from, sizeof r->from) || (r->previous->kind == AZ_PHRASE_START &&
                                                              ds_opening_at(&r->map, &p->from, p->from_window_rate, p->from_window_samples));
}
static int ds_path_action_ready(const struct ds_path_reduction *r, const az_status_occurrence_v1 *action) {
    return r->path && r->path->kind == DS_PATH_ACTION && action && r->has_from && r->has_to &&
           r->previous && ds_endpoint_known(&r->map, r->previous) && memcmp(&r->from, &r->to, sizeof r->from);
}
static int ds_path_lost(const struct ds_path_reduction *r) {
    if (!r->previous) return 0;
    if (!r->path) return 1;
    if (r->path->kind == DS_PATH_BASELINE) return 0;
    if (!r->has_from || !r->has_to || !r->mapped || !ds_endpoint_known(&r->map, r->previous)) return 1;
    return r->path->kind == DS_PATH_TRAVEL && memcmp(r->previous, &r->from, sizeof r->from);
}
static void ds_path_reduce(struct ds_deck *d, const struct ds_path *p, const az_status_occurrence_v1 *action,
                           uint64_t now, az_status_phrase_transition_v1 *pair, uint64_t *routes, uint64_t *lost) {
    struct ds_path_reduction r;
    ds_path_prepare(d, p, &r);
    uint64_t fail = 0;
    if (ds_path_stationary(d, &r, now, pair, routes, &fail)) {
        *lost |= fail;
        return;
    }
    if (ds_path_contiguous(&r)) {
        az_status_phrase_endpoint_v1 active = *r.previous;
        ds_crossings(d, &r.map, p, &active, now, &fail);
    } else if (ds_path_action_ready(&r, action)) {
        *pair = (az_status_phrase_transition_v1) {.size = sizeof *pair, .version = AZ_DATA_VERSION, .discontinuity = action->kind, .from = r.from, .to = r.to};
        *routes |= DS_PHRASE_ROUTE;
    } else if (ds_path_lost(&r)) *lost |= DS_PHRASE_ROUTE;
    if (ds_derived(d, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, now,
                   r.has_to ? &r.to : NULL, r.has_to ? sizeof r.to : 0, &fail)) *routes |= DS_PHRASE_ROUTE;
    *lost |= fail;
}


struct ds_transaction {
    const struct ds_path *path;
    const az_track_ref *expected;
    const struct ds_field *fields;
    size_t count;
    const az_status_occurrence_v1 *action;
    const az_status_phrase_transition_v1 *pair;
    unsigned deck, flags, reason, input_reason;
    int file_only;
    uint64_t epoch, observed, lost, input_lost;
    struct ds_deck *d;
    int boundary, rc, generation_barrier, motion_barrier, bind, stem_bind;
    uint32_t availability;
    unsigned phrase_changed, mapping_renewed;
    uint64_t routes, path_loss, derived_loss;
    az_status_phrase_transition_v1 generated;
};

static int ds_input_shape(struct ds_transaction *t) {
    if (t->deck >= 4 || (t->count && !t->fields) || t->count > AZ_STATUS_TOPIC_COUNT * 9 ||
        (t->flags & ~(DS_BOUNDARY | DS_MOTION | DS_UNIT_INPUT | DS_NATIVE_INPUT | DS_FIELD_LOSS | DS_HEAD_LOSS)) || (t->lost & ~AZ_STATUS_ALL_V1) ||
        (!!t->lost != !!t->reason) || (t->reason & ~15u)) return 0;
    return 1;
}

static int ds_input_action(struct ds_transaction *t) {
    if (t->action && (t->action->size < sizeof *t->action || t->action->version != AZ_DATA_VERSION ||
                      !t->action->ordinal || t->action->kind < AZ_STATUS_SEEK || t->action->kind > AZ_STATUS_WRAP ||
                      (t->action->endpoints & ~3u))) return 0;
    return 1;
}

static int ds_input_pair(struct ds_transaction *t) {
    if (t->pair && (t->pair->size < sizeof *t->pair || t->pair->version != AZ_DATA_VERSION ||
                    t->pair->discontinuity > AZ_STATUS_WRAP || (t->action && t->action->kind != t->pair->discontinuity))) return 0;
    return 1;
}

static int ds_input_fields(struct ds_transaction *t) {
    for (size_t i = 0; i < t->count; i++) {
        const struct ds_field *p = &t->fields[i];
        if (p->topic >= AZ_STATUS_TOPIC_COUNT || p->field >= ds_layout[p->topic].fields ||
            ds_extent(p, t->epoch) == SIZE_MAX) return 0;
    }
    return 1;
}

static int ds_input_loss(struct ds_transaction *t) {
    if ((t->flags & DS_FIELD_LOSS) && (!t->count || !(t->flags & DS_NATIVE_INPUT) || !t->lost))
        return 0;
    if ((t->flags & DS_HEAD_LOSS) && (!(t->flags & DS_NATIVE_INPUT) || !t->lost ||
                                      (t->lost & ~(DS_ACTION_ROUTES | DS_PHRASE_ROUTE | AZ_STATUS_TOPIC(AZ_STATUS_PHASE))))) return 0;
    return 1;
}

static int ds_input_unit(struct ds_transaction *t) {
    if (t->flags & DS_UNIT_INPUT) {
        if (t->flags != DS_UNIT_INPUT || t->action || t->path || t->pair || !t->count) return 0;
        for (size_t i = 0; i < t->count; i++)
            if (t->fields[i].info.scope != AZ_STATUS_UNIT) return 0;
    }
    return 1;
}

static int ds_input_valid(struct ds_transaction *t) {
    return ds_input_shape(t) && ds_input_action(t) && ds_input_pair(t) && ds_input_fields(t) && ds_input_loss(t) && ds_input_unit(t);
}

static void ds_admit_epoch(struct ds_transaction *t) {
    if (t->d->current.sequence == UINT64_MAX) t->rc = AZ_LIMIT;
    else if ((t->boundary && t->epoch && t->epoch <= t->d->last_epoch) ||
             (!(t->flags & DS_BOUNDARY) && t->epoch != t->d->current.epoch) ||
             (t->action && t->action->ordinal <= t->d->last_ordinal)) t->rc = AZ_STALE;
}

static void ds_admit_native(struct ds_transaction *t) {
    if (t->flags & DS_NATIVE_INPUT)
        for (size_t i = 0; i < t->count; ++i)
            if (t->fields[i].topic == AZ_STATUS_STEMS && t->fields[i].info.provider_id != t->d->stem_binding)
                t->rc = AZ_STALE;
}

static void ds_admit_source(struct ds_transaction *t) {
    if (t->expected) {
        struct ds_cell *track = t->d->current.field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
        if (!track || track->info.info.availability != AZ_AVAIL_READY) t->rc = AZ_STALE;
        else {
            const az_track_ref *ref = (const void *) track->data;
            if (ref->source_id != t->expected->source_id || ref->source_generation != t->expected->source_generation ||
                ref->library_id != t->expected->library_id || ref->track_id != t->expected->track_id) t->rc = AZ_STALE;
        }
    }
}

static int ds_coalesced_form(const struct ds_transaction *t, uint64_t first) {
    return t->flags == (DS_NATIVE_INPUT | DS_MOTION) && !t->lost && t->path->kind == DS_PATH_TRAVEL && t->path->endpoints == 3 &&
           t->d->path_order && first < t->path->order;
}

static void ds_admit_coalesced(struct ds_transaction *t) {
    if (t->rc == AZ_OK && t->path->first_order) {
        uint64_t first = t->path->first_order;
        struct ds_map map;
        const az_status_phrase_endpoint_v1 *active = ds_value(&t->d->current, AZ_STATUS_PHRASE_TRANSITION,
                                                              AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
        if (!ds_coalesced_form(t, first) || !ds_map_at(&t->d->current, t->path, &map) ||
            !ds_endpoint_known(&map, active) || !ds_inside_item(&map, active, t->path)) t->rc = AZ_PENDING;
    }
}

static int ds_path_form(const struct ds_transaction *t) {
    return t->epoch && t->path->order && t->path->generation && t->path->kind <= DS_PATH_ACTION &&
           t->path->endpoints <= 3 && (t->path->kind == DS_PATH_ACTION) == !!t->action && !t->pair;
}
static int ds_path_stale(const struct ds_transaction *t) {
    return !t->boundary && (t->path->order <= t->d->path_order || t->path->generation < t->d->motion_generation ||
                            (t->path->kind == DS_PATH_ACTION && t->path->generation == t->d->motion_generation));
}
static int ds_path_order_gap(const struct ds_transaction *t, uint64_t first) {
    return !t->boundary && t->d->path_order && first != t->d->path_order + 1 && !(t->path->kind == DS_PATH_BASELINE && t->lost);
}
static int ds_path_needs_baseline(const struct ds_transaction *t, uint64_t first) {
    return (t->boundary && t->path->kind != DS_PATH_BASELINE) || ds_path_order_gap(t, first) ||
           (t->path->kind == DS_PATH_TRAVEL && t->path->generation != t->d->motion_generation);
}

static void ds_admit_path(struct ds_transaction *t) {
    if (t->path && t->rc == AZ_OK) {
        uint64_t first = t->path->first_order ? t->path->first_order : t->path->order;
        if (!ds_path_form(t)) t->rc = AZ_INVALID;
        else if (t->d->current.sequence > UINT64_MAX - 2052) t->rc = AZ_LIMIT;
        else if (ds_path_stale(t)) t->rc = AZ_STALE;
        else if (ds_path_needs_baseline(t, first)) t->rc = AZ_PENDING;
        ds_admit_coalesced(t);
    }
}

static void ds_begin_transaction(struct ds_transaction *t) {
    uint64_t now = ds_now();
    if (!t->observed) t->observed = now;
    t->generation_barrier = t->path && t->d->motion_generation && t->path->generation != t->d->motion_generation;
    t->motion_barrier = t->boundary || t->action || t->generation_barrier;
    t->availability = (t->flags & DS_MOTION) ? ds_motion_availability(&t->d->current) : 0;
}

static void ds_apply_boundary(struct ds_transaction *t) {
    if (t->boundary) {
        t->d->current.epoch = t->epoch;
        if (t->epoch) t->d->last_epoch = t->epoch;
        t->d->path_order = t->d->motion_generation = 0;
        t->d->head_window_samples = t->d->head_window_rate = 0;
        t->routes |= ds_stale(&t->d->current, AZ_STATUS_ALL_V1, 1, 0, t->observed);
        t->routes |= AZ_STATUS_TOPIC(AZ_STATUS_LOAD);
    }
}

static void ds_reduce_input_path(struct ds_transaction *t) {
    if (t->path && t->path->kind != DS_PATH_BASELINE)
        ds_path_reduce(t->d, t->path, t->action, t->observed, &t->generated, &t->routes, &t->path_loss);
    else if (!t->path && t->action && !t->pair)
        ds_path_reduce(t->d, NULL, t->action, t->observed, &t->generated, &t->routes, &t->path_loss);
}

static void ds_withdraw_lost_heads(struct ds_transaction *t) {
    if (t->flags & DS_HEAD_LOSS) {
        if (t->lost & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)) t->d->head_window_samples = t->d->head_window_rate = 0;
        if (t->lost & AZ_STATUS_TOPIC(AZ_STATUS_POSITION))
            t->routes |= ds_withdraw(&t->d->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, t->observed, &t->path_loss);
        if (t->lost & DS_PHRASE_ROUTE)
            t->routes |= ds_withdraw(&t->d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, t->observed, &t->path_loss);
    }
}

static void ds_invalidate_lost_fields(struct ds_transaction *t) {
    if (t->lost && !(t->flags & (DS_UNIT_INPUT | DS_FIELD_LOSS | DS_HEAD_LOSS))) t->routes |= ds_stale(&t->d->current, t->lost, 0, t->file_only ? AZ_PROV_FILE : (t->flags & DS_NATIVE_INPUT) ? AZ_PROV_NATIVE
                                                                                                                                                                                              : AZ_PROV_NONE,
                                                                                                       t->observed);
}

static void ds_prepare_field_changes(struct ds_transaction *t) {
    /* Without a witnessed landing, a discontinuity cannot carry a head or
     * phase copied before the reposition. An explicit head in this transaction
     * replaces the withdrawal atomically. */
    if (t->action)
        t->routes |= ds_withdraw(&t->d->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, t->observed, &t->path_loss);
    if (t->action || t->generation_barrier)
        t->routes |= ds_withdraw(&t->d->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD, t->observed, &t->path_loss);
    t->bind = t->boundary;
    t->stem_bind = t->boundary || (t->lost & (AZ_STATUS_TOPIC(AZ_STATUS_LOAD) | AZ_STATUS_TOPIC(AZ_STATUS_METADATA)));
}

static const struct ds_field *ds_normalize_reference(struct ds_transaction *t, const struct ds_field *p,
                                                     struct ds_field *normalized, az_status_cue_ref_v1 *canonical) {
    if (p->topic != AZ_STATUS_LOOP || p->field != AZ_STATUS_LOOP_SAVED_ENTRY || p->info.info.availability != AZ_AVAIL_READY) return p;
    *normalized = *p;
    *canonical = *(const az_status_cue_ref_v1 *) p->value;
    const az_cue_entry *row = ds_cue_reference_row(&t->d->current, canonical);
    if (row) {
        canonical->entry = *row;
        normalized->value = canonical;
    } else {
        normalized->info.info.availability = AZ_AVAIL_STALE;
        normalized->info.count = 0;
        normalized->value = NULL;
    }
    return normalized;
}

static int ds_preserve_native_source(const struct ds_cell *old, const struct ds_field *p, int file_only) {
    (void) file_only;
    if (p->topic == AZ_STATUS_LOAD && p->field == AZ_STATUS_LOAD_SOURCE &&
        p->info.info.provenance != AZ_PROV_NATIVE && old && old->info.info.provenance == AZ_PROV_NATIVE &&
        (old->info.info.availability == AZ_AVAIL_READY || p->info.info.availability == AZ_AVAIL_STALE)) return 1;
    return 0;
}

static int ds_preserve_native_file(const struct ds_cell *old, const struct ds_field *p, int file_only) {
    (void) p;
    if (file_only && old && old->info.info.provenance == AZ_PROV_NATIVE &&
        old->info.info.availability == AZ_AVAIL_READY) return 1;
    return 0;
}

static int ds_preserve_file_metadata(const struct ds_cell *old, const struct ds_field *p, int file_only) {
    (void) file_only;
    if ((p->topic == AZ_STATUS_METADATA || (p->topic == AZ_STATUS_KEY && p->field == AZ_STATUS_KEY_ORIGINAL)) &&
        old && old->info.info.provenance == AZ_PROV_FILE &&
        old->info.info.availability == AZ_AVAIL_READY && p->info.info.provenance == AZ_PROV_NATIVE &&
        p->info.info.availability != AZ_AVAIL_READY) return 1;
    return 0;
}

static int ds_preserve_field(struct ds_transaction *t, const struct ds_field *p) {
    const struct ds_cell *old = t->d->current.field[p->topic][p->field];
    return ds_preserve_native_source(old, p, t->file_only) || ds_preserve_native_file(old, p, t->file_only) || ds_preserve_file_metadata(old, p, t->file_only);
}

static int ds_assign_field(struct ds_transaction *t, const struct ds_field *p) {
    size_t extent = ds_extent(p, t->epoch);
    int native_cues = (t->flags & DS_NATIVE_INPUT) && p->topic == AZ_STATUS_CUES &&
                      (p->field == AZ_STATUS_CUES_LIVE_HOT || p->field == AZ_STATUS_CUES_LIVE_MEMORY);
    int changed = native_cues ? ds_set_live_cues(t->d, p, extent, 0) : ds_set(&t->d->current, p, extent);
    if (changed == AZ_LIMIT) {
        t->lost |= AZ_STATUS_TOPIC(p->topic);
        t->reason |= AZ_STATUS_REVISION_LOST;
        struct ds_field stale = *p;
        stale.info.count = 0;
        stale.info.info.availability = AZ_AVAIL_STALE;
        if (ds_set(&t->d->current, &stale, 0) == AZ_LIMIT)
            ds_clear_field(&t->d->current.field[p->topic][p->field]);
        t->rc = AZ_LIMIT;
    }
    return changed;
}

static void ds_field_clock_effects(struct ds_transaction *t, const struct ds_field *p) {
    if (p->topic == AZ_STATUS_POSITION && p->field == AZ_STATUS_POSITION_HEAD) {
        const struct ds_cell *head = ds_ready(&t->d->current, p->topic, p->field);
        t->d->head_window_samples = head ? head->clock.window_samples : 0;
        t->d->head_window_rate = head ? head->clock.window_rate : 0;
    }
    if (p->topic == AZ_STATUS_TRANSPORT && p->field == AZ_STATUS_TRANSPORT_DIRECTION) t->motion_barrier = 1;
}

static void ds_field_binding_effects(struct ds_transaction *t, const struct ds_field *p) {
    if (p->topic == AZ_STATUS_METADATA && p->field == AZ_STATUS_METADATA_PATH) t->stem_bind = 1;
    if (p->topic == AZ_STATUS_LOAD && p->field == AZ_STATUS_LOAD_TRACK) {
        t->bind = 1;
                /* A replacement library on the same playing load must lose all
                 * old file values in this very publication, before its worker runs. */
        t->routes |= ds_stale(&t->d->current, AZ_STATUS_ALL_V1 & ~AZ_STATUS_TOPIC(AZ_STATUS_LOAD), 0, AZ_PROV_FILE, t->observed);
    }
}

static void ds_field_effects(struct ds_transaction *t, const struct ds_field *p, int changed) {
    if (p->topic == AZ_STATUS_PHRASES && p->field < 2 && changed) t->phrase_changed |= 1u << p->field;
    if (p->topic == AZ_STATUS_PHRASES && p->field >= 2 && p->field < 4) t->mapping_renewed |= 1u << (p->field - 2);
    if (changed) {
        t->routes |= AZ_STATUS_TOPIC(p->topic);
        ds_field_clock_effects(t, p);
        ds_field_binding_effects(t, p);
    }
}

static void ds_apply_fields(struct ds_transaction *t) {
    /* Collections precede their references, regardless of input order. */
    for (unsigned references = 0; references < 2; ++references)
        for (size_t i = 0; i < t->count; ++i) {
            const struct ds_field *p = &t->fields[i];
            int is_reference = p->topic == AZ_STATUS_LOOP && p->field == AZ_STATUS_LOOP_SAVED_ENTRY;
            if (is_reference != (int) references) continue;
            struct ds_field normalized;
            az_status_cue_ref_v1 canonical;
            p = ds_normalize_reference(t, p, &normalized, &canonical);
            if (ds_preserve_field(t, p)) continue;
            int changed = ds_assign_field(t, p);
            ds_field_effects(t, p, changed);
        }
}

static void ds_derive_cues_and_key(struct ds_transaction *t) {
    if (t->routes & AZ_STATUS_TOPIC(AZ_STATUS_CUES)) {
        t->routes |= ds_cue_storage_flags(t->d, AZ_STATUS_CUES_LIVE_HOT, t->observed, &t->derived_loss);
        t->routes |= ds_cue_storage_flags(t->d, AZ_STATUS_CUES_LIVE_MEMORY, t->observed, &t->derived_loss);
    }
    if (t->routes & (AZ_STATUS_TOPIC(AZ_STATUS_KEY) | AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS)))
        t->routes |= ds_effective_key(t->d, t->observed, &t->derived_loss);
}

static void ds_rebind_stems(struct ds_transaction *t) {
    if (t->bind || t->stem_bind) {
        t->d->stem_binding = ds.stem_bindings < UINT64_MAX ? ++ds.stem_bindings : 0;
        t->routes |= ds_stale(&t->d->current, AZ_STATUS_TOPIC(AZ_STATUS_STEMS), 0, AZ_PROV_NATIVE, t->observed);
        ds_send_stem_binding(t->deck);
    }
}

static void ds_update_map_clocks(struct ds_transaction *t) {
    for (unsigned i = 0; i < 2; i++)
        if ((t->phrase_changed & ~t->mapping_renewed) & (1u << i))
            t->routes |= ds_withdraw(&t->d->current, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID + i, t->observed, &t->derived_loss);
    if (t->path && (t->path->endpoints & AZ_STATUS_OCCURRENCE_TO)) {
        t->d->head_window_samples = t->path->observed_window_samples;
        t->d->head_window_rate = t->path->observed_window_rate;
    }
}

static void ds_derive_clock_values(struct ds_transaction *t) {
    if (t->routes & (DS_ACTION_ROUTES | AZ_STATUS_TOPIC(AZ_STATUS_METADATA) | AZ_STATUS_TOPIC(AZ_STATUS_LOOP) |
                     AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS))) t->routes |= ds_times(t->d, t->observed, &t->derived_loss);
    if (t->routes & (AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_GRID)))
        t->routes |= ds_phase(t->d, t->observed, &t->derived_loss);
    if (t->routes & (AZ_STATUS_TOPIC(AZ_STATUS_CUES) | AZ_STATUS_TOPIC(AZ_STATUS_LOOP) |
                     AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | AZ_STATUS_TOPIC(AZ_STATUS_GRID)))
        t->routes |= ds_associations(t->d, t->observed, &t->derived_loss);
}

static void ds_check_active_phrase(struct ds_transaction *t) {
    const az_status_phrase_endpoint_v1 *active = ds_value(&t->d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    if (active && !t->path && (t->routes & (AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | AZ_STATUS_TOPIC(AZ_STATUS_GRID)))) {
        struct ds_map map;
        struct ds_path identity = {.grid = active->grid, .phrase_provider = active->phrase_provider_id, .phrase_revision = active->phrase_revision};
        if (!ds_map_at(&t->d->current, &identity, &map) || !ds_endpoint_known(&map, active)) {
            t->routes |= ds_withdraw(&t->d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, t->observed, &t->derived_loss);
            t->path_loss |= DS_PHRASE_ROUTE;
        }
    }
}

static void ds_complete_path(struct ds_transaction *t) {
    if (t->path && t->path->kind == DS_PATH_BASELINE)
        ds_path_reduce(t->d, t->path, NULL, t->observed, &t->generated, &t->routes, &t->path_loss);
    if (t->path) {
        t->d->path_order = t->path->order;
        t->d->motion_generation = t->path->generation;
    }
    if (t->generated.size) t->pair = &t->generated;
}

static void ds_complete_routes(struct ds_transaction *t) {
    if (t->path_loss) {
        t->lost |= t->path_loss;
        t->reason |= AZ_STATUS_CAPTURE_GAP;
    }
    if (t->derived_loss) {
        t->lost |= t->derived_loss;
        t->reason |= AZ_STATUS_REVISION_LOST;
    }
    if (t->action) {
        t->d->last_ordinal = t->action->ordinal;
        t->routes |= DS_ACTION_ROUTES;
    }
    if (t->pair) t->routes |= DS_PHRASE_ROUTE;
}

static int ds_publish_notifications(struct ds_transaction *t, uint64_t cut) {
    struct ds_cell *track = t->d->current.field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
    az_track_ref ref = {0};
    if (t->bind && track && track->info.info.availability == AZ_AVAIL_READY) memcpy(&ref, track->data, sizeof ref);
    struct ds_analysis_cut analysis = {0};
    if (t->routes & AZ_STATUS_TOPIC(AZ_STATUS_LOAD)) analysis = ds_analysis_copy(&t->d->current);
    pthread_mutex_unlock(&ds.mu);
    az_status_host_wake();
    if (t->bind) az_status_host_store_binding(t->deck, t->epoch, ref.source_id ? &ref : NULL, cut);
    if (t->routes & AZ_STATUS_TOPIC(AZ_STATUS_LOAD)) ds_analysis_deliver(t->deck, &analysis);
    return t->rc;
}
static int ds_commit_transaction(struct ds_transaction *t) {
    uint64_t cut = ++t->d->current.sequence;
    t->d->current.observed = t->observed;
    t->d->current.published = ds_now();
    if (t->motion_barrier || ((t->flags & DS_MOTION) && t->availability != ds_motion_availability(&t->d->current)))
        t->flags |= DS_FLUSH_MOTION;
    ds_append(t->d, t->flags, t->routes, t->action, t->pair, t->lost, t->reason);
    ds_proof_cut(t->deck, t->epoch, t->expected, t->file_only, t->flags, t->input_lost, t->input_reason,
                 t->lost, t->reason, t->routes, t->fields, t->count, t->bind, 1);
    return ds_publish_notifications(t, cut);
}

static int ds_finish_transaction(struct ds_transaction *t) {
    if (!t->routes && !t->lost) {
        ds_proof_cut(t->deck, t->epoch, t->expected, t->file_only, t->flags, t->input_lost, t->input_reason,
                     t->lost, t->reason, t->routes, t->fields, t->count, t->bind, 0);
        pthread_mutex_unlock(&ds.mu);
        if (t->path) az_status_host_wake();
        return t->rc;
    }
    return ds_commit_transaction(t);
}
static int ds_apply(const struct ds_path *path, const az_track_ref *expected, int file_only, unsigned deck, uint64_t epoch, unsigned flags, uint64_t observed,
                    const struct ds_field *fields, size_t count,
                    const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair,
                    uint64_t lost, unsigned reason) {
    struct ds_transaction t = {
        .path = path,
        .expected = expected,
        .file_only = file_only,
        .deck = deck,
        .epoch = epoch,
        .flags = flags,
        .observed = observed,
        .fields = fields,
        .count = count,
        .action = action,
        .pair = pair,
        .lost = lost,
        .reason = reason,
        .input_lost = lost,
        .input_reason = reason,
        .rc = AZ_OK,
    };
    if (!ds_input_valid(&t)) return AZ_INVALID;
    pthread_mutex_lock(&ds.mu);
    t.d = &ds.deck[deck];
    if (flags & DS_UNIT_INPUT) t.epoch = t.d->current.epoch;
    t.boundary = (flags & DS_BOUNDARY) && t.d->current.epoch != t.epoch;
    ds_admit_epoch(&t);
    ds_admit_native(&t);
    ds_admit_source(&t);
    ds_admit_path(&t);
    if (t.rc != AZ_OK) {
        pthread_mutex_unlock(&ds.mu);
        return t.rc;
    }
    ds_begin_transaction(&t);
    ds_apply_boundary(&t);
    ds_reduce_input_path(&t);
    ds_withdraw_lost_heads(&t);
    ds_invalidate_lost_fields(&t);
    ds_prepare_field_changes(&t);
    ds_apply_fields(&t);
    ds_derive_cues_and_key(&t);
    ds_rebind_stems(&t);
    ds_update_map_clocks(&t);
    ds_derive_clock_values(&t);
    ds_check_active_phrase(&t);
    ds_complete_path(&t);
    ds_complete_routes(&t);
    return ds_finish_transaction(&t);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Map activation and motion may arrive on separate non-real-time lanes. Order
 * is assigned at their common source, never inferred from arrival or time. */
#define DS_PATH_FIELDS 72
struct ds_pending_path {
    struct ds_path path;
    struct ds_image image;
    struct ds_field fields[DS_PATH_FIELDS];
    struct ds_time_axis clocks[DS_PATH_FIELDS];
    size_t count;
    az_status_occurrence_v1 action;
    az_status_phrase_endpoint_v1 departure, landing;
    unsigned flags;
    uint64_t epoch, observed, lost;
    unsigned reason;
};
static struct ds_pending_path ds_pending[4][DS_PATH_PENDING];
static pthread_mutex_t ds_path_mu[4] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
                                        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};

static void ds_forget_path(struct ds_pending_path *pending) {
    pthread_mutex_lock(&ds.mu);
    ds_image_copy(&pending->image, NULL);
    pthread_mutex_unlock(&ds.mu);
    memset(pending, 0, sizeof *pending);
}
static struct ds_pending_path *ds_pending_slot(unsigned deck, uint64_t epoch, uint64_t order, int *duplicate) {
    struct ds_pending_path *slot = NULL;
    for (unsigned i = 0; i < DS_PATH_PENDING; i++) {
        struct ds_pending_path *pending = &ds_pending[deck][i];
        if (pending->epoch == epoch && pending->path.order == order) {
            *duplicate = 1;
            return pending;
        }
        if (!pending->path.order) slot = pending;
    }
    return slot;
}
static void ds_hold_endpoints(struct ds_pending_path *slot, const struct ds_path *path) {
    slot->path = *path;
    if (path->departure) {
        slot->departure = *path->departure;
        slot->path.departure = &slot->departure;
    }
    if (path->landing) {
        slot->landing = *path->landing;
        slot->path.landing = &slot->landing;
    }
}
static int ds_hold_field(struct ds_pending_path *slot, size_t index, const struct ds_field *field) {
    if (slot->image.field[field->topic][field->field] ||
        ds_set(&slot->image, field, ds_extent(field, slot->epoch)) == AZ_LIMIT) return AZ_LIMIT;
    struct ds_cell *cell = slot->image.field[field->topic][field->field];
    slot->fields[index] = *field;
    slot->fields[index].value = cell && cell->bytes ? cell->data : NULL;
    slot->fields[index].clock = NULL;
    if (cell && cell->clock.units_per_second) {
        slot->clocks[index] = cell->clock;
        slot->fields[index].clock = &slot->clocks[index];
    }
    return AZ_PENDING;
}
static int ds_hold_fields(struct ds_pending_path *slot, const struct ds_field *fields) {
    int rc = AZ_PENDING;
    pthread_mutex_lock(&ds.mu);
    for (size_t i = 0; i < slot->count; i++) {
        rc = ds_hold_field(slot, i, &fields[i]);
        if (rc != AZ_PENDING) break;
    }
    pthread_mutex_unlock(&ds.mu);
    return rc;
}
static int ds_hold_path(unsigned deck, const struct ds_path *p, uint64_t epoch, unsigned flags,
                        uint64_t observed, const struct ds_field *fields, size_t count,
                        const az_status_occurrence_v1 *action, uint64_t lost, unsigned reason) {
    int duplicate = 0;
    struct ds_pending_path *slot = ds_pending_slot(deck, epoch, p->order, &duplicate);
    if (duplicate) return AZ_PENDING;
    if (!slot || count > DS_PATH_FIELDS) return AZ_LIMIT;
    ds_hold_endpoints(slot, p);
    slot->epoch = epoch;
    slot->observed = observed;
    slot->lost = lost;
    slot->reason = reason;
    slot->flags = flags;
    slot->count = count;
    if (action) slot->action = *action;
    int rc = ds_hold_fields(slot, fields);
    if (rc != AZ_PENDING) ds_forget_path(slot);
    az_status_host_wake(); /* budgeting pins can advance an eviction watermark */
    return rc;
}
static void ds_drop_old_paths(unsigned deck, uint64_t epoch) {
    for (unsigned i = 0; i < DS_PATH_PENDING; i++) {
        struct ds_pending_path *pending = &ds_pending[deck][i];
        if (pending->path.order && pending->epoch != epoch) ds_forget_path(pending);
    }
}
static struct ds_pending_path *ds_next_path(unsigned deck, uint64_t epoch) {
    struct ds_pending_path *next = NULL;
    for (unsigned i = 0; i < DS_PATH_PENDING; i++) {
        struct ds_pending_path *candidate = &ds_pending[deck][i];
        if (candidate->epoch != epoch || !candidate->path.order) continue;
        if (!next || candidate->path.order < next->path.order) next = candidate;
    }
    return next;
}
static void ds_flush_paths(unsigned deck, uint64_t epoch) {
    for (;;) {
        struct ds_pending_path *next = ds_next_path(deck, epoch);
        if (!next) return;
        int rc = ds_apply(&next->path, NULL, 0, deck, epoch, next->flags, next->observed,
                          next->fields, next->count, next->action.ordinal ? &next->action : NULL, NULL,
                          next->lost, next->reason);
        if (rc == AZ_PENDING) return;
        ds_forget_path(next);
    }
}
static uint64_t ds_skipped_paths(unsigned deck, uint64_t epoch, const struct ds_path *path) {
    uint64_t lost = 0;
    if (!path || path->kind != DS_PATH_BASELINE) return 0;
    for (unsigned i = 0; i < DS_PATH_PENDING; i++) {
        const struct ds_pending_path *pending = &ds_pending[deck][i];
        if (pending->epoch == epoch && pending->path.order && pending->path.order < path->order) {
            lost |= DS_PHRASE_ROUTE | AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE);
            if (pending->action.ordinal) lost |= DS_ACTION_ROUTES;
        }
    }
    return lost;
}
static int ds_path_committed(int rc, unsigned deck, uint64_t epoch, const struct ds_path *path) {
    return rc == AZ_OK || (rc == AZ_LIMIT && path && ds.deck[deck].current.epoch == epoch &&
                           ds.deck[deck].path_order == path->order);
}
static int ds_resolve_path(int rc, unsigned deck, uint64_t epoch, const struct ds_path *path,
                           unsigned flags, uint64_t observed, const struct ds_field *fields, size_t count,
                           const az_status_occurrence_v1 *action, uint64_t lost, unsigned reason) {
    if (rc == AZ_PENDING && path && !path->first_order) {
        rc = ds_hold_path(deck, path, epoch, flags, observed, fields, count, action, lost, reason);
        if (rc == AZ_LIMIT) {
            uint64_t mask = DS_PHRASE_ROUTE | DS_ACTION_ROUTES | AZ_STATUS_TOPIC(AZ_STATUS_PHASE);
            ds_apply(NULL, NULL, 0, deck, epoch, 0, observed, NULL, 0, NULL, NULL, mask, AZ_STATUS_CAPTURE_GAP);
        }
    } else if (ds_path_committed(rc, deck, epoch, path)) {
        ds_drop_old_paths(deck, epoch);
        ds_flush_paths(deck, epoch);
    }
    return rc;
}
int ds_publish_ordered(const struct ds_path *path, const az_track_ref *expected, int file_only,
                              unsigned deck, uint64_t epoch, unsigned flags, uint64_t observed,
                              const struct ds_field *fields, size_t count,
                              const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair,
                              uint64_t lost, unsigned reason, uint64_t *committed_order) {
    if (deck >= 4) return AZ_INVALID;
    if (!path && !(flags & DS_BOUNDARY))
        return ds_apply(NULL, expected, file_only, deck, epoch, flags, observed, fields, count, action, pair, lost, reason);
    pthread_mutex_lock(&ds_path_mu[deck]);
    uint64_t skipped = lost ? ds_skipped_paths(deck, epoch, path) : 0;
    if (skipped) {
        lost |= skipped;
        reason |= AZ_STATUS_CUT_SKIPPED;
    }
    int rc = ds_apply(path, expected, file_only, deck, epoch, flags, observed, fields, count, action, pair, lost, reason);
    rc = ds_resolve_path(rc, deck, epoch, path, flags, observed, fields, count, action, lost, reason);
    /* A parked successor can commit in this transaction. A limited receipt
     * can also have been rejected before committing. Report the actual cut
     * before another path or load boundary can change it. */
    if (committed_order)
        *committed_order = ds.deck[deck].current.epoch == epoch ? ds.deck[deck].path_order : 0;
    pthread_mutex_unlock(&ds_path_mu[deck]);
    return rc;
}
int ds_publish_checked(const struct ds_path *path, const az_track_ref *expected, int file_only,
                              unsigned deck, uint64_t epoch, unsigned flags, uint64_t observed,
                              const struct ds_field *fields, size_t count,
                              const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair,
                              uint64_t lost, unsigned reason) {
    return ds_publish_ordered(path, expected, file_only, deck, epoch, flags, observed, fields, count,
                              action, pair, lost, reason, NULL);
}


int ds_publish(unsigned deck, uint64_t epoch, unsigned flags, uint64_t observed,
                      const struct ds_field *fields, size_t count, const az_status_occurrence_v1 *action,
                      const az_status_phrase_transition_v1 *pair, uint64_t lost, unsigned reason) {
    return ds_publish_checked(NULL, NULL, 0, deck, epoch, flags, observed, fields, count, action, pair, lost, reason);
}
int ds_publish_file(unsigned deck, uint64_t epoch, const az_track_ref *expected, uint64_t observed,
                           const struct ds_field *fields, size_t count, uint64_t lost, unsigned reason) {
    if (!epoch || !expected) return AZ_INVALID;
    return ds_publish_checked(NULL, expected, 1, deck, epoch, 0, observed, fields, count, NULL, NULL, lost, reason);
}
int ds_retire_file_binding(unsigned deck, uint64_t epoch, const az_track_ref *expected) {
    if (!expected || !epoch) return AZ_INVALID;
    uint64_t now = ds_now();
    struct ds_field stale[] = {
        {AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK, {.info = {AZ_AVAIL_STALE, AZ_PROV_FILE, 0, now}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = epoch}, NULL, NULL},
        {AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE, {.info = {AZ_AVAIL_STALE, AZ_PROV_DERIVED, 0, now}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = epoch}, NULL, NULL}};
    return ds_publish_checked(NULL, expected, 0, deck, epoch, 0, now, stale, 2, NULL, NULL, 0, 0);
}

/* UI owns registrations. The reducer mutex makes the INITIAL image and cursor
 * one cut even while a storage worker or native reducer is committing. */
static uint64_t ds_interest(unsigned deck) {
    uint64_t mask = atomic_load_explicit(&ds_core_load, memory_order_acquire) ? AZ_STATUS_TOPIC(AZ_STATUS_LOAD) : 0;
    for (unsigned i = 0; i < DS_SUBS; i++)
        if (ds.sub[i].handle && (ds.sub[i].watch.deck_mask & (1u << deck))) mask |= ds.sub[i].watch.topics;
    return mask;
}
static uint64_t ds_dependencies(uint64_t mask) {
    if (mask & AZ_STATUS_TOPIC(AZ_STATUS_STEMS)) mask |= AZ_STATUS_TOPIC(AZ_STATUS_METADATA);
    if (mask & AZ_STATUS_TOPIC(AZ_STATUS_KEY)) mask |= AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS);
    if (mask & DS_PHRASE_ROUTE) mask |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | DS_ACTION_ROUTES;
    if (mask & AZ_STATUS_TOPIC(AZ_STATUS_PHASE)) mask |= AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_GRID);
    if (mask & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)) mask |= AZ_STATUS_TOPIC(AZ_STATUS_METADATA) |
                                                            AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_LOOP) | AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS);
    if (mask & AZ_STATUS_TOPIC(AZ_STATUS_LOOP)) mask |= AZ_STATUS_TOPIC(AZ_STATUS_CUES);
    if (mask & AZ_STATUS_TOPIC(AZ_STATUS_PHRASES)) mask |= AZ_STATUS_TOPIC(AZ_STATUS_GRID);
    return mask ? mask | AZ_STATUS_TOPIC(AZ_STATUS_LOAD) : 0;
}
static int ds_watch_valid(const az_deck_watch_v1 *w) {
    if (!w || w->size < sizeof *w || w->version != AZ_DATA_VERSION) return 0;
    return w->deck_mask && !(w->deck_mask & ~AZ_STATUS_ALL_DECKS) && w->topics && !(w->topics & ~AZ_STATUS_ALL_V1) && w->position_hz <= 30;
}

static struct ds_subscription *ds_subscription_slot(int owner) {
    unsigned owned = 0;
    struct ds_subscription *slot = NULL;
    for (unsigned i = 0; i < DS_SUBS; i++) {
        if (!ds.sub[i].handle) slot = &ds.sub[i];
        else if (ds.sub[i].owner == owner) owned++;
    }
    if (!slot || owned >= 4 || ds.handles == UINT64_MAX) return NULL;
    return slot;
}

static void ds_watch_interest(const az_deck_watch_v1 *w) {
    for (unsigned d = 0; d < 4; d++)
        if (w->deck_mask & (1u << d)) {
            uint64_t needs = ds_dependencies(ds_interest(d) | w->topics);
            az_status_host_gui_interest(needs);
            az_status_host_native_interest(d, needs);
            az_status_host_store_interest(d, needs);
        }
}

static void ds_initial_cursors(struct ds_subscription *slot, const az_deck_watch_v1 *w) {
    uint64_t now = ds_now();
    for (unsigned d = 0; d < 4; d++)
        if (w->deck_mask & (1u << d)) {
            struct ds_cursor *c = &slot->cursor[d];
            if (!ds.deck[d].current.sequence) ds.deck[d].current.sequence = 1;
            ds_image_select(&c->last, &ds.deck[d].current, w->topics);
            c->last.observed = c->last.published = now;
            c->initial = 1;
            c->covered = c->last.sequence;
        }
}

int ds_register(int owner, const az_deck_watch_v1 *w, az_deck_status_fn fn, void *user,
                       az_deck_subscription *out) {
    if (owner < 0 || !fn || !out || !ds_watch_valid(w)) return AZ_INVALID;
    if (!ds.enabled) return AZ_UNSUPPORTED;
    struct ds_subscription *slot = ds_subscription_slot(owner);
    if (!slot) return AZ_LIMIT;
    ds_watch_interest(w);
    pthread_mutex_lock(&ds.mu);
    slot->handle = ++ds.handles;
    slot->watch = *w;
    if (!slot->watch.position_hz) slot->watch.position_hz = 30;
    slot->owner = owner;
    slot->fn = fn;
    slot->user = user;
    ds_initial_cursors(slot, w);
    *out = slot->handle;
    pthread_mutex_unlock(&ds.mu);
    az_status_host_wake();
    return AZ_OK;
}
static void ds_remove(struct ds_subscription *s) {
    unsigned decks = s->watch.deck_mask;
    pthread_mutex_lock(&ds.mu);
    for (unsigned d = 0; d < 4; d++) ds_image_copy(&s->cursor[d].last, NULL);
    memset(s, 0, sizeof *s);
    pthread_mutex_unlock(&ds.mu);
    for (unsigned d = 0; d < 4; d++)
        if (decks & (1u << d)) {
            uint64_t needs = ds_dependencies(ds_interest(d));
            az_status_host_native_interest(d, needs);
            az_status_host_store_interest(d, needs);
        }
}
int ds_release(int owner, az_deck_subscription handle) {
    if (owner < 0 || !handle || handle > ds.handles) return AZ_INVALID;
    for (unsigned i = 0; i < DS_SUBS; i++)
        if (ds.sub[i].handle == handle) {
            if (ds.sub[i].owner != owner) return AZ_INVALID;
            ds_remove(&ds.sub[i]);
            return AZ_OK;
        }
    return AZ_STALE;
}
void ds_retire(int owner) {
    for (unsigned i = 0; i < DS_SUBS; i++)
        if (ds.sub[i].handle && ds.sub[i].owner == owner) ds_remove(&ds.sub[i]);
}

struct ds_delivery {
    unsigned motion, owner;
    uint64_t cursor_before;
    struct ds_image pin;
    struct ds_topics values;
    az_deck_status_view_v1 view;
    az_deck_status_event_v1 event;
    az_status_occurrence_v1 action;
    az_status_phrase_transition_v1 pair;
    az_deck_status_fn fn;
    void *user;
};
static void ds_view_fields(unsigned char *topic, unsigned t, const struct ds_image *im, const struct ds_image *previous) {
    az_status_topic_v1 *h = (void *) topic;
    for (unsigned f = 0; f < ds_layout[t].fields; f++) {
        const struct ds_layout *l = &ds_layout[t].field[f];
        struct ds_cell *cell = im->field[t][f];
        uint64_t bit = az_status_field_bit(f);
        if (!previous || previous->field[t][f] != cell) h->changed |= bit;
        if (!cell) continue;
        memcpy(topic + l->receipt, &cell->info, sizeof cell->info);
        if (cell->info.info.availability != AZ_AVAIL_UNSUPPORTED) h->supported |= bit;
        if (cell->info.info.availability != AZ_AVAIL_READY) continue;
        h->valid |= bit;
        if (l->type == DS_INLINE) memcpy(topic + l->value, cell->data, l->width);
        else {
            const void *ptr = cell->data;
            memcpy(topic + l->value, &ptr, sizeof ptr);
        }
    }
}

static void ds_view(struct ds_delivery *out, const struct ds_image *im, const struct ds_image *previous) {
    az_deck_status_event_v1 *e = &out->event;
    az_deck_status_view_v1 *v = &out->view;
    *v = (az_deck_status_view_v1) {.size = sizeof *v, .version = AZ_DATA_VERSION};
    /* The named view pointers are explicit too: never alias a struct as an array. */
    const size_t offsets[AZ_STATUS_TOPIC_COUNT] = {
#define DS_VIEW_MEMBER(F) offsetof(az_deck_status_view_v1, F)
        DS_VIEW_MEMBER(load), DS_VIEW_MEMBER(metadata), DS_VIEW_MEMBER(key), DS_VIEW_MEMBER(cues),
        DS_VIEW_MEMBER(loop), DS_VIEW_MEMBER(grid), DS_VIEW_MEMBER(phrases), DS_VIEW_MEMBER(transport),
        DS_VIEW_MEMBER(position), DS_VIEW_MEMBER(tempo), DS_VIEW_MEMBER(sync), DS_VIEW_MEMBER(settings),
        DS_VIEW_MEMBER(phase), DS_VIEW_MEMBER(stems), DS_VIEW_MEMBER(overview), DS_VIEW_MEMBER(phrase_playback)
#undef DS_VIEW_MEMBER
    };
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
        if (!(e->topics & AZ_STATUS_TOPIC(t))) continue;
        unsigned char *topic = (unsigned char *) &out->values + ds_layout[t].offset;
        az_status_topic_v1 *h = (void *) topic;
        h->size = ds_layout[t].size;
        h->version = AZ_DATA_VERSION;
        memcpy((unsigned char *) v + offsets[t], &topic, sizeof topic);
        ds_view_fields(topic, t, im, previous);
        if (h->changed) e->changed_topics |= AZ_STATUS_TOPIC(t);
    }
    e->view = v;
}
/* Prefer the first retained cut after eviction. Only the routes actually
 * skipped through that cut join the loss. Later unrelated or intact actions
 * survive as ordinary publications. Evicted capture reasons live off-ring. */
static size_t ds_history_after(const struct ds_deck *d, uint64_t sequence) {
    size_t lo = 0, hi = d->length;
    if (!hi || d->history[(d->next + DS_HISTORY - 1) % DS_HISTORY].image.sequence <= sequence) return hi;
    size_t oldest = (d->next + DS_HISTORY - hi) % DS_HISTORY;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (d->history[(oldest + mid) % DS_HISTORY].image.sequence <= sequence) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
struct ds_pending {
    const struct ds_record *chosen;
    const struct ds_image *image;
    uint64_t lost;
    unsigned reason, kind;
};
static void ds_cursor_loss(const struct ds_deck *d, const struct ds_cursor *c, uint64_t topics, struct ds_pending *pending) {
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++)
        if ((topics & AZ_STATUS_TOPIC(t)) &&
            d->evicted[t] > c->covered) {
            pending->lost |= AZ_STATUS_TOPIC(t);
            pending->reason |= AZ_STATUS_EVICTED;
            for (unsigned b = 0; b < 4; b++)
                if (d->loss[b][t] > c->covered) pending->reason |= 1u << b;
        }
    pending->lost |= c->evicted_motion;
    if (c->evicted_motion) pending->reason |= AZ_STATUS_EVICTED;
}

static void ds_resync_record(const struct ds_record *r, uint64_t topics, uint64_t change, uint64_t gap, struct ds_pending *pending) {
    pending->kind = AZ_STATUS_RESYNC;
    if (gap) {
        pending->lost |= gap;
        pending->reason |= r->reason;
    }
    if ((change & ~gap) || (r->action.ordinal && (topics & DS_ACTION_ROUTES)) ||
        (r->pair.size && (topics & DS_PHRASE_ROUTE))) {
        pending->lost |= change;
        pending->reason |= AZ_STATUS_CUT_SKIPPED;
    }
    pending->image = &r->image;
}

static void ds_cursor_history(const struct ds_deck *d, struct ds_cursor *c, uint64_t topics, struct ds_pending *pending) {
    uint64_t after = !pending->lost && c->examined > c->covered ? c->examined : c->covered;
    for (size_t i = ds_history_after(d, after); i < d->length; i++) {
        const struct ds_record *r = &d->history[(d->next + DS_HISTORY - d->length + i) % DS_HISTORY];
        uint64_t change = r->routes & topics, gap = r->lost & topics;
        if (r->motion_barrier) change |= ds_motion_changes(&c->last, &r->image, topics);
        if (pending->lost || gap) {
            ds_resync_record(r, topics, change, gap, pending);
            break;
        }
        if (change) {
            pending->chosen = r;
            pending->image = &r->image;
            break;
        }
            /* This receipt's own routes do not match. Leave a still-owed
             * coalesced sample before it eligible for deadline delivery. */
        c->examined = r->image.sequence;
    }
}

static void ds_motion_deadline(uint64_t due, uint64_t *deadline) {
    int order = *deadline ? compare_timestamps(due, *deadline, 1000000000, 1000000000, 1) : AZ_TIME_BEFORE;
    if (order != AZ_TIME_INVALID && order == AZ_TIME_BEFORE) *deadline = due;
}

static void ds_cursor_motion(const struct ds_deck *d, const struct ds_cursor *c, const struct ds_subscription *s, uint64_t topics, uint64_t now, uint64_t *deadline, struct ds_delivery *out, struct ds_pending *pending) {
    int dirty_motion = !pending->image && d->motion.image.sequence > c->covered &&
                       ds_motion_changes(&c->last, &d->motion.image, topics);
    if (dirty_motion) {
        uint64_t due = c->last_motion_ns + (UINT64_C(1000000000) + s->watch.position_hz - 1) / s->watch.position_hz;
        int due_order = compare_timestamps(now, due, 1000000000, 1000000000, 1);
        if (due_order == AZ_TIME_INVALID) return;
        if (due_order == AZ_TIME_EQUAL || due_order == AZ_TIME_AFTER) {
            pending->image = &d->motion.image;
            out->motion = 1;
        } else {
            ds_motion_deadline(due, deadline);
        }
    }
}

static void ds_delivery_actions(struct ds_delivery *out, const struct ds_pending *pending, uint64_t topics) {
    if (pending->chosen && pending->chosen->action.ordinal && (topics & DS_ACTION_ROUTES)) {
        out->action = pending->chosen->action;
        out->event.occurrence = &out->action;
        out->event.changed_topics |= topics & DS_ACTION_ROUTES;
    }
    if (pending->chosen && pending->chosen->pair.size && (topics & DS_PHRASE_ROUTE)) {
        out->pair = pending->chosen->pair;
        out->event.phrase_transition = &out->pair;
        out->event.changed_topics |= DS_PHRASE_ROUTE;
    }
}

static void ds_deliver_cursor(struct ds_subscription *s, unsigned deck, struct ds_cursor *c, const struct ds_pending *pending, struct ds_delivery *out, uint64_t topics, uint64_t now) {
    ds_image_select(&out->pin, pending->image, topics);
    out->cursor_before = c->covered;
    out->event = (az_deck_status_event_v1) {.size = sizeof out->event, .version = AZ_DATA_VERSION, .kind = pending->kind, .deck = deck, .subscription = s->handle, .sequence = pending->image->sequence, .load_epoch = pending->image->epoch, .observed_ns = pending->image->observed, .published_ns = pending->image->published, .topics = topics, .lost_topics = pending->lost, .loss_reason = pending->reason};
    ds_view(out, pending->image, pending->kind == AZ_STATUS_CHANGE ? &c->last : NULL);
    ds_delivery_actions(out, pending, topics);
    if (pending->kind != AZ_STATUS_CHANGE) out->event.changed_topics = topics;
    if (pending->kind == AZ_STATUS_RESYNC) c->evicted_motion = 0;
    if (pending->kind != AZ_STATUS_CHANGE || out->event.changed_topics & (AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE)))
        c->last_motion_ns = now;
    c->covered = pending->image->sequence;
    ds_image_select(&c->last, pending->image, topics);
    out->owner = (unsigned) s->owner;
    out->fn = s->fn;
    out->user = s->user;
}
static int ds_next(struct ds_subscription *s, unsigned deck, struct ds_delivery *out,
                   uint64_t now, uint64_t *deadline) {
    struct ds_deck *d = &ds.deck[deck];
    struct ds_cursor *c = &s->cursor[deck];
    const uint64_t topics = s->watch.topics;
    struct ds_pending pending = {.kind = AZ_STATUS_CHANGE};
    if (c->initial) {
        pending.image = &c->last;
        pending.kind = AZ_STATUS_INITIAL;
        c->initial = 0;
    } else {
        ds_cursor_loss(d, c, topics, &pending);
        ds_cursor_history(d, c, topics, &pending);
        if (!pending.image && pending.lost) {
            pending.kind = AZ_STATUS_RESYNC;
            pending.image = &d->current;
        }
        ds_cursor_motion(d, c, s, topics, now, deadline, out, &pending);
        if (!pending.image) return 0;
    }
    ds_deliver_cursor(s, deck, c, &pending, out, topics, now);
    return 1;
}
static unsigned ds_latency_bucket(uint64_t ns) {
    uint64_t ms = ns / 1000000 + (ns % 1000000 != 0);
    return ms < DS_LATENCY_BUCKETS ? (unsigned) ms : DS_LATENCY_BUCKETS - 1;
}
static void ds_pick_delivery(struct ds_delivery *call, uint64_t *deadline) {
    unsigned n = 0;
    for (; n < 4 * DS_SUBS; n++) {
        unsigned pos = ds.rotation++ % (4 * DS_SUBS), deck = pos % 4;
        struct ds_subscription *s = &ds.sub[pos / 4];
        if (!s->handle || !(s->watch.deck_mask & (1u << deck)) || !az_status_host_live(s->owner)) continue;
        if (ds_next(s, deck, call, ds_now(), deadline)) break;
    }
}

static void ds_callback_duration(const struct ds_delivery *call, uint64_t elapsed) {
    if (call->owner < DS_SUBS) {
        int slow = compare_timestamps(elapsed, UINT64_C(33000000), 1000000000, 1000000000, 1);
        int maximum = compare_timestamps(elapsed, ds.max_callback_ns[call->owner], 1000000000, 1000000000, 1);
        if (slow != AZ_TIME_INVALID && slow == AZ_TIME_AFTER) ds.slow_callbacks[call->owner]++;
        if (maximum != AZ_TIME_INVALID && maximum == AZ_TIME_AFTER) ds.max_callback_ns[call->owner] = elapsed;
    }
}

static void ds_proof_find_gap(struct ds_proof_record *r) {
    for (unsigned i = ds_proofs.count - 1; i > 0; i--) {
        const struct ds_proof_record *gap = &ds_proofs.record[i - 1];
        if (gap->kind == DS_PROOF_GAP && gap->deck == r->deck &&
            gap->epoch == r->epoch && gap->sequence == r->sequence) {
            r->interval = gap->interval;
            r->source = gap->source;
            break;
        }
    }
}

static void ds_callback_resync(const struct ds_delivery *call, struct ds_metrics *metric) {
    if (call->event.kind == AZ_STATUS_RESYNC) {
        metric->resyncs++;
        metric->last_lost_topics = call->event.lost_topics;
        struct ds_proof_record *r = ds_proof_new(DS_PROOF_RESYNC, call->event.deck,
                                                 call->event.load_epoch, call->event.sequence);
        if (r) {
            r->handle = call->event.subscription;
            r->topics = call->event.topics;
            r->lost = call->event.lost_topics;
            r->reason = call->event.loss_reason;
            r->cursor_before = call->cursor_before;
            ds_proof_find_gap(r);
        }
    }
}

static void ds_callback_dispatch(const struct ds_delivery *call, struct ds_metrics *metric, uint64_t before) {
    int published_order = compare_timestamps(call->event.published_ns, before, 1000000000, 1000000000, 1);
    if (call->event.published_ns && published_order != AZ_TIME_INVALID &&
        (published_order == AZ_TIME_BEFORE || published_order == AZ_TIME_EQUAL))
        metric->dispatch[call->motion][ds_latency_bucket(before - call->event.published_ns)]++;
}

static void ds_callback_latency(const struct ds_delivery *call, struct ds_metrics *metric, uint64_t before) {
    int observed_order = compare_timestamps(call->event.observed_ns, before, 1000000000, 1000000000, 1);
    if (call->event.kind == AZ_STATUS_CHANGE && call->event.observed_ns && observed_order != AZ_TIME_INVALID &&
        (observed_order == AZ_TIME_BEFORE || observed_order == AZ_TIME_EQUAL)) {
        metric->latency[call->motion][ds_latency_bucket(before - call->event.observed_ns)]++;
        ds_callback_dispatch(call, metric, before);
    }
}

unsigned ds_drain(unsigned budget) {
    unsigned delivered = 0;
    uint64_t deadline = 0;
    while (delivered < budget) {
        struct ds_delivery call = {0};
        pthread_mutex_lock(&ds.mu);
        ds_pick_delivery(&call, &deadline);
        pthread_mutex_unlock(&ds.mu);
        if (!call.fn) break;
        uint64_t before = ds_now();
        call.fn(&call.event, call.user);
        uint64_t elapsed = ds_now() - before;
        pthread_mutex_lock(&ds.mu);
        ds_callback_duration(&call, elapsed);
        struct ds_metrics *metric = &ds.metrics[call.event.deck];
        metric->callbacks++;
        ds_callback_resync(&call, metric);
        ds_callback_latency(&call, metric, before);
        ds_image_copy(&call.pin, NULL);
        pthread_mutex_unlock(&ds.mu);
        delivered++;
    }
    if (delivered == budget && budget) az_status_host_wake();
    az_status_host_deadline(deadline);
    return delivered;
}

int sdk_status_analysis_bind(ds_core_load_fn observer) {
    if (!ds.enabled) return AZ_UNSUPPORTED;
    atomic_store_explicit(&ds_core_load, observer, memory_order_release);
    uint64_t topics = AZ_STATUS_TOPIC(AZ_STATUS_LOAD);
    az_status_host_gui_interest(topics);
    for (unsigned deck = 0; deck < 4; ++deck) {
        az_status_host_native_interest(deck, topics);
        az_status_host_store_interest(deck, topics);
        pthread_mutex_lock(&ds.mu);
        struct ds_analysis_cut copy = ds_analysis_copy(&ds.deck[deck].current);
        pthread_mutex_unlock(&ds.mu);
        ds_analysis_deliver(deck, &copy);
    }
    return AZ_OK;
}
static int sa_load_cell_ready(const struct ds_cell *cell, uint64_t load) {
    return cell && cell->info.info.availability == AZ_AVAIL_READY &&
           cell->info.scope == AZ_STATUS_LOAD_SCOPE && cell->info.scope_id == load &&
           cell->bytes == sizeof(az_track_ref);
}
int az_status_loaded_reference(unsigned deck, uint64_t load, az_track_ref *out) {
    if (deck >= 4 || !load || !out) return AZ_INVALID;
    int rc = AZ_BUSY;
    pthread_mutex_lock(&ds.mu);
    const struct ds_image *image = &ds.deck[deck].current;
    const struct ds_cell *cell = image->field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
    if (image->epoch != load) rc = AZ_STALE;
    else if (sa_load_cell_ready(cell, load)) {
        memcpy(out, cell->data, sizeof *out);
        rc = AZ_OK;
    }
    pthread_mutex_unlock(&ds.mu);
    return rc;
}

int ds_enabled(void) { return ds.enabled != 0; }
void ds_enable(void) { ds.enabled = 1; }
void ds_read_stamp(unsigned deck, struct ds_deck_stamp *out) {
    pthread_mutex_lock(&ds.mu);
    *out = (struct ds_deck_stamp) {ds.deck[deck].current.epoch, ds.deck[deck].last_epoch, ds.deck[deck].motion_generation};
    pthread_mutex_unlock(&ds.mu);
}
void ds_read_receipts(unsigned deck, struct ds_field *fields, size_t count, uint64_t *epoch) {
    pthread_mutex_lock(&ds.mu);
    if (epoch) *epoch = ds.deck[deck].current.epoch;
    for (size_t i = 0; i < count; ++i) {
        const struct ds_cell *cell = ds.deck[deck].current.field[fields[i].topic][fields[i].field];
        fields[i].info = cell ? cell->info : (az_status_field_info_v1) {0};
    }
    pthread_mutex_unlock(&ds.mu);
}
void ds_set_stem_binding(void (*bind)(unsigned, uint64_t, uint64_t, const char *)) {
    pthread_mutex_lock(&ds.mu);
    ds_bind_stems = bind;
    pthread_mutex_unlock(&ds.mu);
}
void ds_watch_stems(unsigned deck, int watch) {
    pthread_mutex_lock(&ds.mu);
    ds.deck[deck].watch_stems = watch;
    ds_send_stem_binding(deck);
    pthread_mutex_unlock(&ds.mu);
}
int ds_read_phrase_span(unsigned deck, uint64_t epoch, const struct ds_path *path, struct ds_phrase_span *span) {
    int ready = 0;
    struct ds_map map;
    pthread_mutex_lock(&ds.mu);
    const struct ds_image *im = &ds.deck[deck].current;
    const az_status_phrase_endpoint_v1 *active = ds_value(im, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    if (im->epoch == epoch && ds_map_at(im, path, &map) && ds_endpoint_known(&map, active) && ds_inside_item(&map, active, path)) {
        const az_phrase *row = &map.rows[active->index];
        *span = (struct ds_phrase_span) {ds_grid_point(&map, row->start_beat), ds_grid_point(&map, row->end_beat)};
        ready = 1;
    }
    pthread_mutex_unlock(&ds.mu);
    return ready;
}
int ds_path_inside_span(const struct ds_phrase_span *span, const struct ds_path *p) {
    int from_start = ds_coord_order(&p->from, &span->start, p->from_window_rate, p->from_window_samples);
    int to_start = ds_coord_order(&p->to, &span->start, p->observed_window_rate, p->observed_window_samples);
    int from_end = ds_coord_order(&p->from, &span->end, p->from_window_rate, p->from_window_samples);
    int to_end = ds_coord_order(&p->to, &span->end, p->observed_window_rate, p->observed_window_samples);
    return !p->ended && ds_point_inside(from_start, from_end) && ds_point_inside(to_start, to_end);
}

void ds_capture_metrics(struct ds_metrics_snapshot *snapshot) {
    pthread_mutex_lock(&ds.mu);
    memcpy(snapshot->deck, ds.metrics, sizeof snapshot->deck);
    memcpy(snapshot->slow, ds.slow_callbacks, sizeof snapshot->slow);
    memcpy(snapshot->longest, ds.max_callback_ns, sizeof snapshot->longest);
    for (unsigned d = 0; d < 4; d++) snapshot->sequence[d] = ds.deck[d].current.sequence;
    snapshot->bytes = ds.bytes;
    snapshot->peak = ds.peak;
    snapshot->proof_enabled = ds_proofs.enabled;
    snapshot->proof_count = ds_proofs.count;
    snapshot->proof_overflow = ds_proofs.overflow;
    snapshot->fixed_bytes = sizeof ds + sizeof ds_pending + sizeof ds_proofs;
    snapshot->proof_bytes = sizeof ds_proofs;
    snapshot->proof_touched = offsetof(__typeof__(ds_proofs), record) + snapshot->proof_count * sizeof ds_proofs.record[0];
    snapshot->proof = ds_proofs.record;
    memcpy(snapshot->capture_base, ds_proofs.capture_base, sizeof snapshot->capture_base);
    memcpy(snapshot->revision_base, ds_proofs.revision_base, sizeof snapshot->revision_base);
    memcpy(snapshot->resync_base, ds_proofs.resync_base, sizeof snapshot->resync_base);
    memcpy(snapshot->sequence_base, ds_proofs.sequence_base, sizeof snapshot->sequence_base);
    pthread_mutex_unlock(&ds.mu);
}
void ds_enable_proofs(void) {
    pthread_mutex_lock(&ds.mu);
    for (unsigned d = 0; d < 4; d++) {
        ds_proofs.capture_base[d] = ds.metrics[d].capture_gaps;
        ds_proofs.revision_base[d] = ds.metrics[d].revision_losses;
        ds_proofs.resync_base[d] = ds.metrics[d].resyncs;
        ds_proofs.sequence_base[d] = ds.deck[d].current.sequence;
    }
    ds_proofs.enabled = 1;
    atomic_store_explicit(&ds_proof_enabled, 1, memory_order_release);
    pthread_mutex_unlock(&ds.mu);
}

unsigned ds_owner_subscriptions(int owner) {
    unsigned count = 0;
    pthread_mutex_lock(&ds.mu);
    for (unsigned i = 0; i < DS_SUBS; ++i)
        if (ds.sub[i].handle && ds.sub[i].owner == owner) ++count;
    pthread_mutex_unlock(&ds.mu);
    return count;
}
