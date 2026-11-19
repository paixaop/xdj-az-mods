/* SPDX-License-Identifier: GPL-3.0-only */
/* PageBuffer::Track::setTotalLength commits source_time::Sample at +0x30.
 * Its native page-filled callback owns the row. reset(bool) invalidates the
 * TrackID before releasing buffered pages. Never retain a borrowed row or
 * reader: the deck join below is checked within each owned writer frame. */
static const uint64_t duration_routes = AZ_STATUS_TOPIC(AZ_STATUS_METADATA) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION);
struct DurationValue {
    _Atomic(uint64_t) serial;
    _Atomic(uint64_t) unit;
    _Atomic(uint64_t) epoch;
    _Atomic(uint64_t) load;
    _Atomic(uintptr_t) buffer;
    _Atomic(int64_t) samples;
    _Atomic(uint32_t) rate;
    _Atomic(int) valid;
};
struct DurationState {
    atomic_flag busy;
    _Atomic(unsigned) refresh;
    _Atomic(unsigned) retry;
    _Atomic(uint64_t) bound_load;
    _Atomic(uint64_t) bound_unit;
    _Atomic(uint64_t) bound_epoch;
    _Atomic(uintptr_t) bound_buffer;
    uint64_t unit;
    uint64_t epoch;
    uint64_t load;
    uint64_t seed;
    uintptr_t buffer;
/* owner-qualified identity only;
never dereferenced */
    int64_t samples;
    uint32_t rate;
    int valid;
    DurationValue value;
};
static DurationState duration_state[4] = {[0 ... 3] = {.busy = ATOMIC_FLAG_INIT, .refresh = 0, .retry = 0, .bound_load = 0, .bound_unit = 0, .bound_epoch = 0, .bound_buffer = 0, .unit = 0, .epoch = 0, .load = 0, .seed = 0, .buffer = 0, .samples = 0, .rate = 0, .valid = -1, .value = {.valid = -1}}};
static _Atomic(bool) duration_enabled = false;
static _Atomic(uint64_t) duration_mutation = 0;
static _Atomic(unsigned) duration_writers = 0;
static uintptr_t duration_adapter_table, duration_buffer_table, duration_track_table;
static void *duration_original[4];
struct DurationRead {
    uintptr_t buffer;
    uintptr_t id;
    int64_t samples;
    bool observed;
};
static _Thread_local DurationRead *duration_read;
/* Copy at the concrete reader's native call, while its receiver is owned.
 * The caller only retains the returned scalars; it never calls this getter. */
static int64_t duration_get_length(uintptr_t buffer, uintptr_t id) {
    const bool owned = duration_read && duration_read->buffer == buffer && duration_read->id == id &&
                       word(buffer) == duration_buffer_table;
    int64_t samples = ((int64_t (*)(uintptr_t, uintptr_t)) trampoline_address(&duration_original[2]))(buffer, id);
    if (owned) {
        duration_read->samples = samples;
        duration_read->observed = true;
    }
    return samples;
}
static void duration_lost(unsigned d) {
    if (!atomic_exchange_explicit(&(duration_state[d].refresh), 1, memory_order_acq_rel)) azn_duration_gap(d);
}
static bool duration_length(int64_t samples, uint32_t rate) {
    if (!rate || rate >= 0x7fffffffU) return false;
    const int zero = compare_timestamps(samples, 0, rate, rate, 1);
    const int bound = compare_timestamps(samples, INT64_C(9007199254740992), rate, rate, 1);
    return zero != AZ_TIME_INVALID && zero != AZ_TIME_BEFORE &&
           bound != AZ_TIME_INVALID && bound != AZ_TIME_AFTER;
}
static uintptr_t duration_buffer(const Slot *slot, uintptr_t core) {
    /* Basis owns this adapter until the Unit gate drains; its mapped PCM
     * pointer is compared only as an identity, never dereferenced here. */
    const uintptr_t basis = word(slot->unit + 0x180);
    if (!pointer(basis) || word(basis + 0x30) != core) return 0;
    const uintptr_t reader = word(basis + 0x28);
    if (!pointer(reader) || word(reader) != duration_adapter_table || word(core + 0xa8) != reader) return 0;
    return word(reader + 8);
}
static bool duration_row(uintptr_t buffer, uintptr_t row) {
    const uintptr_t begin = word(buffer + 0x140), end = word(buffer + 0x148);
    /* Address bounds are not time comparisons. No vector traversal or getter. */
    return begin && end >= begin && row >= begin && row < end &&
           (row - begin) % 0xd8 == 0 && end - row >= 0xd8 && word(row) == duration_track_table;
}
/* busy (or drained retirement) serializes this copied certificate. */
static void duration_publish_value(DurationState *v) {
    __auto_type out = &(v->value);
    if (atomic_load_explicit(&(out->serial), memory_order_relaxed) >= UINT64_MAX - 2) {
        atomic_store_explicit(&(out->serial), UINT64_MAX, memory_order_release);
        return;
    }
    atomic_fetch_add_explicit(&(out->serial), 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&(out->unit), v->unit, memory_order_relaxed);
    atomic_store_explicit(&(out->epoch), v->epoch, memory_order_relaxed);
    atomic_store_explicit(&(out->load), v->load, memory_order_relaxed);
    atomic_store_explicit(&(out->buffer), v->buffer, memory_order_relaxed);
    atomic_store_explicit(&(out->samples), v->samples, memory_order_relaxed);
    atomic_store_explicit(&(out->rate), v->rate, memory_order_relaxed);
    atomic_store_explicit(&(out->valid), v->valid, memory_order_relaxed);
    atomic_fetch_add_explicit(&(out->serial), 1, memory_order_release);
}
static bool duration_value_identity(const DurationValue *value, const azsdk_load_sample load, uintptr_t buffer) {
    return atomic_load_explicit(&(value->unit), memory_order_relaxed) == load.unit_generation &&
           atomic_load_explicit(&(value->epoch), memory_order_relaxed) == load.invalidation_epoch &&
           atomic_load_explicit(&(value->load), memory_order_relaxed) == load.load_generation &&
           atomic_load_explicit(&(value->buffer), memory_order_relaxed) == buffer && atomic_load_explicit(&(value->valid), memory_order_relaxed) == 1;
}
static bool duration_repeat(const DurationState *v, const azsdk_load_sample load, uintptr_t buffer, int64_t samples, bool invalidate) {
    if (invalidate) return false;
    __auto_type value = &(v->value);
    const uint64_t serial = atomic_load_explicit(&(value->serial), memory_order_acquire);
    if (!serial || (serial & 1)) return false;
    const bool identity = duration_value_identity(value, load, buffer);
    const int64_t prior = atomic_load_explicit(&(value->samples), memory_order_relaxed);
    const uint32_t rate = atomic_load_explicit(&(value->rate), memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (!identity || atomic_load_explicit(&(value->serial), memory_order_acquire) != serial) return false;
    const int order = compare_timestamps(prior, samples, rate, rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
struct DurationCommit {
    azsdk_load_sample load;
    int64_t samples;
    uint32_t rate;
    bool valid, same;
    uint64_t seed;
    uintptr_t buffer;
};
static void duration_defer(unsigned d, DurationState *v, uint64_t snapshot) {
    /* A reader seed has no native mutation to lose. Its independent retry
     * cannot suppress a later changed/reset writer's capture gap. */
    if (snapshot != UINT64_MAX) atomic_store_explicit(&(v->retry), 1, memory_order_release);
    else duration_lost(d);
}
static bool duration_snapshot_current(uint64_t snapshot) {
    return snapshot == UINT64_MAX || (!atomic_load_explicit(&(duration_writers), memory_order_acquire) &&
                                      snapshot == atomic_load_explicit(&(duration_mutation), memory_order_acquire));
}
static bool duration_load_matches(unsigned d, const azsdk_load_sample load) {
    azsdk_load_sample current = {0};
    return azsdk_load_read((int) d, &current) && current.unit_generation == load.unit_generation &&
           current.load_generation == load.load_generation && current.invalidation_epoch == load.invalidation_epoch;
}
static void duration_publish_binding(DurationState *v, const azsdk_load_sample load, uintptr_t buffer) {
    /* Keep membership readable during unrelated scalar publication. Replace
     * this certificate only at an owned source/lifetime change. */
    if (atomic_load_explicit(&(v->bound_load), memory_order_acquire) == load.load_generation &&
        atomic_load_explicit(&(v->bound_unit), memory_order_relaxed) == load.unit_generation &&
        atomic_load_explicit(&(v->bound_epoch), memory_order_relaxed) == load.invalidation_epoch &&
        atomic_load_explicit(&(v->bound_buffer), memory_order_relaxed) == buffer) return;
    atomic_store_explicit(&(v->bound_load), 0, memory_order_release);
    atomic_store_explicit(&(v->bound_unit), load.unit_generation, memory_order_relaxed);
    atomic_store_explicit(&(v->bound_epoch), load.invalidation_epoch, memory_order_relaxed);
    atomic_store_explicit(&(v->bound_buffer), buffer, memory_order_relaxed);
    if (buffer) atomic_store_explicit(&(v->bound_load), load.load_generation, memory_order_release);
}
static bool duration_same_samples(const DurationState *v, const struct DurationCommit *value) {
    if (!value->valid) return true;
    if (v->rate != value->rate) return false;
    const int order = compare_timestamps(v->samples, value->samples, value->rate, value->rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static bool duration_same_value(const DurationState *v, const struct DurationCommit *value) {
    return v->unit == value->load.unit_generation && v->load == value->load.load_generation && v->epoch == value->load.invalidation_epoch &&
           v->buffer == value->buffer && v->valid == (int) value->valid && duration_same_samples(v, value);
}
static bool duration_redundant(const DurationState *v, const struct DurationCommit *value) {
    return value->same && v->seed == value->seed && !atomic_load_explicit(&(v->refresh), memory_order_acquire) &&
           !atomic_load_explicit(&(v->retry), memory_order_acquire);
}
static void duration_publish(unsigned d, DurationState *v, const struct DurationCommit *value) {
    azstatus_native_receipt receipt = {0};
    receipt.kind = AZN_DURATION;
    receipt.deck = d;
    receipt.unit = value->load.unit_generation;
    receipt.epoch = value->load.invalidation_epoch;
    receipt.load_generation = value->load.load_generation;
    receipt.valid = value->valid;
    receipt.points[0] = (azn_point) {value->valid ? AZN_COORD : AZN_INVALID, value->rate, value->samples};
    v->unit = receipt.unit;
    v->epoch = receipt.epoch;
    v->load = receipt.load_generation;
    v->buffer = value->buffer;
    v->samples = value->samples;
    v->rate = value->rate;
    v->valid = value->valid;
    v->seed = value->seed;
    /* Replaying an identical value for a new seed or retry must leave the
     * sealed certificate readable to a concurrent page-filled callback. */
    if (!value->same) duration_publish_value(v);
    atomic_store_explicit(&(v->retry), 0, memory_order_release);
    atomic_store_explicit(&(v->refresh), 0, memory_order_release);
    if (!azn_offer(&receipt)) duration_lost(d);
}
static bool duration_capture_current(unsigned d, const azsdk_load_sample load, uint64_t snapshot) {
    return duration_snapshot_current(snapshot) && load.unit_generation && load.load_generation && duration_load_matches(d, load);
}
static void duration_commit(unsigned d, const azsdk_load_sample load, int64_t samples, uint32_t rate, bool valid, uint64_t snapshot, uintptr_t buffer) {
    if (!atomic_load_explicit(&(duration_enabled), memory_order_acquire) || !(azn_interest(d) & duration_routes)) return;
    DurationState *v = &duration_state[d];
    if (atomic_flag_test_and_set_explicit(&(v->busy), memory_order_acquire)) {
        duration_defer(d, v, snapshot);
        return;
    }
    EXIT_FLAG_GUARD(unlock, &v->busy);
    if (!duration_capture_current(d, load, snapshot)) return;
    struct DurationCommit value = {.load = load, .samples = samples, .rate = rate, .valid = valid && duration_length(samples, rate), .buffer = buffer};
    duration_publish_binding(v, load, buffer);
    value.seed = azn_seed_id(d);
    value.same = duration_same_value(v, &value);
    azn_capability(d, AZN_CAP_DURATION, 1);
    if (!duration_redundant(v, &value)) duration_publish(d, v, &value);
}
enum DurationBinding {
    DURATION_UNQUALIFIED,
    DURATION_OTHER,
    DURATION_SELECTED,
    DURATION_UNCHANGED
};
static bool duration_bound_current(const DurationState *v, const azsdk_load_sample load, uint64_t bound_load, uintptr_t bound_buffer) {
    return bound_buffer && bound_load == load.load_generation &&
           atomic_load_explicit(&(v->bound_unit), memory_order_relaxed) == load.unit_generation &&
           atomic_load_explicit(&(v->bound_epoch), memory_order_relaxed) == load.invalidation_epoch &&
           atomic_load_explicit(&(v->bound_load), memory_order_acquire) == bound_load;
}
static enum DurationBinding duration_writer_binding(unsigned d, const azsdk_load_sample load, uintptr_t parent, const unsigned char identity[12], int64_t samples, bool invalidate) {
    __auto_type v = &(duration_state[d]);
    const uint64_t bound_load = atomic_load_explicit(&(v->bound_load), memory_order_acquire);
    const uintptr_t bound_buffer = atomic_load_explicit(&(v->bound_buffer), memory_order_relaxed);
    if (!duration_bound_current(v, load, bound_load, bound_buffer) || !duration_load_matches(d, load) ||
        !azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, identity)) return DURATION_UNQUALIFIED;
    if (bound_buffer != parent) return DURATION_OTHER;
    if (duration_repeat(v, load, parent, samples, invalidate)) return DURATION_UNCHANGED;
    /* No Unit lease is held here. Retirement can invalidate the scalar
     * cache after its owner gate drains, so only the atomic certificate is
     * readable on this path. uintptr_t missed cut remains selected and uncertain. */
    return DURATION_SELECTED;
}
/* The native row is owned by the writer; the owner lease protects only the
 * separate core/adapter join. Copy its TrackID before reset can erase it. */
static void duration_writer_owned(unsigned d, const azsdk_load_sample load, uintptr_t parent,
                                  const unsigned char identity[12], int64_t samples, bool invalidate, enum DurationBinding binding) {
    __auto_type slot = &(state().decks[d]);
    AZ_OWNER_LEASE(lease);
    if (!az_owner_held(&slot->gate) && az_owner_enter(&slot->gate, &lease) != AZ_OK) {
        if (binding == DURATION_SELECTED) duration_lost(d);
        else atomic_store_explicit(&(duration_state[d].retry), 1, memory_order_release);
        return;
    }
    const uintptr_t core = atomic_load_explicit(&(slot->core), memory_order_acquire);
    if (!core) return;
    const uintptr_t buffer = duration_buffer(slot, core);
    if (buffer != parent || memcmp(identity, ((const void *) (core + 0x18)), 12)) return;
    const uint32_t rate = (uint32_t) (integer(core + 0x68));
    duration_commit(d, load, samples, rate, !invalidate, UINT64_MAX, parent);
}
static void duration_writer_deck(unsigned d, uintptr_t parent, const unsigned char identity[12], int64_t samples, bool invalidate) {
    if (!(azn_interest(d) & duration_routes)) return;
    azsdk_load_sample load = {0};
    if (!azsdk_load_read((int) (d), &load) || !load.load_generation ||
        !azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, identity)) return;
    const enum DurationBinding binding = duration_writer_binding(d, load, parent, identity, samples, invalidate);
    /* Page-filled callbacks repeat the already committed total length.
     * They do not need a new Unit reservation or a recovery publication.
     * uintptr_t different owned buffer is not this deck's duration stream. */
    if (binding == DURATION_OTHER || binding == DURATION_UNCHANGED) return;
    duration_writer_owned(d, load, parent, identity, samples, invalidate, binding);
}
static void duration_writer(uintptr_t row, bool invalidate) {
    if (word(row) != duration_track_table) return;
    /* Track's native constructor receives PageBuffer's embedded config at
     * parent+8. The setter/reset itself uses this reference, so parent is
     * live in this frame independently of any cached mapped reader. */
    const uintptr_t config = word(row + 8);
    if (!pointer(config)) return;
    const uintptr_t parent = config - 8;
    if (word(parent) != duration_buffer_table || !duration_row(parent, row)) return;
    unsigned char identity[12];
    memcpy(identity, ((const void *) (row + 0x20)), sizeof identity);
    const int64_t samples = ((int64_t) (word(row + 0x30)));
    for (unsigned d = 0; d < 4; ++d) duration_writer_deck(d, parent, identity, samples, invalidate);
}
typedef struct DurationMutation DurationMutation;
struct DurationMutation {
    unsigned char reserved;
};
static void duration_mutation_scope_init(DurationMutation *ctx);
static void duration_mutation_scope_drop(DurationMutation *ctx);
static void duration_mutation_scope_init(DurationMutation *ctx) {
    (void) ctx;
    *ctx = (DurationMutation) {.reserved = 0};

    atomic_fetch_add_explicit(&(duration_writers), 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&(duration_mutation), 1, memory_order_acq_rel);
}
static void duration_mutation_scope_drop(DurationMutation *ctx) {
    (void) ctx;

    atomic_fetch_add_explicit(&(duration_mutation), 1, memory_order_release);
    atomic_fetch_sub_explicit(&(duration_writers), 1, memory_order_release);
}

static void duration_set(uintptr_t row, int64_t samples) {
    DurationMutation mutation __attribute__((cleanup(duration_mutation_scope_drop)));
    duration_mutation_scope_init(&mutation);
    ((void (*)(uintptr_t, int64_t)) trampoline_address(&duration_original[0]))(row, samples);
    if (atomic_load_explicit(&(duration_enabled), memory_order_acquire)) duration_writer(row, false);
}
static uintptr_t duration_reset(uintptr_t row, unsigned reset) {
    DurationMutation mutation __attribute__((cleanup(duration_mutation_scope_drop)));
    duration_mutation_scope_init(&mutation);
    if (atomic_load_explicit(&(duration_enabled), memory_order_acquire)) duration_writer(row, true);
    return ((uintptr_t (*)(uintptr_t, unsigned)) trampoline_address(&duration_original[1]))(row, reset);
}
/* Both PageBuffer destructor variants destroy their row vector through this
 * non-deleting Track destructor before freeing the parent/config. */
static void duration_destroy(uintptr_t row) {
    DurationMutation mutation __attribute__((cleanup(duration_mutation_scope_drop)));
    duration_mutation_scope_init(&mutation);
    if (atomic_load_explicit(&(duration_enabled), memory_order_acquire)) duration_writer(row, true);
    ((void (*)(uintptr_t)) trampoline_address(&duration_original[3]))(row);
}
static void duration_retire(unsigned d) {
    /* Called after the owner gate drains all producer/seed frames. */
    __auto_type v = &(duration_state[d]);
    atomic_store_explicit(&(v->bound_load), 0, memory_order_release);
    v->unit = v->epoch = v->load = v->seed = 0;
    v->buffer = 0;
    v->valid = -1;
    duration_publish_value(v);
    azn_capability(d, AZN_CAP_DURATION, 0);
    atomic_store_explicit(&(v->refresh), 0, memory_order_release);
    atomic_store_explicit(&(v->retry), 0, memory_order_release);
}
static bool duration_entries_match(const State *s, int build, const uintptr_t *profile) {
    static const uint32_t entries[][4] = {
        {0xa9b97bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4},
        {0xa9b47bfd, 0x910003fd, 0xa90363f7, 0xaa0003f7},
        {0xa9be7bfd, 0x910003fd, 0xa90153f3, 0xaa0103f3}};
    for (unsigned i = 0; i < 3; ++i)
        if (!duration_original[i] && memcmp(((const void *) (s->bias + profile[i])), entries[i], sizeof entries[i])) return false;
    static const uint32_t destroy_entries[][4] = {
        {0xa9be7bfd, 0xf000f481, 0x913a4021, 0x910003fd},
        {0xa9be7bfd, 0xf0010c41, 0x9113a021, 0x910003fd}};
    if (!duration_original[3] && memcmp(((const void *) (s->bias + profile[3])),
                                        destroy_entries[build - 1], sizeof destroy_entries[0])) return false;
    return true;
}
static void duration_install(State *s, int build, azn_setting_patch_fn patch) {
    if (atomic_load_explicit(&(duration_enabled), memory_order_acquire)) return;
    static const uintptr_t sites[][7] = {
        {0x778660, 0x773fc0, 0x770eb8, 0x7668a8, 0x27b82e8, 0x25f9c00, 0x25f9e90},
        {0x7b9f60, 0x7b58c0, 0x7b27b8, 0x7a8128, 0x2b04fa0, 0x2933258, 0x29334e8}};
    const uintptr_t *profile = sites[build - 1];
    if (!duration_entries_match(s, build, profile)) return;
    void *hooks[] = {((void *) (duration_set)), ((void *) (duration_reset)),
                     ((void *) (duration_get_length)), ((void *) (duration_destroy))};
    for (unsigned i = 0; i < 4; ++i)
        if (!duration_original[i] && patch(s->bias + profile[i], hooks[i], &duration_original[i], "status-source-duration") != 1) return;
    duration_adapter_table = s->bias + profile[4];
    duration_buffer_table = s->bias + profile[5];
    duration_track_table = s->bias + profile[6];
    atomic_store_explicit(&(duration_enabled), true, memory_order_release);
}
