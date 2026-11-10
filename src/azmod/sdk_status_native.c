/* SPDX-License-Identifier: GPL-3.0-only */
/* Included after the status reducer in the host translation unit. */
#include "../common/azstatus_native.h"
#include "../common/azstatus_memory.h"
#include <poll.h>
#include <errno.h>
#include <dlfcn.h>
#include <float.h>

static _Atomic(const struct azstatus_native_v1 *) ds_native_api;
extern const struct azn_file_cue_callbacks *azfile_status_callbacks(void) __attribute__((weak));
extern const struct azn_file_map_callbacks *azfile_map_callbacks(void) __attribute__((weak));
extern void azfile_map_pair(unsigned deck, uint64_t epoch, const uint64_t tokens[3]) __attribute__((weak));
extern int azfile_status_ref(unsigned deck, uint64_t epoch, uint64_t token, az_status_cue_ref_v1 *ref) __attribute__((weak));
static _Atomic(const struct azn_file_cue_api *) ds_file_cues;
static _Atomic unsigned ds_file_refresh[4];
static struct {
    uint64_t token, load, unit;
    uint32_t field;
    struct azn_hot_link native;
} ds_file_working[4];
void azfile_native_refresh(unsigned deck, uint64_t epoch) {
    const struct azstatus_native_v1 *api = atomic_load_explicit(&ds_native_api, memory_order_acquire);
    if (deck >= 4 || !epoch || !api) return;
    atomic_store_explicit(&ds_file_refresh[deck], 1, memory_order_release);
    uint64_t one = 1;
    int fd = api->wake_fd();
    if (fd >= 0) {
        ssize_t n = write(fd, &one, sizeof one);
        if (n < 0 && errno == EINTR) n = write(fd, &one, sizeof one);
        (void) n;
    }
}
__attribute__((visibility("hidden"))) void azfile_native_transport(unsigned stage, uintptr_t packet, unsigned peer) {
    const struct azn_file_cue_api *api = atomic_load_explicit(&ds_file_cues, memory_order_acquire);
    if (api) api->transport(stage, packet, peer);
}
static _Atomic uint64_t ds_native_topics[4];
static pthread_mutex_t ds_native_reduce_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned ds_native_caps[4];
static pthread_mutex_t ds_native_attach_mu = PTHREAD_MUTEX_INITIALIZER;
/* Only the native reducer worker mutates these. The invalidation fence and the
 * matched public load epoch are separate domains. */
static uint64_t ds_native_epoch[4], ds_native_fence[4];
static void ds_gui_load(unsigned deck);
static int ds_native_terminal[4];
static struct {
    uint64_t load, normal_unit, generation, phrase_provider, phrase_revision;
    az_status_grid_ref_v1 grid;
    az_status_coord_v1 landing;
    int ended, has_normal, normal, availability;
} ds_end_state[4];
static uint64_t ds_native_motion_order[4], ds_native_motion_load[4];
static uint64_t ds_native_motion_gap[4];
static struct {
    unsigned count;
    struct azstatus_native_receipt sample[DS_PATH_PENDING];
} ds_pending_head[4];
static struct azstatus_native_receipt ds_pending_slip[4];
#define DS_NATIVE_PENDING_BYTES (sizeof ds_pending_head + sizeof ds_pending_slip)
static void ds_native_slip_head(const struct azstatus_native_receipt *r);
static int ds_native_path(const struct ds_path *path, unsigned d, uint64_t epoch, unsigned flags,
                          uint64_t observed, const struct ds_field *fields, size_t count,
                          const az_status_occurrence_v1 *action, uint64_t lost, unsigned reason) {
    uint64_t committed = 0;
    int rc = ds_publish_ordered(path, NULL, 0, d, epoch, flags, observed, fields, count, action, NULL,
                                lost, reason, &committed);
    ds_native_motion_load[d] = epoch;
    ds_native_motion_order[d] = committed;
    if (ds_pending_slip[d].motion_generation) {
        const struct azstatus_native_receipt pending = ds_pending_slip[d];
        ds_pending_slip[d].motion_generation = 0;
        ds_native_slip_head(&pending);
    }
    return rc;
}
static uint64_t ds_native_lti_owner[4], ds_native_lti_fence[4];
struct ds_native_revision {
    uint64_t revision, scope, provider, source_revision;
    struct ds_time_axis clock;
    int64_t ticks[2];
    unsigned availability, count;
    unsigned char bytes[sizeof(az_status_cue_ref_v1)];
};
/* Native bank editions and public semantic revisions are separate domains. */
static struct {
    uint64_t load, provider, native_revision, public_revision;
} ds_hot_edition[4];
static az_cue_entry ds_hot_rows[4][8], ds_memory_rows[4][AZ_NATIVE_CUE_LIMIT];
static int64_t ds_hot_ticks[4][16], ds_memory_ticks[4][AZ_NATIVE_CUE_LIMIT * 2];
static struct ds_native_revision ds_native_revisions[4][AZ_STATUS_TOPIC_COUNT][9];
static int ds_native_stamp(unsigned deck, struct ds_field *fields, unsigned count) {
    for (unsigned n = 0; n < count; ++n) {
        struct ds_field *f = &fields[n];
        struct ds_native_revision *last = &ds_native_revisions[deck][f->topic][f->field];
        const struct ds_layout *layout = &ds_layout[f->topic].field[f->field];
        size_t width = layout->type == DS_CUES ? f->info.count * sizeof(az_cue_entry) : layout->type == DS_CUE_REF ? sizeof(az_status_cue_ref_v1)
                                                                                    : layout->type == DS_STRING    ? f->info.count + 1u
                                                                                                                   : layout->width;
        int hot = f->topic == AZ_STATUS_CUES && f->field == AZ_STATUS_CUES_LIVE_HOT;
        int memory = f->topic == AZ_STATUS_CUES && f->field == AZ_STATUS_CUES_LIVE_MEMORY;
        int scalar_time = f->topic == AZ_STATUS_METADATA &&
                          (f->field == AZ_STATUS_METADATA_DURATION_S || f->field == AZ_STATUS_METADATA_DURATION_PRECISION_S);
        if (f->clock && f->clock->ticks && !hot && !memory && (f->info.count > 1 ||
            (!scalar_time && (f->topic != AZ_STATUS_CUES || f->field != AZ_STATUS_CUES_MAIN)))) return 0;
        unsigned char *stored = hot ? (unsigned char *) ds_hot_rows[deck] : memory ? (unsigned char *) ds_memory_rows[deck]
                                                                                   : last->bytes;
        if (width > (hot ? sizeof ds_hot_rows[deck] : memory ? sizeof ds_memory_rows[deck]
                                                             : sizeof last->bytes)) return 0;
        int ready = f->info.info.availability == AZ_AVAIL_READY;
        uint64_t source_revision = f->info.info.revision;
        int same_value = !ready || ((hot || memory)
                                        ? source_revision && source_revision == last->source_revision
                                        : ds_payload_same(f->topic, f->field, stored, f->value, width, &last->clock, f->clock));
        if (!last->revision || f->info.scope_id != last->scope || f->info.provider_id != last->provider ||
            f->info.info.availability != last->availability || f->info.count != last->count ||
            !same_value) {
            if (last->revision == UINT64_MAX) return 0;
            last->revision++;
            last->scope = f->info.scope_id;
            last->provider = f->info.provider_id;
            last->availability = f->info.info.availability;
            last->count = f->info.count;
            last->source_revision = source_revision;
            last->clock = f->clock ? *f->clock : (struct ds_time_axis) {0};
            last->clock.stored_cue_ms = NULL;
            if (last->clock.ticks) {
                int64_t *ticks = hot ? ds_hot_ticks[deck] : memory ? ds_memory_ticks[deck] : last->ticks;
                size_t n = hot || memory ? f->info.count * 2 : scalar_time ? 1u : 2u;
                memcpy(ticks, last->clock.ticks, n * sizeof *ticks);
                last->clock.ticks = ticks;
            }
            if (ready) memcpy(stored, f->value, width);
        }
        f->info.info.revision = last->revision;
    }
    return 1;
}

static struct ds_field ds_native_field(const struct azstatus_native_receipt *r,
                                       unsigned topic, unsigned field, unsigned availability, const void *value) {
    unsigned scope = (topic == AZ_STATUS_LOAD || (topic == AZ_STATUS_METADATA &&
        field != AZ_STATUS_METADATA_DURATION_S && field != AZ_STATUS_METADATA_DURATION_PRECISION_S)) ? AZ_STATUS_UNIT : AZ_STATUS_LOAD_SCOPE;
    return (struct ds_field) {.topic = topic, .field = field, .value = value, .info = {.scope = scope, .scope_id = scope == AZ_STATUS_UNIT ? r->unit : r->load_generation, .provider_id = r->unit, .count = availability == AZ_AVAIL_READY, .info = {.provenance = AZ_PROV_NATIVE, .availability = availability, .observed_ns = r->observed_ns}}};
}

/* The native juce::String is bounded to the SDK field capacity. A partial
 * final character at that bound is dropped. Malformed interior bytes are not published. */
static int ds_native_text(char text[192]) {
    unsigned n = (unsigned) strnlen(text, 192);
    if (!n || n >= 192) return -1;
    for (unsigned i = 0; i < n;) {
        unsigned c = (unsigned char) text[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        unsigned length = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3
                                                   : c >= 0xf0 && c <= 0xf4   ? 4
                                                                              : 0;
        if (!length) return -1;
        if (length > n - i) {
            if (n != 191) return -1;
            text[i] = 0;
            return (int) i;
        }
        unsigned second = (unsigned char) text[i + 1];
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
            (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) return -1;
        for (unsigned k = 1; k < length; ++k)
            if (((unsigned char) text[i + k] & 0xc0) != 0x80) return -1;
        i += length;
    }
    return (int) n;
}
static void ds_native_metadata(unsigned deck, const struct azstatus_native_receipt *r,
                               struct ds_field *fields, char text[2][192]) {
    memcpy(text[0], r->title, 192);
    memcpy(text[1], r->artist, 192);
    unsigned fallback = r->valid && r->a ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
    fields[0] = ds_native_field(r, AZ_STATUS_METADATA, AZ_STATUS_METADATA_CONTENT_ID,
                                r->valid && !r->a && r->content_id ? AZ_AVAIL_READY : fallback, &r->content_id);
    for (unsigned i = 0; i < 2; ++i) {
        int n = r->valid && !r->a ? ds_native_text(text[i]) : -1;
        fields[i + 1] = ds_native_field(r, AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE + i,
                                        n > 0 ? AZ_AVAIL_READY : fallback, text[i]);
        fields[i + 1].info.count = n > 0 ? (unsigned) n : 0;
    }
    ds_native_lti_owner[deck] = r->unit;
    ds_native_lti_fence[deck] = r->epoch;
}
static void ds_native_map_stamp(struct ds_path *path, const struct azstatus_native_receipt *r) {
    if (!r->map.owner || !r->map.phrases.provider || !r->map.grid.provider) return;
    path->phrase_provider = r->map.phrases.provider;
    path->phrase_revision = r->map.phrases.revision;
    path->grid = (az_status_grid_ref_v1) {r->load_generation, r->map.grid.provider, r->map.grid.revision,
                                          AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
}
static void ds_native_windows(struct ds_path *path, const struct azstatus_native_receipt *r) {
    if (r->size < offsetof(struct azstatus_native_receipt, from_window_rate) + sizeof r->from_window_rate) return;
    path->observed_window_samples = r->observed_window_samples;
    path->observed_window_rate = r->observed_window_rate;
    path->from_window_samples = r->from_window_samples;
    path->from_window_rate = r->from_window_rate;
}
static int ds_native_coord(const struct azn_point *p, az_status_coord_v1 *out) {
    /* A native stored coordinate is an integer in this source clock. This
     * validates its representation, independent of the observation window. */
    if (p->state != AZN_COORD || compare_timestamps(p->samples, 0, p->rate, p->rate, 1) == AZ_TIME_INVALID) return 0;
    *out = (az_status_coord_v1) {.seconds = (double) p->samples / p->rate, .samples = p->samples, .rate_hz = p->rate};
    return 1;
}
static int ds_hold_head(unsigned deck, const struct azstatus_native_receipt *r) {
    unsigned count = ds_pending_head[deck].count;
    struct azstatus_native_receipt *samples = ds_pending_head[deck].sample;
    unsigned i = 0;
    while (i < count && samples[i].motion_order < r->motion_order) i++;
    if (i < count && samples[i].motion_order == r->motion_order) return AZ_PENDING;
    if (count == DS_PATH_PENDING) return AZ_LIMIT;
    memmove(samples + i + 1, samples + i, (count - i) * sizeof *samples);
    samples[i] = *r;
    ds_pending_head[deck].count++;
    return AZ_PENDING;
}
static int ds_native_motion(const struct azstatus_native_receipt *r, uint64_t first_order) {
    unsigned d = r->deck;
    if (!r->load_generation || r->load_generation != ds_native_epoch[d] ||
        r->epoch != ds_native_fence[d] || !r->motion_order || !r->motion_generation) return AZ_STALE;
    if (ds_native_motion_load[d] != r->load_generation) {
        ds_native_motion_load[d] = r->load_generation;
        ds_native_motion_order[d] = 0;
    }
    if (r->motion_order <= ds_native_motion_order[d]) return AZ_STALE;
    uint64_t lost = r->lost_topics;
    if (first_order != ds_native_motion_order[d] + 1) {
        /* A sample from a generation whose barrier is still owed has no
         * publishable revision yet. Preserve the writer's receipt until the
         * barrier arrives, unless capture already reported a break. */
        if (!lost && r->a == AZN_MEASURE && first_order == r->motion_order &&
            ds_native_motion_gap[d] != r->load_generation) {
            pthread_mutex_lock(&ds.mu);
            const uint64_t generation = ds.deck[d].motion_generation;
            pthread_mutex_unlock(&ds.mu);
            if (r->motion_generation > generation) {
                if (ds_hold_head(d, r) == AZ_PENDING) return AZ_PENDING;
                lost |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT);
            }
        }
        lost |= AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    }
    struct ds_path path = {.order = r->motion_order, .first_order = first_order != r->motion_order ? first_order : 0, .generation = r->motion_generation, .kind = DS_PATH_BASELINE};
    ds_native_windows(&path, r);
    az_status_coord_v1 *coords[2] = {&path.from, &path.to};
    for (unsigned i = 0; i < 2; ++i) {
        const struct azn_point *p = &r->points[i];
        if (!ds_native_coord(p, coords[i])) continue;
        path.endpoints |= i == 0 ? AZ_STATUS_OCCURRENCE_FROM : AZ_STATUS_OCCURRENCE_TO;
    }
    if (r->size >= offsetof(struct azstatus_native_receipt, motion_flags) + sizeof r->motion_flags) {
        path.defer_terminal = !lost && !!(r->motion_flags & AZN_TERMINAL_ARRIVAL);
        path.opening = !!(r->motion_flags & AZN_AT_OPENING) && !r->occurrence_ordinal &&
                       (r->a == AZN_MOTION_SEED || r->a == AZN_REPOSITION || (r->a == AZN_MEASURE && r->c)) &&
                       (path.endpoints & AZ_STATUS_OCCURRENCE_TO);
        if ((r->motion_flags & AZN_AT_OPENING) && !r->occurrence_ordinal && r->a == AZN_MEASURE &&
            path.endpoints == 3 && path.from.rate_hz == path.to.rate_hz) {
            const int stationary = compare_timestamps(path.from.samples, path.to.samples,
                                                       path.to.rate_hz, path.to.rate_hz, 1);
            if (stationary != AZ_TIME_INVALID && stationary == AZ_TIME_EQUAL) path.opening = 1;
        }
        const int play_position = path.endpoints == 3 && path.from.rate_hz == path.to.rate_hz
            ? compare_timestamps(path.from.samples, path.to.samples, path.to.rate_hz,
                                 path.observed_window_rate, path.observed_window_samples) : AZ_TIME_INVALID;
        path.began_play = !lost && !!(r->motion_flags & AZN_BEGAN_PLAY) && r->a == AZN_MEASURE &&
                          play_position != AZ_TIME_INVALID && play_position == AZ_TIME_EQUAL;
    }
    az_status_coord_v1 measured = path.to;
    if (path.defer_terminal && r->points[2].state == AZN_COORD) {
        az_status_coord_v1 limit;
        if (!ds_native_coord(&r->points[2], &limit) || limit.rate_hz != measured.rate_hz ||
            limit.rate_hz != path.from.rate_hz) return AZ_INVALID;
        int from = compare_timestamps(path.from.samples, limit.samples, limit.rate_hz,
                                      path.from_window_rate, path.from_window_samples);
        int to = compare_timestamps(measured.samples, limit.samples, limit.rate_hz,
                                    path.observed_window_rate, path.observed_window_samples);
        if (from == AZ_TIME_INVALID || to == AZ_TIME_INVALID || (from != AZ_TIME_BEFORE && from != AZ_TIME_EQUAL) ||
            (to != AZ_TIME_EQUAL && to != AZ_TIME_AFTER)) return AZ_INVALID;
        path.terminal = limit;
    }
    struct ds_field head = ds_native_field(r, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD,
                                           (path.endpoints & AZ_STATUS_OCCURRENCE_TO) ? AZ_AVAIL_READY : AZ_AVAIL_STALE, &measured);
    struct ds_time_axis head_clock = {.units_per_second = measured.rate_hz,
                                      .window_samples = path.observed_window_samples,
                                      .window_rate = path.observed_window_rate};
    head.clock = &head_clock;
    struct ds_native_revision previous;
    if (path.first_order) previous = ds_native_revisions[d][head.topic][head.field];
    if (!ds_native_stamp(d, &head, 1)) return AZ_STALE;
    ds_native_map_stamp(&path, r);
    if (r->a == AZN_MEASURE && !r->c && !path.opening && path.phrase_provider &&
        !(lost & AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION))) path.kind = DS_PATH_TRAVEL;
    az_status_occurrence_v1 occurrence = {0};
    if ((r->a == AZN_WRAP || r->a == AZN_JUMP || r->a == AZN_SEEK) && r->occurrence_ordinal) {
        path.kind = DS_PATH_ACTION;
        occurrence = (az_status_occurrence_v1) {.size = sizeof occurrence, .version = AZ_DATA_VERSION, .kind = r->a == AZN_WRAP ? AZ_STATUS_WRAP : r->a == AZN_SEEK ? AZ_STATUS_SEEK
                                                                                                                                                                    : AZ_STATUS_JUMP,
                                                .ordinal = r->occurrence_ordinal,
                                                .endpoints = path.endpoints,
                                                .from = path.from,
                                                .to = path.to};
    }
    if (occurrence.ordinal && lost) {
        /* Preserve the intact action after the loss cut even if earlier
         * measurements from the same generation did not reach the consumer. */
        if (r->motion_order > ds_native_motion_order[d] + 1 && r->motion_generation > 1) {
            struct ds_path baseline = path;
            baseline.order = r->motion_order - 1;
            baseline.kind = DS_PATH_BASELINE;
            baseline.generation = r->motion_generation - 1;
            baseline.endpoints = 0;
            if (path.endpoints & AZ_STATUS_OCCURRENCE_FROM) {
                baseline.endpoints = AZ_STATUS_OCCURRENCE_TO;
                baseline.to = path.from;
                baseline.observed_window_samples = path.from_window_samples;
                baseline.observed_window_rate = path.from_window_rate;
            }
            ds_native_path(&baseline, d, r->load_generation, DS_NATIVE_INPUT | DS_HEAD_LOSS, r->observed_ns,
                           NULL, 0, NULL, lost, AZ_STATUS_CAPTURE_GAP);
        } else ds_publish(d, r->load_generation, DS_NATIVE_INPUT | DS_HEAD_LOSS, r->observed_ns, NULL, 0, NULL, NULL, lost, AZ_STATUS_CAPTURE_GAP);
        lost = 0;
    }
    unsigned flags = DS_NATIVE_INPUT | (lost ? DS_HEAD_LOSS : 0);
    if (r->a == AZN_MEASURE && !lost) flags |= DS_MOTION;
    struct ds_field fields[2] = {head};
    unsigned count = 1;
    int departed = ds_end_state[d].ended && ds_end_state[d].load == r->load_generation &&
                   (path.endpoints & AZ_STATUS_OCCURRENCE_TO) && path.to.rate_hz != ds_end_state[d].landing.rate_hz;
    if (ds_end_state[d].ended && ds_end_state[d].load == r->load_generation &&
        (path.endpoints & AZ_STATUS_OCCURRENCE_TO) && path.to.rate_hz == ds_end_state[d].landing.rate_hz) {
        int order = compare_timestamps(path.to.samples, ds_end_state[d].landing.samples, path.to.rate_hz,
                                       path.observed_window_rate, path.observed_window_samples);
        departed = order != AZ_TIME_INVALID && (order == AZ_TIME_BEFORE || order == AZ_TIME_AFTER);
    }
    /* A completed source can keep rendering its clamped integer endpoint.
     * That is a baseline of the same confirmed END, not another trip through
     * the map's trailing GAP. Require the original map and writer generation;
     * a map change or a real departure still takes the normal path. */
    if (!lost && path.kind == DS_PATH_TRAVEL && ds_end_state[d].ended &&
        ds_end_state[d].load == r->load_generation && ds_end_state[d].generation == path.generation &&
        path.endpoints == 3 && path.from.rate_hz == ds_end_state[d].landing.rate_hz &&
        path.to.rate_hz == ds_end_state[d].landing.rate_hz &&
        path.phrase_provider == ds_end_state[d].phrase_provider && path.phrase_revision == ds_end_state[d].phrase_revision &&
        !memcmp(&path.grid, &ds_end_state[d].grid, sizeof path.grid)) {
        const int from_end = compare_timestamps(path.from.samples, ds_end_state[d].landing.samples,
                                                path.from.rate_hz, path.from.rate_hz, 1);
        const int to_end = compare_timestamps(path.to.samples, ds_end_state[d].landing.samples,
                                              path.to.rate_hz, path.to.rate_hz, 1);
        if (from_end != AZ_TIME_INVALID && to_end != AZ_TIME_INVALID &&
            from_end == AZ_TIME_EQUAL && to_end == AZ_TIME_EQUAL) {
            path.kind = DS_PATH_BASELINE;
            path.ended = 1;
            path.defer_terminal = 0;
        }
    }
    if (departed) {
        fields[1] = ds_native_field(r, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, ds_end_state[d].has_normal ? (unsigned) ds_end_state[d].availability : AZ_AVAIL_STALE, &ds_end_state[d].normal);
        fields[1].info.provider_id = ds_end_state[d].normal_unit;
        if (!ds_native_stamp(d, &fields[1], 1)) return AZ_LIMIT;
        count = 2;
        flags &= ~DS_MOTION;
    }
    int rc = ds_native_path(&path, d, r->load_generation, flags, r->observed_ns, fields, count, occurrence.ordinal ? &occurrence : NULL,
                            lost, lost ? AZ_STATUS_CAPTURE_GAP : 0);
    if (ds_native_motion_order[d] >= path.order) {
        ds_native_motion_gap[d] = 0;
        if (departed) ds_end_state[d].ended = 0;
        /* The burst caller retries declined spans as single measurements. */
        if (rc == AZ_LIMIT && path.first_order) rc = AZ_OK;
    } else if (path.first_order) ds_native_revisions[d][head.topic][head.field] = previous;
    return rc;
}
static void ds_flush_heads(unsigned deck) {
    struct azstatus_native_receipt *samples = ds_pending_head[deck].sample;
    while (ds_pending_head[deck].count) {
        const struct azstatus_native_receipt first = samples[0];
        const int current = first.load_generation == ds_native_epoch[deck] && first.epoch == ds_native_fence[deck];
        const uint64_t order = ds_native_motion_order[deck];
        if (current && (ds_native_motion_load[deck] != first.load_generation ||
                        (first.motion_order > order && first.motion_order - order > 1))) return;
        const unsigned remaining = --ds_pending_head[deck].count;
        memmove(samples, samples + 1, remaining * sizeof *samples);
        if (current) ds_native_motion(&first, first.motion_order);
    }
}
/* The triplet is the raw contents of a confirmed play-pause notification,
 * not normalized transport. Its unit generation and retirement are independent
 * of the matched-load and BpmState lifetimes. */
static void ds_native_play(const struct azstatus_native_receipt *r) {
    static uint64_t owner[4], revision[4], retired[4];
    unsigned d = r->deck;
    if (!r->unit || r->unit < owner[d] || r->unit <= retired[d]) return;
    if (r->unit > owner[d]) {
        owner[d] = r->unit;
        revision[d] = 0;
    }
    if (r->valid < 0) retired[d] = r->unit;
    else if (r->revision && r->revision <= revision[d]) return;
    if (r->revision) revision[d] = r->revision;
    uint64_t id = AZSP_RAW | r->unit;
    const int values[] = {r->a, r->b, r->c};
    unsigned avail = r->valid < 0 ? AZ_AVAIL_UNSUPPORTED : r->valid ? AZ_AVAIL_READY
                                                                    : AZ_AVAIL_STALE;
    struct ds_field fields[3];
    for (unsigned i = 0; i < 3; i++)
        fields[i] = (struct ds_field) {AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RAW_MODE_IS_2 + i, {.info = {avail, AZ_PROV_NATIVE, 0, r->observed_ns}, .scope = AZ_STATUS_UNIT, .scope_id = id, .provider_id = id, .count = avail == AZ_AVAIL_READY}, &values[i], NULL};
    if (ds_native_stamp(d, fields, 3))
        ds_publish(d, 0, DS_UNIT_INPUT, r->observed_ns, fields, 3, NULL, NULL, r->lost_topics,
                   r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
}
/* Preserve each native integer coordinate exactly. Different source rates
 * share a checked common integer clock; their bank has no single observation
 * window, so timestamp equivalence remains unavailable for that mixed bank. */
static int ds_native_cue_clock(unsigned count, const az_cue_entry *rows, const struct azn_point (*points)[2],
                                int64_t *ticks, struct ds_time_axis *clock) {
    uint64_t units = 1;
    unsigned first_rate = 0, uniform = 1;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            if (!(rows[i].valid & (j ? AZ_CUE_END : AZ_CUE_START))) continue;
            const struct azn_point *p = &points[i][j];
            if (p->state != AZN_COORD || !p->rate || p->rate >= 0x7fffffffU) return 0;
            int order = compare_timestamps(p->samples, 0, p->rate, p->rate, 1);
            if (order == AZ_TIME_INVALID || order == AZ_TIME_BEFORE) return 0;
            if (!first_rate) first_rate = p->rate;
            else if (p->rate != first_rate) uniform = 0;
            uint64_t a = units, b = p->rate;
            while (b) { uint64_t remainder = a % b; a = b; b = remainder; }
            units = units / a * p->rate;
            if (units > UINT32_MAX) return 0;
        }
    if (!first_rate) return 0;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            ticks[2 * i + j] = 0;
            if (!(rows[i].valid & (j ? AZ_CUE_END : AZ_CUE_START))) continue;
            const struct azn_point *p = &points[i][j];
            if (__builtin_mul_overflow(p->samples, (int64_t) (units / p->rate), &ticks[2 * i + j])) return 0;
        }
    *clock = (struct ds_time_axis) {units, ticks, uniform ? 1u : 0u, uniform ? first_rate : 0u, NULL};
    return 1;
}
static void ds_native_hot(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;
    if (!r->load_generation || r->load_generation != ds_native_epoch[deck] ||
        r->epoch != ds_native_fence[deck] || !r->attempt) return;
    az_cue_entry rows[8] = {0};
    struct azn_point points[8][2] = {{{0}}};
    int64_t ticks[16];
    int64_t stored_ms[16];
    struct ds_time_axis clock = {0};
    unsigned count = 0, availability = AZ_AVAIL_EMPTY;
    const struct azn_hot_receipt *hot = r->size >= sizeof *hot ? (const struct azn_hot_receipt *) r : NULL;
    if (!r->valid) availability = AZ_AVAIL_STALE;
    else
        for (unsigned i = 0; i < 8; ++i) {
            const struct azn_point *in = &r->hot[i].in, *out = &r->hot[i].out;
            if (in->state == AZN_ABSENT && out->state == AZN_ABSENT) continue;
            if (in->state != AZN_COORD || compare_timestamps(in->samples, 0, in->rate, in->rate, 1) == AZ_TIME_INVALID ||
                (out->state != AZN_ABSENT && (out->state != AZN_COORD || out->rate != in->rate ||
                                              compare_timestamps(out->samples, 0, out->rate, out->rate, 1) == AZ_TIME_INVALID))) {
                availability = AZ_AVAIL_STALE;
                count = 0;
                break;
            }
            int loop = out->state == AZN_COORD;
            points[count][0] = *in;
            points[count][1] = *out;
            if (hot) memcpy(stored_ms + 2 * count, hot->details.stored_ms[i], sizeof hot->details.stored_ms[i]);
            az_cue_entry *entry = &rows[count++];
            *entry = (az_cue_entry) {.size = sizeof *entry, .version = AZ_DATA_VERSION, .classification = AZ_CUE_HOT, .slot = i + 1, .entry_id = i + 1, .type = loop ? AZ_CUE_LOOP : AZ_CUE_POINT, .valid = AZ_CUE_START | AZ_CUE_COLOR | (loop ? AZ_CUE_END : 0), .rgb = r->hot[i].rgb, .start_s = (double) in->samples / in->rate, .end_s = loop ? (double) out->samples / out->rate : 0, .saved = -1, .modified = -1, .auto_active = -1};
            if (hot && (r->hot[i].reserved & AZN_HOT_AUTO_ACTIVE)) {
                entry->valid |= AZ_CUE_AUTO_ACTIVE;
                entry->auto_active = !!(r->hot[i].reserved & AZN_HOT_AUTO_ON);
            }
            if (hot && (r->hot[i].reserved & AZN_HOT_COMMENT) && memchr(hot->details.comments[i], '\0', sizeof entry->comment)) {
                entry->valid |= AZ_CUE_COMMENT;
                memcpy(entry->comment, hot->details.comments[i], sizeof entry->comment);
            }
            availability = AZ_AVAIL_READY;
        }
    if (availability == AZ_AVAIL_READY && !ds_native_cue_clock(count, rows, points, ticks, &clock)) {
        availability = AZ_AVAIL_STALE;
        count = 0;
    }
    if (availability == AZ_AVAIL_READY && hot) clock.stored_cue_ms = stored_ms;
    struct ds_field f = ds_native_field(r, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT, availability, rows);
    if (availability == AZ_AVAIL_READY) f.clock = &clock;
    f.info.provider_id = r->attempt;
    f.info.count = count;
    f.info.info.revision = r->size >= offsetof(struct azstatus_native_receipt, revision) + sizeof r->revision ? r->revision : 0;
    struct ds_native_revision *previous = &ds_native_revisions[deck][AZ_STATUS_CUES][AZ_STATUS_CUES_LIVE_HOT];
    uint64_t prior = previous->revision;
    ds_hot_edition[deck].native_revision = 0;
    if (ds_native_stamp(deck, &f, 1)) {
        /* A missed A→B→A can recover identical visible rows. Give the recovered
         * bank a fresh public edition so an old association cannot survive. */
        if (r->lost_topics && prior == f.info.info.revision && prior < UINT64_MAX)
            f.info.info.revision = ++previous->revision;
        int rc = ds_publish(deck, r->load_generation, DS_NATIVE_INPUT | (r->lost_topics ? DS_FIELD_LOSS : 0),
                            r->observed_ns, &f, 1, NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
        if (rc == AZ_OK && availability == AZ_AVAIL_READY && r->revision) {
            ds_hot_edition[deck].load = r->load_generation;
            ds_hot_edition[deck].provider = r->attempt;
            ds_hot_edition[deck].native_revision = r->revision;
            ds_hot_edition[deck].public_revision = f.info.info.revision;
        }
    }
}
static void ds_native_field_gap(const struct azstatus_native_receipt *r, unsigned topic, unsigned field) {
    unsigned d = r->deck;
    const uint64_t epoch = ds_native_epoch[d];
    struct ds_field f = {.topic = topic, .field = field, .info = {.scope = AZ_STATUS_LOAD_SCOPE, .scope_id = epoch, .info = {.availability = AZ_AVAIL_STALE, .provenance = AZ_PROV_NATIVE, .observed_ns = r->observed_ns}}};
    pthread_mutex_lock(&ds.mu);
    struct ds_cell *old = ds.deck[d].current.field[topic][field];
    if (old && old->info.scope_id == epoch) f.info.provider_id = old->info.provider_id;
    pthread_mutex_unlock(&ds.mu);
    if (ds_native_stamp(d, &f, 1))
        ds_publish(d, epoch, DS_NATIVE_INPUT | DS_FIELD_LOSS, r->observed_ns, &f, 1, NULL, NULL,
                   AZ_STATUS_TOPIC(topic), AZ_STATUS_CAPTURE_GAP);
}
static void ds_native_settings(const struct azstatus_native_receipt *r, unsigned kind, int gap) {
    unsigned d = r->deck, i = kind == AZN_QUANTIZE ? 0 : kind == AZN_SYNC      ? 2
                                                     : kind == AZN_SLIDER_LOCK ? 3
                                                                               : 1;
    static uint64_t current[4][4], retired[4][4];
    uint64_t unit = gap ? current[d][i] : r->unit;
    if (unit && !gap && (unit <= retired[d][i] || unit < current[d][i])) return;
    if (!gap && !unit) return;
    if (r->valid < 0 && !gap) retired[d][i] = unit;
    current[d][i] = unit;
    unsigned avail = gap ? AZ_AVAIL_STALE : r->valid < 0                       ? AZ_AVAIL_UNSUPPORTED
                                        : r->valid && (r->a == 0 || r->a == 1) ? AZ_AVAIL_READY
                                                                               : AZ_AVAIL_STALE;
    const unsigned topic = i == 3 ? AZ_STATUS_TEMPO : i == 2 ? AZ_STATUS_SYNC
                                                             : AZ_STATUS_SETTINGS;
    unsigned n = i == 1 || i == 2 ? 2 : 1;
    struct ds_field f[2];
    for (unsigned k = 0; k < n; k++) {
        unsigned field = i == 3 ? AZ_STATUS_TEMPO_SLIDER_LOCKED : i == 2 ? (k ? AZ_STATUS_SYNC_RAW_MODE : AZ_STATUS_SYNC_ENABLED)
                                                              : i        ? AZ_STATUS_SETTINGS_MASTER_TEMPO + k
                                                                         : AZ_STATUS_SETTINGS_QUANTIZE;
        int raw = i == 2 && k;
        unsigned status = raw && avail == AZ_AVAIL_READY && r->b != 0 && r->b != 2 ? AZ_AVAIL_STALE : avail;
        f[k] = (struct ds_field) {topic, field, {.info = {status, AZ_PROV_NATIVE, 0, r->observed_ns}, .scope = AZ_STATUS_UNIT, .scope_id = unit, .provider_id = unit, .count = status == AZ_AVAIL_READY}, raw ? &r->b : &r->a, NULL};
    }
    if (ds_native_stamp(d, f, n))
        ds_publish(d, 0, DS_UNIT_INPUT, r->observed_ns, f, n, NULL, NULL, gap ? r->lost_topics : 0,
                   gap ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_native_sync_role(const struct azstatus_native_receipt *r, int gap) {
    static uint64_t current[4], retired[4];
    unsigned d = r->deck;
    uint64_t owner = gap ? current[d] : r->unit;
    if (!owner || owner <= retired[d] || owner < current[d]) return;
    if (!gap && r->valid < 0) retired[d] = owner;
    current[d] = owner;
    struct ds_field fields[2];
    const unsigned selectors[] = {AZ_STATUS_SYNC_FOLLOWING, AZ_STATUS_SYNC_LOCKED};
    const int values[] = {r->a, r->b};
    for (unsigned i = 0; i < 2; i++) {
        unsigned avail = gap ? AZ_AVAIL_STALE : r->valid < 0                                      ? AZ_AVAIL_UNSUPPORTED
                                            : r->valid == 1 && (values[i] == 0 || values[i] == 1) ? AZ_AVAIL_READY
                                                                                                  : AZ_AVAIL_STALE;
        fields[i] = (struct ds_field) {AZ_STATUS_SYNC, selectors[i], {.info = {avail, AZ_PROV_NATIVE, 0, r->observed_ns}, .scope = AZ_STATUS_UNIT, .scope_id = owner, .provider_id = owner, .count = avail == AZ_AVAIL_READY}, &values[i], NULL};
    }
    if (ds_native_stamp(d, fields, 2)) ds_publish(d, 0, DS_UNIT_INPUT, r->observed_ns, fields, 2, NULL, NULL,
                                                  gap ? r->lost_topics : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_native_key(const struct azstatus_native_receipt *r) {
    static uint64_t current[4], retired[4];
    if (!r->attempt || !r->load_generation || r->load_generation != ds_native_epoch[r->deck] ||
        r->epoch != ds_native_fence[r->deck] || r->attempt < current[r->deck] || r->attempt <= retired[r->deck]) return;
    current[r->deck] = r->attempt;
    if (r->valid < 0) retired[r->deck] = r->attempt;
    unsigned a = r->valid == 1 && r->a >= -12 && r->a <= 12 ? AZ_AVAIL_READY : AZ_AVAIL_STALE;
    struct ds_field f = ds_native_field(r, AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES, a, &r->a);
    f.info.provider_id = r->attempt;
    if (ds_native_stamp(r->deck, &f, 1)) ds_publish(r->deck, r->load_generation, DS_NATIVE_INPUT, r->observed_ns,
                                                    &f, 1, NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_native_slip_head(const struct azstatus_native_receipt *r) {
    static uint64_t owner[4], retired[4];
    unsigned d = r->deck;
    if (!r->attempt || r->attempt < owner[d] || r->attempt <= retired[d] ||
        !r->load_generation || r->load_generation != ds_native_epoch[d] || r->epoch != ds_native_fence[d]) return;
    az_status_coord_v1 head = {0};
    unsigned avail = (unsigned) r->valid;
    if (avail != AZ_AVAIL_READY && avail != AZ_AVAIL_EMPTY && avail != AZ_AVAIL_STALE) return;
    const uint64_t sample_generation = r->size >= offsetof(struct azstatus_native_receipt, motion_generation) +
        sizeof r->motion_generation ? r->motion_generation : 0;
    if (!r->c && sample_generation) {
        pthread_mutex_lock(&ds.mu);
        const uint64_t generation = ds.deck[d].motion_generation;
        pthread_mutex_unlock(&ds.mu);
        if (sample_generation < generation) return;
        if (sample_generation > generation) {
            struct azstatus_native_receipt *pending = &ds_pending_slip[d];
            const int same_load = pending->motion_generation && pending->load_generation == r->load_generation;
            if (!same_load || pending->motion_generation <= sample_generation) {
                uint64_t lost = same_load ? pending->lost_topics : 0;
                /* Ordinary measurements can coalesce. If a validity or owner
                 * cut is overwritten while waiting, the next baseline owes loss. */
                if (same_load && (pending->valid != r->valid || pending->attempt != r->attempt))
                    lost |= AZ_STATUS_TOPIC(AZ_STATUS_POSITION);
                *pending = *r;
                pending->lost_topics |= lost;
            }
            return;
        }
    } else if (avail == AZ_AVAIL_READY) {
        /* Old peers can still report absence, but cannot order a coordinate. */
        avail = AZ_AVAIL_STALE;
    }
    if (avail == AZ_AVAIL_READY && !ds_native_coord(&r->points[0], &head)) avail = AZ_AVAIL_STALE;
    owner[d] = r->attempt;
    if (r->c) {
        retired[d] = r->attempt;
        if (ds_pending_slip[d].attempt <= r->attempt) ds_pending_slip[d].motion_generation = 0;
    }
    struct ds_field f = ds_native_field(r, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD, avail, &head);
    struct ds_path timing = {0};
    ds_native_windows(&timing, r);
    struct ds_time_axis clock = {.units_per_second = head.rate_hz,
                                 .window_samples = timing.observed_window_samples,
                                 .window_rate = timing.observed_window_rate};
    f.clock = &clock;
    f.info.provider_id = r->attempt;
    const uint64_t lost = r->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION);
    unsigned flags = DS_NATIVE_INPUT | (lost ? DS_FIELD_LOSS : r->b ? 0 : DS_MOTION);
    if (ds_native_stamp(d, &f, 1)) ds_publish(d, r->load_generation, flags,
                                              r->observed_ns, &f, 1, NULL, NULL, lost, lost ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_native_stems(const struct azstatus_native_receipt *r) {
    unsigned d = r->deck;
    if (!r->attempt || !r->load_generation || r->load_generation != ds_native_epoch[d] ||
        r->epoch != ds_native_fence[d] || r->valid < 0 || r->valid > AZ_AVAIL_STALE ||
        r->c < 0 || r->c > AZ_AVAIL_STALE || (unsigned) r->a > 1 || (unsigned) r->b > 1) return;
    struct ds_field fields[] = {
        ds_native_field(r, AZ_STATUS_STEMS, AZ_STATUS_STEMS_AVAILABLE, (unsigned) r->valid, &r->a),
        ds_native_field(r, AZ_STATUS_STEMS, AZ_STATUS_STEMS_ENABLED, (unsigned) r->c, &r->b)};
    fields[0].info.provider_id = fields[1].info.provider_id = r->attempt;
    if (ds_native_stamp(d, fields, 2))
        ds_publish(d, r->load_generation, DS_NATIVE_INPUT, r->observed_ns, fields, 2, NULL, NULL, 0, 0);
}
#include "sdk_status_maps_consumer.inc"
#include "sdk_status_memory_consumer.inc"
static void ds_native_rate(const struct azstatus_native_receipt *r, int gap) {
    const unsigned d = r->deck;
    uint64_t epoch = gap ? ds_native_epoch[d] : r->load_generation;
    if (!epoch || (!gap && (r->epoch != ds_native_fence[d] || epoch != ds_native_epoch[d] || !r->unit || !r->attempt))) return;
    struct azstatus_native_receipt source = *r;
    source.epoch = ds_native_fence[d];
    source.load_generation = epoch;
    double speed = 0;
    memcpy(&speed, &r->points[0].samples, sizeof speed);
    if (!gap && (r->valid < 0 || r->valid > AZ_AVAIL_STALE || r->b < 0 || r->b > AZ_AVAIL_STALE ||
                 r->a < -1 || r->a > 1 || !isfinite(speed) || speed < 0 || speed > 8)) return;
    struct ds_field f[] = {ds_native_field(&source, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE, gap ? AZ_AVAIL_STALE : (unsigned) r->valid, &speed),
                           ds_native_field(&source, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_DIRECTION, gap ? AZ_AVAIL_STALE : (unsigned) r->b, &r->a)};
    for (unsigned i = 0; i < 2; i++) {
        f[i].info.scope = AZ_STATUS_LOAD_SCOPE;
        f[i].info.scope_id = epoch;
    }
    if (gap) {
        pthread_mutex_lock(&ds.mu);
        for (unsigned i = 0; i < 2; i++) {
            struct ds_cell *old = ds.deck[d].current.field[f[i].topic][f[i].field];
            if (old && old->info.scope_id == epoch) f[i].info.provider_id = old->info.provider_id;
        }
        pthread_mutex_unlock(&ds.mu);
    }
    if (ds_native_stamp(d, f, 2))
        ds_publish(d, epoch, DS_NATIVE_INPUT | (r->lost_topics ? DS_FIELD_LOSS : 0), r->observed_ns, f, 2, NULL, NULL,
                   r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
}
/* A terminal decision and normal playback are independent native writers.
 * While terminal is positively asserted it takes precedence. Retain the
 * exact last normal receipt so a terminal withdrawal can re-expose it. */
static void ds_normal_transport(const struct azstatus_native_receipt *r, int gap) {
    const unsigned d = r->deck;
    if (!r->load_generation || r->load_generation != ds_native_epoch[d] || r->epoch != ds_native_fence[d]) return;
    if (ds_end_state[d].load != r->load_generation) {
        memset(&ds_end_state[d], 0, sizeof ds_end_state[d]);
        ds_end_state[d].load = r->load_generation;
    }
    if (!gap) ds_end_state[d].normal_unit = r->unit;
    ds_end_state[d].normal = r->a;
    ds_end_state[d].availability = gap ? AZ_AVAIL_STALE : r->valid;
    ds_end_state[d].has_normal = 1;
    if (ds_end_state[d].ended && !gap) return;
    int value = ds_end_state[d].ended ? AZ_PLAYBACK_ENDED : r->a;
    struct ds_field f = ds_native_field(r, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE,
                                        ds_end_state[d].ended ? AZ_AVAIL_READY : ds_end_state[d].availability, &value);
    if (ds_native_stamp(d, &f, 1)) ds_publish(d, r->load_generation, DS_NATIVE_INPUT | (gap ? DS_FIELD_LOSS : 0),
                                              r->observed_ns, &f, 1, NULL, NULL, gap ? AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_end_apply(const struct azstatus_native_receipt *r, int gap) {
    unsigned d = r->deck;
    uint64_t epoch = gap ? ds_native_epoch[d] : r->load_generation;
    if (!epoch || epoch != ds_native_epoch[d] || (!gap && (r->epoch != ds_native_fence[d] || !r->motion_order || !r->motion_generation || r->valid != 1 || (unsigned) r->a > 1))) return;
    if (ds_end_state[d].load != epoch) {
        memset(&ds_end_state[d], 0, sizeof ds_end_state[d]);
        ds_end_state[d].load = epoch;
    }
    int ended = !gap && r->a;
    unsigned availability = ended ? AZ_AVAIL_READY : (!gap && ds_end_state[d].has_normal ? (unsigned) ds_end_state[d].availability : AZ_AVAIL_STALE);
    int state = ended ? AZ_PLAYBACK_ENDED : ds_end_state[d].normal;
    struct azstatus_native_receipt origin = *r;
    if (!ended && ds_end_state[d].has_normal) origin.unit = ds_end_state[d].normal_unit;
    origin.epoch = ds_native_fence[d];
    origin.load_generation = epoch;
    origin.observed_ns = r->observed_ns;
    struct ds_field f = ds_native_field(&origin, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, availability, &state);
    if (!ds_native_stamp(d, &f, 1)) return;
    uint64_t lost = r->lost_topics;
    if (gap) {
        struct ds_field fields[2] = {f};
        unsigned count = 1;
        pthread_mutex_lock(&ds.mu);
        const struct ds_cell *active = ds_ready(&ds.deck[d].current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
        if (active && active->info.scope_id == epoch) {
            fields[1] = (struct ds_field) {AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, active->info, NULL, NULL};
            fields[1].info.info.availability = AZ_AVAIL_STALE;
            fields[1].info.info.observed_ns = r->observed_ns;
            fields[1].info.count = 0;
            count = 2;
        }
        pthread_mutex_unlock(&ds.mu);
        ds_end_state[d].ended = 0;
        ds_publish(d, epoch, DS_NATIVE_INPUT | DS_FIELD_LOSS, r->observed_ns, fields, count, NULL, NULL, lost, AZ_STATUS_CAPTURE_GAP);
        return;
    }
    struct ds_path p = {.kind = DS_PATH_BASELINE, .order = r->motion_order, .generation = r->motion_generation, .ended = ended};
    ds_native_windows(&p, r);
    ds_native_map_stamp(&p, r);
    if (ds_native_coord(&r->points[1], &p.to)) {
        p.from = p.to;
        p.endpoints = 3;
    }
    if (!lost && p.phrase_provider && (r->motion_flags & AZN_TERMINAL_CONTINUATION)) {
        p.kind = DS_PATH_TRAVEL;
        p.confirm_terminal = 1;
    }
    if (r->motion_order > ds_native_motion_order[d] + 1) {
        lost |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
        p.kind = DS_PATH_BASELINE;
        p.confirm_terminal = 0;
    }
    struct ds_field fields[2] = {f};
    struct ds_time_axis head_clock = {.units_per_second = p.to.rate_hz,
                                      .window_samples = p.observed_window_samples,
                                      .window_rate = p.observed_window_rate};
    unsigned count = 1;
    if (ended && (p.endpoints & AZ_STATUS_OCCURRENCE_TO)) {
        fields[1] = ds_native_field(&origin, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, AZ_AVAIL_READY, &p.to);
        fields[1].clock = &head_clock;
        if (!ds_native_stamp(d, &fields[1], 1)) return;
        count = 2;
    }
    int rc = ds_native_path(&p, d, epoch, DS_NATIVE_INPUT | (lost ? DS_FIELD_LOSS : 0), r->observed_ns, fields, count,
                            NULL, lost, lost ? AZ_STATUS_CAPTURE_GAP : 0);
    if ((rc == AZ_OK || rc == AZ_LIMIT) && ds_native_motion_order[d] >= p.order) {
        ds_end_state[d].ended = ended;
        ds_end_state[d].landing = p.to;
        ds_end_state[d].generation = p.generation;
        ds_end_state[d].phrase_provider = p.phrase_provider;
        ds_end_state[d].phrase_revision = p.phrase_revision;
        ds_end_state[d].grid = p.grid;
    }
}
/* Recheck only the association when a saved collection becomes eligible.
 * The independent native loop, cue and lock fields have their own writers.
 * Refresh requests are consumed by the normal reducer after queued writes. */
static int ds_working_loop_ref(unsigned deck, uint64_t epoch, uint64_t token, unsigned file_field,
                               const struct azn_hot_link *link, az_status_cue_ref_v1 *ref) {
    /* A physical FILE witness fixes the association's source. If its edition
     * is unavailable, withdraw it; an intact LIVE copy is a different target,
     * even when IDs, revisions and bounds happen to coincide. */
    if (token) return azfile_status_ref && azfile_status_ref(deck, epoch, token, ref) &&
        ref->load_epoch == epoch && ref->provenance == AZ_PROV_FILE && ref->cue_field == file_field &&
        ref->entry.type == AZ_CUE_LOOP && (ref->entry.valid & (AZ_CUE_START | AZ_CUE_END)) == (AZ_CUE_START | AZ_CUE_END);
    int memory = link->state == AZN_MEMORY_LINK_READY;
    uint64_t revision = 0;
    if (memory && ds_mem_epoch[deck] == epoch && ds_mem_provider[deck] == link->provider &&
        link->stamp == ds_mem_revision[deck]) revision = ds_mem_public_revision[deck];
    if (link->state == AZN_HOT_LINK_READY && ds_hot_edition[deck].load == epoch &&
        ds_hot_edition[deck].provider == link->provider && link->stamp == ds_hot_edition[deck].native_revision)
        revision = ds_hot_edition[deck].public_revision;
    return link->stamp && revision && ds_native_loop_ref(deck, epoch, link->provider, revision, memory ? AZ_STATUS_CUES_LIVE_MEMORY : AZ_STATUS_CUES_LIVE_HOT, link->slot, ref);
}
static void ds_file_loop_refresh(unsigned deck) {
    if (!atomic_exchange_explicit(&ds_file_refresh[deck], 0, memory_order_acq_rel)) return;
    const uint64_t epoch = ds_native_epoch[deck];
    if (!epoch || epoch != ds_file_working[deck].load ||
        !ds_file_working[deck].token) return;
    az_status_cue_ref_v1 ref = {0};
    int found = ds_working_loop_ref(deck, epoch, ds_file_working[deck].token, ds_file_working[deck].field,
                                    &ds_file_working[deck].native, &ref);
    struct azstatus_native_receipt r = {.unit = ds_file_working[deck].unit, .load_generation = epoch};
    struct ds_field f = ds_native_field(&r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY, found ? AZ_AVAIL_READY : AZ_AVAIL_STALE, found ? &ref : NULL);
    if (ds_native_stamp(deck, &f, 1)) ds_publish(deck, epoch, DS_NATIVE_INPUT, 0, &f, 1, NULL, NULL, 0, 0);
}
/* Duration owns only these two scalar fields. Its source loss leaves native
 * head/phase history intact; ds_times withdraws dependent remaining estimates. */
static void ds_native_duration(const struct azstatus_native_receipt *r, int gap) {
    unsigned d = r->deck;
    uint64_t load = ds_native_epoch[d];
    if (!load || (!gap && (!r->unit || r->load_generation != load || r->epoch != ds_native_fence[d]))) return;
    struct azstatus_native_receipt origin = *r;
    origin.load_generation = load;
    origin.epoch = ds_native_fence[d];
    az_status_coord_v1 coord = {0};
    int ready = !gap && r->valid == 1 && r->points[0].rate < 0x7fffffffU && ds_native_coord(&r->points[0], &coord);
    const int zero = ready ? compare_timestamps(coord.samples, 0, coord.rate_hz, coord.rate_hz, 1) : AZ_TIME_INVALID;
    const int bound = ready ? compare_timestamps(coord.samples, INT64_C(9007199254740992), coord.rate_hz, coord.rate_hz, 1) : AZ_TIME_INVALID;
    ready = ready && zero != AZ_TIME_INVALID && zero != AZ_TIME_BEFORE && bound != AZ_TIME_INVALID && bound != AZ_TIME_AFTER;
    double values[2] = {ready ? coord.seconds : 0, ready ? 1.0 / coord.rate_hz : 0};
    int64_t ticks[2] = {coord.samples, 1};
    struct ds_time_axis clocks[2] = {
        {.units_per_second = coord.rate_hz, .ticks = &ticks[0], .window_samples = 1, .window_rate = coord.rate_hz},
        {.units_per_second = coord.rate_hz, .ticks = &ticks[1], .window_samples = 1, .window_rate = coord.rate_hz}};
    struct ds_field fields[2];
    const unsigned keys[2] = {AZ_STATUS_METADATA_DURATION_S, AZ_STATUS_METADATA_DURATION_PRECISION_S};
    for (unsigned i = 0; i < 2; ++i) {
        fields[i] = ds_native_field(&origin, AZ_STATUS_METADATA, keys[i], ready ? AZ_AVAIL_READY : AZ_AVAIL_STALE, &values[i]);
        if (ready) fields[i].clock = &clocks[i];
    }
    if (gap) {
        pthread_mutex_lock(&ds.mu);
        for (unsigned i = 0; i < 2; ++i) {
            const struct ds_cell *old = ds.deck[d].current.field[AZ_STATUS_METADATA][keys[i]];
            if (old && old->info.info.provenance == AZ_PROV_NATIVE) {
                fields[i].info.provider_id = old->info.provider_id;
            }
        }
        pthread_mutex_unlock(&ds.mu);
    }
    if (!ds_native_stamp(d, fields, 2)) return;
    const uint64_t loss = gap ? r->lost_topics : 0;
    ds_publish(d, load, DS_NATIVE_INPUT | (gap ? DS_FIELD_LOSS : 0), r->observed_ns, fields, 2, NULL, NULL,
               loss, loss ? AZ_STATUS_CAPTURE_GAP : 0);
    if (!ready) sdk_status_store_reseed(d, load, AZ_STATUS_TOPIC(AZ_STATUS_METADATA));
}
static void ds_native_apply_receipt(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;
    if (deck >= 4) return;
    if (r->kind == AZN_DURATION || (r->kind == AZN_GAP && r->b == AZN_DURATION)) {
        ds_native_duration(r, r->kind == AZN_GAP);
        return;
    }
    if (r->kind == AZN_END || (r->kind == AZN_GAP && r->b == AZN_END)) {
        ds_end_apply(r, r->kind == AZN_GAP);
        return;
    }
    if (r->kind == AZN_TRANSPORT_STATE) {
        ds_normal_transport(r, 0);
        return;
    }
    if (r->kind == AZN_GAP && r->b == AZN_TRANSPORT_STATE) {
        struct azstatus_native_receipt source = *r;
        source.load_generation = ds_native_epoch[deck];
        source.epoch = ds_native_fence[deck];
        ds_normal_transport(&source, 1);
        return;
    }
    if (r->kind == AZN_SYNC_ROLE || (r->kind == AZN_GAP && r->b == AZN_SYNC_ROLE)) {
        ds_native_sync_role(r, r->kind == AZN_GAP);
        return;
    }
    if (r->kind == AZN_RATE || (r->kind == AZN_GAP && r->b == AZN_RATE)) {
        ds_native_rate(r, r->kind == AZN_GAP);
        return;
    }
    if (r->kind == AZN_MAPS) {
        ds_native_map(r);
        return;
    }
    if (r->kind == AZN_MEMORY) {
        ds_native_memory(r);
        if (r->lost_topics) ds_native_memory_seed(deck);
        return;
    }
    if (r->kind == AZN_GAP && r->b == AZN_MEMORY) {
        ds_mem_public_revision[deck] = 0;
        ds_native_field_gap(r, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY);
        ds_native_memory_seed(deck);
        return;
    }
    if (r->kind == AZN_KEY_SHIFT) {
        ds_native_key(r);
        return;
    }
    if (r->kind == AZN_GAP && r->b == AZN_RELOOP) {
        ds_native_field_gap(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE);
        return;
    }
    if (r->kind == AZN_RELOOP) {
        if (!r->unit || !r->load_generation || r->load_generation != ds_native_epoch[deck] ||
            r->epoch != ds_native_fence[deck] || r->reloop < AZN_RELOOP_DENIED || r->reloop > AZN_RELOOP_ALLOWED) return;
        int allowed = r->reloop == AZN_RELOOP_ALLOWED;
        unsigned avail = r->reloop == AZN_RELOOP_STALE ? AZ_AVAIL_STALE : r->reloop == AZN_RELOOP_UNKNOWN ? AZ_AVAIL_UNSUPPORTED
                                                                                                          : AZ_AVAIL_READY;
        struct ds_field f = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE, avail, &allowed);
        if (ds_native_stamp(deck, &f, 1)) ds_publish(deck, r->load_generation, DS_NATIVE_INPUT | (r->lost_topics ? DS_FIELD_LOSS : 0),
                                                     r->observed_ns, &f, 1, NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
        return;
    }
    if (r->kind == AZN_SLIP_HEAD) {
        ds_native_slip_head(r);
        return;
    }
    if (r->kind == AZN_GAP && r->b == AZN_SLIP_HEAD) {
        ds_pending_slip[deck].motion_generation = 0;
        ds_native_field_gap(r, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD);
        return;
    }
    if (r->kind == AZN_GAP && r->b == AZN_KEY_SHIFT) {
        ds_native_field_gap(r, AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES);
        return;
    }
    if (r->kind == AZN_STEMS) {
        ds_native_stems(r);
        return;
    }
    if (r->kind == AZN_PLAY) {
        ds_native_play(r);
        return;
    }
    if (r->kind == AZN_QUANTIZE || r->kind == AZN_KEY_LOCK || r->kind == AZN_SYNC || r->kind == AZN_SLIDER_LOCK) {
        ds_native_settings(r, r->kind, 0);
        return;
    }
    if (r->kind == AZN_GAP && (r->b == AZN_QUANTIZE || r->b == AZN_KEY_LOCK || r->b == AZN_SYNC || r->b == AZN_SLIDER_LOCK)) {
        ds_native_settings(r, (unsigned) r->b, 1);
        return;
    }
    if (r->kind == AZN_GAP && r->b == AZN_HOT_CUES) {
        ds_hot_edition[deck].native_revision = 0;
        ds_native_field_gap(r, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
        return;
    }
    if (r->kind == AZN_GAP && r->b == AZN_MAPS) {
        ds_publish(deck, ds_native_epoch[deck], DS_NATIVE_INPUT, r->observed_ns, NULL, 0, NULL, NULL,
                   r->lost_topics, AZ_STATUS_CAPTURE_GAP);
        const struct azn_maps_api *map = ds_map_service();
        if (map && map->seed) map->seed(deck);
        return;
    }
    if (r->kind == AZN_GAP) {
        if (r->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_LOOP)) ds_file_working[deck].token = 0;
        if (r->b == AZN_MOTION && (r->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION))) {
            ds_pending_head[deck].count = 0;
            ds_native_motion_gap[deck] = ds_native_epoch[deck];
        }
        unsigned flags = DS_NATIVE_INPUT | (r->b == AZN_MOTION ? DS_HEAD_LOSS : 0);
        ds_publish(deck, ds_native_epoch[deck], flags, r->observed_ns, NULL, 0, NULL, NULL,
                   r->lost_topics, AZ_STATUS_CAPTURE_GAP);
        return;
    }
    if (r->kind == AZN_MOTION) {
        ds_native_motion(r, r->motion_order);
        return;
    }
    if (r->kind == AZN_HOT_CUES) {
        ds_native_hot(r);
        return;
    }
    if (r->kind == AZN_METADATA) {
        if (!r->unit || r->unit < ds_native_lti_owner[deck] || r->epoch < ds_native_fence[deck]) return;
        /* A pending attempt can race LTI's own commit. A unit-scoped display
         * value cannot cross a still-active older public load. */
        if (r->epoch > ds_native_fence[deck] && ds_native_epoch[deck]) {
            ds_publish(deck, 0, DS_BOUNDARY, r->observed_ns, NULL, 0, NULL, NULL, 0, 0);
            ds_native_epoch[deck] = 0;
        }
        struct ds_field fields[3];
        char text[2][192];
        ds_native_metadata(deck, r, fields, text);
        if (ds_native_stamp(deck, fields, 3)) {
            ds_publish(deck, ds_native_epoch[deck], 0, r->observed_ns, fields, 3, NULL, NULL, 0, 0);
            if (ds_native_epoch[deck] && (!r->valid || r->a || fields[0].info.info.availability != AZ_AVAIL_READY ||
                                          fields[1].info.info.availability != AZ_AVAIL_READY || fields[2].info.info.availability != AZ_AVAIL_READY))
                sdk_status_store_reseed(deck, ds_native_epoch[deck], AZ_STATUS_TOPIC(AZ_STATUS_METADATA));
        }
        return;
    }
    if (!r->epoch || !r->unit || r->epoch < ds_native_fence[deck]) return;
    if (r->kind == AZN_LOAD) {
        if (r->a == AZN_LOADING && r->epoch == ds_native_fence[deck] && ds_native_terminal[deck]) return;
        uint64_t active = r->a == AZN_LOADED ? r->load_generation : 0;
        if (r->a == AZN_LOADED && !active) return;
        struct ds_field fields[7] = {
            ds_native_field(r, AZ_STATUS_LOAD, AZ_STATUS_LOAD_STATE,
                            r->a == AZN_UNKNOWN ? AZ_AVAIL_STALE : AZ_AVAIL_READY, &r->a),
            ds_native_field(r, AZ_STATUS_LOAD, AZ_STATUS_LOAD_ATTEMPT,
                            r->valid ? AZ_AVAIL_READY : AZ_AVAIL_STALE, &r->attempt)};
        unsigned count = 2;
        if (active && r->size >= offsetof(struct azstatus_native_receipt, original_bpm_state) + sizeof r->original_bpm_state &&
            r->original_bpm_state >= AZN_BPM_READY && r->original_bpm_state <= AZN_BPM_UNAVAILABLE) {
            unsigned availability = r->original_bpm_state == AZN_BPM_EMPTY ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
            if (r->original_bpm_state == AZN_BPM_READY && isfinite(r->original_bpm) &&
                r->original_bpm > 0 && r->original_bpm < DBL_MAX) availability = AZ_AVAIL_READY;
            fields[count++] = ds_native_field(r, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM,
                                             availability, &r->original_bpm);
        }
        if (active && r->size >= offsetof(struct azstatus_native_receipt, source_kind) + sizeof r->source_kind &&
            r->source_kind >= AZ_STATUS_SOURCE_USB1 && r->source_kind <= AZ_STATUS_SOURCE_OTHER) {
            fields[count] = ds_native_field(r, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE, AZ_AVAIL_READY, &r->source_kind);
            fields[count].info.scope = AZ_STATUS_LOAD_SCOPE;
            fields[count].info.scope_id = active;
            ++count;
        }
        char text[2][192];
        struct azstatus_native_receipt stale = {.kind = AZN_METADATA, .unit = ds_native_lti_owner[deck], .epoch = r->epoch, .observed_ns = r->observed_ns};
        if (stale.unit && r->epoch > ds_native_lti_fence[deck]) {
            ds_native_metadata(deck, &stale, fields + count, text);
            count += 3;
        }
        if (!ds_native_stamp(deck, fields, count)) return;
        int rc = ds_publish(deck, active, DS_BOUNDARY, r->observed_ns, fields, count, NULL, NULL, 0, 0);
        if (rc == AZ_OK || rc == AZ_LIMIT) {
            if (active != ds_native_epoch[deck] || r->epoch != ds_native_fence[deck])
                ds_pending_slip[deck].motion_generation = 0;
            ds_native_epoch[deck] = active;
            ds_native_fence[deck] = r->epoch;
            ds_native_terminal[deck] = r->a != AZN_LOADING;
            ss_native_load(deck, active, r->size >= offsetof(struct azstatus_native_receipt, source_pin) + sizeof r->source_pin ? r->source_pin : 0);
            ds_gui_load(deck);
        }
        return;
    }
    if (r->kind != AZN_LOOP || r->epoch != ds_native_fence[deck] ||
        !r->load_generation || r->load_generation != ds_native_epoch[deck]) return;
    static const unsigned ordinals[] = {AZ_STATUS_LOOP_RETAINED_IN, AZ_STATUS_LOOP_RETAINED_OUT,
                                        AZ_STATUS_LOOP_WORKING_IN, AZ_STATUS_LOOP_WORKING_OUT};
    az_status_coord_v1 coords[4] = {0};
    struct ds_time_axis clocks[5] = {0};
    int64_t cue_ticks[2] = {r->points[0].samples, r->points[1].samples};
    struct ds_field fields[10];
    for (unsigned i = 0; i < 4; ++i) {
        const struct azn_point *p = &r->points[i];
        unsigned availability = p->state == AZN_ABSENT ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
        if (ds_native_coord(p, &coords[i])) {
            availability = AZ_AVAIL_READY;
        }
        fields[i] = ds_native_field(r, AZ_STATUS_LOOP, ordinals[i], availability, &coords[i]);
        clocks[i] = (struct ds_time_axis) {.units_per_second = p->rate, .window_samples = 1, .window_rate = p->rate};
        fields[i].clock = &clocks[i];
    }
    fields[4] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED,
                                r->valid ? AZ_AVAIL_READY : AZ_AVAIL_STALE, &r->a);
    int defined = fields[0].info.info.availability == AZ_AVAIL_READY && fields[1].info.info.availability == AZ_AVAIL_READY;
    fields[5] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_DEFINED,
                                (fields[0].info.info.availability == AZ_AVAIL_STALE || fields[1].info.info.availability == AZ_AVAIL_STALE)
                                    ? AZ_AVAIL_STALE
                                    : AZ_AVAIL_READY,
                                &defined);
    az_cue_entry main_cue = {.size = sizeof main_cue, .version = AZ_DATA_VERSION, .classification = AZ_CUE_MAIN, .saved = -1, .modified = -1, .auto_active = -1};
    unsigned availability = AZ_AVAIL_STALE;
    if (fields[0].info.info.availability == AZ_AVAIL_READY && fields[1].info.info.availability != AZ_AVAIL_STALE &&
        (!defined || r->points[0].rate == r->points[1].rate)) {
        main_cue.type = defined ? AZ_CUE_LOOP : AZ_CUE_POINT;
        main_cue.valid = AZ_CUE_START | (defined ? AZ_CUE_END : 0);
        main_cue.start_s = coords[0].seconds;
        main_cue.end_s = coords[1].seconds;
        availability = AZ_AVAIL_READY;
    } else if (r->points[0].state == AZN_ABSENT && r->points[1].state == AZN_ABSENT)
        availability = AZ_AVAIL_EMPTY;
    fields[6] = ds_native_field(r, AZ_STATUS_CUES, AZ_STATUS_CUES_MAIN, availability, &main_cue);
    clocks[4] = (struct ds_time_axis) {.units_per_second = r->points[0].rate, .ticks = cue_ticks, .window_samples = 1, .window_rate = r->points[0].rate};
    fields[6].clock = &clocks[4];
    unsigned count = 7;
    int mode = 0;
    if (r->c >= AZN_MODE_TIME && r->c <= AZN_MODE_UNKNOWN) {
        unsigned avail = r->c <= AZN_MODE_BEAT ? AZ_AVAIL_READY : r->c == AZN_MODE_EMPTY ? AZ_AVAIL_EMPTY
                                                              : r->c == AZN_MODE_STALE   ? AZ_AVAIL_STALE
                                                                                         : AZ_AVAIL_UNSUPPORTED;
        if (r->c == AZN_MODE_TIME) mode = AZ_STATUS_LOOP_MODE_TIME;
        if (r->c == AZN_MODE_BEAT) mode = AZ_STATUS_LOOP_MODE_BEAT;
        fields[count++] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_MODE, avail, &mode);
    }
    int denied = 0;
    if (r->size >= offsetof(struct azstatus_native_receipt, reloop) + sizeof r->reloop && r->reloop >= AZN_RELOOP_DENIED && r->reloop <= AZN_RELOOP_STALE) {
        unsigned avail = r->reloop == AZN_RELOOP_DENIED ? AZ_AVAIL_READY : r->reloop == AZN_RELOOP_STALE ? AZ_AVAIL_STALE
                                                                                                         : AZ_AVAIL_UNSUPPORTED;
        fields[count++] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE, avail, &denied);
    }
    az_status_cue_ref_v1 ref = {0};
    ds_file_working[deck].token = 0;
    if (r->size >= offsetof(struct azstatus_native_receipt, hot_link) + sizeof r->hot_link && r->hot_link.state) {
        const struct azn_hot_link *link = &r->hot_link;
        unsigned avail = link->state == AZN_HOT_LINK_EMPTY ? AZ_AVAIL_EMPTY : AZ_AVAIL_UNSUPPORTED;
        if (link->state == AZN_HOT_LINK_READY || link->state == AZN_MEMORY_LINK_READY) {
            int memory = link->state == AZN_MEMORY_LINK_READY;
            unsigned file_field = memory ? AZ_STATUS_CUES_SAVED_MEMORY : AZ_STATUS_CUES_SAVED_HOT;
            int working = r->a && r->valid && fields[2].info.info.availability == AZ_AVAIL_READY &&
                          fields[3].info.info.availability == AZ_AVAIL_READY;
            uint64_t token = r->size >= offsetof(struct azstatus_native_receipt, file_row_token) + sizeof r->file_row_token ? r->file_row_token : 0;
            if (working && token) {
                ds_file_working[deck].token = r->file_row_token;
                ds_file_working[deck].load = r->load_generation;
                ds_file_working[deck].unit = r->unit;
                ds_file_working[deck].field = file_field;
                ds_file_working[deck].native = *link;
            }
            int current = working && ds_working_loop_ref(deck, r->load_generation, token, file_field, link, &ref);
            avail = current ? AZ_AVAIL_READY : AZ_AVAIL_STALE;
        }
        fields[count++] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY, avail, &ref);
    }
    if (!ds_native_stamp(deck, fields, count)) return;
    ds_publish(deck, r->load_generation, DS_NATIVE_INPUT, r->observed_ns, fields, count, NULL, NULL,
               r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_native_apply(const struct azstatus_native_receipt *r) {
    ds_native_apply_receipt(r);
    if (r->deck < 4 && ds_pending_head[r->deck].count) ds_flush_heads(r->deck);
}

/* Run on a core thread, serialized with native reduction. Registration may
 * call this before pinning INITIAL. Only fields the producer actually installed
 * on this unit gain coverage. Never withdraw a field owned by a file provider. */
static void ds_native_ready(unsigned deck, const struct azstatus_native_v1 *api) {
    static const struct {
        unsigned cap, topic, field;
    } fields[] = {
        {AZN_CAP_TRANSPORT_STATE | AZN_CAP_END, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE},
        {AZN_CAP_KEY_SHIFT, AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES},
        {AZN_CAP_SLIP_HEAD, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD},
        {AZN_CAP_STEMS, AZ_STATUS_STEMS, AZ_STATUS_STEMS_AVAILABLE},
        {AZN_CAP_STEMS, AZ_STATUS_STEMS, AZ_STATUS_STEMS_ENABLED},
        {AZN_CAP_LOAD, AZ_STATUS_LOAD, AZ_STATUS_LOAD_STATE},
        {AZN_CAP_LOAD, AZ_STATUS_LOAD, AZ_STATUS_LOAD_ATTEMPT},
        {AZN_CAP_ORIGINAL_BPM, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM},
        {AZN_CAP_MOTION, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD},
        {AZN_CAP_RATE, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_DIRECTION},
        {AZN_CAP_RATE, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE},
        {AZN_CAP_HOT, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT},
        {AZN_CAP_HOT_LINK | AZN_CAP_MEMORY_LINK, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY},
        {AZN_CAP_MEMORY, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY},
        {AZN_CAP_PLAY_RAW, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RAW_MODE_IS_2},
        {AZN_CAP_PLAY_RAW, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RAW_PLAY_PAUSE},
        {AZN_CAP_PLAY_RAW, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RAW_AUX},
        {AZN_CAP_QUANTIZE, AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_QUANTIZE},
        {AZN_CAP_KEY_LOCK, AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_MASTER_TEMPO},
        {AZN_CAP_KEY_LOCK, AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_KEY_LOCK},
        {AZN_CAP_SLIDER_LOCK, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_SLIDER_LOCKED},
        {AZN_CAP_SYNC_ROLE, AZ_STATUS_SYNC, AZ_STATUS_SYNC_FOLLOWING},
        {AZN_CAP_SYNC_ROLE, AZ_STATUS_SYNC, AZ_STATUS_SYNC_LOCKED},
        {AZN_CAP_SYNC, AZ_STATUS_SYNC, AZ_STATUS_SYNC_ENABLED},
        {AZN_CAP_SYNC, AZ_STATUS_SYNC, AZ_STATUS_SYNC_RAW_MODE},
        {AZN_CAP_DURATION, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_S},
        {AZN_CAP_DURATION, AZ_STATUS_METADATA, AZ_STATUS_METADATA_DURATION_PRECISION_S},
        {AZN_CAP_LTI, AZ_STATUS_METADATA, AZ_STATUS_METADATA_CONTENT_ID},
        {AZN_CAP_LTI, AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE},
        {AZN_CAP_LTI, AZ_STATUS_METADATA, AZ_STATUS_METADATA_ARTIST},
        {AZN_CAP_LOOP, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RETAINED_IN},
        {AZN_CAP_LOOP, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RETAINED_OUT},
        {AZN_CAP_LOOP, AZ_STATUS_LOOP, AZ_STATUS_LOOP_DEFINED},
        {AZN_CAP_LOOP, AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED},
        {AZN_CAP_LOOP, AZ_STATUS_LOOP, AZ_STATUS_LOOP_WORKING_IN},
        {AZN_CAP_LOOP, AZ_STATUS_LOOP, AZ_STATUS_LOOP_WORKING_OUT},
        {AZN_CAP_LOOP_MODE, AZ_STATUS_LOOP, AZ_STATUS_LOOP_MODE},
        {AZN_CAP_RELOOP, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE},
        {AZN_CAP_LOOP, AZ_STATUS_CUES, AZ_STATUS_CUES_MAIN}};
    if (api->size < offsetof(struct azstatus_native_v1, capabilities) + sizeof api->capabilities ||
        !api->capabilities) return;
    unsigned caps = api->capabilities(deck), changed = caps ^ ds_native_caps[deck];
    if (!changed) return;
    struct ds_field updates[sizeof fields / sizeof fields[0]];
    size_t count = 0;
    pthread_mutex_lock(&ds.mu);
    uint64_t epoch = ds.deck[deck].current.epoch;
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        unsigned cap = fields[i].cap, t = fields[i].topic, f = fields[i].field;
        if (!(changed & cap)) continue;
        struct ds_cell *old = ds.deck[deck].current.field[t][f];
        if (old && (old->info.info.provenance == AZ_PROV_FILE ||
                    ((caps & cap) && old->info.info.availability != AZ_AVAIL_UNSUPPORTED))) continue;
        updates[count++] = (struct ds_field) {.topic = t, .field = f, .info = {.scope = t == AZ_STATUS_LOAD || (t == AZ_STATUS_METADATA && cap != AZN_CAP_DURATION) || (t == AZ_STATUS_TRANSPORT && cap == AZN_CAP_PLAY_RAW) || cap == AZN_CAP_SLIDER_LOCK || t == AZ_STATUS_SYNC || t == AZ_STATUS_SETTINGS ? AZ_STATUS_UNIT : AZ_STATUS_LOAD_SCOPE, .info = {.provenance = AZ_PROV_NATIVE, .availability = (caps & cap) ? AZ_AVAIL_LOADING : AZ_AVAIL_UNSUPPORTED}}};
    }
    pthread_mutex_unlock(&ds.mu);
    ds_native_caps[deck] = caps;
    if (count && ds_native_stamp(deck, updates, (unsigned) count)) {
        ds_publish(deck, epoch, 0, ds_now(), updates, count, NULL, NULL, 0, 0);
        if (epoch && (changed & AZN_CAP_ORIGINAL_BPM) && !(caps & AZN_CAP_ORIGINAL_BPM))
            sdk_status_store_reseed(deck, epoch, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO));
        if (epoch && ((changed & AZN_CAP_LTI && !(caps & AZN_CAP_LTI)) ||
                      (changed & AZN_CAP_DURATION && !(caps & AZN_CAP_DURATION))))
            sdk_status_store_reseed(deck, epoch, AZ_STATUS_TOPIC(AZ_STATUS_METADATA));
    }
}
/* Only combine already queued measurements inside one qualified phrase. A
 * change of direction, any other receipt, or a half-open boundary ends the
 * run. The reducer revalidates the interval under its commit lock. */
static int ds_native_leg(const struct azstatus_native_receipt *r, unsigned d) {
    if (r->size != sizeof *r || r->version != 1 || r->deck != d || r->kind != AZN_MOTION ||
        r->a != AZN_MEASURE || r->b || r->c || r->valid || r->lost_topics || r->history_only ||
        r->occurrence_ordinal || r->reloop || r->motion_flags || !r->observed_ns || !r->motion_generation || !r->motion_order ||
        !r->unit || !r->epoch || !r->load_generation || !r->map.owner || r->map.changed ||
        r->map.grid.content || r->map.phrases.content || !r->map.grid.revision || !r->map.phrases.revision ||
        !r->map.grid.provider || !r->map.phrases.provider) return 0;
    const struct azn_point *p = r->points;
    if (p[0].state != AZN_COORD || p[1].state != AZN_COORD || !p[0].rate || p[0].rate != p[1].rate ||
        !r->observed_window_samples || !r->observed_window_rate || !r->from_window_samples || !r->from_window_rate) return 0;
    int order = compare_timestamps(p[1].samples, p[0].samples, p[0].rate,
                                   r->observed_window_rate, r->observed_window_samples);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_AFTER ? 1 : order == AZ_TIME_BEFORE ? -1
                                                                : 0;
}
static unsigned ds_native_interior(const struct azstatus_native_receipt *rows, unsigned count, unsigned d) {
    if (count < 2) return 1;
    const struct azstatus_native_receipt *r = rows;
    int direction = ds_native_leg(r, d);
    if (!direction || r->epoch != ds_native_fence[d] || r->load_generation != ds_native_motion_load[d] ||
        r->motion_order != ds_native_motion_order[d] + 1) return 1;
    struct ds_map map;
    struct ds_path path = {0};
    ds_native_windows(&path, r);
    ds_native_map_stamp(&path, r);
    ds_native_coord(&r->points[0], &path.from);
    ds_native_coord(&r->points[1], &path.to);
    unsigned n = 1;
    pthread_mutex_lock(&ds.mu);
    const struct ds_image *im = &ds.deck[d].current;
    const az_status_phrase_endpoint_v1 *active = ds_value(im, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    if (im->epoch == r->load_generation && ds_map_at(im, &path, &map) && ds_endpoint_known(&map, active) &&
        ds_inside_item(&map, active, &path)) {
        for (; n < count; ++n) {
            const struct azstatus_native_receipt *next = &rows[n], *prev = &rows[n - 1];
            if (ds_native_leg(next, d) != direction || next->unit != r->unit || next->epoch != r->epoch ||
                next->load_generation != r->load_generation || next->motion_generation != r->motion_generation ||
                next->motion_order != prev->motion_order + 1 ||
                memcmp(&next->map, &r->map, sizeof r->map) || next->points[0].rate != prev->points[1].rate) break;
            const int adjacent = compare_timestamps(next->points[0].samples, prev->points[1].samples, prev->points[1].rate,
                                                    prev->observed_window_rate, prev->observed_window_samples);
            if (adjacent == AZ_TIME_INVALID || adjacent != AZ_TIME_EQUAL) break;
            /* Capture timestamps share CLOCK_MONOTONIC's nanosecond clock.
             * The drain budget is 16 ms, checked at its one-nanosecond tick. */
            int order = compare_timestamps(next->observed_ns, prev->observed_ns, 1000000000, 1000000000, 1);
            if (order == AZ_TIME_INVALID || order == AZ_TIME_BEFORE) break;
            int elapsed = compare_timestamps(next->observed_ns - r->observed_ns, UINT64_C(16000000),
                                             1000000000, 1000000000, 1);
            if (elapsed == AZ_TIME_INVALID || elapsed == AZ_TIME_AFTER) break;
            ds_native_coord(&next->points[1], &path.to);
            path.observed_window_samples = next->observed_window_samples;
            path.observed_window_rate = next->observed_window_rate;
            if (!ds_inside_item(&map, active, &path)) break;
        }
    }
    pthread_mutex_unlock(&ds.mu);
    return n;
}
static void ds_native_consume(const struct azstatus_native_v1 *api, unsigned d, const struct azstatus_native_receipt *r) {
    if (r->version != 1 || r->deck != d || r->size < offsetof(struct azstatus_native_receipt, content_id) ||
        ((r->kind == AZN_METADATA || r->kind == AZN_HOT_CUES) && r->size < offsetof(struct azstatus_native_receipt, motion_order)) ||
        (r->kind == AZN_MOTION && r->size < offsetof(struct azstatus_native_receipt, revision)) ||
        (r->kind == AZN_MEMORY && r->size < offsetof(struct azstatus_native_receipt, memory_serial) + sizeof r->memory_serial)) return;
    ds_native_apply(r);
    if (r->kind != AZN_GAP) return;
    struct azstatus_native_receipt seed = {.size = sizeof seed, .version = 1};
    if (r->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_LOAD)) {
        if (api->load_seed(d, &seed)) ds_native_apply(&seed);
        else if (ds_native_epoch[d]) {
            ds_publish(d, 0, DS_BOUNDARY, 0, NULL, 0, NULL, NULL, 0, 0);
            ds_native_epoch[d] = 0;
        }
    }
    if ((r->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_STEMS)) &&
        api->size >= offsetof(struct azstatus_native_v1, stems_seed) + sizeof api->stems_seed && api->stems_seed) {
        seed = (struct azstatus_native_receipt) {.size = sizeof seed, .version = 1};
        if (api->stems_seed(d, &seed)) ds_native_apply(&seed);
    }
    if ((r->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_METADATA)) &&
        api->size >= offsetof(struct azstatus_native_v1, metadata_seed) + sizeof api->metadata_seed && api->metadata_seed) {
        seed = (struct azstatus_native_receipt) {.size = sizeof seed, .version = 1};
        if (api->metadata_seed(d, &seed)) ds_native_apply(&seed);
    }
}
static unsigned ds_native_drain(const struct azstatus_native_v1 *api, unsigned d) {
    struct azstatus_native_receipt rows[8];
    struct azn_hot_receipt packet;
    unsigned count = 0;
    int hot = 0;
    for (; count < sizeof rows / sizeof rows[0]; ++count) {
        packet.base = (struct azstatus_native_receipt) {.size = sizeof packet, .version = 1};
        if (!api->take(d, &packet.base)) break;
        if (packet.base.kind == AZN_HOT_CUES && packet.base.size >= sizeof packet) {
            hot = 1;
            break;
        }
        rows[count] = packet.base;
    }
    for (unsigned i = 0; i < count;) {
        unsigned n = ds_native_interior(rows + i, count - i, d);
        if (n > 1) {
            struct azstatus_native_receipt last = rows[i + n - 1];
            last.points[0] = rows[i].points[0];
            last.from_window_samples = rows[i].from_window_samples;
            last.from_window_rate = rows[i].from_window_rate;
            if (ds_native_motion(&last, rows[i].motion_order) == AZ_OK) {
                if (ds_pending_head[d].count) ds_flush_heads(d);
                i += n;
                continue;
            }
        }
        ds_native_consume(api, d, &rows[i++]);
    }
    if (hot) ds_native_consume(api, d, &packet.base);
    return count + hot;
}
static void *ds_native_run(void *arg) {
    const struct azstatus_native_v1 *api = arg;
    struct pollfd p = {.fd = api->wake_fd(), .events = POLLIN};
    for (;;) {
        int result = poll(&p, 1, -1);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || !(p.revents & POLLIN)) break;
        uint64_t counter;
        if (read(p.fd, &counter, sizeof counter) < 0 && errno != EAGAIN && errno != EINTR) break;
        unsigned progress;
        do {
            progress = 0;
            pthread_mutex_lock(&ds_native_reduce_mu);
            for (unsigned d = 0; d < 4; ++d) {
                unsigned n = ds_native_drain(api, d);
                progress += n;
                if (!n) ds_file_loop_refresh(d);
                ds_native_ready(d, api);
            }
            pthread_mutex_unlock(&ds_native_reduce_mu);
        } while (progress);
    }
    return NULL;
}

static void ds_native_start(void) {
    pthread_mutex_lock(&ds_native_attach_mu);
    if (!atomic_load_explicit(&ds_native_api, memory_order_acquire)) {
        azstatus_native_fn get = (azstatus_native_fn) dlsym(RTLD_DEFAULT, "azstem_status_service_v1");
        const struct azstatus_native_v1 *api = get ? get() : NULL;
        if (api && api->version == 1 && api->size >= offsetof(struct azstatus_native_v1, interest) + sizeof api->interest && api->wake_fd && api->take &&
            api->load_seed && api->interest && api->wake_fd() >= 0) {
            if (api->size >= offsetof(struct azstatus_native_v1, register_source_capture) + sizeof api->register_source_capture &&
                api->register_source_capture) (void) api->register_source_capture(azjs_capture);
            if (api->size >= offsetof(struct azstatus_native_v1, install_settings) + sizeof api->install_settings &&
                api->install_settings) (void) api->install_settings(g_ep147_build, azm_install_island_hook_at);
            if (api->size >= offsetof(struct azstatus_native_v1, install_terminal) + sizeof api->install_terminal &&
                api->install_terminal) (void) api->install_terminal(g_ep147_build, azm_install_island_hook_at);
            pthread_t thread;
            if (azfile_status_callbacks && azfile_status_ref && api->size >= offsetof(struct azstatus_native_v1, file_cues) + sizeof api->file_cues && api->file_cues) {
                const struct azn_file_cue_api *file = api->file_cues();
                if (file && file->version == 1 && file->size >= offsetof(struct azn_file_cue_api, observe_input) + sizeof file->observe_input && file->install && file->transport &&
                    file->install(g_ep147_build, azm_install_island_hook_at, azfile_status_callbacks())) {
                    atomic_store_explicit(&ds_file_cues, file, memory_order_release);
                    if (azfile_map_callbacks && file->size >= offsetof(struct azn_file_cue_api, install_maps) + sizeof file->install_maps && file->install_maps)
                        (void) file->install_maps(g_ep147_build, azm_install_island_hook_at, azfile_map_callbacks());
                }
            }
            atomic_store_explicit(&ds_native_api, api, memory_order_release);
            if (!az_thread_create(&thread, NULL, ds_native_run, (void *) api)) {
                pthread_detach(thread);
                const struct azn_memory_api *memory = ds_memory_api();
                if (memory && memory->connect) memory->connect();
                pthread_mutex_lock(&ds.mu);
                if (api->size >= offsetof(struct azstatus_native_v1, bind_stems) + sizeof api->bind_stems)
                    ds_bind_stems = api->bind_stems;
                pthread_mutex_unlock(&ds.mu);
                for (unsigned d = 0; d < 4; ++d)
                    api->interest(d, atomic_load_explicit(&ds_native_topics[d], memory_order_acquire));
            } else atomic_store_explicit(&ds_native_api, NULL, memory_order_release);
        }
    }
    pthread_mutex_unlock(&ds_native_attach_mu);
}
static void ds_native_interest(unsigned deck, uint64_t topics) {
    if (deck >= 4) return;
    atomic_store_explicit(&ds_native_topics[deck], topics, memory_order_release);
    pthread_mutex_lock(&ds.mu);
    ds.deck[deck].watch_stems = (topics & AZ_STATUS_TOPIC(AZ_STATUS_STEMS)) != 0;
    ds_send_stem_binding(deck);
    pthread_mutex_unlock(&ds.mu);
    const struct azstatus_native_v1 *api = atomic_load_explicit(&ds_native_api, memory_order_acquire);
    if (api) {
        api->interest(deck, topics);
        pthread_mutex_lock(&ds_native_reduce_mu);
        ds_native_ready(deck, api);
        pthread_mutex_unlock(&ds_native_reduce_mu);
    }
}
