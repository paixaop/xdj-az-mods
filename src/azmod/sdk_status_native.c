/* SPDX-License-Identifier: GPL-3.0-only */
/* Included after the status reducer in the host translation unit. */
#include "../common/azstatus_native.h"
#include "../common/azstatus_memory.h"
#include "../common/azanalysis_native.h"
#include <poll.h>
#include <errno.h>
#include <dlfcn.h>
#include <float.h>
#include "../common/azcuetext.h"

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
struct ds_native_storage {
    struct ds_native_revision *last;
    unsigned char *bytes;
    int64_t *ticks;
    size_t width, capacity;
    int bank, scalar_time;
};
static size_t ds_native_value_width(const struct ds_field *field) {
    const struct ds_layout *layout = &ds_layout[field->topic].field[field->field];
    switch (layout->type) {
    case DS_CUES: return field->info.count * sizeof(az_cue_entry);
    case DS_CUE_REF: return sizeof(az_status_cue_ref_v1);
    case DS_STRING: return field->info.count + 1u;
    default: return layout->width;
    }
}
static struct ds_native_storage ds_native_storage(unsigned deck, const struct ds_field *f) {
    struct ds_native_revision *last = &ds_native_revisions[deck][f->topic][f->field];
    struct ds_native_storage storage = {last, last->bytes, last->ticks, ds_native_value_width(f), sizeof last->bytes, 0, 0};
    storage.scalar_time = f->topic == AZ_STATUS_METADATA &&
                          (f->field == AZ_STATUS_METADATA_DURATION_S || f->field == AZ_STATUS_METADATA_DURATION_PRECISION_S);
    if (f->topic != AZ_STATUS_CUES) return storage;
    if (f->field == AZ_STATUS_CUES_LIVE_HOT) {
        storage.bytes = (unsigned char *) ds_hot_rows[deck];
        storage.ticks = ds_hot_ticks[deck];
        storage.capacity = sizeof ds_hot_rows[deck];
        storage.bank = 1;
    } else if (f->field == AZ_STATUS_CUES_LIVE_MEMORY) {
        storage.bytes = (unsigned char *) ds_memory_rows[deck];
        storage.ticks = ds_memory_ticks[deck];
        storage.capacity = sizeof ds_memory_rows[deck];
        storage.bank = 1;
    }
    return storage;
}
static int ds_native_clock_fits(const struct ds_field *f, const struct ds_native_storage *storage) {
    if (!f->clock || !f->clock->ticks || storage->bank) return 1;
    return f->info.count <= 1 && (storage->scalar_time || (f->topic == AZ_STATUS_CUES && f->field == AZ_STATUS_CUES_MAIN));
}
static int ds_native_value_same(const struct ds_field *f, const struct ds_native_storage *storage) {
    if (f->info.info.availability != AZ_AVAIL_READY) return 1;
    if (storage->bank) return f->info.info.revision && f->info.info.revision == storage->last->source_revision;
    return ds_payload_same(f->topic, f->field, storage->bytes, f->value, storage->width, &storage->last->clock, f->clock);
}
static int ds_native_revision_same(const struct ds_field *f, const struct ds_native_storage *storage) {
    const struct ds_native_revision *last = storage->last;
    return last->revision && f->info.scope_id == last->scope && f->info.provider_id == last->provider &&
           f->info.info.availability == last->availability && f->info.count == last->count && ds_native_value_same(f, storage);
}
static void ds_native_store_revision(const struct ds_field *f, struct ds_native_storage *storage) {
    struct ds_native_revision *last = storage->last;
    last->revision++;
    last->scope = f->info.scope_id;
    last->provider = f->info.provider_id;
    last->availability = f->info.info.availability;
    last->count = f->info.count;
    last->source_revision = f->info.info.revision;
    last->clock = f->clock ? *f->clock : (struct ds_time_axis) {0};
    last->clock.stored_cue_ms = NULL;
    if (last->clock.ticks) {
        size_t count = storage->bank ? f->info.count * 2 : storage->scalar_time ? 1u
                                                                                : 2u;
        memcpy(storage->ticks, last->clock.ticks, count * sizeof *storage->ticks);
        last->clock.ticks = storage->ticks;
    }
    if (f->info.info.availability == AZ_AVAIL_READY) memcpy(storage->bytes, f->value, storage->width);
}
static int ds_native_stamp(unsigned deck, struct ds_field *fields, unsigned count) {
    for (unsigned n = 0; n < count; ++n) {
        struct ds_field *f = &fields[n];
        struct ds_native_storage storage = ds_native_storage(deck, f);
        if (!ds_native_clock_fits(f, &storage) || storage.width > storage.capacity) return 0;
        if (!ds_native_revision_same(f, &storage)) {
            if (storage.last->revision == UINT64_MAX) return 0;
            ds_native_store_revision(f, &storage);
        }
        f->info.info.revision = storage.last->revision;
    }
    return 1;
}

static struct ds_field ds_native_field(const struct azstatus_native_receipt *r,
                                       unsigned topic, unsigned field, unsigned availability, const void *value) {
    unsigned scope = (topic == AZ_STATUS_LOAD || (topic == AZ_STATUS_METADATA &&
                                                  field != AZ_STATUS_METADATA_DURATION_S && field != AZ_STATUS_METADATA_DURATION_PRECISION_S))
                         ? AZ_STATUS_UNIT
                         : AZ_STATUS_LOAD_SCOPE;
    return (struct ds_field) {.topic = topic, .field = field, .value = value, .info = {.scope = scope, .scope_id = scope == AZ_STATUS_UNIT ? r->unit : r->load_generation, .provider_id = r->unit, .count = availability == AZ_AVAIL_READY, .info = {.provenance = AZ_PROV_NATIVE, .availability = availability, .observed_ns = r->observed_ns}}};
}

/* The native juce::String is bounded to the SDK field capacity. A partial
 * final character at that bound is dropped. Malformed interior bytes are not published. */
static int ds_native_text(char text[192]) {
    unsigned n = (unsigned) strnlen(text, 192);
    if (!n || n >= 192) return -1;
    for (unsigned i = 0; i < n;) {
        unsigned length = azcue_utf8_width((unsigned char) text[i]);
        if (!length) return -1;
        if (length > n - i) {
            if (n != 191) return -1;
            text[i] = 0;
            return (int) i;
        }
        if (!azcue_utf8_scalar(text + i, length)) return -1;
        i += length;
    }
    return (int) n;
}
static void ds_native_metadata_text(const struct azstatus_native_receipt *r, unsigned fallback, struct ds_field *fields, char text[2][192]) {
    for (unsigned i = 0; i < 2; ++i) {
        int n = r->valid && !r->a ? ds_native_text(text[i]) : -1;
        fields[i + 1] = ds_native_field(r, AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE + i,
                                        n > 0 ? AZ_AVAIL_READY : fallback, text[i]);
        fields[i + 1].info.count = n > 0 ? (unsigned) n : 0;
    }
}
static void ds_native_metadata(unsigned deck, const struct azstatus_native_receipt *r, struct ds_field *fields, char text[2][192]) {
    memcpy(text[0], r->title, 192);
    memcpy(text[1], r->artist, 192);
    unsigned fallback = r->valid && r->a ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
    fields[0] = ds_native_field(r, AZ_STATUS_METADATA, AZ_STATUS_METADATA_CONTENT_ID,
                                r->valid && !r->a && r->content_id ? AZ_AVAIL_READY : fallback, &r->content_id);
    ds_native_metadata_text(r, fallback, fields, text);
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
static int ds_native_loaded(const struct azstatus_native_receipt *r) {
    return r->load_generation && r->load_generation == ds_native_epoch[r->deck] && r->epoch == ds_native_fence[r->deck];
}
static int ds_native_owner_valid(uint64_t unit, uint64_t owner, uint64_t retired) {
    return unit && unit >= owner && unit > retired;
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
struct ds_motion_receipt {
    uint64_t lost;
    struct ds_path path;
    az_status_coord_v1 measured;
    struct ds_field fields[2];
    struct ds_time_axis head_clock;
    struct ds_native_revision previous;
    az_status_occurrence_v1 occurrence;
    unsigned flags, count;
    int departed;
};
static int ds_motion_admit(const struct azstatus_native_receipt *r) {
    unsigned d = r->deck;
    if (!ds_native_loaded(r) || !r->motion_order || !r->motion_generation) return 0;
    if (ds_native_motion_load[d] != r->load_generation) {
        ds_native_motion_load[d] = r->load_generation;
        ds_native_motion_order[d] = 0;
    }
    return r->motion_order > ds_native_motion_order[d];
}
static int ds_motion_may_wait(const struct azstatus_native_receipt *r, uint64_t first_order, uint64_t lost) {
    return !lost && r->a == AZN_MEASURE && first_order == r->motion_order && ds_native_motion_gap[r->deck] != r->load_generation;
}
static int ds_motion_wait_barrier(const struct azstatus_native_receipt *r, uint64_t *lost) {
    struct ds_deck_stamp stamp;
    ds_read_stamp(r->deck, &stamp);
    const uint64_t generation = stamp.motion_generation;
    if (r->motion_generation <= generation) return 0;
    if (ds_hold_head(r->deck, r) == AZ_PENDING) return 1;
    *lost |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT);
    return 0;
}
static int ds_motion_order_loss(const struct azstatus_native_receipt *r, uint64_t first_order, uint64_t *lost) {
    if (first_order == ds_native_motion_order[r->deck] + 1) return 0;
    /* A sample from a generation whose barrier is still owed has no
     * publishable revision yet. Preserve the writer's receipt until the
     * barrier arrives, unless capture already reported a break. */
    if (ds_motion_may_wait(r, first_order, *lost) && ds_motion_wait_barrier(r, lost)) return 1;
    *lost |= AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    return 0;
}
static void ds_motion_coordinates(const struct azstatus_native_receipt *r, struct ds_path *path) {
    az_status_coord_v1 *coords[2] = {&path->from, &path->to};
    for (unsigned i = 0; i < 2; ++i) {
        if (!ds_native_coord(&r->points[i], coords[i])) continue;
        path->endpoints |= i == 0 ? AZ_STATUS_OCCURRENCE_FROM : AZ_STATUS_OCCURRENCE_TO;
    }
}
static int ds_motion_opening_kind(const struct azstatus_native_receipt *r) {
    return r->a == AZN_MOTION_SEED || r->a == AZN_REPOSITION || (r->a == AZN_MEASURE && r->c);
}
static int ds_motion_same_rate(const struct ds_path *path) {
    return path->endpoints == 3 && path->from.rate_hz == path->to.rate_hz;
}
static int ds_motion_stationary_opening(const struct azstatus_native_receipt *r, const struct ds_path *path) {
    if (!(r->motion_flags & AZN_AT_OPENING) || r->occurrence_ordinal || r->a != AZN_MEASURE || !ds_motion_same_rate(path)) return 0;
    int order = compare_timestamps(path->from.samples, path->to.samples, path->to.rate_hz, path->to.rate_hz, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static int ds_motion_began_play(const struct azstatus_native_receipt *r, const struct ds_path *path, uint64_t lost) {
    int position = ds_motion_same_rate(path) ? compare_timestamps(path->from.samples, path->to.samples, path->to.rate_hz,
                                                                  path->observed_window_rate, path->observed_window_samples)
                                             : AZ_TIME_INVALID;
    return !lost && (r->motion_flags & AZN_BEGAN_PLAY) && r->a == AZN_MEASURE && position != AZ_TIME_INVALID && position == AZ_TIME_EQUAL;
}
static void ds_motion_flags(const struct azstatus_native_receipt *r, struct ds_path *path, uint64_t lost) {
    if (r->size < offsetof(struct azstatus_native_receipt, motion_flags) + sizeof r->motion_flags) return;
    path->defer_terminal = !lost && !!(r->motion_flags & AZN_TERMINAL_ARRIVAL);
    path->opening = !!(r->motion_flags & AZN_AT_OPENING) && !r->occurrence_ordinal && ds_motion_opening_kind(r) && (path->endpoints & AZ_STATUS_OCCURRENCE_TO);
    if (ds_motion_stationary_opening(r, path)) path->opening = 1;
    path->began_play = ds_motion_began_play(r, path, lost);
}
static int ds_motion_terminal_orders(int from, int to) {
    return from != AZ_TIME_INVALID && to != AZ_TIME_INVALID && (from == AZ_TIME_BEFORE || from == AZ_TIME_EQUAL) &&
           (to == AZ_TIME_EQUAL || to == AZ_TIME_AFTER);
}
static int ds_motion_terminal(const struct azstatus_native_receipt *r, struct ds_motion_receipt *motion) {
    struct ds_path *path = &motion->path;
    if (!path->defer_terminal || r->points[2].state != AZN_COORD) return 1;
    az_status_coord_v1 limit;
    if (!ds_native_coord(&r->points[2], &limit) || limit.rate_hz != motion->measured.rate_hz || limit.rate_hz != path->from.rate_hz) return 0;
    int from = compare_timestamps(path->from.samples, limit.samples, limit.rate_hz, path->from_window_rate, path->from_window_samples);
    int to = compare_timestamps(motion->measured.samples, limit.samples, limit.rate_hz, path->observed_window_rate, path->observed_window_samples);
    if (!ds_motion_terminal_orders(from, to)) return 0;
    path->terminal = limit;
    return 1;
}
static int ds_motion_head(const struct azstatus_native_receipt *r, struct ds_motion_receipt *motion) {
    struct ds_path *path = &motion->path;
    struct ds_field *head = &motion->fields[0];
    *head = ds_native_field(r, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, (path->endpoints & AZ_STATUS_OCCURRENCE_TO) ? AZ_AVAIL_READY : AZ_AVAIL_STALE, &motion->measured);
    motion->head_clock = (struct ds_time_axis) {.units_per_second = motion->measured.rate_hz, .window_samples = path->observed_window_samples, .window_rate = path->observed_window_rate};
    head->clock = &motion->head_clock;
    if (path->first_order) motion->previous = ds_native_revisions[r->deck][head->topic][head->field];
    return ds_native_stamp(r->deck, head, 1);
}
static int ds_motion_action_kind(const struct azstatus_native_receipt *r) {
    return (r->a == AZN_WRAP || r->a == AZN_JUMP || r->a == AZN_SEEK) && r->occurrence_ordinal;
}
static void ds_motion_classify(const struct azstatus_native_receipt *r, struct ds_motion_receipt *motion) {
    struct ds_path *path = &motion->path;
    ds_native_map_stamp(path, r);
    if (r->a == AZN_MEASURE && !r->c && !path->opening && path->phrase_provider &&
        !(motion->lost & AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION))) path->kind = DS_PATH_TRAVEL;
    if (ds_motion_action_kind(r)) {
        path->kind = DS_PATH_ACTION;
        motion->occurrence = (az_status_occurrence_v1) {.size = sizeof motion->occurrence, .version = AZ_DATA_VERSION, .kind = r->a == AZN_WRAP ? AZ_STATUS_WRAP : r->a == AZN_SEEK ? AZ_STATUS_SEEK
                                                                                                                                                                                    : AZ_STATUS_JUMP,
                                                        .ordinal = r->occurrence_ordinal,
                                                        .endpoints = path->endpoints,
                                                        .from = path->from,
                                                        .to = path->to};
    }
}
static void ds_motion_action_baseline(const struct azstatus_native_receipt *r, const struct ds_motion_receipt *motion) {
    struct ds_path baseline = motion->path;
    baseline.order = r->motion_order - 1;
    baseline.kind = DS_PATH_BASELINE;
    baseline.generation = r->motion_generation - 1;
    baseline.endpoints = 0;
    if (motion->path.endpoints & AZ_STATUS_OCCURRENCE_FROM) {
        baseline.endpoints = AZ_STATUS_OCCURRENCE_TO;
        baseline.to = motion->path.from;
        baseline.observed_window_samples = motion->path.from_window_samples;
        baseline.observed_window_rate = motion->path.from_window_rate;
    }
    ds_native_path(&baseline, r->deck, r->load_generation, DS_NATIVE_INPUT | DS_HEAD_LOSS, r->observed_ns,
                   NULL, 0, NULL, motion->lost, AZ_STATUS_CAPTURE_GAP);
}
static void ds_motion_action_loss(const struct azstatus_native_receipt *r, struct ds_motion_receipt *motion) {
    if (!motion->occurrence.ordinal || !motion->lost) return;
    /* Preserve the intact action after the loss cut even if earlier
     * measurements from the same generation did not reach the consumer. */
    if (r->motion_order > ds_native_motion_order[r->deck] + 1 && r->motion_generation > 1) ds_motion_action_baseline(r, motion);
    else ds_publish(r->deck, r->load_generation, DS_NATIVE_INPUT | DS_HEAD_LOSS, r->observed_ns, NULL, 0, NULL, NULL, motion->lost, AZ_STATUS_CAPTURE_GAP);
    motion->lost = 0;
}
static int ds_motion_departed(const struct azstatus_native_receipt *r, const struct ds_path *path) {
    unsigned d = r->deck;
    if (!ds_end_state[d].ended || ds_end_state[d].load != r->load_generation || !(path->endpoints & AZ_STATUS_OCCURRENCE_TO)) return 0;
    if (path->to.rate_hz != ds_end_state[d].landing.rate_hz) return 1;
    int order = compare_timestamps(path->to.samples, ds_end_state[d].landing.samples, path->to.rate_hz, path->observed_window_rate, path->observed_window_samples);
    return order != AZ_TIME_INVALID && (order == AZ_TIME_BEFORE || order == AZ_TIME_AFTER);
}
static int ds_motion_end_binding(const struct azstatus_native_receipt *r, const struct ds_path *path) {
    unsigned d = r->deck;
    return ds_end_state[d].ended && ds_end_state[d].load == r->load_generation && ds_end_state[d].generation == path->generation &&
           path->phrase_provider == ds_end_state[d].phrase_provider && path->phrase_revision == ds_end_state[d].phrase_revision &&
           !memcmp(&path->grid, &ds_end_state[d].grid, sizeof path->grid);
}
static int ds_motion_at_end(unsigned d, const struct ds_path *path) {
    if (path->endpoints != 3 || path->from.rate_hz != ds_end_state[d].landing.rate_hz || path->to.rate_hz != ds_end_state[d].landing.rate_hz) return 0;
    int from = compare_timestamps(path->from.samples, ds_end_state[d].landing.samples, path->from.rate_hz, path->from.rate_hz, 1);
    int to = compare_timestamps(path->to.samples, ds_end_state[d].landing.samples, path->to.rate_hz, path->to.rate_hz, 1);
    return from != AZ_TIME_INVALID && to != AZ_TIME_INVALID && from == AZ_TIME_EQUAL && to == AZ_TIME_EQUAL;
}
static void ds_motion_preserve_end(const struct azstatus_native_receipt *r, struct ds_motion_receipt *motion) {
    /* A completed source can keep rendering its clamped integer endpoint.
     * That is a baseline of the same confirmed END, not another trip through
     * the map's trailing GAP. Require the original map and writer generation;
     * a map change or a real departure still takes the normal path. */
    if (motion->lost || motion->path.kind != DS_PATH_TRAVEL || !ds_motion_end_binding(r, &motion->path) || !ds_motion_at_end(r->deck, &motion->path)) return;
    motion->path.kind = DS_PATH_BASELINE;
    motion->path.ended = 1;
    motion->path.defer_terminal = 0;
}
static int ds_motion_transport(const struct azstatus_native_receipt *r, struct ds_motion_receipt *motion) {
    unsigned d = r->deck;
    motion->flags = DS_NATIVE_INPUT | (motion->lost ? DS_HEAD_LOSS : 0);
    if (r->a == AZN_MEASURE && !motion->lost) motion->flags |= DS_MOTION;
    motion->count = 1;
    motion->departed = ds_motion_departed(r, &motion->path);
    ds_motion_preserve_end(r, motion);
    if (motion->departed) {
        motion->fields[1] = ds_native_field(r, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, ds_end_state[d].has_normal ? (unsigned) ds_end_state[d].availability : AZ_AVAIL_STALE, &ds_end_state[d].normal);
        motion->fields[1].info.provider_id = ds_end_state[d].normal_unit;
        if (!ds_native_stamp(d, &motion->fields[1], 1)) return 0;
        motion->count = 2;
        motion->flags &= ~DS_MOTION;
    }
    return 1;
}
static int ds_motion_commit(const struct azstatus_native_receipt *r, struct ds_motion_receipt *motion) {
    unsigned d = r->deck;
    int rc = ds_native_path(&motion->path, d, r->load_generation, motion->flags, r->observed_ns, motion->fields, motion->count,
                            motion->occurrence.ordinal ? &motion->occurrence : NULL, motion->lost, motion->lost ? AZ_STATUS_CAPTURE_GAP : 0);
    if (ds_native_motion_order[d] >= motion->path.order) {
        ds_native_motion_gap[d] = 0;
        if (motion->departed) ds_end_state[d].ended = 0;
        /* The burst caller retries declined spans as single measurements. */
        if (rc == AZ_LIMIT && motion->path.first_order) rc = AZ_OK;
    } else if (motion->path.first_order) ds_native_revisions[d][motion->fields[0].topic][motion->fields[0].field] = motion->previous;
    return rc;
}
static int ds_native_motion(const struct azstatus_native_receipt *r, uint64_t first_order) {
    if (!ds_motion_admit(r)) return AZ_STALE;
    struct ds_motion_receipt motion = {.lost = r->lost_topics,
                                       .path = {.order = r->motion_order, .first_order = first_order != r->motion_order ? first_order : 0, .generation = r->motion_generation, .kind = DS_PATH_BASELINE}};
    if (ds_motion_order_loss(r, first_order, &motion.lost)) return AZ_PENDING;
    ds_native_windows(&motion.path, r);
    ds_motion_coordinates(r, &motion.path);
    ds_motion_flags(r, &motion.path, motion.lost);
    motion.measured = motion.path.to;
    if (!ds_motion_terminal(r, &motion)) return AZ_INVALID;
    if (!ds_motion_head(r, &motion)) return AZ_STALE;
    ds_motion_classify(r, &motion);
    ds_motion_action_loss(r, &motion);
    if (!ds_motion_transport(r, &motion)) return AZ_LIMIT;
    return ds_motion_commit(r, &motion);
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
static int ds_native_play_revision(const struct azstatus_native_receipt *r, uint64_t *owner, uint64_t *revision, uint64_t *retired) {
    if (!ds_native_owner_valid(r->unit, *owner, *retired)) return 0;
    if (r->unit > *owner) {
        *owner = r->unit;
        *revision = 0;
    }
    if (r->valid < 0) *retired = r->unit;
    else if (r->revision && r->revision <= *revision) return 0;
    if (r->revision) *revision = r->revision;
    return 1;
}
static void ds_native_play(const struct azstatus_native_receipt *r) {
    static uint64_t owner[4], revision[4], retired[4];
    unsigned d = r->deck;
    if (!ds_native_play_revision(r, &owner[d], &revision[d], &retired[d])) return;
    uint64_t id = AZSP_RAW | r->unit;
    const int values[] = {r->a, r->b, r->c};
    unsigned avail = r->valid < 0 ? AZ_AVAIL_UNSUPPORTED : r->valid ? AZ_AVAIL_READY
                                                                    : AZ_AVAIL_STALE;
    struct ds_field fields[3];
    for (unsigned i = 0; i < 3; i++) fields[i] = (struct ds_field) {AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RAW_MODE_IS_2 + i, {.info = {avail, AZ_PROV_NATIVE, 0, r->observed_ns}, .scope = AZ_STATUS_UNIT, .scope_id = id, .provider_id = id, .count = avail == AZ_AVAIL_READY}, &values[i], NULL};
    if (ds_native_stamp(d, fields, 3)) ds_publish(d, 0, DS_UNIT_INPUT, r->observed_ns, fields, 3, NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
}
/* Preserve each native integer coordinate exactly. Different source rates
 * share a checked common integer clock; their bank has no single observation
 * window, so timestamp equivalence remains unavailable for that mixed bank. */
struct ds_native_cue_units {
    uint64_t units;
    unsigned first_rate, uniform;
};
static uint64_t ds_native_gcd(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}
static int ds_native_extend_clock(struct ds_native_cue_units *clock, const struct azn_point *point) {
    if (point->state != AZN_COORD || !point->rate || point->rate >= 0x7fffffffU) return 0;
    int order = compare_timestamps(point->samples, 0, point->rate, point->rate, 1);
    if (order == AZ_TIME_INVALID || order == AZ_TIME_BEFORE) return 0;
    if (!clock->first_rate) clock->first_rate = point->rate;
    else if (point->rate != clock->first_rate) clock->uniform = 0;
    clock->units = clock->units / ds_native_gcd(clock->units, point->rate) * point->rate;
    return clock->units <= UINT32_MAX;
}
static int ds_native_cue_units(unsigned count, const az_cue_entry *rows, const struct azn_point (*points)[2], struct ds_native_cue_units *clock) {
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            if (!(rows[i].valid & (j ? AZ_CUE_END : AZ_CUE_START))) continue;
            if (!ds_native_extend_clock(clock, &points[i][j])) return 0;
        }
    return clock->first_rate != 0;
}
static int ds_native_cue_ticks(unsigned count, const az_cue_entry *rows, const struct azn_point (*points)[2], uint64_t units, int64_t *ticks) {
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            ticks[2 * i + j] = 0;
            if (!(rows[i].valid & (j ? AZ_CUE_END : AZ_CUE_START))) continue;
            const struct azn_point *point = &points[i][j];
            if (__builtin_mul_overflow(point->samples, (int64_t) (units / point->rate), &ticks[2 * i + j])) return 0;
        }
    return 1;
}
static int ds_native_cue_clock(unsigned count, const az_cue_entry *rows, const struct azn_point (*points)[2], int64_t *ticks, struct ds_time_axis *clock) {
    struct ds_native_cue_units source = {1, 0, 1};
    if (!ds_native_cue_units(count, rows, points, &source) || !ds_native_cue_ticks(count, rows, points, source.units, ticks)) return 0;
    *clock = (struct ds_time_axis) {source.units, ticks, source.uniform ? 1u : 0u, source.uniform ? source.first_rate : 0u, NULL};
    return 1;
}
struct ds_hot_receipt {
    az_cue_entry rows[8];
    struct azn_point points[8][2];
    int64_t ticks[16], stored_ms[16];
    struct ds_time_axis clock;
    unsigned count, availability;
    const struct azn_hot_receipt *packet;
};
static int ds_hot_coordinate(const struct azn_point *point) {
    return point->state == AZN_COORD && compare_timestamps(point->samples, 0, point->rate, point->rate, 1) != AZ_TIME_INVALID;
}
static int ds_hot_pair(const struct azn_point *in, const struct azn_point *out) {
    if (!ds_hot_coordinate(in)) return 0;
    return out->state == AZN_ABSENT || (out->rate == in->rate && ds_hot_coordinate(out));
}
static void ds_hot_details(const struct azstatus_native_receipt *r, const struct azn_hot_receipt *hot, unsigned i, az_cue_entry *entry) {
    if (!hot) return;
    if (r->hot[i].reserved & AZN_HOT_AUTO_ACTIVE) {
        entry->valid |= AZ_CUE_AUTO_ACTIVE;
        entry->auto_active = !!(r->hot[i].reserved & AZN_HOT_AUTO_ON);
    }
    if ((r->hot[i].reserved & AZN_HOT_COMMENT) && memchr(hot->details.comments[i], 0, sizeof entry->comment)) {
        entry->valid |= AZ_CUE_COMMENT;
        memcpy(entry->comment, hot->details.comments[i], sizeof entry->comment);
    }
}
static void ds_hot_row(const struct azstatus_native_receipt *r, struct ds_hot_receipt *hot, unsigned i) {
    const struct azn_point *in = &r->hot[i].in, *out = &r->hot[i].out;
    int loop = out->state == AZN_COORD;
    hot->points[hot->count][0] = *in;
    hot->points[hot->count][1] = *out;
    if (hot->packet) memcpy(hot->stored_ms + 2 * hot->count, hot->packet->details.stored_ms[i], sizeof hot->packet->details.stored_ms[i]);
    az_cue_entry *entry = &hot->rows[hot->count++];
    *entry = (az_cue_entry) {.size = sizeof *entry, .version = AZ_DATA_VERSION, .classification = AZ_CUE_HOT, .slot = i + 1, .entry_id = i + 1, .type = loop ? AZ_CUE_LOOP : AZ_CUE_POINT, .valid = AZ_CUE_START | AZ_CUE_COLOR | (loop ? AZ_CUE_END : 0), .rgb = r->hot[i].rgb, .start_s = (double) in->samples / in->rate, .end_s = loop ? (double) out->samples / out->rate : 0, .saved = -1, .modified = -1, .auto_active = -1};
    ds_hot_details(r, hot->packet, i, entry);
    hot->availability = AZ_AVAIL_READY;
}
static int ds_hot_collect(const struct azstatus_native_receipt *r, struct ds_hot_receipt *hot) {
    if (!r->valid) return 0;
    for (unsigned i = 0; i < 8; ++i) {
        const struct azn_point *in = &r->hot[i].in, *out = &r->hot[i].out;
        if (in->state == AZN_ABSENT && out->state == AZN_ABSENT) continue;
        if (!ds_hot_pair(in, out)) return 0;
        ds_hot_row(r, hot, i);
    }
    if (hot->availability != AZ_AVAIL_READY) return 1;
    if (!ds_native_cue_clock(hot->count, hot->rows, hot->points, hot->ticks, &hot->clock)) return 0;
    if (hot->packet) hot->clock.stored_cue_ms = hot->stored_ms;
    return 1;
}
static void ds_hot_confirm_edition(const struct azstatus_native_receipt *r, const struct ds_field *field, int result) {
    if (result != AZ_OK || field->info.info.availability != AZ_AVAIL_READY || !r->revision) return;
    ds_hot_edition[r->deck].load = r->load_generation;
    ds_hot_edition[r->deck].provider = r->attempt;
    ds_hot_edition[r->deck].native_revision = r->revision;
    ds_hot_edition[r->deck].public_revision = field->info.info.revision;
}
static void ds_hot_publish(const struct azstatus_native_receipt *r, struct ds_field *field) {
    unsigned deck = r->deck;
    struct ds_native_revision *previous = &ds_native_revisions[deck][AZ_STATUS_CUES][AZ_STATUS_CUES_LIVE_HOT];
    uint64_t prior = previous->revision;
    ds_hot_edition[deck].native_revision = 0;
    if (!ds_native_stamp(deck, field, 1)) return;
    /* A missed A→B→A can recover identical visible rows. Give the recovered
     * bank a fresh public edition so an old association cannot survive. */
    if (r->lost_topics && prior == field->info.info.revision && prior < UINT64_MAX) field->info.info.revision = ++previous->revision;
    int rc = ds_publish(deck, r->load_generation, DS_NATIVE_INPUT | (r->lost_topics ? DS_FIELD_LOSS : 0),
                        r->observed_ns, field, 1, NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
    ds_hot_confirm_edition(r, field, rc);
}
static void ds_native_hot(const struct azstatus_native_receipt *r) {
    if (!ds_native_loaded(r) || !r->attempt) return;
    struct ds_hot_receipt hot = {.availability = AZ_AVAIL_EMPTY,
                                 .packet = r->size >= sizeof(struct azn_hot_receipt) ? (const struct azn_hot_receipt *) r : NULL};
    if (!ds_hot_collect(r, &hot)) {
        hot.availability = AZ_AVAIL_STALE;
        hot.count = 0;
    }
    struct ds_field field = ds_native_field(r, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT, hot.availability, hot.rows);
    if (hot.availability == AZ_AVAIL_READY) field.clock = &hot.clock;
    field.info.provider_id = r->attempt;
    field.info.count = hot.count;
    field.info.info.revision = r->size >= offsetof(struct azstatus_native_receipt, revision) + sizeof r->revision ? r->revision : 0;
    ds_hot_publish(r, &field);
}
static void ds_native_field_gap(const struct azstatus_native_receipt *r, unsigned topic, unsigned field) {
    unsigned d = r->deck;
    const uint64_t epoch = ds_native_epoch[d];
    struct ds_field f = {.topic = topic, .field = field, .info = {.scope = AZ_STATUS_LOAD_SCOPE, .scope_id = epoch, .info = {.availability = AZ_AVAIL_STALE, .provenance = AZ_PROV_NATIVE, .observed_ns = r->observed_ns}}};
    struct ds_field old = {.topic = topic, .field = field};
    ds_read_receipts(d, &old, 1, NULL);
    if (old.info.scope_id == epoch) f.info.provider_id = old.info.provider_id;
    if (ds_native_stamp(d, &f, 1))
        ds_publish(d, epoch, DS_NATIVE_INPUT | DS_FIELD_LOSS, r->observed_ns, &f, 1, NULL, NULL,
                   AZ_STATUS_TOPIC(topic), AZ_STATUS_CAPTURE_GAP);
}
static unsigned ds_native_boolean_availability(int valid, int value, int gap, int strict) {
    if (gap) return AZ_AVAIL_STALE;
    if (valid < 0) return AZ_AVAIL_UNSUPPORTED;
    int accepted = strict ? valid == 1 : valid != 0;
    return accepted && (value == 0 || value == 1) ? AZ_AVAIL_READY : AZ_AVAIL_STALE;
}
static unsigned ds_native_setting_index(unsigned kind) {
    if (kind == AZN_QUANTIZE) return 0;
    if (kind == AZN_SYNC) return 2;
    return kind == AZN_SLIDER_LOCK ? 3 : 1;
}
static int ds_native_setting_owner(const struct azstatus_native_receipt *r, int gap, uint64_t *current, uint64_t *retired, uint64_t *unit) {
    *unit = gap ? *current : r->unit;
    if (!gap && !ds_native_owner_valid(*unit, *current, *retired)) return 0;
    if (r->valid < 0 && !gap) *retired = *unit;
    *current = *unit;
    return 1;
}
static unsigned ds_native_setting_fields(const struct azstatus_native_receipt *r, unsigned index, unsigned availability, uint64_t unit, struct ds_field fields[2]) {
    static const struct {
        unsigned topic, count, fields[2];
    } settings[] = {
        {AZ_STATUS_SETTINGS, 1, {AZ_STATUS_SETTINGS_QUANTIZE, 0}},
        {AZ_STATUS_SETTINGS, 2, {AZ_STATUS_SETTINGS_MASTER_TEMPO, AZ_STATUS_SETTINGS_KEY_LOCK}},
        {AZ_STATUS_SYNC, 2, {AZ_STATUS_SYNC_ENABLED, AZ_STATUS_SYNC_RAW_MODE}},
        {AZ_STATUS_TEMPO, 1, {AZ_STATUS_TEMPO_SLIDER_LOCKED, 0}},
    };
    for (unsigned k = 0; k < settings[index].count; k++) {
        int raw = index == 2 && k;
        unsigned status = raw && availability == AZ_AVAIL_READY && r->b != 0 && r->b != 2 ? AZ_AVAIL_STALE : availability;
        fields[k] = (struct ds_field) {settings[index].topic, settings[index].fields[k], {.info = {status, AZ_PROV_NATIVE, 0, r->observed_ns}, .scope = AZ_STATUS_UNIT, .scope_id = unit, .provider_id = unit, .count = status == AZ_AVAIL_READY}, raw ? &r->b : &r->a, NULL};
    }
    return settings[index].count;
}
static void ds_native_settings(const struct azstatus_native_receipt *r, unsigned kind, int gap) {
    static uint64_t current[4][4], retired[4][4];
    unsigned d = r->deck, index = ds_native_setting_index(kind);
    uint64_t unit;
    if (!ds_native_setting_owner(r, gap, &current[d][index], &retired[d][index], &unit)) return;
    struct ds_field fields[2];
    unsigned count = ds_native_setting_fields(r, index, ds_native_boolean_availability(r->valid, r->a, gap, 0), unit, fields);
    if (ds_native_stamp(d, fields, count)) ds_publish(d, 0, DS_UNIT_INPUT, r->observed_ns, fields, count, NULL, NULL, gap ? r->lost_topics : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_native_sync_role(const struct azstatus_native_receipt *r, int gap) {
    static uint64_t current[4], retired[4];
    unsigned d = r->deck;
    uint64_t owner = gap ? current[d] : r->unit;
    if (!ds_native_owner_valid(owner, current[d], retired[d])) return;
    if (!gap && r->valid < 0) retired[d] = owner;
    current[d] = owner;
    struct ds_field fields[2];
    const unsigned selectors[] = {AZ_STATUS_SYNC_FOLLOWING, AZ_STATUS_SYNC_LOCKED};
    const int values[] = {r->a, r->b};
    for (unsigned i = 0; i < 2; i++) {
        unsigned avail = ds_native_boolean_availability(r->valid, values[i], gap, 1);
        fields[i] = (struct ds_field) {AZ_STATUS_SYNC, selectors[i], {.info = {avail, AZ_PROV_NATIVE, 0, r->observed_ns}, .scope = AZ_STATUS_UNIT, .scope_id = owner, .provider_id = owner, .count = avail == AZ_AVAIL_READY}, &values[i], NULL};
    }
    if (ds_native_stamp(d, fields, 2)) ds_publish(d, 0, DS_UNIT_INPUT, r->observed_ns, fields, 2, NULL, NULL,
                                                  gap ? r->lost_topics : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_native_key(const struct azstatus_native_receipt *r) {
    static uint64_t current[4], retired[4];
    if (!ds_native_loaded(r) || !ds_native_owner_valid(r->attempt, current[r->deck], retired[r->deck])) return;
    current[r->deck] = r->attempt;
    if (r->valid < 0) retired[r->deck] = r->attempt;
    unsigned a = r->valid == 1 && r->a >= -12 && r->a <= 12 ? AZ_AVAIL_READY : AZ_AVAIL_STALE;
    struct ds_field f = ds_native_field(r, AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES, a, &r->a);
    f.info.provider_id = r->attempt;
    if (ds_native_stamp(r->deck, &f, 1)) ds_publish(r->deck, r->load_generation, DS_NATIVE_INPUT, r->observed_ns,
                                                    &f, 1, NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_slip_pending(const struct azstatus_native_receipt *r, uint64_t generation) {
    struct azstatus_native_receipt *pending = &ds_pending_slip[r->deck];
    int same_load = pending->motion_generation && pending->load_generation == r->load_generation;
    if (same_load && pending->motion_generation > generation) return;
    uint64_t lost = same_load ? pending->lost_topics : 0;
    /* Ordinary measurements can coalesce. If a validity or owner
     * cut is overwritten while waiting, the next baseline owes loss. */
    if (same_load && (pending->valid != r->valid || pending->attempt != r->attempt)) lost |= AZ_STATUS_TOPIC(AZ_STATUS_POSITION);
    *pending = *r;
    pending->lost_topics |= lost;
}
static int ds_slip_order(const struct azstatus_native_receipt *r, unsigned *availability) {
    uint64_t sample = r->size >= offsetof(struct azstatus_native_receipt, motion_generation) + sizeof r->motion_generation ? r->motion_generation : 0;
    if (!r->c && sample) {
        struct ds_deck_stamp stamp;
        ds_read_stamp(r->deck, &stamp);
        uint64_t generation = stamp.motion_generation;
        if (sample < generation) return 0;
        if (sample > generation) {
            ds_slip_pending(r, sample);
            return 0;
        }
    } else if (*availability == AZ_AVAIL_READY) {
        /* Old peers can still report absence, but cannot order a coordinate. */
        *availability = AZ_AVAIL_STALE;
    }
    return 1;
}
static int ds_slip_availability(unsigned availability) {
    return availability == AZ_AVAIL_READY || availability == AZ_AVAIL_EMPTY || availability == AZ_AVAIL_STALE;
}
static void ds_slip_publish(const struct azstatus_native_receipt *r, unsigned availability, const az_status_coord_v1 *head) {
    struct ds_field field = ds_native_field(r, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD, availability, head);
    struct ds_path timing = {0};
    ds_native_windows(&timing, r);
    struct ds_time_axis clock = {.units_per_second = head->rate_hz, .window_samples = timing.observed_window_samples, .window_rate = timing.observed_window_rate};
    field.clock = &clock;
    field.info.provider_id = r->attempt;
    uint64_t lost = r->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION);
    unsigned flags = DS_NATIVE_INPUT | (lost ? DS_FIELD_LOSS : r->b ? 0
                                                                    : DS_MOTION);
    if (ds_native_stamp(r->deck, &field, 1)) ds_publish(r->deck, r->load_generation, flags, r->observed_ns, &field, 1, NULL, NULL, lost, lost ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_slip_retire(const struct azstatus_native_receipt *r, uint64_t *retired) {
    if (!r->c) return;
    *retired = r->attempt;
    if (ds_pending_slip[r->deck].attempt <= r->attempt) ds_pending_slip[r->deck].motion_generation = 0;
}
static void ds_native_slip_head(const struct azstatus_native_receipt *r) {
    static uint64_t owner[4], retired[4];
    unsigned d = r->deck;
    if (!ds_native_loaded(r) || !ds_native_owner_valid(r->attempt, owner[d], retired[d])) return;
    unsigned availability = (unsigned) r->valid;
    if (!ds_slip_availability(availability) || !ds_slip_order(r, &availability)) return;
    az_status_coord_v1 head = {0};
    if (availability == AZ_AVAIL_READY && !ds_native_coord(&r->points[0], &head)) availability = AZ_AVAIL_STALE;
    owner[d] = r->attempt;
    ds_slip_retire(r, &retired[d]);
    ds_slip_publish(r, availability, &head);
}
static int ds_native_availability(int value) { return value >= 0 && value <= AZ_AVAIL_STALE; }
static void ds_native_stems(const struct azstatus_native_receipt *r) {
    unsigned d = r->deck;
    if (!r->attempt || !ds_native_loaded(r) || !ds_native_availability(r->valid) || !ds_native_availability(r->c) || (unsigned) r->a > 1 || (unsigned) r->b > 1) return;
    struct ds_field fields[] = {
        ds_native_field(r, AZ_STATUS_STEMS, AZ_STATUS_STEMS_AVAILABLE, (unsigned) r->valid, &r->a),
        ds_native_field(r, AZ_STATUS_STEMS, AZ_STATUS_STEMS_ENABLED, (unsigned) r->c, &r->b)};
    fields[0].info.provider_id = fields[1].info.provider_id = r->attempt;
    if (ds_native_stamp(d, fields, 2))
        ds_publish(d, r->load_generation, DS_NATIVE_INPUT, r->observed_ns, fields, 2, NULL, NULL, 0, 0);
}
#include "sdk_status_maps_consumer.inc"
#include "sdk_status_memory_consumer.inc"
static int ds_native_rate_source(const struct azstatus_native_receipt *r, int gap, uint64_t epoch) {
    return epoch && (gap || (ds_native_loaded(r) && r->unit && r->attempt));
}
static int ds_native_rate_payload(const struct azstatus_native_receipt *r, double speed) {
    return r->valid >= 0 && r->valid <= AZ_AVAIL_STALE && r->b >= 0 && r->b <= AZ_AVAIL_STALE &&
           r->a >= -1 && r->a <= 1 && isfinite(speed) && speed >= 0 && speed <= 8;
}
static void ds_native_rate_providers(unsigned d, uint64_t epoch, struct ds_field fields[2]) {
    struct ds_field old[2] = {fields[0], fields[1]};
    ds_read_receipts(d, old, 2, NULL);
    for (unsigned i = 0; i < 2; i++)
        if (old[i].info.scope_id == epoch) fields[i].info.provider_id = old[i].info.provider_id;
}
static void ds_native_rate_fields(const struct azstatus_native_receipt *source, const struct azstatus_native_receipt *r,
                                  int gap, uint64_t epoch, const double *speed, struct ds_field fields[2]) {
    fields[0] = ds_native_field(source, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_RATE, gap ? AZ_AVAIL_STALE : (unsigned) r->valid, speed);
    fields[1] = ds_native_field(source, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_DIRECTION, gap ? AZ_AVAIL_STALE : (unsigned) r->b, &r->a);
    for (unsigned i = 0; i < 2; i++) {
        fields[i].info.scope = AZ_STATUS_LOAD_SCOPE;
        fields[i].info.scope_id = epoch;
    }
}
static void ds_native_rate_publish(const struct azstatus_native_receipt *r, uint64_t epoch, struct ds_field fields[2]) {
    if (ds_native_stamp(r->deck, fields, 2)) ds_publish(r->deck, epoch, DS_NATIVE_INPUT | (r->lost_topics ? DS_FIELD_LOSS : 0), r->observed_ns,
                                                        fields, 2, NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_native_rate(const struct azstatus_native_receipt *r, int gap) {
    unsigned d = r->deck;
    uint64_t epoch = gap ? ds_native_epoch[d] : r->load_generation;
    if (!ds_native_rate_source(r, gap, epoch)) return;
    struct azstatus_native_receipt source = *r;
    source.epoch = ds_native_fence[d];
    source.load_generation = epoch;
    double speed = 0;
    memcpy(&speed, &r->points[0].samples, sizeof speed);
    if (!gap && !ds_native_rate_payload(r, speed)) return;
    struct ds_field fields[2];
    ds_native_rate_fields(&source, r, gap, epoch, &speed, fields);
    if (gap) ds_native_rate_providers(d, epoch, fields);
    ds_native_rate_publish(r, epoch, fields);
}
/* A terminal decision and normal playback are independent native writers.
 * While terminal is positively asserted it takes precedence. Retain the
 * exact last normal receipt so a terminal withdrawal can re-expose it. */
static void ds_end_load(unsigned d, uint64_t load) {
    if (ds_end_state[d].load == load) return;
    memset(&ds_end_state[d], 0, sizeof ds_end_state[d]);
    ds_end_state[d].load = load;
}
static void ds_normal_publish(const struct azstatus_native_receipt *r, int gap) {
    unsigned d = r->deck;
    int value = ds_end_state[d].ended ? AZ_PLAYBACK_ENDED : r->a;
    struct ds_field field = ds_native_field(r, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE,
                                            ds_end_state[d].ended ? AZ_AVAIL_READY : ds_end_state[d].availability, &value);
    if (ds_native_stamp(d, &field, 1)) ds_publish(d, r->load_generation, DS_NATIVE_INPUT | (gap ? DS_FIELD_LOSS : 0), r->observed_ns,
                                                  &field, 1, NULL, NULL, gap ? AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
}
static void ds_normal_transport(const struct azstatus_native_receipt *r, int gap) {
    unsigned d = r->deck;
    if (!ds_native_loaded(r)) return;
    ds_end_load(d, r->load_generation);
    if (!gap) ds_end_state[d].normal_unit = r->unit;
    ds_end_state[d].normal = r->a;
    ds_end_state[d].availability = gap ? AZ_AVAIL_STALE : r->valid;
    ds_end_state[d].has_normal = 1;
    if (ds_end_state[d].ended && !gap) return;
    ds_normal_publish(r, gap);
}
struct ds_end_receipt {
    uint64_t epoch, lost;
    int ended, state;
    struct azstatus_native_receipt origin;
    struct ds_field fields[2];
    struct ds_path path;
    struct ds_time_axis head_clock;
    unsigned count;
};
static int ds_end_receipt_valid(const struct azstatus_native_receipt *r, int gap, uint64_t epoch) {
    if (!epoch || epoch != ds_native_epoch[r->deck]) return 0;
    return gap || (r->epoch == ds_native_fence[r->deck] && r->motion_order && r->motion_generation && r->valid == 1 && (unsigned) r->a <= 1);
}
static int ds_end_prepare(const struct azstatus_native_receipt *r, int gap, struct ds_end_receipt *end) {
    unsigned d = r->deck;
    ds_end_load(d, end->epoch);
    end->ended = !gap && r->a;
    unsigned availability = end->ended ? AZ_AVAIL_READY : (!gap && ds_end_state[d].has_normal ? (unsigned) ds_end_state[d].availability : AZ_AVAIL_STALE);
    end->state = end->ended ? AZ_PLAYBACK_ENDED : ds_end_state[d].normal;
    end->origin = *r;
    if (!end->ended && ds_end_state[d].has_normal) end->origin.unit = ds_end_state[d].normal_unit;
    end->origin.epoch = ds_native_fence[d];
    end->origin.load_generation = end->epoch;
    end->origin.observed_ns = r->observed_ns;
    end->fields[0] = ds_native_field(&end->origin, AZ_STATUS_TRANSPORT, AZ_STATUS_TRANSPORT_STATE, availability, &end->state);
    end->count = 1;
    return ds_native_stamp(d, end->fields, 1);
}
static void ds_end_gap(const struct azstatus_native_receipt *r, struct ds_end_receipt *end) {
    unsigned d = r->deck;
    struct ds_field active = {.topic = AZ_STATUS_PHRASE_TRANSITION, .field = AZ_STATUS_PHRASE_TRANSITION_ACTIVE};
    ds_read_receipts(d, &active, 1, NULL);
    if (active.info.info.availability == AZ_AVAIL_READY && active.info.scope_id == end->epoch) {
        end->fields[1] = active;
        end->fields[1].info.info.availability = AZ_AVAIL_STALE;
        end->fields[1].info.info.observed_ns = r->observed_ns;
        end->fields[1].info.count = 0;
        end->count = 2;
    }
    ds_end_state[d].ended = 0;
    ds_publish(d, end->epoch, DS_NATIVE_INPUT | DS_FIELD_LOSS, r->observed_ns, end->fields, end->count, NULL, NULL, end->lost, AZ_STATUS_CAPTURE_GAP);
}
static void ds_end_path(const struct azstatus_native_receipt *r, struct ds_end_receipt *end) {
    struct ds_path *path = &end->path;
    *path = (struct ds_path) {.kind = DS_PATH_BASELINE, .order = r->motion_order, .generation = r->motion_generation, .ended = end->ended};
    ds_native_windows(path, r);
    ds_native_map_stamp(path, r);
    if (ds_native_coord(&r->points[1], &path->to)) {
        path->from = path->to;
        path->endpoints = 3;
    }
    if (!end->lost && path->phrase_provider && (r->motion_flags & AZN_TERMINAL_CONTINUATION)) {
        path->kind = DS_PATH_TRAVEL;
        path->confirm_terminal = 1;
    }
    if (r->motion_order > ds_native_motion_order[r->deck] + 1) {
        end->lost |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
        path->kind = DS_PATH_BASELINE;
        path->confirm_terminal = 0;
    }
}
static int ds_end_head(const struct azstatus_native_receipt *r, struct ds_end_receipt *end) {
    end->head_clock = (struct ds_time_axis) {.units_per_second = end->path.to.rate_hz,
                                             .window_samples = end->path.observed_window_samples,
                                             .window_rate = end->path.observed_window_rate};
    if (!end->ended || !(end->path.endpoints & AZ_STATUS_OCCURRENCE_TO)) return 1;
    end->fields[1] = ds_native_field(&end->origin, AZ_STATUS_POSITION, AZ_STATUS_POSITION_HEAD, AZ_AVAIL_READY, &end->path.to);
    end->fields[1].clock = &end->head_clock;
    if (!ds_native_stamp(r->deck, &end->fields[1], 1)) return 0;
    end->count = 2;
    return 1;
}
static void ds_end_commit(const struct azstatus_native_receipt *r, struct ds_end_receipt *end) {
    unsigned d = r->deck;
    int rc = ds_native_path(&end->path, d, end->epoch, DS_NATIVE_INPUT | (end->lost ? DS_FIELD_LOSS : 0), r->observed_ns,
                            end->fields, end->count, NULL, end->lost, end->lost ? AZ_STATUS_CAPTURE_GAP : 0);
    if ((rc != AZ_OK && rc != AZ_LIMIT) || ds_native_motion_order[d] < end->path.order) return;
    ds_end_state[d].ended = end->ended;
    ds_end_state[d].landing = end->path.to;
    ds_end_state[d].generation = end->path.generation;
    ds_end_state[d].phrase_provider = end->path.phrase_provider;
    ds_end_state[d].phrase_revision = end->path.phrase_revision;
    ds_end_state[d].grid = end->path.grid;
}
static void ds_end_apply(const struct azstatus_native_receipt *r, int gap) {
    struct ds_end_receipt end = {.epoch = gap ? ds_native_epoch[r->deck] : r->load_generation, .lost = r->lost_topics};
    if (!ds_end_receipt_valid(r, gap, end.epoch) || !ds_end_prepare(r, gap, &end)) return;
    if (gap) {
        ds_end_gap(r, &end);
        return;
    }
    ds_end_path(r, &end);
    if (ds_end_head(r, &end)) ds_end_commit(r, &end);
}
/* Recheck only the association when a saved collection becomes eligible.
 * The independent native loop, cue and lock fields have their own writers.
 * Refresh requests are consumed by the normal reducer after queued writes. */
static int ds_file_loop_reference(unsigned deck, uint64_t epoch, uint64_t token, unsigned file_field, az_status_cue_ref_v1 *ref) {
    return azfile_status_ref && azfile_status_ref(deck, epoch, token, ref) && ref->load_epoch == epoch && ref->provenance == AZ_PROV_FILE &&
           ref->cue_field == file_field && ref->entry.type == AZ_CUE_LOOP && (ref->entry.valid & (AZ_CUE_START | AZ_CUE_END)) == (AZ_CUE_START | AZ_CUE_END);
}
static uint64_t ds_memory_loop_revision(unsigned deck, uint64_t epoch, const struct azn_hot_link *link) {
    return ds_mem_epoch[deck] == epoch && ds_mem_provider[deck] == link->provider && link->stamp == ds_mem_revision[deck] ? ds_mem_public_revision[deck] : 0;
}
static uint64_t ds_hot_loop_revision(unsigned deck, uint64_t epoch, const struct azn_hot_link *link) {
    return link->state == AZN_HOT_LINK_READY && ds_hot_edition[deck].load == epoch && ds_hot_edition[deck].provider == link->provider &&
                   link->stamp == ds_hot_edition[deck].native_revision
               ? ds_hot_edition[deck].public_revision
               : 0;
}
static int ds_working_loop_ref(unsigned deck, uint64_t epoch, uint64_t token, unsigned file_field,
                               const struct azn_hot_link *link, az_status_cue_ref_v1 *ref) {
    /* A physical FILE witness fixes the association's source. If its edition
     * is unavailable, withdraw it; an intact LIVE copy is a different target,
     * even when IDs, revisions and bounds happen to coincide. */
    if (token) return ds_file_loop_reference(deck, epoch, token, file_field, ref);
    int memory = link->state == AZN_MEMORY_LINK_READY;
    uint64_t revision = memory ? ds_memory_loop_revision(deck, epoch, link) : ds_hot_loop_revision(deck, epoch, link);
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
static int ds_duration_source(const struct azstatus_native_receipt *r, int gap) {
    return ds_native_epoch[r->deck] && (gap || (r->unit && ds_native_loaded(r)));
}
static int ds_duration_coordinate(const struct azstatus_native_receipt *r, int gap, az_status_coord_v1 *coord) {
    if (gap || r->valid != 1 || r->points[0].rate >= 0x7fffffffU || !ds_native_coord(&r->points[0], coord)) return 0;
    int zero = compare_timestamps(coord->samples, 0, coord->rate_hz, coord->rate_hz, 1);
    int bound = compare_timestamps(coord->samples, INT64_C(9007199254740992), coord->rate_hz, coord->rate_hz, 1);
    return zero != AZ_TIME_INVALID && zero != AZ_TIME_BEFORE && bound != AZ_TIME_INVALID && bound != AZ_TIME_AFTER;
}
struct ds_duration_receipt {
    double values[2];
    int64_t ticks[2];
    struct ds_time_axis clocks[2];
    struct ds_field fields[2];
};
static void ds_duration_fields(const struct azstatus_native_receipt *origin, const az_status_coord_v1 *coord, int ready, struct ds_duration_receipt *duration) {
    duration->values[0] = ready ? coord->seconds : 0;
    duration->values[1] = ready ? 1.0 / coord->rate_hz : 0;
    duration->ticks[0] = coord->samples;
    duration->ticks[1] = 1;
    const unsigned keys[2] = {AZ_STATUS_METADATA_DURATION_S, AZ_STATUS_METADATA_DURATION_PRECISION_S};
    for (unsigned i = 0; i < 2; ++i) {
        duration->clocks[i] = (struct ds_time_axis) {.units_per_second = coord->rate_hz, .ticks = &duration->ticks[i], .window_samples = 1, .window_rate = coord->rate_hz};
        duration->fields[i] = ds_native_field(origin, AZ_STATUS_METADATA, keys[i], ready ? AZ_AVAIL_READY : AZ_AVAIL_STALE, &duration->values[i]);
        if (ready) duration->fields[i].clock = &duration->clocks[i];
    }
}
static void ds_duration_providers(unsigned d, struct ds_field fields[2]) {
    struct ds_field old[2] = {fields[0], fields[1]};
    ds_read_receipts(d, old, 2, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (old[i].info.info.provenance == AZ_PROV_NATIVE) fields[i].info.provider_id = old[i].info.provider_id;
}
static void ds_native_duration(const struct azstatus_native_receipt *r, int gap) {
    if (!ds_duration_source(r, gap)) return;
    unsigned d = r->deck;
    uint64_t load = ds_native_epoch[d];
    struct azstatus_native_receipt origin = *r;
    origin.load_generation = load;
    origin.epoch = ds_native_fence[d];
    az_status_coord_v1 coord = {0};
    int ready = ds_duration_coordinate(r, gap, &coord);
    struct ds_duration_receipt duration;
    ds_duration_fields(&origin, &coord, ready, &duration);
    if (gap) ds_duration_providers(d, duration.fields);
    if (!ds_native_stamp(d, duration.fields, 2)) return;
    const uint64_t loss = gap ? r->lost_topics : 0;
    ds_publish(d, load, DS_NATIVE_INPUT | (gap ? DS_FIELD_LOSS : 0), r->observed_ns, duration.fields, 2, NULL, NULL, loss, loss ? AZ_STATUS_CAPTURE_GAP : 0);
    if (!ready) sdk_status_store_reseed(d, load, AZ_STATUS_TOPIC(AZ_STATUS_METADATA));
}
static void ds_receipt_duration(const struct azstatus_native_receipt *r) {
    ds_native_duration(r, 0);
}
static void ds_receipt_duration_gap(const struct azstatus_native_receipt *r) {
    ds_native_duration(r, 1);
}
static void ds_receipt_end(const struct azstatus_native_receipt *r) {
    ds_end_apply(r, 0);
}
static void ds_receipt_end_gap(const struct azstatus_native_receipt *r) {
    ds_end_apply(r, 1);
}
static void ds_receipt_sync_role(const struct azstatus_native_receipt *r) {
    ds_native_sync_role(r, 0);
}
static void ds_receipt_sync_role_gap(const struct azstatus_native_receipt *r) {
    ds_native_sync_role(r, 1);
}
static void ds_receipt_rate(const struct azstatus_native_receipt *r) {
    ds_native_rate(r, 0);
}
static void ds_receipt_rate_gap(const struct azstatus_native_receipt *r) {
    ds_native_rate(r, 1);
}
static void ds_receipt_transport(const struct azstatus_native_receipt *r) {
    ds_normal_transport(r, 0);
}
static void ds_receipt_transport_gap(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

    struct azstatus_native_receipt source = *r;
    source.load_generation = ds_native_epoch[deck];
    source.epoch = ds_native_fence[deck];
    ds_normal_transport(&source, 1);
    return;
}
static void ds_receipt_memory(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

    ds_native_memory(r);
    if (r->lost_topics) ds_native_memory_seed(deck);
    return;
}
static void ds_receipt_memory_gap(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

    ds_mem_public_revision[deck] = 0;
    ds_native_field_gap(r, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY);
    ds_native_memory_seed(deck);
    return;
}
static void ds_receipt_reloop_gap(const struct azstatus_native_receipt *r) {

    ds_native_field_gap(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE);
    return;
}
static unsigned ds_reloop_availability(unsigned reloop) {
    if (reloop == AZN_RELOOP_STALE) return AZ_AVAIL_STALE;
    return reloop == AZN_RELOOP_UNKNOWN ? AZ_AVAIL_UNSUPPORTED : AZ_AVAIL_READY;
}
static void ds_receipt_reloop(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

    if (!r->unit || !ds_native_loaded(r) || r->reloop < AZN_RELOOP_DENIED || r->reloop > AZN_RELOOP_ALLOWED) return;
    int allowed = r->reloop == AZN_RELOOP_ALLOWED;
    unsigned avail = ds_reloop_availability(r->reloop);
    struct ds_field f = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE, avail, &allowed);
    if (ds_native_stamp(deck, &f, 1)) ds_publish(deck, r->load_generation, DS_NATIVE_INPUT | (r->lost_topics ? DS_FIELD_LOSS : 0),
                                                 r->observed_ns, &f, 1, NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
    return;
}
static void ds_receipt_slip_gap(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

    ds_pending_slip[deck].motion_generation = 0;
    ds_native_field_gap(r, AZ_STATUS_POSITION, AZ_STATUS_POSITION_SLIP_HEAD);
    return;
}
static void ds_receipt_key_gap(const struct azstatus_native_receipt *r) {

    ds_native_field_gap(r, AZ_STATUS_KEY, AZ_STATUS_KEY_SHIFT_SEMITONES);
    return;
}
static void ds_receipt_settings(const struct azstatus_native_receipt *r) {
    ds_native_settings(r, r->kind, 0);
}
static void ds_receipt_settings_gap(const struct azstatus_native_receipt *r) {
    ds_native_settings(r, (unsigned) r->b, 1);
}
static void ds_receipt_hot_gap(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

    ds_hot_edition[deck].native_revision = 0;
    ds_native_field_gap(r, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_HOT);
    return;
}
static void ds_receipt_maps_gap(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

    ds_publish(deck, ds_native_epoch[deck], DS_NATIVE_INPUT, r->observed_ns, NULL, 0, NULL, NULL,
               r->lost_topics, AZ_STATUS_CAPTURE_GAP);
    const struct azn_maps_api *map = ds_map_service();
    if (map && map->seed) map->seed(deck);
    return;
}
static void ds_receipt_gap(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

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
static void ds_receipt_motion(const struct azstatus_native_receipt *r) {
    ds_native_motion(r, r->motion_order);
}
static int ds_metadata_needs_reseed(const struct azstatus_native_receipt *r, const struct ds_field fields[3]) {
    if (!ds_native_epoch[r->deck]) return 0;
    if (!r->valid || r->a) return 1;
    for (unsigned i = 0; i < 3; ++i)
        if (fields[i].info.info.availability != AZ_AVAIL_READY) return 1;
    return 0;
}
static void ds_receipt_metadata(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;

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
        if (ds_metadata_needs_reseed(r, fields))
            sdk_status_store_reseed(deck, ds_native_epoch[deck], AZ_STATUS_TOPIC(AZ_STATUS_METADATA));
    }
    return;
}
struct ds_load_receipt {
    struct ds_field fields[7];
    char text[2][192];
    struct azstatus_native_receipt stale;
    uint64_t active;
    unsigned count;
};
static unsigned ds_original_bpm_availability(const struct azstatus_native_receipt *r) {
    if (r->original_bpm_state == AZN_BPM_READY && isfinite(r->original_bpm) && r->original_bpm > 0 && r->original_bpm < DBL_MAX) return AZ_AVAIL_READY;
    return r->original_bpm_state == AZN_BPM_EMPTY ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
}
static void ds_load_original_bpm(const struct azstatus_native_receipt *r, struct ds_load_receipt *load) {
    if (!load->active || r->size < offsetof(struct azstatus_native_receipt, original_bpm_state) + sizeof r->original_bpm_state ||
        r->original_bpm_state < AZN_BPM_READY || r->original_bpm_state > AZN_BPM_UNAVAILABLE) return;
    load->fields[load->count++] = ds_native_field(r, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_ORIGINAL_BPM,
                                                  ds_original_bpm_availability(r), &r->original_bpm);
}
static void ds_load_source(const struct azstatus_native_receipt *r, struct ds_load_receipt *load) {
    if (!load->active || r->size < offsetof(struct azstatus_native_receipt, source_kind) + sizeof r->source_kind ||
        r->source_kind < AZ_STATUS_SOURCE_USB1 || r->source_kind > AZ_STATUS_SOURCE_OTHER) return;
    struct ds_field *field = &load->fields[load->count++];
    *field = ds_native_field(r, AZ_STATUS_LOAD, AZ_STATUS_LOAD_SOURCE, AZ_AVAIL_READY, &r->source_kind);
    field->info.scope = AZ_STATUS_LOAD_SCOPE;
    field->info.scope_id = load->active;
}
static void ds_load_metadata(const struct azstatus_native_receipt *r, struct ds_load_receipt *load) {
    unsigned deck = r->deck;
    load->stale = (struct azstatus_native_receipt) {.kind = AZN_METADATA, .unit = ds_native_lti_owner[deck], .epoch = r->epoch, .observed_ns = r->observed_ns};
    if (load->stale.unit && r->epoch > ds_native_lti_fence[deck]) {
        ds_native_metadata(deck, &load->stale, load->fields + load->count, load->text);
        load->count += 3;
    }
}
static void ds_load_commit(const struct azstatus_native_receipt *r, struct ds_load_receipt *load) {
    unsigned deck = r->deck;
    if (!ds_native_stamp(deck, load->fields, load->count)) return;
    int rc = ds_publish(deck, load->active, DS_BOUNDARY, r->observed_ns, load->fields, load->count, NULL, NULL, 0, 0);
    if (rc != AZ_OK && rc != AZ_LIMIT) return;
    if (load->active != ds_native_epoch[deck] || r->epoch != ds_native_fence[deck]) ds_pending_slip[deck].motion_generation = 0;
    ds_native_epoch[deck] = load->active;
    ds_native_fence[deck] = r->epoch;
    ds_native_terminal[deck] = r->a != AZN_LOADING;
    ss_native_load(deck, load->active, r->size >= offsetof(struct azstatus_native_receipt, source_pin) + sizeof r->source_pin ? r->source_pin : 0);
    ds_gui_load(deck);
}
static int ds_load_receipt_valid(const struct azstatus_native_receipt *r) {
    unsigned deck = r->deck;
    if (!r->epoch || !r->unit || r->epoch < ds_native_fence[deck]) return 0;
    return r->a != AZN_LOADING || r->epoch != ds_native_fence[deck] || !ds_native_terminal[deck];
}
static void ds_receipt_load(const struct azstatus_native_receipt *r) {
    if (!ds_load_receipt_valid(r)) return;
    struct ds_load_receipt load = {.active = r->a == AZN_LOADED ? r->load_generation : 0, .count = 2};
    if (r->a == AZN_LOADED && !load.active) return;
    load.fields[0] = ds_native_field(r, AZ_STATUS_LOAD, AZ_STATUS_LOAD_STATE, r->a == AZN_UNKNOWN ? AZ_AVAIL_STALE : AZ_AVAIL_READY, &r->a);
    load.fields[1] = ds_native_field(r, AZ_STATUS_LOAD, AZ_STATUS_LOAD_ATTEMPT, r->valid ? AZ_AVAIL_READY : AZ_AVAIL_STALE, &r->attempt);
    ds_load_original_bpm(r, &load);
    ds_load_source(r, &load);
    ds_load_metadata(r, &load);
    ds_load_commit(r, &load);
}
struct ds_loop_receipt {
    az_status_coord_v1 coords[4];
    struct ds_time_axis clocks[5];
    int64_t cue_ticks[2];
    struct ds_field fields[10];
    int defined, mode, denied;
    az_cue_entry main_cue;
    az_status_cue_ref_v1 ref;
    unsigned count;
};
static void ds_loop_coordinates(const struct azstatus_native_receipt *r, struct ds_loop_receipt *loop) {
    static const unsigned ordinals[] = {AZ_STATUS_LOOP_RETAINED_IN, AZ_STATUS_LOOP_RETAINED_OUT, AZ_STATUS_LOOP_WORKING_IN, AZ_STATUS_LOOP_WORKING_OUT};
    for (unsigned i = 0; i < 4; ++i) {
        const struct azn_point *point = &r->points[i];
        unsigned availability = point->state == AZN_ABSENT ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
        if (ds_native_coord(point, &loop->coords[i])) availability = AZ_AVAIL_READY;
        loop->fields[i] = ds_native_field(r, AZ_STATUS_LOOP, ordinals[i], availability, &loop->coords[i]);
        loop->clocks[i] = (struct ds_time_axis) {.units_per_second = point->rate, .window_samples = 1, .window_rate = point->rate};
        loop->fields[i].clock = &loop->clocks[i];
    }
}
static void ds_loop_engagement(const struct azstatus_native_receipt *r, struct ds_loop_receipt *loop) {
    loop->fields[4] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_ENGAGED, r->valid ? AZ_AVAIL_READY : AZ_AVAIL_STALE, &r->a);
    unsigned in = loop->fields[0].info.info.availability, out = loop->fields[1].info.info.availability;
    loop->defined = in == AZ_AVAIL_READY && out == AZ_AVAIL_READY;
    unsigned availability = in == AZ_AVAIL_STALE || out == AZ_AVAIL_STALE ? AZ_AVAIL_STALE : AZ_AVAIL_READY;
    loop->fields[5] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_DEFINED, availability, &loop->defined);
}
static int ds_loop_main_ready(const struct azstatus_native_receipt *r, const struct ds_loop_receipt *loop) {
    return loop->fields[0].info.info.availability == AZ_AVAIL_READY && loop->fields[1].info.info.availability != AZ_AVAIL_STALE &&
           (!loop->defined || r->points[0].rate == r->points[1].rate);
}
static void ds_loop_main_cue(const struct azstatus_native_receipt *r, struct ds_loop_receipt *loop) {
    loop->main_cue = (az_cue_entry) {.size = sizeof loop->main_cue, .version = AZ_DATA_VERSION, .classification = AZ_CUE_MAIN, .saved = -1, .modified = -1, .auto_active = -1};
    unsigned availability = AZ_AVAIL_STALE;
    if (ds_loop_main_ready(r, loop)) {
        loop->main_cue.type = loop->defined ? AZ_CUE_LOOP : AZ_CUE_POINT;
        loop->main_cue.valid = AZ_CUE_START | (loop->defined ? AZ_CUE_END : 0);
        loop->main_cue.start_s = loop->coords[0].seconds;
        loop->main_cue.end_s = loop->coords[1].seconds;
        availability = AZ_AVAIL_READY;
    } else if (r->points[0].state == AZN_ABSENT && r->points[1].state == AZN_ABSENT) availability = AZ_AVAIL_EMPTY;
    loop->fields[6] = ds_native_field(r, AZ_STATUS_CUES, AZ_STATUS_CUES_MAIN, availability, &loop->main_cue);
    loop->cue_ticks[0] = r->points[0].samples;
    loop->cue_ticks[1] = r->points[1].samples;
    loop->clocks[4] = (struct ds_time_axis) {.units_per_second = r->points[0].rate, .ticks = loop->cue_ticks, .window_samples = 1, .window_rate = r->points[0].rate};
    loop->fields[6].clock = &loop->clocks[4];
    loop->count = 7;
}
static unsigned ds_loop_mode_availability(int mode) {
    if (mode <= AZN_MODE_BEAT) return AZ_AVAIL_READY;
    if (mode == AZN_MODE_EMPTY) return AZ_AVAIL_EMPTY;
    return mode == AZN_MODE_STALE ? AZ_AVAIL_STALE : AZ_AVAIL_UNSUPPORTED;
}
static void ds_loop_mode(const struct azstatus_native_receipt *r, struct ds_loop_receipt *loop) {
    if (r->c < AZN_MODE_TIME || r->c > AZN_MODE_UNKNOWN) return;
    if (r->c == AZN_MODE_TIME) loop->mode = AZ_STATUS_LOOP_MODE_TIME;
    if (r->c == AZN_MODE_BEAT) loop->mode = AZ_STATUS_LOOP_MODE_BEAT;
    loop->fields[loop->count++] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_MODE, ds_loop_mode_availability(r->c), &loop->mode);
}
static void ds_loop_reloop(const struct azstatus_native_receipt *r, struct ds_loop_receipt *loop) {
    if (r->size < offsetof(struct azstatus_native_receipt, reloop) + sizeof r->reloop || r->reloop < AZN_RELOOP_DENIED || r->reloop > AZN_RELOOP_STALE) return;
    unsigned availability = r->reloop == AZN_RELOOP_DENIED ? AZ_AVAIL_READY : r->reloop == AZN_RELOOP_STALE ? AZ_AVAIL_STALE
                                                                                                            : AZ_AVAIL_UNSUPPORTED;
    loop->fields[loop->count++] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_RELOOP_AVAILABLE, availability, &loop->denied);
}
static int ds_loop_working(const struct azstatus_native_receipt *r, const struct ds_loop_receipt *loop) {
    return r->a && r->valid && loop->fields[2].info.info.availability == AZ_AVAIL_READY && loop->fields[3].info.info.availability == AZ_AVAIL_READY;
}
static unsigned ds_loop_link_reference(const struct azstatus_native_receipt *r, struct ds_loop_receipt *loop) {
    const struct azn_hot_link *link = &r->hot_link;
    int memory = link->state == AZN_MEMORY_LINK_READY;
    unsigned file_field = memory ? AZ_STATUS_CUES_SAVED_MEMORY : AZ_STATUS_CUES_SAVED_HOT;
    int working = ds_loop_working(r, loop);
    uint64_t token = r->size >= offsetof(struct azstatus_native_receipt, file_row_token) + sizeof r->file_row_token ? r->file_row_token : 0;
    if (working && token) {
        ds_file_working[r->deck].token = token;
        ds_file_working[r->deck].load = r->load_generation;
        ds_file_working[r->deck].unit = r->unit;
        ds_file_working[r->deck].field = file_field;
        ds_file_working[r->deck].native = *link;
    }
    int current = working && ds_working_loop_ref(r->deck, r->load_generation, token, file_field, link, &loop->ref);
    return current ? AZ_AVAIL_READY : AZ_AVAIL_STALE;
}
static void ds_loop_saved_entry(const struct azstatus_native_receipt *r, struct ds_loop_receipt *loop) {
    ds_file_working[r->deck].token = 0;
    if (r->size < offsetof(struct azstatus_native_receipt, hot_link) + sizeof r->hot_link || !r->hot_link.state) return;
    unsigned availability = r->hot_link.state == AZN_HOT_LINK_EMPTY ? AZ_AVAIL_EMPTY : AZ_AVAIL_UNSUPPORTED;
    if (r->hot_link.state == AZN_HOT_LINK_READY || r->hot_link.state == AZN_MEMORY_LINK_READY) availability = ds_loop_link_reference(r, loop);
    loop->fields[loop->count++] = ds_native_field(r, AZ_STATUS_LOOP, AZ_STATUS_LOOP_SAVED_ENTRY, availability, &loop->ref);
}
static int ds_loop_receipt_valid(const struct azstatus_native_receipt *r) {
    return r->epoch && r->unit && r->epoch >= ds_native_fence[r->deck] && r->kind == AZN_LOOP && r->epoch == ds_native_fence[r->deck] &&
           r->load_generation && r->load_generation == ds_native_epoch[r->deck];
}
static void ds_receipt_loop(const struct azstatus_native_receipt *r) {
    if (!ds_loop_receipt_valid(r)) return;
    struct ds_loop_receipt loop = {0};
    ds_loop_coordinates(r, &loop);
    ds_loop_engagement(r, &loop);
    ds_loop_main_cue(r, &loop);
    ds_loop_mode(r, &loop);
    ds_loop_reloop(r, &loop);
    ds_loop_saved_entry(r, &loop);
    if (!ds_native_stamp(r->deck, loop.fields, loop.count)) return;
    ds_publish(r->deck, r->load_generation, DS_NATIVE_INPUT, r->observed_ns, loop.fields, loop.count, NULL, NULL,
               r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
}
struct ds_native_dispatch {
    void (*receipt)(const struct azstatus_native_receipt *);
    void (*gap)(const struct azstatus_native_receipt *);
};
static void ds_native_apply_receipt(const struct azstatus_native_receipt *r) {
    static const struct ds_native_dispatch dispatch[] = {
        [AZN_DURATION] = {ds_receipt_duration, ds_receipt_duration_gap},
        [AZN_END] = {ds_receipt_end, ds_receipt_end_gap},
        [AZN_SYNC_ROLE] = {ds_receipt_sync_role, ds_receipt_sync_role_gap},
        [AZN_RATE] = {ds_receipt_rate, ds_receipt_rate_gap},
        [AZN_TRANSPORT_STATE] = {ds_receipt_transport, ds_receipt_transport_gap},
        [AZN_MAPS] = {ds_native_map, ds_receipt_maps_gap},
        [AZN_MEMORY] = {ds_receipt_memory, ds_receipt_memory_gap},
        [AZN_KEY_SHIFT] = {ds_native_key, ds_receipt_key_gap},
        [AZN_RELOOP] = {ds_receipt_reloop, ds_receipt_reloop_gap},
        [AZN_SLIP_HEAD] = {ds_native_slip_head, ds_receipt_slip_gap},
        [AZN_STEMS] = {ds_native_stems, NULL},
        [AZN_PLAY] = {ds_native_play, NULL},
        [AZN_QUANTIZE] = {ds_receipt_settings, ds_receipt_settings_gap},
        [AZN_KEY_LOCK] = {ds_receipt_settings, ds_receipt_settings_gap},
        [AZN_SYNC] = {ds_receipt_settings, ds_receipt_settings_gap},
        [AZN_SLIDER_LOCK] = {ds_receipt_settings, ds_receipt_settings_gap},
        [AZN_MOTION] = {ds_receipt_motion, NULL},
        [AZN_HOT_CUES] = {ds_native_hot, ds_receipt_hot_gap},
        [AZN_METADATA] = {ds_receipt_metadata, NULL},
        [AZN_LOAD] = {ds_receipt_load, NULL},
        [AZN_LOOP] = {ds_receipt_loop, NULL},
    };
    if (r->deck >= 4) return;
    unsigned kind = r->kind == AZN_GAP ? (unsigned) r->b : r->kind;
    void (*handler)(const struct azstatus_native_receipt *) = NULL;
    if (kind < sizeof dispatch / sizeof dispatch[0]) handler = r->kind == AZN_GAP ? dispatch[kind].gap : dispatch[kind].receipt;
    if (handler) handler(r);
    else if (r->kind == AZN_GAP) ds_receipt_gap(r);
}
static void ds_native_apply(const struct azstatus_native_receipt *r) {
    ds_native_apply_receipt(r);
    if (r->deck < 4 && ds_pending_head[r->deck].count) ds_flush_heads(r->deck);
}

/* Run on a core thread, serialized with native reduction. Registration may
 * call this before pinning INITIAL. Only fields the producer actually installed
 * on this unit gain coverage. Never withdraw a field owned by a file provider. */
struct ds_native_cap_field {
    unsigned cap, topic, field;
};
static const struct ds_native_cap_field ds_native_cap_fields[] = {
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
static unsigned ds_native_cap_scope(unsigned topic, unsigned cap) {
    return topic == AZ_STATUS_LOAD || (topic == AZ_STATUS_METADATA && cap != AZN_CAP_DURATION) ||
                   (topic == AZ_STATUS_TRANSPORT && cap == AZN_CAP_PLAY_RAW) || cap == AZN_CAP_SLIDER_LOCK ||
                   topic == AZ_STATUS_SYNC || topic == AZ_STATUS_SETTINGS
               ? AZ_STATUS_UNIT
               : AZ_STATUS_LOAD_SCOPE;
}
static int ds_native_cap_preserve(const az_status_field_info_v1 *old, unsigned caps, unsigned cap) {
    return old->scope && (old->info.provenance == AZ_PROV_FILE || ((caps & cap) && old->info.availability != AZ_AVAIL_UNSUPPORTED));
}
static unsigned ds_native_cap_updates(unsigned deck, unsigned caps, unsigned changed, struct ds_field *updates, uint64_t *epoch) {
    unsigned count = 0;
    struct ds_field prior[sizeof ds_native_cap_fields / sizeof ds_native_cap_fields[0]];
    for (size_t i = 0; i < sizeof prior / sizeof prior[0]; ++i)
        prior[i] = (struct ds_field) {.topic = ds_native_cap_fields[i].topic, .field = ds_native_cap_fields[i].field};
    ds_read_receipts(deck, prior, sizeof prior / sizeof prior[0], epoch);
    for (size_t i = 0; i < sizeof ds_native_cap_fields / sizeof ds_native_cap_fields[0]; ++i) {
        const struct ds_native_cap_field *field = &ds_native_cap_fields[i];
        if (!(changed & field->cap)) continue;
        if (ds_native_cap_preserve(&prior[i].info, caps, field->cap)) continue;
        updates[count++] = (struct ds_field) {.topic = field->topic, .field = field->field, .info = {.scope = ds_native_cap_scope(field->topic, field->cap), .info = {.provenance = AZ_PROV_NATIVE, .availability = (caps & field->cap) ? AZ_AVAIL_LOADING : AZ_AVAIL_UNSUPPORTED}}};
    }
    return count;
}
static void ds_native_cap_reseed(unsigned deck, uint64_t epoch, unsigned caps, unsigned changed) {
    if (!epoch) return;
    unsigned dropped = changed & ~caps;
    if (dropped & AZN_CAP_ORIGINAL_BPM) sdk_status_store_reseed(deck, epoch, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO));
    if (dropped & (AZN_CAP_LTI | AZN_CAP_DURATION)) sdk_status_store_reseed(deck, epoch, AZ_STATUS_TOPIC(AZ_STATUS_METADATA));
}
static void ds_native_ready(unsigned deck, const struct azstatus_native_v1 *api) {
    if (api->size < offsetof(struct azstatus_native_v1, capabilities) + sizeof api->capabilities || !api->capabilities) return;
    unsigned caps = api->capabilities(deck), changed = caps ^ ds_native_caps[deck];
    if (!changed) return;
    struct ds_field updates[sizeof ds_native_cap_fields / sizeof ds_native_cap_fields[0]];
    uint64_t epoch;
    unsigned count = ds_native_cap_updates(deck, caps, changed, updates, &epoch);
    ds_native_caps[deck] = caps;
    if (count && ds_native_stamp(deck, updates, count)) {
        ds_publish(deck, epoch, 0, ds_now(), updates, count, NULL, NULL, 0, 0);
        ds_native_cap_reseed(deck, epoch, caps, changed);
    }
}
/* Only combine already queued measurements inside one qualified phrase. A
 * change of direction, any other receipt, or a half-open boundary ends the
 * run. The reducer revalidates the interval under its commit lock. */
static int ds_native_leg_header(const struct azstatus_native_receipt *r, unsigned deck) {
    return r->size == sizeof *r && r->version == 1 && r->deck == deck && r->kind == AZN_MOTION;
}
static int ds_native_leg_plain(const struct azstatus_native_receipt *r) {
    return r->a == AZN_MEASURE && !r->b && !r->c && !r->valid && !r->lost_topics && !r->history_only && !r->occurrence_ordinal && !r->reloop && !r->motion_flags;
}
static int ds_native_leg_source(const struct azstatus_native_receipt *r) {
    return r->observed_ns && r->motion_generation && r->motion_order && r->unit && r->epoch && r->load_generation;
}
static int ds_native_leg_map(const struct azstatus_native_receipt *r) {
    return r->map.owner && !r->map.changed && !r->map.grid.content && !r->map.phrases.content &&
           r->map.grid.revision && r->map.phrases.revision && r->map.grid.provider && r->map.phrases.provider;
}
static int ds_native_leg_clock(const struct azstatus_native_receipt *r) {
    const struct azn_point *p = r->points;
    return p[0].state == AZN_COORD && p[1].state == AZN_COORD && p[0].rate && p[0].rate == p[1].rate &&
           r->observed_window_samples && r->observed_window_rate && r->from_window_samples && r->from_window_rate;
}
static int ds_native_leg(const struct azstatus_native_receipt *r, unsigned d) {
    if (!ds_native_leg_header(r, d) || !ds_native_leg_plain(r) || !ds_native_leg_source(r) || !ds_native_leg_map(r) || !ds_native_leg_clock(r)) return 0;
    int order = compare_timestamps(r->points[1].samples, r->points[0].samples, r->points[0].rate, r->observed_window_rate, r->observed_window_samples);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_AFTER ? 1 : order == AZ_TIME_BEFORE ? -1
                                                                : 0;
}
static int ds_native_leg_follows(const struct azstatus_native_receipt *next, const struct azstatus_native_receipt *first,
                                 const struct azstatus_native_receipt *prev, unsigned d, int direction) {
    return ds_native_leg(next, d) == direction && next->unit == first->unit && next->epoch == first->epoch &&
           next->load_generation == first->load_generation && next->motion_generation == first->motion_generation &&
           next->motion_order == prev->motion_order + 1 && !memcmp(&next->map, &first->map, sizeof first->map) && next->points[0].rate == prev->points[1].rate;
}
static int ds_native_leg_adjacent(const struct azstatus_native_receipt *next, const struct azstatus_native_receipt *prev) {
    int adjacent = compare_timestamps(next->points[0].samples, prev->points[1].samples, prev->points[1].rate,
                                      prev->observed_window_rate, prev->observed_window_samples);
    return adjacent != AZ_TIME_INVALID && adjacent == AZ_TIME_EQUAL;
}
static int ds_native_leg_budget(const struct azstatus_native_receipt *next, const struct azstatus_native_receipt *first,
                                const struct azstatus_native_receipt *prev) {
    /* Capture timestamps share CLOCK_MONOTONIC's nanosecond clock.
     * The drain budget is 16 ms, checked at its one-nanosecond tick. */
    int order = compare_timestamps(next->observed_ns, prev->observed_ns, 1000000000, 1000000000, 1);
    if (order == AZ_TIME_INVALID || order == AZ_TIME_BEFORE) return 0;
    int elapsed = compare_timestamps(next->observed_ns - first->observed_ns, UINT64_C(16000000), 1000000000, 1000000000, 1);
    return elapsed != AZ_TIME_INVALID && elapsed != AZ_TIME_AFTER;
}
static unsigned ds_native_interior_rows(const struct azstatus_native_receipt *rows, unsigned count, unsigned d, int direction,
                                        const struct ds_phrase_span *span, struct ds_path *path) {
    unsigned n = 1;
    for (; n < count; ++n) {
        const struct azstatus_native_receipt *next = &rows[n], *prev = &rows[n - 1];
        if (!ds_native_leg_follows(next, rows, prev, d, direction) || !ds_native_leg_adjacent(next, prev) || !ds_native_leg_budget(next, rows, prev)) break;
        ds_native_coord(&next->points[1], &path->to);
        path->observed_window_samples = next->observed_window_samples;
        path->observed_window_rate = next->observed_window_rate;
        if (!ds_path_inside_span(span, path)) break;
    }
    return n;
}
static unsigned ds_native_interior_cut(const struct azstatus_native_receipt *rows, unsigned count, unsigned d, int direction, struct ds_path *path) {
    struct ds_phrase_span span;
    if (!ds_read_phrase_span(d, rows->load_generation, path, &span)) return 1;
    return ds_native_interior_rows(rows, count, d, direction, &span, path);
}
static unsigned ds_native_interior(const struct azstatus_native_receipt *rows, unsigned count, unsigned d) {
    if (count < 2) return 1;
    int direction = ds_native_leg(rows, d);
    if (!direction || rows->epoch != ds_native_fence[d] || rows->load_generation != ds_native_motion_load[d] || rows->motion_order != ds_native_motion_order[d] + 1) return 1;
    struct ds_path path = {0};
    ds_native_windows(&path, rows);
    ds_native_map_stamp(&path, rows);
    ds_native_coord(&rows->points[0], &path.from);
    ds_native_coord(&rows->points[1], &path.to);
    return ds_native_interior_cut(rows, count, d, direction, &path);
}
static size_t ds_native_receipt_minimum(unsigned kind) {
    switch (kind) {
    case AZN_METADATA:
    case AZN_HOT_CUES: return offsetof(struct azstatus_native_receipt, motion_order);
    case AZN_MOTION: return offsetof(struct azstatus_native_receipt, revision);
    case AZN_MEMORY: return offsetof(struct azstatus_native_receipt, memory_serial) + sizeof(((struct azstatus_native_receipt *) 0)->memory_serial);
    default: return offsetof(struct azstatus_native_receipt, content_id);
    }
}
static void ds_native_load_reseed(const struct azstatus_native_v1 *api, unsigned d) {
    struct azstatus_native_receipt seed = {.size = sizeof seed, .version = 1};
    if (api->load_seed(d, &seed)) ds_native_apply(&seed);
    else if (ds_native_epoch[d]) {
        ds_publish(d, 0, DS_BOUNDARY, 0, NULL, 0, NULL, NULL, 0, 0);
        ds_native_epoch[d] = 0;
    }
}
static void ds_native_stems_reseed(const struct azstatus_native_v1 *api, unsigned d, uint64_t lost) {
    if (!(lost & AZ_STATUS_TOPIC(AZ_STATUS_STEMS)) || api->size < offsetof(struct azstatus_native_v1, stems_seed) + sizeof api->stems_seed || !api->stems_seed) return;
    struct azstatus_native_receipt seed = {.size = sizeof seed, .version = 1};
    if (api->stems_seed(d, &seed)) ds_native_apply(&seed);
}
static void ds_native_metadata_reseed(const struct azstatus_native_v1 *api, unsigned d, uint64_t lost) {
    if (!(lost & AZ_STATUS_TOPIC(AZ_STATUS_METADATA)) || api->size < offsetof(struct azstatus_native_v1, metadata_seed) + sizeof api->metadata_seed || !api->metadata_seed) return;
    struct azstatus_native_receipt seed = {.size = sizeof seed, .version = 1};
    if (api->metadata_seed(d, &seed)) ds_native_apply(&seed);
}
static void ds_native_consume(const struct azstatus_native_v1 *api, unsigned d, const struct azstatus_native_receipt *r) {
    if (r->version != 1 || r->deck != d || r->size < ds_native_receipt_minimum(r->kind)) return;
    ds_native_apply(r);
    if (r->kind != AZN_GAP) return;
    if (r->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_LOAD)) ds_native_load_reseed(api, d);
    ds_native_stems_reseed(api, d, r->lost_topics);
    ds_native_metadata_reseed(api, d, r->lost_topics);
}
static int ds_native_consume_span(const struct azstatus_native_receipt *rows, unsigned count, unsigned deck) {
    struct azstatus_native_receipt last = rows[count - 1];
    last.points[0] = rows[0].points[0];
    last.from_window_samples = rows[0].from_window_samples;
    last.from_window_rate = rows[0].from_window_rate;
    if (ds_native_motion(&last, rows[0].motion_order) != AZ_OK) return 0;
    if (ds_pending_head[deck].count) ds_flush_heads(deck);
    return 1;
}
static void ds_native_consume_rows(const struct azstatus_native_v1 *api, unsigned d, const struct azstatus_native_receipt *rows, unsigned count) {
    for (unsigned i = 0; i < count;) {
        unsigned n = ds_native_interior(rows + i, count - i, d);
        if (n > 1 && ds_native_consume_span(rows + i, n, d)) i += n;
        else ds_native_consume(api, d, &rows[i++]);
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
    ds_native_consume_rows(api, d, rows, count);
    if (hot) ds_native_consume(api, d, &packet.base);
    return count + hot;
}
static unsigned ds_native_drain_decks(const struct azstatus_native_v1 *api) {
    unsigned progress = 0;
    pthread_mutex_lock(&ds_native_reduce_mu);
    for (unsigned d = 0; d < 4; ++d) {
        unsigned n = ds_native_drain(api, d);
        progress += n;
        if (!n) ds_file_loop_refresh(d);
        ds_native_ready(d, api);
    }
    pthread_mutex_unlock(&ds_native_reduce_mu);
    return progress;
}
static int ds_native_wake_read(int fd) {
    uint64_t counter;
    if (read(fd, &counter, sizeof counter) >= 0) return 1;
    return errno == EAGAIN || errno == EINTR;
}
static void *ds_native_run(void *arg) {
    const struct azstatus_native_v1 *api = arg;
    struct pollfd p = {.fd = api->wake_fd(), .events = POLLIN};
    for (;;) {
        int result = poll(&p, 1, -1);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || !(p.revents & POLLIN)) break;
        if (!ds_native_wake_read(p.fd)) break;
        while (ds_native_drain_decks(api)) {}
    }
    return NULL;
}

static int ds_native_service_valid(const struct azstatus_native_v1 *api) {
    return api && api->version == 1 && api->size >= offsetof(struct azstatus_native_v1, interest) + sizeof api->interest &&
           api->wake_fd && api->take && api->load_seed && api->interest && api->wake_fd() >= 0;
}
static void ds_native_install_writers(const struct azstatus_native_v1 *api) {
    if (api->size >= offsetof(struct azstatus_native_v1, register_source_capture) + sizeof api->register_source_capture && api->register_source_capture)
        (void) api->register_source_capture(azjs_capture);
    if (api->size >= offsetof(struct azstatus_native_v1, install_settings) + sizeof api->install_settings && api->install_settings)
        (void) api->install_settings(g_ep147_build, azm_install_island_hook_at);
    if (api->size >= offsetof(struct azstatus_native_v1, install_terminal) + sizeof api->install_terminal && api->install_terminal)
        (void) api->install_terminal(g_ep147_build, azm_install_island_hook_at);
}
static void ds_native_install_analysis(const struct azstatus_native_v1 *api) {
    if (api->size < offsetof(struct azstatus_native_v1, analysis_source) + sizeof api->analysis_source || !api->analysis_source) return;
    const struct azn_analysis_source_api *analysis = api->analysis_source();
    if (analysis && analysis->version == 1 && analysis->size >= sizeof *analysis && analysis->install)
        (void) analysis->install(g_ep147_build, azm_install_island_hook_at);
}
static int ds_native_file_api_valid(const struct azn_file_cue_api *file) {
    return file && file->version == 1 && file->size >= offsetof(struct azn_file_cue_api, observe_input) + sizeof file->observe_input && file->install && file->transport;
}
static void ds_native_install_file_maps(const struct azn_file_cue_api *file) {
    if (azfile_map_callbacks && file->size >= offsetof(struct azn_file_cue_api, install_maps) + sizeof file->install_maps && file->install_maps)
        (void) file->install_maps(g_ep147_build, azm_install_island_hook_at, azfile_map_callbacks());
}
static void ds_native_install_files(const struct azstatus_native_v1 *api) {
    if (!azfile_status_callbacks || !azfile_status_ref || api->size < offsetof(struct azstatus_native_v1, file_cues) + sizeof api->file_cues || !api->file_cues) return;
    const struct azn_file_cue_api *file = api->file_cues();
    if (!ds_native_file_api_valid(file) || !file->install(g_ep147_build, azm_install_island_hook_at, azfile_status_callbacks())) return;
    atomic_store_explicit(&ds_file_cues, file, memory_order_release);
    ds_native_install_file_maps(file);
}
static void ds_native_connect(const struct azstatus_native_v1 *api, pthread_t thread) {
    pthread_detach(thread);
    const struct azn_memory_api *memory = ds_memory_api();
    if (memory && memory->connect) memory->connect();
    if (api->size >= offsetof(struct azstatus_native_v1, bind_stems) + sizeof api->bind_stems) ds_set_stem_binding(api->bind_stems);
    for (unsigned d = 0; d < 4; ++d) api->interest(d, atomic_load_explicit(&ds_native_topics[d], memory_order_acquire));
}
static void ds_native_launch(const struct azstatus_native_v1 *api) {
    pthread_t thread;
    atomic_store_explicit(&ds_native_api, api, memory_order_release);
    if (!az_thread_create(&thread, NULL, ds_native_run, (void *) api)) ds_native_connect(api, thread);
    else atomic_store_explicit(&ds_native_api, NULL, memory_order_release);
}
static void ds_native_start(void) {
    pthread_mutex_lock(&ds_native_attach_mu);
    if (!atomic_load_explicit(&ds_native_api, memory_order_acquire)) {
        azstatus_native_fn get = (azstatus_native_fn) dlsym(RTLD_DEFAULT, "azstem_status_service_v1");
        const struct azstatus_native_v1 *api = get ? get() : NULL;
        if (ds_native_service_valid(api)) {
            ds_native_install_writers(api);
            ds_native_install_analysis(api);
            ds_native_install_files(api);
            ds_native_launch(api);
        }
    }
    pthread_mutex_unlock(&ds_native_attach_mu);
}
static void ds_native_interest(unsigned deck, uint64_t topics) {
    if (deck >= 4) return;
    atomic_store_explicit(&ds_native_topics[deck], topics, memory_order_release);
    ds_watch_stems(deck, (topics & AZ_STATUS_TOPIC(AZ_STATUS_STEMS)) != 0);
    const struct azstatus_native_v1 *api = atomic_load_explicit(&ds_native_api, memory_order_acquire);
    if (api) {
        api->interest(deck, topics);
        pthread_mutex_lock(&ds_native_reduce_mu);
        ds_native_ready(deck, api);
        pthread_mutex_unlock(&ds_native_reduce_mu);
    }
}
