/* SPDX-License-Identifier: GPL-3.0-only */
/* UnitStateServer commits one coherent PlayerState payload. PlayerCore's
 * track-end gate compares the measured head with its owned reader's length
 * for the exact accepted TrackID. The rest of the stock predicate gates
 * playback, cueing, auxiliary state, slip, loop and source override. */

#include "sdk_status_duration.inc"

static const uint64_t end_routes = AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
struct EndState {
    _Atomic(uint64_t) refresh;
    _Atomic(unsigned) relevant;
    _Atomic(unsigned) quiet_rate;
    _Atomic(uint64_t) quiet_load;
    _Atomic(uint64_t) quiet_epoch;
    _Atomic(uint64_t) quiet_length;
    uint64_t epoch;
    uint64_t load;
    uint64_t length;
    uint64_t seed;
    uint64_t pending_order;
    uint64_t pending_generation;
    uint64_t confirmed_generation;
    uint64_t confirmed_owner;
    uint64_t confirmed_grid;
    uint64_t confirmed_grid_revision;
    uint64_t confirmed_phrase;
    uint64_t confirmed_phrase_revision;
    uint32_t length_rate;
    int ended;
};
static EndState end_state[4] = {[0 ... 3] = {.refresh = 0, .relevant = 0, .quiet_rate = 0, .quiet_load = 0, .quiet_epoch = 0, .quiet_length = 0, .epoch = 0, .load = 0, .length = 0, .seed = 0, .pending_order = 0, .pending_generation = 0, .confirmed_generation = 0, .confirmed_owner = 0, .confirmed_grid = 0, .confirmed_grid_revision = 0, .confirmed_phrase = 0, .confirmed_phrase_revision = 0, .length_rate = 0, .ended = -1}};
struct EndRead {
    unsigned d;
    uintptr_t core;
    azsdk_load_sample load;
    uint64_t length;
    uint32_t rate;
    uint64_t duration_stamp;
    DurationRead duration;
    bool duration_admitted;
};
static _Thread_local EndRead *end_read;
/* The stock range writer asks its reader for the source length, clamps the
 * owned Position, then tail-calls its consumer. These calls are on the same
 * thread with no intervening calls. */
static _Thread_local struct EndClamp {
    EndRead frame;
    uint64_t before;
} end_clamp;
static _Atomic(bool) end_enabled = false;
static void *end_original[3];
static uintptr_t end_length_return, end_clamp_return;
static void end_record_length(EndRead *frame, uintptr_t call, uintptr_t reader, uintptr_t id, int64_t result) {
    if (!frame) return;
    const int positive = compare_timestamps(result, 0, frame->rate, frame->rate, 1);
    if (call == end_length_return && result != INT64_MAX && positive != AZ_TIME_INVALID && positive == AZ_TIME_AFTER &&
        id == frame->core + 0x18 && word(frame->core + 0xa8) == reader &&
        azn_load_track_matches(frame->d, frame->load.invalidation_epoch, frame->load.load_generation, (const unsigned char *) id))
        frame->length = (uint64_t) result;
}
static bool end_clamp_times(uint64_t before, int64_t length, uint32_t rate) {
    const int representable = compare_timestamps(before, UINT64_C(9007199254740992), rate, rate, 1);
    const int positive = compare_timestamps(length, 0, rate, rate, 1);
    const int overshoot = compare_timestamps(before, length, rate, rate, 1);
    return positive != AZ_TIME_INVALID && positive == AZ_TIME_AFTER && overshoot != AZ_TIME_INVALID && overshoot == AZ_TIME_AFTER &&
           representable != AZ_TIME_INVALID && representable <= AZ_TIME_EQUAL;
}
static void end_record_clamp(unsigned d, uintptr_t core, uintptr_t id, uint32_t rate, uint64_t before, int64_t length) {
    EndRead *clamp = &end_clamp.frame;
    if (!end_clamp_times(before, length, rate) || !azsdk_load_read((int) d, &clamp->load) ||
        !azn_load_track_matches(d, clamp->load.invalidation_epoch, clamp->load.load_generation, (const unsigned char *) id)) return;
    clamp->d = d;
    clamp->core = core;
    clamp->length = (uint64_t) length;
    clamp->rate = rate;
    end_clamp.before = before;
}
static void end_clamp_length(uintptr_t reader, uintptr_t id, int64_t result) {
    end_clamp.before = 0;
    if (!processing || result == INT64_MAX) return;
    uintptr_t core = atomic_load_explicit(&(processing->core), memory_order_acquire);
    unsigned d = (unsigned) (processing - state().decks);
    uint32_t rate = core ? (uint32_t) integer(core + 0x68) : 0;
    if (core && id == core + 0x18 && word(core + 0xa8) == reader && atomic_load_explicit(&(end_state[d].relevant), memory_order_acquire))
        end_record_clamp(d, core, id, rate, word(core + 0x28), result);
}
static int64_t end_reader_length(uintptr_t reader, uintptr_t id) {
    const uintptr_t call = (uintptr_t) __builtin_return_address(0);
    int64_t result = ((int64_t (*)(uintptr_t, uintptr_t)) trampoline_address(&end_original[1]))(reader, id);
    if (!atomic_load_explicit(&(end_enabled), memory_order_acquire)) return result;
    end_record_length(end_read, call, reader, id, result);
    if (call == end_clamp_return) end_clamp_length(reader, id, result);
    return result;
}
static bool end_clamp_landed(const EndRead *frame, uintptr_t position) {
    const int landing = compare_timestamps(word(position + 0x18), frame->length, frame->rate, frame->rate, 1);
    return landing != AZ_TIME_INVALID && landing == AZ_TIME_EQUAL &&
           azn_load_track_matches(frame->d, frame->load.invalidation_epoch, frame->load.load_generation, (const unsigned char *) (position + 8));
}
static bool end_clamp_pending(const EndState *e, const Motion *m, const EndRead *frame, int length) {
    return e->pending_order && e->pending_order == m->order && e->epoch == frame->load.invalidation_epoch &&
           e->load == frame->load.load_generation && e->length_rate == frame->rate && length != AZ_TIME_INVALID && length == AZ_TIME_EQUAL;
}
static bool end_clamp_motion(const EndState *e, const Motion *m, uintptr_t position, int departure) {
    return m->load == e->load && m->known && departure != AZ_TIME_INVALID && departure == AZ_TIME_EQUAL &&
           !memcmp(m->identity, (const void *) (position + 8), 12) && !atomic_load_explicit(&(m->foreign), memory_order_acquire) &&
           m->captured_interference == atomic_load_explicit(&(m->interference), memory_order_acquire);
}
static void end_accept_clamp(const EndRead *frame, uint64_t before, uintptr_t position) {
    __auto_type slot = &(state().decks[frame->d]);
    if (!before || position != frame->core + 0x10 || atomic_load_explicit(&(slot->core), memory_order_acquire) != frame->core) return;
    if (!end_clamp_landed(frame, position)) return;
    __auto_type m = &(slot->motion);
    __auto_type e = &(end_state[frame->d]);
    unsigned reservation = MOTION_FREE;
    if (!atomic_compare_exchange_strong_explicit(&(m->busy), &(reservation), MOTION_OBSERVE, memory_order_acquire, memory_order_acquire)) return;
    const int length = compare_timestamps(e->length, frame->length, frame->rate, frame->rate, 1);
    const int departure = compare_timestamps(m->previous, before, frame->rate, frame->rate, 1);
    if (end_clamp_pending(e, m, frame, length) && end_clamp_motion(e, m, position, departure)) {
        /* The last measured block already covered the playable interval.
         * This internal clamp adds no reverse travel or command occurrence. */
        m->previous = frame->length;
    }
    atomic_store_explicit(&(m->busy), MOTION_FREE, memory_order_release);
}
static uintptr_t end_range_consumer(uintptr_t self, uintptr_t frames, uintptr_t position, unsigned flag) {
    if (end_clamp.before) {
        uint64_t before = end_clamp.before;
        end_clamp.before = 0;
        end_accept_clamp(&(end_clamp.frame), before, position);
    }
    return ((uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, unsigned)) trampoline_address(&end_original[2]))(self, frames, position, flag);
}
static bool end_payload_loop(uintptr_t property) {
    return byte(property + 0x30) && (byte(property + 0x34) & 31) && byte(property + 0x36) &&
           integer(property + 0x38) && word(property + 0x40) != INT64_MAX;
}
static bool end_payload(uintptr_t payload) {
    const bool loop = end_payload_loop(payload + 0xb00);
    return byte(payload + 0x14) && byte(payload + 0x15) && !byte(payload + 0x16) && !byte(payload + 0x19) &&
           !byte(payload + 0x59) && !loop && !byte(payload + 0xcf3);
}
static void end_lost(unsigned d) {
    /* Report once per outstanding gap and recheck at the next genuine unit
     * publication, even if the machine stays parked at the end. */
    if (!atomic_exchange_explicit(&(end_state[d].refresh), 1, memory_order_acq_rel)) azn_end_gap(d);
}
static bool end_continuous_motion(const EndState *e, const azstatus_native_receipt *r) {
    return r->a == AZN_MEASURE && !r->c && !r->lost_topics && r->points[0].state == AZN_COORD && r->points[1].state == AZN_COORD &&
           r->points[0].rate == r->points[1].rate && r->points[0].rate == e->length_rate;
}
static void end_extend_candidate(unsigned d, EndState *e, const azstatus_native_receipt *r, bool continuous) {
    if (!e->pending_order) return;
    /* Adjacent measurements can refine an overlapping terminal candidate.
     * A changed generation/order cannot extend it. */
    if (!continuous || r->motion_order != e->pending_order + 1 || r->motion_generation != e->pending_generation) end_lost(d);
    e->pending_order = 0;
}
static bool end_same_confirmation(const EndState *e, const azstatus_native_receipt *r, bool continuous) {
    return continuous && e->ended == 1 && !atomic_load_explicit(&(e->refresh), memory_order_acquire) &&
           r->motion_generation == e->confirmed_generation && r->map.owner == e->confirmed_owner &&
           r->map.grid.provider == e->confirmed_grid && r->map.grid.revision == e->confirmed_grid_revision &&
           r->map.phrases.provider == e->confirmed_phrase && r->map.phrases.revision == e->confirmed_phrase_revision;
}
static bool end_motion_settled(const EndState *e, const azstatus_native_receipt *r) {
    const int from = compare_timestamps(r->points[0].samples, e->length, e->length_rate, e->length_rate, 1);
    const int to = compare_timestamps(r->points[1].samples, e->length, e->length_rate, e->length_rate, 1);
    return from != AZ_TIME_INVALID && to != AZ_TIME_INVALID && from == AZ_TIME_EQUAL && to == AZ_TIME_EQUAL;
}
static bool end_crossed_boundary(bool continuous, int from, int to) {
    return continuous && from != AZ_TIME_INVALID && to != AZ_TIME_INVALID && (from == AZ_TIME_BEFORE || from == AZ_TIME_EQUAL) &&
           (to == AZ_TIME_EQUAL || to == AZ_TIME_AFTER);
}
static void end_mark_motion(unsigned d, azstatus_native_receipt *r) {
    EndState *e = &end_state[d];
    if (e->epoch != r->epoch || e->load != r->load_generation || !e->length) return;
    const bool continuous = end_continuous_motion(e, r);
    end_extend_candidate(d, e, r, continuous);
    const int from = compare_timestamps(r->points[0].samples, e->length, e->length_rate, r->from_window_rate, r->from_window_samples);
    const int to = compare_timestamps(r->points[1].samples, e->length, e->length_rate, r->observed_window_rate, r->observed_window_samples);
    if (end_same_confirmation(e, r, continuous) && end_motion_settled(e, r)) return;
    if (end_crossed_boundary(continuous, from, to)) {
        /* Keep the actual measurement and independently owned source limit.
         * Suppress only that terminal phrase edge until the clamp confirms it. */
        r->points[2] = (azn_point) {AZN_COORD, r->points[1].rate, (int64_t) e->length};
        r->motion_flags |= AZN_TERMINAL_ARRIVAL;
        e->pending_order = r->motion_order;
        e->pending_generation = r->motion_generation;
        atomic_store_explicit(&(e->relevant), 1, memory_order_release);
    }
}
struct EndObservation {
    azsdk_load_sample current;
    uint64_t sample, seed, fault;
    int at_end, length_order, head_order;
    bool ended;
};
static bool end_load_current(const EndRead *frame, uintptr_t payload, azsdk_load_sample *current) {
    return azsdk_load_read((int) frame->d, current) && current->load_generation && current->load_generation == frame->load.load_generation &&
           (uint32_t) integer(frame->core + 0x68) == frame->rate && current->invalidation_epoch == frame->load.invalidation_epoch &&
           current->unit_generation == frame->load.unit_generation &&
           azn_load_track_matches(frame->d, current->invalidation_epoch, current->load_generation, (const unsigned char *) (payload + 0x28));
}
static bool end_read_observation(const EndRead *frame, uintptr_t payload, struct EndObservation *value) {
    if (!end_load_current(frame, payload, &value->current)) return false;
    value->sample = word(payload + 0x38);
    value->at_end = compare_timestamps(value->sample, frame->length, frame->rate, frame->rate, 1);
    value->ended = end_payload(payload) && frame->length && value->at_end != AZ_TIME_INVALID && value->at_end == AZ_TIME_EQUAL;
    return true;
}
static bool end_quiet_identity(const EndState *e, const EndRead *frame, const azsdk_load_sample *load) {
    return frame->length && atomic_load_explicit(&(e->quiet_load), memory_order_acquire) == load->load_generation &&
           atomic_load_explicit(&(e->quiet_epoch), memory_order_relaxed) == load->invalidation_epoch &&
           atomic_load_explicit(&(e->quiet_rate), memory_order_relaxed) == frame->rate;
}
static bool end_quiet(const EndState *e, const EndRead *frame, const struct EndObservation *value) {
    /* Ordinary nonterminal frames must not contend with the audio motion
     * writer. A new length, candidate or retirement enrolls under its order. */
    if (value->ended || !end_quiet_identity(e, frame, &value->current)) return false;
    const int length = compare_timestamps(atomic_load_explicit(&(e->quiet_length), memory_order_relaxed), frame->length, frame->rate, frame->rate, 1);
    return length != AZ_TIME_INVALID && length == AZ_TIME_EQUAL &&
           !atomic_load_explicit(&(e->relevant), memory_order_acquire) && !atomic_load_explicit(&(e->refresh), memory_order_acquire);
}
static bool end_reserve_motion(unsigned d, EndState *e, Motion *m, bool ended) {
    unsigned reservation = MOTION_FREE;
    if (atomic_compare_exchange_strong_explicit(&(m->busy), &reservation, MOTION_OBSERVE, memory_order_acquire, memory_order_acquire)) return true;
    if (ended || atomic_load_explicit(&(e->relevant), memory_order_acquire)) end_lost(d);
    return false;
}
static void end_bind_load(EndState *e, const azsdk_load_sample *load) {
    if (e->load == load->load_generation && e->epoch == load->invalidation_epoch) return;
    e->load = load->load_generation;
    e->epoch = load->invalidation_epoch;
    e->length = 0;
    e->ended = -1;
    e->pending_order = 0;
    e->seed = 0;
    atomic_store_explicit(&(e->relevant), 0, memory_order_release);
}
static bool end_length_matches(const EndState *e, const EndRead *frame, int order) {
    return e->length_rate == frame->rate && order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static void end_update_pending(unsigned d, EndState *e, const EndRead *frame, int order) {
    if (e->pending_order && !end_length_matches(e, frame, order)) {
        end_lost(d);
        e->pending_order = 0;
    }
}
static bool end_waiting_for_clamp(const EndState *e, const Motion *m, const EndRead *frame, const struct EndObservation *value) {
    /* A unit cut between render overshoot and the range clamp is not an END
     * decision. Wait for the independently owned source to settle. */
    return e->pending_order && end_length_matches(e, frame, value->length_order) && value->at_end == AZ_TIME_AFTER && m->known &&
           value->head_order != AZ_TIME_INVALID && value->head_order == AZ_TIME_EQUAL && m->order == e->pending_order;
}
static void end_drop_length(unsigned d, EndState *e) {
    if (e->ended == 1 || e->pending_order) end_lost(d);
    atomic_store_explicit(&(e->quiet_load), 0, memory_order_release);
    e->length = 0;
    e->ended = -1;
    e->pending_order = 0;
}
static bool end_length_usable(const EndRead *frame, int at_end) {
    return frame->length && at_end != AZ_TIME_AFTER && at_end != AZ_TIME_INVALID;
}
static void end_publish_quiet(unsigned d, EndState *e, const EndRead *frame) {
    e->length = frame->length;
    e->length_rate = frame->rate;
    atomic_store_explicit(&(e->quiet_epoch), e->epoch, memory_order_relaxed);
    atomic_store_explicit(&(e->quiet_length), e->length, memory_order_relaxed);
    atomic_store_explicit(&(e->quiet_rate), frame->rate, memory_order_relaxed);
    atomic_store_explicit(&(e->quiet_load), e->load, memory_order_release);
    azn_capability(d, AZN_CAP_END, 1);
}
static bool end_redundant(const EndState *e, const struct EndObservation *value) {
    return !e->pending_order && !value->fault && (e->ended == (int) value->ended || (e->ended < 0 && !value->ended)) &&
           (!value->ended || e->seed == value->seed);
}
static bool end_motion_current(const EndState *e, const Motion *m, int head_order) {
    return m->load == e->load && m->known && head_order != AZ_TIME_INVALID && head_order == AZ_TIME_EQUAL && m->generation && m->order != UINT64_MAX &&
           !atomic_load_explicit(&(m->foreign), memory_order_acquire) && m->captured_interference == atomic_load_explicit(&(m->interference), memory_order_acquire);
}
static bool end_motion_commit_ready(unsigned d, const EndState *e, const Motion *m, const struct EndObservation *value) {
    if (end_motion_current(e, m, value->head_order)) return true;
    if (value->ended || e->pending_order) end_lost(d);
    return false;
}
static void end_receipt_map(unsigned d, uint64_t load, azstatus_native_receipt *receipt) {
    const uint64_t clock = read_map_clock(d, load, &receipt->map);
    if (clock == UINT64_MAX || atomic_load_explicit(&(map_mutations[d].active), memory_order_acquire)) {
        receipt->map = (aznm_packet) {0};
        receipt->motion_flags &= ~AZN_TERMINAL_CONTINUATION;
        receipt->lost_topics |= AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    }
    atomic_store_explicit(&(map_mutations[d].last_order), receipt->motion_order, memory_order_release);
}
static void end_make_receipt(unsigned d, const EndState *e, Motion *m, const struct EndObservation *value, azstatus_native_receipt *r) {
    r->kind = AZN_END;
    r->deck = d;
    r->unit = value->current.unit_generation;
    r->epoch = e->epoch;
    r->load_generation = e->load;
    r->a = value->ended;
    r->valid = 1;
    r->motion_generation = m->generation;
    if (e->pending_order && e->pending_order == m->order && !value->fault) r->motion_flags |= AZN_TERMINAL_CONTINUATION;
    if (value->ended) r->motion_flags |= AZN_TERMINAL_CONFIRMED;
    r->points[0] = r->points[1] = (azn_point) {AZN_COORD, m->rate, (int64_t) value->sample};
    /* This witnesses the actual integer Position store after range clamping,
     * without reconstructing a render interval from the seed. */
    r->observed_window_samples = r->from_window_samples = 1;
    r->observed_window_rate = r->from_window_rate = m->rate;
    r->motion_order = ++m->order;
    end_receipt_map(d, e->load, r);
}
static void end_commit_receipt(unsigned d, EndState *e, const Motion *m, const struct EndObservation *value, azstatus_native_receipt *r) {
    e->pending_order = 0;
    e->ended = value->ended;
    e->confirmed_generation = value->ended ? m->generation : 0;
    e->confirmed_owner = r->map.owner;
    e->confirmed_grid = r->map.grid.provider;
    e->confirmed_grid_revision = r->map.grid.revision;
    e->confirmed_phrase = r->map.phrases.provider;
    e->confirmed_phrase_revision = r->map.phrases.revision;
    e->seed = value->seed;
    atomic_store_explicit(&(e->relevant), value->ended, memory_order_release);
    atomic_store_explicit(&(e->refresh), 0, memory_order_release);
    if (!azn_offer(r)) end_lost(d);
}
static void end_complete_owned(const EndRead *frame, struct EndObservation *value) {
    unsigned d = frame->d;
    EndState *e = &end_state[d];
    Motion *m = &state().decks[d].motion;
    end_bind_load(e, &value->current);
    value->length_order = compare_timestamps(frame->length, e->length, frame->rate, frame->rate, 1);
    value->head_order = compare_timestamps(m->previous, value->sample, frame->rate, frame->rate, 1);
    end_update_pending(d, e, frame, value->length_order);
    if (end_waiting_for_clamp(e, m, frame, value)) return;
    if (!end_length_usable(frame, value->at_end)) {
        end_drop_length(d, e);
        return;
    }
    end_publish_quiet(d, e, frame);
    value->seed = azn_seed_id(d);
    value->fault = atomic_load_explicit(&(e->refresh), memory_order_acquire);
    if (end_redundant(e, value)) {
        e->ended = value->ended;
        e->seed = value->seed;
        return;
    }
    if (!end_motion_commit_ready(d, e, m, value)) return;
    azstatus_native_receipt receipt = {0};
    end_make_receipt(d, e, m, value, &receipt);
    end_commit_receipt(d, e, m, value, &receipt);
}
static void end_commit(EndRead *frame, uintptr_t payload) {
    struct EndObservation value = {0};
    if (!end_read_observation(frame, payload, &value)) return;
    Motion *m = &state().decks[frame->d].motion;
    EndState *e = &end_state[frame->d];
    if (end_quiet(e, frame, &value) || !end_reserve_motion(frame->d, e, m, value.ended)) return;
    EXIT_ATOMIC_GUARD(release, &m->busy);
    end_complete_owned(frame, &value);
}
struct EndRestore {
    EndRead *old;
    DurationRead *duration_old;
};
static void endrestore_drop(struct EndRestore *scope) {
    end_read = scope->old;
    duration_read = scope->duration_old;
}

typedef uintptr_t (*EndSnapshot)(uintptr_t, uintptr_t);
static void end_release_lease(struct az_owner_lease *lease) {
    if (az_owner_lease_owner(lease)) az_owner_lease_end(lease);
}
static bool end_snapshot_candidate(unsigned d, uintptr_t core, EndRead *frame) {
    return atomic_load_explicit(&(state().decks[d].core), memory_order_acquire) == core &&
           (azn_interest(d) & (end_routes | duration_routes)) && azsdk_load_read((int) d, &frame->load) && frame->load.load_generation;
}
static void end_prepare_frame(unsigned d, Slot *slot, uintptr_t core, EndRead *frame) {
    frame->d = d;
    frame->core = core;
    frame->duration_stamp = atomic_load_explicit(&(duration_mutation), memory_order_acquire);
    if (frame->duration_admitted) {
        frame->duration.buffer = duration_buffer(slot, core);
        frame->duration.id = core + 0x18;
    }
    frame->rate = (uint32_t) integer(core + 0x68);
}
static bool end_duration_admit(const EndRead *frame, Slot *slot, struct az_owner_lease *lease) {
    return frame->duration_admitted && (az_owner_held(&slot->gate) || az_owner_enter(&slot->gate, lease) == AZ_OK);
}
static bool end_duration_current(const EndRead *frame, const Slot *slot, uintptr_t payload) {
    return atomic_load_explicit(&(slot->core), memory_order_acquire) == frame->core && atomic_load_explicit(&(duration_enabled), memory_order_acquire) &&
           !atomic_load_explicit(&(duration_writers), memory_order_acquire) &&
           frame->duration_stamp == atomic_load_explicit(&(duration_mutation), memory_order_acquire) &&
           frame->rate == (uint32_t) integer(frame->core + 0x68) && frame->duration.buffer == duration_buffer(slot, frame->core) &&
           azn_load_track_matches(frame->d, frame->load.invalidation_epoch, frame->load.load_generation, (const unsigned char *) (payload + 0x28));
}
static void end_finish_duration(const EndRead *frame, Slot *slot, uintptr_t payload, struct az_owner_lease *lease) {
    if (end_duration_admit(frame, slot, lease) && end_duration_current(frame, slot, payload))
        duration_commit(frame->d, frame->load, frame->duration.samples, frame->rate, frame->duration.observed, frame->duration_stamp, frame->duration.buffer);
}
static bool end_snapshot_attempt(unsigned d, uintptr_t core, uintptr_t self, uintptr_t payload, EndRead *frame, EndSnapshot original, uintptr_t *result) {
    Slot *slot = &state().decks[d];
    AZ_OWNER_LEASE(lease);
    frame->duration_admitted = az_owner_held(&slot->gate) || az_owner_enter(&slot->gate, &lease) == AZ_OK;
    if (atomic_load_explicit(&(slot->core), memory_order_acquire) != core) return false;
    end_prepare_frame(d, slot, core, frame);
    /* The concrete PageBuffer hook owns the source receiver. Retain only a
     * scalar identity across the snapshot, releasing the deck reservation. */
    end_release_lease(&lease);
    struct EndRestore restore __attribute__((cleanup(endrestore_drop))) = {end_read, duration_read};
    end_read = frame;
    duration_read = &frame->duration;
    *result = original(self, payload);
    end_finish_duration(frame, slot, payload, &lease);
    end_release_lease(&lease);
    if (azn_interest(d) & end_routes) end_commit(frame, payload);
    return true;
}
static uintptr_t end_snapshot(uintptr_t self, uintptr_t payload) {
    EndSnapshot original = (EndSnapshot) trampoline_address(&end_original[0]);
    if (!atomic_load_explicit(&(end_enabled), memory_order_acquire)) return original(self, payload);
    uintptr_t core = word(self + 0x1f38);
    EndRead frame = {.length = 0, .rate = 0, .duration_stamp = 0, .duration_admitted = false};
    for (unsigned d = 0; d < 4; d++) {
        if (!end_snapshot_candidate(d, core, &frame)) continue;
        uintptr_t result;
        if (end_snapshot_attempt(d, core, self, payload, &frame, original, &result)) return result;
    }
    return original(self, payload);
}
static bool end_clamp_entry(uintptr_t clamp) {
    return (uint32_t) integer(clamp - 4) == 0xd63f0040u && (uint32_t) integer(clamp) == 0xf9400e81u &&
           (uint32_t) integer(clamp + 16) == 0x9a80d033u && (uint32_t) integer(clamp + 44) == 0xf9000e93u;
}
static void end_install(State *s, int build, azn_setting_patch_fn patch) {
    if (atomic_load_explicit(&(end_enabled), memory_order_acquire)) {
        duration_install(s, build, patch);
        return;
    }
    static const uintptr_t sites[][5] = {{0x14b0060, 0xd8d338, 0x13f8448, 0x13578a0, 0x14a3ea4},
                                         {0x156b538, 0xe18148, 0x14b37a0, 0x1412c08, 0x155f590}};
    static const uint32_t entries[][4] = {{0xa9b37bfd, 0x910003fd, 0xa90153f3, 0xaa0103f3},
                                          {0xa9b87bfd, 0x910003fd, 0xa90153f3, 0xf9400413},
                                          {0xa9bc7bfd, 0x910003fd, 0xa90153f3, 0x9100e014}};
    const uintptr_t clamp = s->bias + sites[build - 1][4];
    if (!end_clamp_entry(clamp)) return;
    void *hooks[] = {((void *) (end_snapshot)), ((void *) (end_reader_length)),
                     ((void *) (end_range_consumer))};
    for (unsigned i = 0; i < 3; i++) {
        if (end_original[i]) continue;
        uintptr_t addr = s->bias + sites[build - 1][i];
        if (memcmp(((void *) (addr)), entries[i], sizeof entries[i])) return;
        if (patch(addr, hooks[i], &end_original[i], "status-track-end") != 1) return;
    }
    end_length_return = s->bias + sites[build - 1][3];
    end_clamp_return = clamp;
    duration_install(s, build, patch);
    atomic_store_explicit(&(end_enabled), true, memory_order_release);
}
