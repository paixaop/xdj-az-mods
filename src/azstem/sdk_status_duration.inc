/* SPDX-License-Identifier: GPL-3.0-only */
/* PageBuffer::Track::setTotalLength commits source_time::Sample at +0x30.
 * Its native page-filled callback owns the row. reset(bool) invalidates the
 * TrackID before releasing buffered pages. Never retain a borrowed row or
 * reader: the deck join below is checked within each owned writer frame. */
static constexpr uint64_t duration_routes = AZ_STATUS_TOPIC(AZ_STATUS_METADATA) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION);
struct DurationState {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<unsigned> refresh {0}, retry {0};
    std::atomic<uint64_t> bound_load {0}, bound_unit {0}, bound_epoch {0};
    std::atomic<uintptr_t> bound_buffer {0};
    uint64_t unit = 0, epoch = 0, load = 0, seed = 0;
    uintptr_t buffer = 0; /* owner-qualified identity only; never dereferenced */
    int64_t samples = 0;
    uint32_t rate = 0;
    int valid = -1;
    /* Scalar certificate readable while an unrelated snapshot holds busy.
     * All fields are atomic: a failed sequence cut never races the writer. */
    struct Value {
        std::atomic<uint64_t> serial {0}, unit {0}, epoch {0}, load {0};
        std::atomic<uintptr_t> buffer {0};
        std::atomic<int64_t> samples {0};
        std::atomic<uint32_t> rate {0};
        std::atomic<int> valid {-1};
    } value;
};
static DurationState duration_state[4];
static std::atomic<bool> duration_enabled {false};
static std::atomic<uint64_t> duration_mutation {0};
static std::atomic<unsigned> duration_writers {0};
static uintptr_t duration_adapter_table, duration_buffer_table, duration_track_table;
static void *duration_original[4];
struct DurationRead {
    uintptr_t buffer = 0, id = 0;
    int64_t samples = 0;
    bool observed = false;
};
static thread_local DurationRead *duration_read;
/* Copy at the concrete reader's native call, while its receiver is owned.
 * The caller only retains the returned scalars; it never calls this getter. */
static int64_t duration_get_length(uintptr_t buffer, uintptr_t id) {
    const bool owned = duration_read && duration_read->buffer == buffer && duration_read->id == id &&
                       word(buffer) == duration_buffer_table;
    int64_t samples = trampoline<int64_t (*)(uintptr_t, uintptr_t)>(&duration_original[2])(buffer, id);
    if (owned) {
        duration_read->samples = samples;
        duration_read->observed = true;
    }
    return samples;
}
static void duration_lost(unsigned d) {
    if (!duration_state[d].refresh.exchange(1, std::memory_order_acq_rel)) azn_duration_gap(d);
}
static bool duration_length(int64_t samples, uint32_t rate) {
    if (!rate || rate >= 0x7fffffffU) return false;
    const int zero = compare_timestamps(samples, 0, rate, rate, 1);
    const int bound = compare_timestamps(samples, INT64_C(9007199254740992), rate, rate, 1);
    return zero != AZ_TIME_INVALID && zero != AZ_TIME_BEFORE &&
           bound != AZ_TIME_INVALID && bound != AZ_TIME_AFTER;
}
static uintptr_t duration_buffer(const Slot &slot, uintptr_t core) {
    /* Basis owns this adapter until the Unit gate drains; its mapped PCM
     * pointer is compared only as an identity, never dereferenced here. */
    const uintptr_t basis = word(slot.unit + 0x180);
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
static void duration_publish_value(DurationState &v) {
    auto &out = v.value;
    if (out.serial.load(std::memory_order_relaxed) >= UINT64_MAX - 2) {
        out.serial.store(UINT64_MAX, std::memory_order_release);
        return;
    }
    out.serial.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    out.unit.store(v.unit, std::memory_order_relaxed);
    out.epoch.store(v.epoch, std::memory_order_relaxed);
    out.load.store(v.load, std::memory_order_relaxed);
    out.buffer.store(v.buffer, std::memory_order_relaxed);
    out.samples.store(v.samples, std::memory_order_relaxed);
    out.rate.store(v.rate, std::memory_order_relaxed);
    out.valid.store(v.valid, std::memory_order_relaxed);
    out.serial.fetch_add(1, std::memory_order_release);
}
static bool duration_repeat(const DurationState &v, const azsdk_load_sample &load,
                            uintptr_t buffer, int64_t samples, bool invalidate) {
    if (invalidate) return false;
    const auto &value = v.value;
    const uint64_t serial = value.serial.load(std::memory_order_acquire);
    if (!serial || (serial & 1)) return false;
    const bool identity = value.unit.load(std::memory_order_relaxed) == load.unit_generation &&
        value.epoch.load(std::memory_order_relaxed) == load.invalidation_epoch &&
        value.load.load(std::memory_order_relaxed) == load.load_generation &&
        value.buffer.load(std::memory_order_relaxed) == buffer && value.valid.load(std::memory_order_relaxed) == 1;
    const int64_t prior = value.samples.load(std::memory_order_relaxed);
    const uint32_t rate = value.rate.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (!identity || value.serial.load(std::memory_order_acquire) != serial) return false;
    const int order = compare_timestamps(prior, samples, rate, rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static void duration_commit(unsigned d, const azsdk_load_sample &load, int64_t samples, uint32_t rate, bool valid,
                            uint64_t snapshot = UINT64_MAX, uintptr_t buffer = 0) {
    if (!duration_enabled.load(std::memory_order_acquire) || !(azn_interest(d) & duration_routes)) return;
    auto &v = duration_state[d];
    if (v.busy.test_and_set(std::memory_order_acquire)) {
        /* A reader seed has no native mutation to lose. Its independent
         * retry cannot suppress a later changed/reset writer's capture gap. */
        if (snapshot != UINT64_MAX) v.retry.store(1, std::memory_order_release);
        else duration_lost(d);
        return;
    }
    struct Unlock { DurationState &v; ~Unlock() { v.busy.clear(std::memory_order_release); } } unlock {v};
    if (snapshot != UINT64_MAX && (duration_writers.load(std::memory_order_acquire) ||
        snapshot != duration_mutation.load(std::memory_order_acquire))) return;
    azsdk_load_sample current {};
    if (!load.unit_generation || !load.load_generation || !azsdk_load_read(int(d), &current) ||
        current.unit_generation != load.unit_generation || current.load_generation != load.load_generation ||
        current.invalidation_epoch != load.invalidation_epoch) return;
    valid = valid && duration_length(samples, rate);
    /* Keep source membership readable while an unrelated scalar publication
     * holds busy. A changed writer must still identify its selected stream.
     * Replace this certificate only at an owned source/lifetime change. */
    if (v.bound_load.load(std::memory_order_acquire) != load.load_generation ||
        v.bound_unit.load(std::memory_order_relaxed) != load.unit_generation ||
        v.bound_epoch.load(std::memory_order_relaxed) != load.invalidation_epoch ||
        v.bound_buffer.load(std::memory_order_relaxed) != buffer) {
        v.bound_load.store(0, std::memory_order_release);
        v.bound_unit.store(load.unit_generation, std::memory_order_relaxed);
        v.bound_epoch.store(load.invalidation_epoch, std::memory_order_relaxed);
        v.bound_buffer.store(buffer, std::memory_order_relaxed);
        if (buffer) v.bound_load.store(load.load_generation, std::memory_order_release);
    }
    const uint64_t seed = azn_seed_id(d);
    const int order = valid && v.rate == rate ? compare_timestamps(v.samples, samples, rate, rate, 1) : AZ_TIME_INVALID;
    const bool same = v.unit == load.unit_generation && v.load == load.load_generation && v.epoch == load.invalidation_epoch &&
                      v.buffer == buffer &&
                      v.valid == int(valid) && (!valid || (order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL));
    azn_capability(d, AZN_CAP_DURATION, 1);
    if (same && v.seed == seed && !v.refresh.load(std::memory_order_acquire) && !v.retry.load(std::memory_order_acquire)) return;
    azstatus_native_receipt r {};
    r.kind = AZN_DURATION; r.deck = d; r.unit = load.unit_generation;
    r.epoch = load.invalidation_epoch; r.load_generation = load.load_generation;
    r.valid = valid;
    r.points[0] = {valid ? AZN_COORD : AZN_INVALID, rate, samples};
    v.unit = r.unit; v.epoch = r.epoch; v.load = r.load_generation;
    v.buffer = buffer;
    v.samples = samples; v.rate = rate; v.valid = valid; v.seed = seed;
    /* Replaying an identical value for a new seed or retry must leave the
     * sealed certificate readable to a concurrent page-filled callback. */
    if (!same) duration_publish_value(v);
    v.retry.store(0, std::memory_order_release);
    v.refresh.store(0, std::memory_order_release);
    if (!azn_offer(&r)) duration_lost(d);
}
enum DurationBinding { DURATION_UNQUALIFIED, DURATION_OTHER, DURATION_SELECTED, DURATION_UNCHANGED };
static DurationBinding duration_writer_binding(unsigned d, const azsdk_load_sample &load,
                                                uintptr_t parent, const unsigned char identity[12],
                                                int64_t samples, bool invalidate) {
    auto &v = duration_state[d];
    const uint64_t bound_load = v.bound_load.load(std::memory_order_acquire);
    const uintptr_t bound_buffer = v.bound_buffer.load(std::memory_order_relaxed);
    azsdk_load_sample current {};
    if (!bound_buffer || bound_load != load.load_generation ||
        v.bound_unit.load(std::memory_order_relaxed) != load.unit_generation ||
        v.bound_epoch.load(std::memory_order_relaxed) != load.invalidation_epoch ||
        v.bound_load.load(std::memory_order_acquire) != bound_load ||
        !azsdk_load_read(int(d), &current) || current.unit_generation != load.unit_generation ||
        current.load_generation != load.load_generation || current.invalidation_epoch != load.invalidation_epoch ||
        !azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, identity)) return DURATION_UNQUALIFIED;
    if (bound_buffer != parent) return DURATION_OTHER;
    if (duration_repeat(v, load, parent, samples, invalidate)) return DURATION_UNCHANGED;
    /* No Unit lease is held here. Retirement can invalidate the scalar
     * cache after its owner gate drains, so only the atomic certificate is
     * readable on this path. A missed cut remains selected and uncertain. */
    return DURATION_SELECTED;
}
/* The native row is owned by the writer; the owner lease protects only the
 * separate core/adapter join. Copy its TrackID before reset can erase it. */
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
    memcpy(identity, reinterpret_cast<const void *>(row + 0x20), sizeof identity);
    const int64_t samples = static_cast<int64_t>(word(row + 0x30));
    for (unsigned d = 0; d < 4; ++d) {
        if (!(azn_interest(d) & duration_routes)) continue;
        azsdk_load_sample load {};
        if (!azsdk_load_read(int(d), &load) || !load.load_generation ||
            !azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, identity)) continue;
        const DurationBinding binding = duration_writer_binding(d, load, parent, identity, samples, invalidate);
        /* Page-filled callbacks repeat the already committed total length.
         * They do not need a new Unit reservation or a recovery publication.
         * A different owned buffer is not this deck's duration stream. */
        if (binding == DURATION_OTHER || binding == DURATION_UNCHANGED) continue;
        auto &slot = state().decks[d];
        azsdk_native::OwnerGate::Lease lease;
        if (!slot.gate.held_by_current_thread() && slot.gate.enter(lease) != AZ_OK) {
            if (binding == DURATION_SELECTED) duration_lost(d);
            else duration_state[d].retry.store(1, std::memory_order_release);
            continue;
        }
        const uintptr_t core = slot.core.load(std::memory_order_acquire);
        if (!core) continue;
        const uintptr_t buffer = duration_buffer(slot, core);
        if (buffer != parent || memcmp(identity, reinterpret_cast<const void *>(core + 0x18), 12)) continue;
        const uint32_t rate = uint32_t(integer(core + 0x68));
        duration_commit(d, load, samples, rate, !invalidate, UINT64_MAX, parent);
    }
}
struct DurationMutation {
    DurationMutation() {
        duration_writers.fetch_add(1, std::memory_order_acq_rel);
        duration_mutation.fetch_add(1, std::memory_order_acq_rel);
    }
    ~DurationMutation() {
        duration_mutation.fetch_add(1, std::memory_order_release);
        duration_writers.fetch_sub(1, std::memory_order_release);
    }
};
static void duration_set(uintptr_t row, int64_t samples) {
    DurationMutation mutation;
    trampoline<void (*)(uintptr_t, int64_t)>(&duration_original[0])(row, samples);
    if (duration_enabled.load(std::memory_order_acquire)) duration_writer(row, false);
}
static uintptr_t duration_reset(uintptr_t row, unsigned reset) {
    DurationMutation mutation;
    if (duration_enabled.load(std::memory_order_acquire)) duration_writer(row, true);
    return trampoline<uintptr_t (*)(uintptr_t, unsigned)>(&duration_original[1])(row, reset);
}
/* Both PageBuffer destructor variants destroy their row vector through this
 * non-deleting Track destructor before freeing the parent/config. */
static void duration_destroy(uintptr_t row) {
    DurationMutation mutation;
    if (duration_enabled.load(std::memory_order_acquire)) duration_writer(row, true);
    trampoline<void (*)(uintptr_t)>(&duration_original[3])(row);
}
static void duration_retire(unsigned d) {
    /* Called after the owner gate drains all producer/seed frames. */
    auto &v = duration_state[d];
    v.bound_load.store(0, std::memory_order_release);
    v.unit = v.epoch = v.load = v.seed = 0;
    v.buffer = 0;
    v.valid = -1;
    duration_publish_value(v);
    azn_capability(d, AZN_CAP_DURATION, 0);
    v.refresh.store(0, std::memory_order_release);
    v.retry.store(0, std::memory_order_release);
}
static void duration_install(State &s, int build, azn_setting_patch_fn patch) {
    if (duration_enabled.load(std::memory_order_acquire)) return;
    static const uintptr_t sites[][7] = {
        {0x778660, 0x773fc0, 0x770eb8, 0x7668a8, 0x27b82e8, 0x25f9c00, 0x25f9e90},
        {0x7b9f60, 0x7b58c0, 0x7b27b8, 0x7a8128, 0x2b04fa0, 0x2933258, 0x29334e8}};
    static const uint32_t entries[][4] = {
        {0xa9b97bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4},
        {0xa9b47bfd, 0x910003fd, 0xa90363f7, 0xaa0003f7},
        {0xa9be7bfd, 0x910003fd, 0xa90153f3, 0xaa0103f3}};
    const uintptr_t *profile = sites[build - 1];
    for (unsigned i = 0; i < 3; ++i)
        if (!duration_original[i] && memcmp(reinterpret_cast<const void *>(s.bias + profile[i]), entries[i], sizeof entries[i])) return;
    static const uint32_t destroy_entries[][4] = {
        {0xa9be7bfd, 0xf000f481, 0x913a4021, 0x910003fd},
        {0xa9be7bfd, 0xf0010c41, 0x9113a021, 0x910003fd}};
    if (!duration_original[3] && memcmp(reinterpret_cast<const void *>(s.bias + profile[3]),
                                      destroy_entries[build - 1], sizeof destroy_entries[0])) return;
    void *hooks[] = {reinterpret_cast<void *>(duration_set), reinterpret_cast<void *>(duration_reset),
                     reinterpret_cast<void *>(duration_get_length), reinterpret_cast<void *>(duration_destroy)};
    for (unsigned i = 0; i < 4; ++i)
        if (!duration_original[i] && patch(s.bias + profile[i], hooks[i], &duration_original[i], "status-source-duration") != 1) return;
    duration_adapter_table = s.bias + profile[4];
    duration_buffer_table = s.bias + profile[5];
    duration_track_table = s.bias + profile[6];
    duration_enabled.store(true, std::memory_order_release);
}
