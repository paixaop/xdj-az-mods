/* SPDX-License-Identifier: GPL-3.0-only */
/* All state below belongs to reducer/UI, never to a real-time writer. Each
 * retained cut shares immutable field revisions. The separate motion slot
 * cannot evict a discrete transition. Callback pins outlive cursor removal. */
#include "sdk_status_layout.h"
#include "sdk_status_proof.h"
#include "../common/azstatus_identity.h"
#include "../common/aztimestamp.h"
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define DS_HISTORY 256u
#define DS_SUBS 32u
#define DS_BYTES (16u * 1024u * 1024u)
#define DS_ACTION_ROUTES (AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION))
#define DS_PHRASE_ROUTE AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION)
#define DS_MOTION_TOPICS (AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE))
/* Internal journal flag, added only after the reducer commits the cut. */
#define DS_FLUSH_MOTION 64u
static uint64_t ds_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}
static void ds_wake(void);
static void ds_deadline(uint64_t ns);
static void sdk_status_store_binding(unsigned deck, uint64_t epoch, const az_track_ref *ref, uint64_t cut);
static void sdk_status_store_interest(unsigned deck, uint64_t topics);
static void ds_native_interest(unsigned deck, uint64_t topics);
static int ds_live(int owner);
static void ds_gui_interest(uint64_t topics);

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
/* One-millisecond upper-bound buckets; the last bucket is over 250 ms. */
#define DS_LATENCY_BUCKETS 252
struct ds_metrics {
    uint64_t high_water, evictions, capture_gaps, revision_losses, callbacks, resyncs;
    uint64_t last_lost_topics;
    uint64_t latency[2][DS_LATENCY_BUCKETS];
    uint64_t dispatch[2][DS_LATENCY_BUCKETS];
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
#define DS_PROOF_CAPACITY 16384u
struct ds_proof_record {
    uint64_t id, interval, sequence, epoch, input_lost, lost, routes, pending;
    uint64_t settled, ready, unavailable, handle, topics, cursor_before;
    az_track_ref source;
    struct ds_proof_context context;
    unsigned kind, deck, file_only, flags, input_reason, reason, committed;
};
static struct {
    unsigned enabled, count, overflow;
    uint64_t capture_base[4], revision_base[4], resync_base[4], sequence_base[4];
    struct { uint64_t interval, epoch, pending; az_track_ref source; } active[4];
    struct ds_proof_record record[DS_PROOF_CAPACITY];
} ds_proofs;
static struct ds_proof_record *ds_proof_new(unsigned kind, unsigned deck, uint64_t epoch, uint64_t sequence) {
    if (!ds_proofs.enabled) return NULL;
    if (ds_proofs.count == DS_PROOF_CAPACITY) { ds_proofs.overflow = 1; return NULL; }
    struct ds_proof_record *r = &ds_proofs.record[ds_proofs.count++];
    r->id = ds_proofs.count;
    r->kind = kind; r->deck = deck; r->epoch = epoch; r->sequence = sequence;
    return r;
}
static void ds_proof_cut(unsigned deck, uint64_t epoch, const az_track_ref *source, int file_only,
                         unsigned flags, uint64_t input_lost, unsigned input_reason,
                         uint64_t lost, unsigned reason, uint64_t routes, const struct ds_field *fields,
                         size_t count, int retire, unsigned committed) {
    if (!ds_proofs.enabled) return;
    if (retire && lost && ds_proofs.active[deck].interval)
        ds_proof_cut(deck, epoch, NULL, 0, flags, 0, 0, 0, 0, routes, NULL, 0, 1, committed);
    unsigned kind = lost ? DS_PROOF_GAP : retire ? DS_PROOF_RETIRE : file_only ? ds_proof_context.kind : 0;
    if (!kind || (kind != DS_PROOF_GAP && !ds_proofs.active[deck].interval)) return;
    if (kind == DS_PROOF_BASELINE && !(ds_proof_context.topics & ds_proofs.active[deck].pending)) return;
    struct ds_proof_record *r = ds_proof_new(kind, deck, epoch, ds.deck[deck].current.sequence);
    if (!r) return;
    if (file_only || (kind == DS_PROOF_RETIRE && ds_proof_context.kind == DS_PROOF_RETIRE))
        r->context = ds_proof_context;
    r->topics = r->context.topics;
    r->file_only = file_only; r->flags = flags; r->committed = committed;
    r->input_lost = input_lost; r->input_reason = input_reason;
    r->lost = lost; r->reason = reason; r->routes = routes;
    if (source) r->source = *source;
    if (kind == DS_PROOF_GAP && file_only) {
        if (!ds_proofs.active[deck].interval) {
            ds_proofs.active[deck].interval = r->id;
            ds_proofs.active[deck].epoch = epoch;
            if (source) ds_proofs.active[deck].source = *source;
        }
        ds_proofs.active[deck].pending |= input_lost;
    }
    if (kind != DS_PROOF_GAP || file_only) {
        r->interval = ds_proofs.active[deck].interval;
        if (!source) r->source = ds_proofs.active[deck].source;
    }
    if (kind == DS_PROOF_BASELINE && file_only && epoch == ds_proofs.active[deck].epoch) {
        uint64_t loading = 0;
        for (size_t i = 0; i < count; i++) {
            uint64_t bit = AZ_STATUS_TOPIC(fields[i].topic);
            unsigned availability = fields[i].info.info.availability;
            r->settled |= bit;
            if (availability == AZ_AVAIL_LOADING) loading |= bit;
            if (availability != AZ_AVAIL_READY && availability != AZ_AVAIL_EMPTY) r->unavailable |= bit;
        }
        r->settled &= ds_proofs.active[deck].pending & ~loading;
        r->unavailable &= r->settled;
        r->ready = r->settled & ~r->unavailable;
        ds_proofs.active[deck].pending &= ~r->settled;
    }
    if (kind == DS_PROOF_RETIRE) {
        r->context.cause |= retire ? DS_PROOF_BOUNDARY : 0;
        r->settled = r->unavailable = ds_proofs.active[deck].pending;
        ds_proofs.active[deck].pending = 0;
    }
    r->pending = ds_proofs.active[deck].pending;
    if (!r->pending) memset(&ds_proofs.active[deck], 0, sizeof ds_proofs.active[deck]);
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
static int ds_payload_same(unsigned topic, unsigned field, const void *before, const void *after,
                           size_t bytes, const struct ds_time_axis *a, const struct ds_time_axis *b) {
    if (!before || !after) return 0;
    unsigned type = ds_layout[topic].field[field].type;
    if ((topic == AZ_STATUS_POSITION && field <= AZ_STATUS_POSITION_SLIP_HEAD) ||
        (topic == AZ_STATUS_LOOP && (field == AZ_STATUS_LOOP_RETAINED_IN || field == AZ_STATUS_LOOP_RETAINED_OUT ||
                                     field == AZ_STATUS_LOOP_WORKING_IN || field == AZ_STATUS_LOOP_WORKING_OUT))) {
        const az_status_coord_v1 *old = before, *next = after;
        if (!b || old->rate_hz != next->rate_hz || old->reserved != next->reserved) return 0;
        int order = next->rate_hz
                        ? ds_compare_source_times(old->samples, old->rate_hz, next->samples, next->rate_hz,
                                                  b->window_rate, b->window_samples)
                        : compare_timestamps(old->seconds, next->seconds, 1, b->window_rate, b->window_samples);
        if (order == AZ_TIME_INVALID) return 0;
        return order == AZ_TIME_EQUAL;
    }
    if (type == DS_BEATS) {
        const az_beat *old = before, *next = after;
        for (unsigned i = 0; i < bytes / sizeof *old; i++) {
            if (!ds_axis_equal(a, b, i) || old[i].bpm != next[i].bpm ||
                old[i].beat_in_bar != next[i].beat_in_bar || old[i].reserved != next[i].reserved) return 0;
        }
        return 1;
    }
    if (type == DS_CUES) {
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
    if (type == DS_WAVE) {
        const az_waveform *old = before, *next = after;
        return old->sample_count == next->sample_count && ds_axis_equal(a, b, 0) &&
               !memcmp(old->samples, next->samples, next->sample_count);
    }
    if (type == DS_CUE_REF) {
        const az_status_cue_ref_v1 *old = before, *next = after;
        return old->load_epoch == next->load_epoch && old->provider_id == next->provider_id &&
               old->collection_revision == next->collection_revision && old->provenance == next->provenance &&
               old->cue_field == next->cue_field && old->entry.entry_id == next->entry.entry_id &&
               old->entry.slot == next->entry.slot && old->entry.classification == next->entry.classification;
    }
    if ((topic == AZ_STATUS_METADATA && field >= AZ_STATUS_METADATA_DURATION_S) ||
        (topic == AZ_STATUS_POSITION && field >= AZ_STATUS_POSITION_REMAINING_S)) {
        if (!b) return 0;
        if (a && a->ticks && b->ticks) return ds_axis_equal(a, b, 0);
        int order = compare_timestamps(*(const double *) before, *(const double *) after, 1,
                                       b->window_rate, b->window_samples);
        if (order == AZ_TIME_INVALID) return 0;
        return order == AZ_TIME_EQUAL;
    }
    return !memcmp(before, after, bytes);
}
static void ds_image_select(struct ds_image *dst, const struct ds_image *src, uint64_t topics) {
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
        if (!(topics & AZ_STATUS_TOPIC(t))) continue;
        for (unsigned f = 0; f < ds_layout[t].fields; f++) {
            struct ds_cell *next = src ? src->field[t][f] : NULL;
            if (next == dst->field[t][f]) continue;
            if (next) next->refs++;
            ds_drop(dst->field[t][f]);
            dst->field[t][f] = next;
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
static void ds_send_stem_binding(unsigned deck) {
    struct ds_deck *d = &ds.deck[deck];
    if (!ds_bind_stems || !d->watch_stems || !d->stem_binding) return;
    const struct ds_cell *path = d->current.field[AZ_STATUS_METADATA][AZ_STATUS_METADATA_PATH];
    const struct ds_cell *ref = d->current.field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
    const char *src = NULL;
    if (d->current.epoch && ref && ref->info.info.availability == AZ_AVAIL_READY && path &&
        path->info.scope_id == d->current.epoch && path->info.info.provenance == AZ_PROV_FILE &&
        path->info.info.availability == AZ_AVAIL_READY && path->bytes && path->data[0] == '/')
        src = (const char *) path->data;
    ds_bind_stems(deck, d->stem_binding, d->current.epoch, src);
}

static size_t ds_extent(const struct ds_field *p, uint64_t epoch) {
    const struct ds_layout *layout = &ds_layout[p->topic].field[p->field];
    const az_status_field_info_v1 *info = &p->info;
    if (info->info.availability > AZ_AVAIL_STALE ||
        info->info.provenance > AZ_PROV_DERIVED) return SIZE_MAX;
    if (info->info.availability != AZ_AVAIL_READY)
        return info->count ? SIZE_MAX : 0;
    if (!p->value || !info->scope_id ||
        (info->scope != AZ_STATUS_UNIT && info->scope != AZ_STATUS_LOAD_SCOPE) ||
        (info->scope == AZ_STATUS_LOAD_SCOPE && info->scope_id != epoch)) return SIZE_MAX;
    if (info->count > layout->limit) return SIZE_MAX;
    if (p->clock && (!p->clock->units_per_second || p->clock->units_per_second > UINT32_MAX ||
                     (layout->type == DS_BEATS && !p->clock->ticks))) return SIZE_MAX;
    switch (layout->type) {
    case DS_INLINE: return info->count == 1 ? layout->width : SIZE_MAX;
    case DS_STRING:
        if (info->count >= layout->limit ||
            strnlen(p->value, layout->limit) != info->count) return SIZE_MAX;
        return info->count + 1;
    case DS_CUES: return info->count ? info->count * sizeof(az_cue_entry) : SIZE_MAX;
    case DS_BEATS: return info->count ? info->count * sizeof(az_beat) : SIZE_MAX;
    case DS_PHRASES: return info->count ? info->count * sizeof(az_phrase) : SIZE_MAX;
    case DS_CUE_REF: return info->count == 1 ? sizeof(az_status_cue_ref_v1) : SIZE_MAX;
    case DS_ENDPOINT: return info->count == 1 ? sizeof(az_status_phrase_endpoint_v1) : SIZE_MAX;
    case DS_WAVE: {
        const az_waveform *w = p->value;
        if (info->count != 1 || w->size < sizeof *w || w->version != AZ_DATA_VERSION ||
            w->representation != AZ_WAVE_PREVIEW || !w->samples || !w->sample_count ||
            w->sample_count > 4096 || !p->clock || !p->clock->ticks || !isfinite(w->seconds_per_sample)) return SIZE_MAX;
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
    }
    return SIZE_MAX;
}
static int ds_same(const struct ds_cell *a, const struct ds_field *b, size_t bytes) {
    if (!a) return b->info.info.availability == AZ_AVAIL_UNSUPPORTED && !b->info.scope_id &&
                   !b->info.provider_id && !b->info.info.provenance && !b->info.info.revision;
    if (a->bytes != bytes || a->info.scope != b->info.scope || a->info.scope_id != b->info.scope_id ||
        a->info.provider_id != b->info.provider_id || a->info.count != b->info.count ||
        a->info.info.availability != b->info.info.availability ||
        a->info.info.provenance != b->info.info.provenance ||
        a->info.info.revision != b->info.info.revision) return 0;
    if (!bytes) return 1;
    /* Native immutable banks carry their writer's semantic edition. Their
     * identity is independent of the public seconds representation. */
    if (b->topic == AZ_STATUS_CUES && (b->field == AZ_STATUS_CUES_LIVE_HOT || b->field == AZ_STATUS_CUES_LIVE_MEMORY) &&
        b->info.info.provenance == AZ_PROV_NATIVE && b->info.info.revision) return 1;
    /* Older private map providers retain exact immutable editions but cannot
     * expose source ticks. Keep their raw lists without deriving boundaries. */
    if (b->topic == AZ_STATUS_GRID && b->info.info.provenance == AZ_PROV_NATIVE && b->info.info.revision &&
        !a->clock.ticks && (!b->clock || !b->clock->ticks)) return 1;
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
static void ds_evict(struct ds_deck *d, struct ds_record *r) {
    ds.metrics[d - ds.deck].evictions++;
    if (r->motion_barrier) {
        unsigned deck = (unsigned) (d - ds.deck);
        for (unsigned i = 0; i < DS_SUBS; i++) {
            struct ds_subscription *s = &ds.sub[i];
            struct ds_cursor *c = &s->cursor[deck];
            if (s->handle && (s->watch.deck_mask & (1u << deck)) && c->covered < r->image.sequence)
                c->evicted_motion |= ds_motion_changes(&c->last, &r->image, s->watch.topics);
        }
    }
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
static int ds_set(struct ds_image *im, const struct ds_field *p, size_t bytes) {
    struct ds_cell **slot = &im->field[p->topic][p->field];
    if (ds_same(*slot, p, bytes)) return 0;
    unsigned ticks_per_row = ds_layout[p->topic].field[p->field].type == DS_CUES ? 2 : 1;
    size_t tick_bytes = bytes && p->clock && p->clock->ticks ? p->info.count * ticks_per_row * sizeof(int64_t) : 0;
    size_t padding = tick_bytes ? (sizeof(int64_t) - bytes % sizeof(int64_t)) % sizeof(int64_t) : 0;
    size_t key_bytes = tick_bytes && ticks_per_row == 2 && p->clock->stored_cue_ms ? tick_bytes : 0;
    size_t clock_bytes = padding + tick_bytes + key_bytes;
    size_t allocation = sizeof(struct ds_cell) + bytes + clock_bytes;
    if (!ds_make_room(allocation)) return AZ_LIMIT;
    struct ds_cell *c = malloc(allocation);
    if (!c) return AZ_LIMIT;
    c->refs = 1;
    c->bytes = bytes;
    c->clock_bytes = clock_bytes;
    c->native_cue_revision = 0;
    c->info = p->info;
    c->clock = bytes && p->clock ? *p->clock : (struct ds_time_axis) {0};
    c->clock.ticks = tick_bytes ? (const void *) (c->data + bytes + padding) : NULL;
    c->clock.stored_cue_ms = key_bytes ? c->clock.ticks + tick_bytes / sizeof(int64_t) : NULL;
    if (tick_bytes) memcpy(c->data + bytes + padding, p->clock->ticks, tick_bytes);
    if (key_bytes) memcpy(c->data + bytes + padding + tick_bytes, p->clock->stored_cue_ms, key_bytes);
    if (bytes) {
        if (ds_layout[p->topic].field[p->field].type == DS_WAVE) {
            az_waveform *w = (void *) c->data;
            memcpy(w, p->value, sizeof *w);
            memcpy(c->data + sizeof *w, w->samples, w->sample_count);
            w->samples = c->data + sizeof *w;
        } else memcpy(c->data, p->value, bytes);
    }
    ds.bytes += allocation;
    if (ds.bytes > ds.peak) ds.peak = ds.bytes;
    ds_drop(*slot);
    *slot = c;
    return 1;
}

/* The public edition also changes when a native row's storage evidence
 * changes. Preserve the reducer's separate writer edition for recall proof. */
static int ds_set_live_cues(struct ds_deck *d, const struct ds_field *p, size_t bytes, int annotations) {
    const struct ds_cell *old = d->current.field[AZ_STATUS_CUES][p->field];
    uint64_t native = p->info.info.revision;
    if (!annotations && old && old->native_cue_revision == native &&
        old->info.scope_id == p->info.scope_id && old->info.provider_id == p->info.provider_id &&
        old->info.info.availability == p->info.info.availability && old->info.count == p->info.count &&
        old->info.info.provenance == p->info.info.provenance) return 0;
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
static uint64_t ds_stale(struct ds_image *im, uint64_t mask, int boundary, unsigned provenance, uint64_t observed) {
    uint64_t changed = 0;
    for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++) {
        if (!(mask & AZ_STATUS_TOPIC(t))) continue;
        for (unsigned f = 0; f < ds_layout[t].fields; f++) {
            struct ds_cell **slot = &im->field[t][f];
            int gui_tempo = t == AZ_STATUS_TEMPO && f == AZ_STATUS_TEMPO_CURRENT_BPM;
            if (!*slot || (provenance && (*slot)->info.info.provenance != provenance) || (boundary && (*slot)->info.scope != AZ_STATUS_LOAD_SCOPE && !gui_tempo)) continue;
            struct ds_field stale = {t, f, (*slot)->info, NULL, NULL};
            if (stale.info.info.availability != AZ_AVAIL_UNSUPPORTED)
                stale.info.info.availability = AZ_AVAIL_STALE;
            stale.info.info.observed_ns = observed;
            stale.info.count = 0;
            if (boundary && stale.info.scope == AZ_STATUS_LOAD_SCOPE) stale.info.scope_id = im->epoch;
            int rc = ds_set(im, &stale, 0);
            if (rc == AZ_LIMIT) ds_clear_field(slot);
            if (rc) changed |= AZ_STATUS_TOPIC(t);
        }
    }
    return changed;
}

static void ds_append(struct ds_deck *d, unsigned flags, uint64_t routes,
                      const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair,
                      uint64_t lost, unsigned reason) {
    struct ds_record *r = &d->motion;
    if (!(flags & DS_MOTION) || (flags & DS_FLUSH_MOTION) || (routes & ~DS_MOTION_TOPICS) || lost || action || pair) {
        r = &d->history[d->next];
        if (d->length == DS_HISTORY) ds_evict(d, r);
        d->next = (d->next + 1) % DS_HISTORY;
        if (d->length < DS_HISTORY) d->length++;
    }
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

#include "sdk_status_derived.c"
#include "sdk_status_path.c"

static int ds_apply(const struct ds_path *path, const az_track_ref *expected, int file_only, unsigned deck, uint64_t epoch, unsigned flags, uint64_t observed,
                    const struct ds_field *fields, size_t count,
                    const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair,
                    uint64_t lost, unsigned reason) {
    const uint64_t input_lost = lost;
    const unsigned input_reason = reason;
    if (deck >= 4 || (count && !fields) || count > AZ_STATUS_TOPIC_COUNT * 9 ||
        (flags & ~(DS_BOUNDARY | DS_MOTION | DS_UNIT_INPUT | DS_NATIVE_INPUT | DS_FIELD_LOSS | DS_HEAD_LOSS)) || (lost & ~AZ_STATUS_ALL_V1) ||
        (!!lost != !!reason) || (reason & ~15u)) return AZ_INVALID;
    if (action && (action->size < sizeof *action || action->version != AZ_DATA_VERSION ||
                   !action->ordinal || action->kind < AZ_STATUS_SEEK || action->kind > AZ_STATUS_WRAP ||
                   (action->endpoints & ~3u))) return AZ_INVALID;
    if (pair && (pair->size < sizeof *pair || pair->version != AZ_DATA_VERSION ||
                 pair->discontinuity > AZ_STATUS_WRAP || (action && action->kind != pair->discontinuity))) return AZ_INVALID;
    for (size_t i = 0; i < count; i++) {
        const struct ds_field *p = &fields[i];
        if (p->topic >= AZ_STATUS_TOPIC_COUNT || p->field >= ds_layout[p->topic].fields ||
            ds_extent(p, epoch) == SIZE_MAX) return AZ_INVALID;
    }
    if ((flags & DS_FIELD_LOSS) && (!count || !(flags & DS_NATIVE_INPUT) || !lost))
        return AZ_INVALID;
    if ((flags & DS_HEAD_LOSS) && (!(flags & DS_NATIVE_INPUT) || !lost ||
                                   (lost & ~(DS_ACTION_ROUTES | DS_PHRASE_ROUTE | AZ_STATUS_TOPIC(AZ_STATUS_PHASE))))) return AZ_INVALID;
    if (flags & DS_UNIT_INPUT) {
        if (flags != DS_UNIT_INPUT || action || path || pair || !count) return AZ_INVALID;
        for (size_t i = 0; i < count; i++)
            if (fields[i].info.scope != AZ_STATUS_UNIT) return AZ_INVALID;
    }
    pthread_mutex_lock(&ds.mu);
    struct ds_deck *d = &ds.deck[deck];
    if (flags & DS_UNIT_INPUT) epoch = d->current.epoch;
    int boundary = (flags & DS_BOUNDARY) && d->current.epoch != epoch;
    int rc = AZ_OK;
    if (d->current.sequence == UINT64_MAX) rc = AZ_LIMIT;
    else if ((boundary && epoch && epoch <= d->last_epoch) ||
             (!(flags & DS_BOUNDARY) && epoch != d->current.epoch) ||
             (action && action->ordinal <= d->last_ordinal)) rc = AZ_STALE;
    if (flags & DS_NATIVE_INPUT)
        for (size_t i = 0; i < count; ++i)
            if (fields[i].topic == AZ_STATUS_STEMS && fields[i].info.provider_id != d->stem_binding)
                rc = AZ_STALE;
    if (expected) {
        struct ds_cell *track = d->current.field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
        if (!track || track->info.info.availability != AZ_AVAIL_READY) rc = AZ_STALE;
        else {
            const az_track_ref *ref = (const void *) track->data;
            if (ref->source_id != expected->source_id || ref->source_generation != expected->source_generation ||
                ref->library_id != expected->library_id || ref->track_id != expected->track_id) rc = AZ_STALE;
        }
    }
    if (path && rc == AZ_OK) {
        uint64_t first = path->first_order ? path->first_order : path->order;
        if (!epoch || !path->order || !path->generation || path->kind > DS_PATH_ACTION ||
            path->endpoints > 3 || (path->kind == DS_PATH_ACTION) != !!action || pair) rc = AZ_INVALID;
        else if (d->current.sequence > UINT64_MAX - 2052) rc = AZ_LIMIT;
        else if (!boundary && (path->order <= d->path_order || path->generation < d->motion_generation ||
                               (path->kind == DS_PATH_ACTION && path->generation == d->motion_generation))) rc = AZ_STALE;
        else if ((boundary && path->kind != DS_PATH_BASELINE) ||
                 (!boundary && d->path_order && first != d->path_order + 1 &&
                  !(path->kind == DS_PATH_BASELINE && lost)) ||
                 (path->kind == DS_PATH_TRAVEL && path->generation != d->motion_generation)) rc = AZ_PENDING;
        if (rc == AZ_OK && path->first_order) {
            struct ds_map map;
            const az_status_phrase_endpoint_v1 *active = ds_value(&d->current, AZ_STATUS_PHRASE_TRANSITION,
                                                                  AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
            if (flags != (DS_NATIVE_INPUT | DS_MOTION) || lost || path->kind != DS_PATH_TRAVEL || path->endpoints != 3 ||
                !d->path_order || first >= path->order || !ds_map_at(&d->current, path, &map) ||
                !ds_endpoint_known(&map, active) || !ds_inside_item(&map, active, path)) rc = AZ_PENDING;
        }
    }
    if (rc != AZ_OK) {
        pthread_mutex_unlock(&ds.mu);
        return rc;
    }
    uint64_t routes = 0;
    uint64_t now = ds_now();
    if (!observed) observed = now;
    int generation_barrier = path && d->motion_generation && path->generation != d->motion_generation;
    int motion_barrier = boundary || action || generation_barrier;
    uint32_t availability = (flags & DS_MOTION) ? ds_motion_availability(&d->current) : 0;
    if (boundary) {
        d->current.epoch = epoch;
        if (epoch) d->last_epoch = epoch;
        d->path_order = d->motion_generation = 0;
        d->head_window_samples = d->head_window_rate = 0;
        routes |= ds_stale(&d->current, AZ_STATUS_ALL_V1, 1, 0, observed);
        routes |= AZ_STATUS_TOPIC(AZ_STATUS_LOAD);
    }
    az_status_phrase_transition_v1 generated = {0};
    uint64_t path_loss = 0;
    if (path && path->kind != DS_PATH_BASELINE)
        ds_path_reduce(d, path, action, observed, &generated, &routes, &path_loss);
    else if (!path && action && !pair)
        ds_path_reduce(d, NULL, action, observed, &generated, &routes, &path_loss);
    if (flags & DS_HEAD_LOSS) {
        if (lost & AZ_STATUS_TOPIC(AZ_STATUS_POSITION)) d->head_window_samples = d->head_window_rate = 0;
        if (lost & AZ_STATUS_TOPIC(AZ_STATUS_POSITION))
            routes |= ds_withdraw(&d->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, observed, &path_loss);
        if (lost & DS_PHRASE_ROUTE)
            routes |= ds_withdraw(&d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, observed, &path_loss);
    }
    if (lost && !(flags & (DS_UNIT_INPUT | DS_FIELD_LOSS | DS_HEAD_LOSS))) routes |= ds_stale(&d->current, lost, 0, file_only ? AZ_PROV_FILE : (flags & DS_NATIVE_INPUT) ? AZ_PROV_NATIVE
                                                                                                                                                                         : AZ_PROV_NONE,
                                                                                              observed);
    unsigned phrase_changed = 0, mapping_renewed = 0;
    /* Without a witnessed landing, a discontinuity cannot carry a head or
     * phase copied before the reposition. An explicit head in this transaction
     * replaces the withdrawal atomically. */
    if (action)
        routes |= ds_withdraw(&d->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, observed, &path_loss);
    if (action || generation_barrier)
        routes |= ds_withdraw(&d->current, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD, observed, &path_loss);
    int bind = boundary;
    int stem_bind = boundary || (lost & (AZ_STATUS_TOPIC(AZ_STATUS_LOAD) | AZ_STATUS_TOPIC(AZ_STATUS_METADATA)));
    /* Apply collections before their references, regardless of input order. */
    for (unsigned references = 0; references < 2; ++references)
    for (size_t i = 0; i < count; i++) {
        const struct ds_field *p = &fields[i];
        int is_reference = p->topic == AZ_STATUS_LOOP && p->field == AZ_STATUS_LOOP_SAVED_ENTRY;
        if (is_reference != (int) references) continue;
        struct ds_field normalized;
        az_status_cue_ref_v1 canonical;
        if (is_reference && p->info.info.availability == AZ_AVAIL_READY) {
            normalized = *p;
            canonical = *(const az_status_cue_ref_v1 *) p->value;
            const az_cue_entry *row = ds_cue_reference_row(&d->current, &canonical);
            if (row) {
                canonical.entry = *row;
                normalized.value = &canonical;
            } else {
                normalized.info.info.availability = AZ_AVAIL_STALE;
                normalized.info.count = 0;
                normalized.value = NULL;
            }
            p = &normalized;
        }
        const struct ds_cell *old = d->current.field[p->topic][p->field];
        if (p->topic == AZ_STATUS_LOAD && p->field == AZ_STATUS_LOAD_SOURCE &&
            p->info.info.provenance != AZ_PROV_NATIVE && old && old->info.info.provenance == AZ_PROV_NATIVE &&
            (old->info.info.availability == AZ_AVAIL_READY || p->info.info.availability == AZ_AVAIL_STALE)) continue;
        if (file_only && old && old->info.info.provenance == AZ_PROV_NATIVE &&
            old->info.info.availability == AZ_AVAIL_READY) continue;
        if ((p->topic == AZ_STATUS_METADATA || (p->topic == AZ_STATUS_KEY && p->field == AZ_STATUS_KEY_ORIGINAL)) &&
            old && old->info.info.provenance == AZ_PROV_FILE &&
            old->info.info.availability == AZ_AVAIL_READY && p->info.info.provenance == AZ_PROV_NATIVE &&
            p->info.info.availability != AZ_AVAIL_READY) continue;
        size_t extent = ds_extent(p, epoch);
        int native_cues = (flags & DS_NATIVE_INPUT) && p->topic == AZ_STATUS_CUES &&
            (p->field == AZ_STATUS_CUES_LIVE_HOT || p->field == AZ_STATUS_CUES_LIVE_MEMORY);
        int changed = native_cues ? ds_set_live_cues(d, p, extent, 0) : ds_set(&d->current, p, extent);
        if (changed == AZ_LIMIT) {
            lost |= AZ_STATUS_TOPIC(p->topic);
            reason |= AZ_STATUS_REVISION_LOST;
            struct ds_field stale = *p;
            stale.info.count = 0;
            stale.info.info.availability = AZ_AVAIL_STALE;
            if (ds_set(&d->current, &stale, 0) == AZ_LIMIT)
                ds_clear_field(&d->current.field[p->topic][p->field]);
            rc = AZ_LIMIT;
        }
        if (p->topic == AZ_STATUS_PHRASES && p->field < 2 && changed) phrase_changed |= 1u << p->field;
        if (p->topic == AZ_STATUS_PHRASES && p->field >= 2 && p->field < 4) mapping_renewed |= 1u << (p->field - 2);
        if (changed) {
            routes |= AZ_STATUS_TOPIC(p->topic);
            if (p->topic == AZ_STATUS_POSITION && p->field == AZ_STATUS_POSITION_HEAD) {
                const struct ds_cell *head = ds_ready(&d->current, p->topic, p->field);
                d->head_window_samples = head ? head->clock.window_samples : 0;
                d->head_window_rate = head ? head->clock.window_rate : 0;
            }
            if (p->topic == AZ_STATUS_TRANSPORT && p->field == AZ_STATUS_TRANSPORT_DIRECTION) motion_barrier = 1;
            if (p->topic == AZ_STATUS_METADATA && p->field == AZ_STATUS_METADATA_PATH) stem_bind = 1;
            if (p->topic == AZ_STATUS_LOAD && p->field == AZ_STATUS_LOAD_TRACK) {
                bind = 1;
                /* A replacement library on the same playing load must lose all
                 * old file values in this very publication, before its worker runs. */
                routes |= ds_stale(&d->current, AZ_STATUS_ALL_V1 & ~AZ_STATUS_TOPIC(AZ_STATUS_LOAD), 0, AZ_PROV_FILE, observed);
            }
        }
    }
    uint64_t derived_loss = 0;
    if (routes & AZ_STATUS_TOPIC(AZ_STATUS_CUES)) {
        routes |= ds_cue_storage_flags(d, AZ_STATUS_CUES_LIVE_HOT, observed, &derived_loss);
        routes |= ds_cue_storage_flags(d, AZ_STATUS_CUES_LIVE_MEMORY, observed, &derived_loss);
    }
    if (routes & (AZ_STATUS_TOPIC(AZ_STATUS_KEY) | AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS)))
        routes |= ds_effective_key(d, observed, &derived_loss);
    if (bind || stem_bind) {
        d->stem_binding = ds.stem_bindings < UINT64_MAX ? ++ds.stem_bindings : 0;
        routes |= ds_stale(&d->current, AZ_STATUS_TOPIC(AZ_STATUS_STEMS), 0, AZ_PROV_NATIVE, observed);
        ds_send_stem_binding(deck);
    }
    for (unsigned i = 0; i < 2; i++)
        if ((phrase_changed & ~mapping_renewed) & (1u << i))
            routes |= ds_withdraw(&d->current, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID + i, observed, &derived_loss);
    if (path && (path->endpoints & AZ_STATUS_OCCURRENCE_TO)) {
        d->head_window_samples = path->observed_window_samples;
        d->head_window_rate = path->observed_window_rate;
    }
    if (routes & (DS_ACTION_ROUTES | AZ_STATUS_TOPIC(AZ_STATUS_METADATA) | AZ_STATUS_TOPIC(AZ_STATUS_LOOP) |
                  AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS))) routes |= ds_times(d, observed, &derived_loss);
    if (routes & (AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_GRID)))
        routes |= ds_phase(d, observed, &derived_loss);
    if (routes & (AZ_STATUS_TOPIC(AZ_STATUS_CUES) | AZ_STATUS_TOPIC(AZ_STATUS_LOOP) |
                  AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | AZ_STATUS_TOPIC(AZ_STATUS_GRID)))
        routes |= ds_associations(d, observed, &derived_loss);
    const az_status_phrase_endpoint_v1 *active = ds_value(&d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    if (active && !path && (routes & (AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | AZ_STATUS_TOPIC(AZ_STATUS_GRID)))) {
        struct ds_map map;
        struct ds_path identity = {.grid = active->grid, .phrase_provider = active->phrase_provider_id, .phrase_revision = active->phrase_revision};
        if (!ds_map_at(&d->current, &identity, &map) || !ds_endpoint_known(&map, active)) {
            routes |= ds_withdraw(&d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, observed, &derived_loss);
            path_loss |= DS_PHRASE_ROUTE;
        }
    }
    if (path && path->kind == DS_PATH_BASELINE)
        ds_path_reduce(d, path, NULL, observed, &generated, &routes, &path_loss);
    if (path) {
        d->path_order = path->order;
        d->motion_generation = path->generation;
    }
    if (generated.size) pair = &generated;
    if (path_loss) {
        lost |= path_loss;
        reason |= AZ_STATUS_CAPTURE_GAP;
    }
    if (derived_loss) {
        lost |= derived_loss;
        reason |= AZ_STATUS_REVISION_LOST;
    }
    if (action) {
        d->last_ordinal = action->ordinal;
        routes |= DS_ACTION_ROUTES;
    }
    if (pair) routes |= DS_PHRASE_ROUTE;
    if (!routes && !lost) {
        ds_proof_cut(deck, epoch, expected, file_only, flags, input_lost, input_reason,
                     lost, reason, routes, fields, count, bind, 0);
        pthread_mutex_unlock(&ds.mu);
        if (path) ds_wake();
        return rc;
    }
    uint64_t cut = ++d->current.sequence;
    d->current.observed = observed;
    d->current.published = ds_now();
    if (motion_barrier || ((flags & DS_MOTION) && availability != ds_motion_availability(&d->current)))
        flags |= DS_FLUSH_MOTION;
    ds_append(d, flags, routes, action, pair, lost, reason);
    ds_proof_cut(deck, epoch, expected, file_only, flags, input_lost, input_reason,
                 lost, reason, routes, fields, count, bind, 1);
    struct ds_cell *track = d->current.field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
    az_track_ref ref = {0};
    if (bind && track && track->info.info.availability == AZ_AVAIL_READY) memcpy(&ref, track->data, sizeof ref);
    pthread_mutex_unlock(&ds.mu);
    ds_wake();
    if (bind) sdk_status_store_binding(deck, epoch, ref.source_id ? &ref : NULL, cut);
    return rc;
}

#include "sdk_status_order.c"

static int ds_publish(unsigned deck, uint64_t epoch, unsigned flags, uint64_t observed,
                      const struct ds_field *fields, size_t count, const az_status_occurrence_v1 *action,
                      const az_status_phrase_transition_v1 *pair, uint64_t lost, unsigned reason) {
    return ds_publish_checked(NULL, NULL, 0, deck, epoch, flags, observed, fields, count, action, pair, lost, reason);
}
static int ds_publish_file(unsigned deck, uint64_t epoch, const az_track_ref *expected, uint64_t observed,
                           const struct ds_field *fields, size_t count, uint64_t lost, unsigned reason) {
    if (!epoch || !expected) return AZ_INVALID;
    return ds_publish_checked(NULL, expected, 1, deck, epoch, 0, observed, fields, count, NULL, NULL, lost, reason);
}
static int ds_retire_file_binding(unsigned deck, uint64_t epoch, const az_track_ref *expected) {
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
    uint64_t mask = 0;
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
static int ds_register(int owner, const az_deck_watch_v1 *w, az_deck_status_fn fn, void *user,
                       az_deck_subscription *out) {
    if (owner < 0 || !w || w->size < sizeof *w || w->version != AZ_DATA_VERSION ||
        !w->deck_mask || (w->deck_mask & ~AZ_STATUS_ALL_DECKS) || !w->topics ||
        (w->topics & ~AZ_STATUS_ALL_V1) || w->position_hz > 30 || !fn || !out) return AZ_INVALID;
    if (!ds.enabled) return AZ_UNSUPPORTED;
    unsigned owned = 0;
    struct ds_subscription *slot = NULL;
    for (unsigned i = 0; i < DS_SUBS; i++) {
        if (!ds.sub[i].handle) slot = &ds.sub[i];
        else if (ds.sub[i].owner == owner) owned++;
    }
    if (!slot || owned >= 4 || ds.handles == UINT64_MAX) return AZ_LIMIT;
    for (unsigned d = 0; d < 4; d++)
        if (w->deck_mask & (1u << d)) {
            uint64_t needs = ds_dependencies(ds_interest(d) | w->topics);
            ds_gui_interest(needs);
            ds_native_interest(d, needs);
            sdk_status_store_interest(d, needs);
        }
    pthread_mutex_lock(&ds.mu);
    slot->handle = ++ds.handles;
    slot->watch = *w;
    if (!slot->watch.position_hz) slot->watch.position_hz = 30;
    slot->owner = owner;
    slot->fn = fn;
    slot->user = user;
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
    *out = slot->handle;
    pthread_mutex_unlock(&ds.mu);
    ds_wake();
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
            ds_native_interest(d, needs);
            sdk_status_store_interest(d, needs);
        }
}
static int ds_release(int owner, az_deck_subscription handle) {
    if (owner < 0 || !handle || handle > ds.handles) return AZ_INVALID;
    for (unsigned i = 0; i < DS_SUBS; i++)
        if (ds.sub[i].handle == handle) {
            if (ds.sub[i].owner != owner) return AZ_INVALID;
            ds_remove(&ds.sub[i]);
            return AZ_OK;
        }
    return AZ_STALE;
}
static void ds_retire(int owner) {
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
static int ds_next(struct ds_subscription *s, unsigned deck, struct ds_delivery *out,
                   uint64_t now, uint64_t *deadline) {
    struct ds_deck *d = &ds.deck[deck];
    struct ds_cursor *c = &s->cursor[deck];
    const uint64_t topics = s->watch.topics;
    const struct ds_record *chosen = NULL;
    const struct ds_image *im = NULL;
    uint64_t lost = 0;
    unsigned reason = 0, kind = AZ_STATUS_CHANGE;
    if (c->initial) {
        im = &c->last;
        kind = AZ_STATUS_INITIAL;
        c->initial = 0;
    } else {
        for (unsigned t = 0; t < AZ_STATUS_TOPIC_COUNT; t++)
            if ((topics & AZ_STATUS_TOPIC(t)) &&
                d->evicted[t] > c->covered) {
                lost |= AZ_STATUS_TOPIC(t);
                reason |= AZ_STATUS_EVICTED;
                for (unsigned b = 0; b < 4; b++)
                    if (d->loss[b][t] > c->covered) reason |= 1u << b;
            }
        lost |= c->evicted_motion;
        if (c->evicted_motion) reason |= AZ_STATUS_EVICTED;
        uint64_t after = !lost && c->examined > c->covered ? c->examined : c->covered;
        for (size_t i = ds_history_after(d, after); i < d->length; i++) {
            const struct ds_record *r = &d->history[(d->next + DS_HISTORY - d->length + i) % DS_HISTORY];
            uint64_t change = r->routes & topics, gap = r->lost & topics;
            if (r->motion_barrier) change |= ds_motion_changes(&c->last, &r->image, topics);
            if (lost || gap) {
                kind = AZ_STATUS_RESYNC;
                if (gap) {
                    lost |= gap;
                    reason |= r->reason;
                }
                if ((change & ~gap) || (r->action.ordinal && (topics & DS_ACTION_ROUTES)) ||
                    (r->pair.size && (topics & DS_PHRASE_ROUTE))) {
                    lost |= change;
                    reason |= AZ_STATUS_CUT_SKIPPED;
                }
                im = &r->image;
                break;
            }
            if (change) {
                chosen = r;
                im = &r->image;
                break;
            }
            /* This receipt's own routes do not match. Leave a still-owed
             * coalesced sample before it eligible for deadline delivery. */
            c->examined = r->image.sequence;
        }
        if (!im && lost) {
            kind = AZ_STATUS_RESYNC;
            im = &d->current;
        }
        int dirty_motion = !im && d->motion.image.sequence > c->covered &&
                           ds_motion_changes(&c->last, &d->motion.image, topics);
        if (dirty_motion) {
            uint64_t due = c->last_motion_ns + (UINT64_C(1000000000) + s->watch.position_hz - 1) / s->watch.position_hz;
            int due_order = compare_timestamps(now, due, 1000000000, 1000000000, 1);
            if (due_order == AZ_TIME_INVALID) return 0;
            if (due_order == AZ_TIME_EQUAL || due_order == AZ_TIME_AFTER) {
                im = &d->motion.image;
                out->motion = 1;
            } else {
                int order = *deadline ? compare_timestamps(due, *deadline, 1000000000, 1000000000, 1) : AZ_TIME_BEFORE;
                if (order != AZ_TIME_INVALID && order == AZ_TIME_BEFORE) *deadline = due;
            }
        }
        if (!im) return 0;
    }
    ds_image_select(&out->pin, im, topics);
    out->cursor_before = c->covered;
    out->event = (az_deck_status_event_v1) {.size = sizeof out->event, .version = AZ_DATA_VERSION, .kind = kind, .deck = deck, .subscription = s->handle, .sequence = im->sequence, .load_epoch = im->epoch, .observed_ns = im->observed, .published_ns = im->published, .topics = topics, .lost_topics = lost, .loss_reason = reason};
    ds_view(out, im, kind == AZ_STATUS_CHANGE ? &c->last : NULL);
    if (chosen && chosen->action.ordinal && (topics & DS_ACTION_ROUTES)) {
        out->action = chosen->action;
        out->event.occurrence = &out->action;
        out->event.changed_topics |= topics & DS_ACTION_ROUTES;
    }
    if (chosen && chosen->pair.size && (topics & DS_PHRASE_ROUTE)) {
        out->pair = chosen->pair;
        out->event.phrase_transition = &out->pair;
        out->event.changed_topics |= DS_PHRASE_ROUTE;
    }
    if (kind != AZ_STATUS_CHANGE) out->event.changed_topics = topics;
    if (kind == AZ_STATUS_RESYNC) c->evicted_motion = 0;
    if (kind != AZ_STATUS_CHANGE || out->event.changed_topics & (AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE)))
        c->last_motion_ns = now;
    c->covered = im->sequence;
    ds_image_select(&c->last, im, topics);
    out->owner = (unsigned) s->owner;
    out->fn = s->fn;
    out->user = s->user;
    return 1;
}
static unsigned ds_latency_bucket(uint64_t ns) {
    uint64_t ms = ns / 1000000 + (ns % 1000000 != 0);
    return ms < DS_LATENCY_BUCKETS ? (unsigned) ms : DS_LATENCY_BUCKETS - 1;
}
static unsigned ds_drain(unsigned budget) {
    unsigned delivered = 0;
    uint64_t deadline = 0;
    while (delivered < budget) {
        struct ds_delivery call = {0};
        pthread_mutex_lock(&ds.mu);
        unsigned n = 0;
        for (; n < 4 * DS_SUBS; n++) {
            unsigned pos = ds.rotation++ % (4 * DS_SUBS), deck = pos % 4;
            struct ds_subscription *s = &ds.sub[pos / 4];
            if (!s->handle || !(s->watch.deck_mask & (1u << deck)) || !ds_live(s->owner)) continue;
            if (ds_next(s, deck, &call, ds_now(), &deadline)) break;
        }
        pthread_mutex_unlock(&ds.mu);
        if (!call.fn) break;
        uint64_t before = ds_now();
        call.fn(&call.event, call.user);
        uint64_t elapsed = ds_now() - before;
        pthread_mutex_lock(&ds.mu);
        if (call.owner < DS_SUBS) {
            int slow = compare_timestamps(elapsed, UINT64_C(33000000), 1000000000, 1000000000, 1);
            int maximum = compare_timestamps(elapsed, ds.max_callback_ns[call.owner], 1000000000, 1000000000, 1);
            if (slow != AZ_TIME_INVALID && slow == AZ_TIME_AFTER) ds.slow_callbacks[call.owner]++;
            if (maximum != AZ_TIME_INVALID && maximum == AZ_TIME_AFTER) ds.max_callback_ns[call.owner] = elapsed;
        }
        struct ds_metrics *metric = &ds.metrics[call.event.deck];
        metric->callbacks++;
        if (call.event.kind == AZ_STATUS_RESYNC) {
            metric->resyncs++;
            metric->last_lost_topics = call.event.lost_topics;
            struct ds_proof_record *r = ds_proof_new(DS_PROOF_RESYNC, call.event.deck,
                                                    call.event.load_epoch, call.event.sequence);
            if (r) {
                r->handle = call.event.subscription; r->topics = call.event.topics;
                r->lost = call.event.lost_topics; r->reason = call.event.loss_reason;
                r->cursor_before = call.cursor_before;
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
        }
        int observed_order = compare_timestamps(call.event.observed_ns, before, 1000000000, 1000000000, 1);
        if (call.event.kind == AZ_STATUS_CHANGE && call.event.observed_ns && observed_order != AZ_TIME_INVALID &&
            (observed_order == AZ_TIME_BEFORE || observed_order == AZ_TIME_EQUAL)) {
            metric->latency[call.motion][ds_latency_bucket(before - call.event.observed_ns)]++;
            int published_order = compare_timestamps(call.event.published_ns, before, 1000000000, 1000000000, 1);
            if (call.event.published_ns && published_order != AZ_TIME_INVALID &&
                (published_order == AZ_TIME_BEFORE || published_order == AZ_TIME_EQUAL))
                metric->dispatch[call.motion][ds_latency_bucket(before - call.event.published_ns)]++;
        }
        ds_image_copy(&call.pin, NULL);
        pthread_mutex_unlock(&ds.mu);
        delivered++;
    }
    if (delivered == budget && budget) ds_wake();
    ds_deadline(deadline);
    return delivered;
}
