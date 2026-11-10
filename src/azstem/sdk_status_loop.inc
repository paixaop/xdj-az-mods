/* SPDX-License-Identifier: GPL-3.0-only */
/* Included in the PlayerCore owner bridge. Both admitted images update the
 * CueProperty mutation counter at +8 in the leaf methods and the outer kind setter. The
 * working property is the CueContext member at +0x160. Kind zero is a separate
 * retained pointer. No callback notification or UI sampling supplies an edge. */
#include "sdk_status_loop_profiles.h"
static void *status_original[7];
constexpr uint64_t status_loop_route = AZ_STATUS_TOPIC(AZ_STATUS_LOOP);
constexpr uint64_t status_retained_routes = status_loop_route | AZ_STATUS_TOPIC(AZ_STATUS_CUES);

static azn_point status_point(uintptr_t source) noexcept {
    if (empty_position(source)) return {AZN_ABSENT, 0, 0};
    if (!valid_position(source)) return {AZN_INVALID, 0, 0};
    const auto rate = unsigned(integer(source + 0x30));
    uint64_t samples = word(source + 0x10), us = word(source + 0x38);
    const int us_range = compare_timestamps(us, UINT64_C(9007199254740992), 1000000, rate, 1);
    if (!word(source + 0x20) || !rate || rate >= 0x7fffffffu ||
        us_range == AZ_TIME_INVALID || us_range > AZ_TIME_EQUAL) return {AZN_INVALID, 0, 0};
    using Wide = unsigned __int128;
    const int consistent = compare_timestamps(Wide(samples) * 1000000u, Wide(us) * rate,
                                              uint64_t(rate) * 1000000u, rate, 1);
    if (consistent == AZ_TIME_INVALID || consistent != AZ_TIME_EQUAL) return {AZN_INVALID, 0, 0};
    return {AZN_COORD, rate, static_cast<int64_t>(samples)};
}
static void status_points(uintptr_t property, azn_point *out) noexcept {
    out[0] = status_point(property + 0x30);
    out[1] = status_point(property + 0x88);
    const int interval = compare_timestamps(out[1].samples, out[0].samples, out[1].rate, out[0].rate, 1);
    if (out[0].state == AZN_COORD && out[1].state == AZN_COORD &&
        (out[0].rate != out[1].rate || std::memcmp(reinterpret_cast<const void *>(property + 0x30), reinterpret_cast<const void *>(property + 0x88), 12) ||
         interval == AZ_TIME_INVALID || interval != AZ_TIME_AFTER))
        out[1] = {AZN_INVALID, 0, 0};
}
/* Each property endpoint is one committed integer source-sample point. */
static bool status_same_point(const azn_point &a, const azn_point &b) noexcept {
    if (a.state != b.state || a.rate != b.rate) return false;
    if (a.state != AZN_COORD) return true;
    const int order = compare_timestamps(a.samples, b.samples, a.rate, a.rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static bool status_qualify_points(Slot &slot, uintptr_t property, azn_point *out) noexcept {
    bool same = true;
    uintptr_t core = slot.core.load(std::memory_order_acquire);
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t source = property + (i ? 0x88u : 0x30u);
        if (out[i].state != AZN_ABSENT && byte(source) && (byte(source + 4) & 31u) && byte(source + 6) &&
            integer(source + 8) && std::memcmp(reinterpret_cast<const void *>(core + 0x18), reinterpret_cast<const void *>(source), 12)) {
            out[i] = {AZN_INVALID, 0, 0};
            same = false;
        }
    }
    return same;
}
static bool status_properties(Slot &slot, uintptr_t &retained, uintptr_t &active) noexcept {
    if (!coherent(slot) || !set_bindings(slot)) return false;
    uintptr_t context = word(slot.facade + 0x68);
    if (!pointer(context)) return false;
    retained = word(context + 0x18);
    active = context + 0x160;
    const auto &p = state().profile;
    return table(retained, p.cue_table) && (table(active, p.cue_table) || table(active, p.cue_base_table));
}
static bool status_source_loaded(unsigned d, Slot &slot, const azsdk_load_sample &load) noexcept {
    const uintptr_t core = slot.core.load(std::memory_order_acquire);
    unsigned char source[12];
    std::memcpy(source, reinterpret_cast<const void *>(core + 0x18), sizeof source);
    return azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, source) != 0;
}

#include "sdk_status_reloop.inc"

/* A copy from the firmware's indexed A–H property is a causal association.
 * Compare the entire bank incarnation and revision on both sides of the copy.
 * An independently constructed property with identical bounds cannot match. */
static azn_hot_link copied_hot(Slot &s, uint64_t load, uintptr_t source) noexcept {
    azn_hot_link link {};
    auto &h = s.hot;
    if (!source || h.busy.test_and_set(std::memory_order_acquire)) return link;
    struct Unlock {
        Slot::HotState &h;
        ~Unlock() { h.busy.clear(std::memory_order_release); }
    } guard {h};
    if (!h.ready || h.load != load || !h.serial || !h.provider ||
        azn_cue_loss_cut(unsigned(&s - state().decks), 1) != h.loss_cut) return link;
    uintptr_t context = word(s.facade + 0x68);
    const auto &profile = state().profile;
    for (unsigned i = 0; i < 8; ++i) {
        const uintptr_t p = word(context + 0x20 + i * 8);
        if (p != h.property[i] || (!table(p, profile.cue_table) && !table(p, profile.cue_base_table)) ||
            unsigned(integer(p + 8)) != h.revision[i]) return {};
        azn_point pair[2];
        status_points(p, pair);
        status_qualify_points(s, p, pair);
        unsigned rgb = (byte(p + 0x10) << 16) | (byte(p + 0x11) << 8) | byte(p + 0x12);
        if (!status_same_point(pair[0], h.previous[i].in) || !status_same_point(pair[1], h.previous[i].out) || rgb != h.previous[i].rgb) return {};
        if (p == source && h.previous[i].in.state == AZN_COORD && h.previous[i].out.state == AZN_COORD)
            link = {h.provider, h.serial, i + 1, AZN_HOT_LINK_READY};
    }
    return link;
}
static void status_working_file(Slot &slot, uint64_t token) noexcept {
    if (slot.working_file_row == token) return;
#ifdef AZ_STATUS_MEMORY_PRODUCER
    if (token) azn_file_row_hold(token, 1);
    if (slot.working_file_row) azn_file_row_hold(slot.working_file_row, 0);
#endif
    slot.working_file_row = token;
}
static void status_loop_capture(unsigned d, Slot &slot, uintptr_t written,
                                const azsdk_load_sample *before, const azn_hot_link *copied = nullptr, uint64_t file_row = 0) noexcept {
    const bool interested = (azn_interest(d) & status_retained_routes) != 0;
    if (!interested && !written) return;
    if (slot.status_busy.test_and_set(std::memory_order_acquire)) {
        azn_gap(d, status_retained_routes);
        return;
    }
    struct Unlock {
        Slot &s;
        ~Unlock() { s.status_busy.clear(std::memory_order_release); }
    } unlock {slot};
    azsdk_load_sample load {};
    if (!azsdk_load_read(int(d), &load)) return;
    if (before && (before->load_generation != load.load_generation ||
                   before->unit_generation != load.unit_generation || before->invalidation_epoch != load.invalidation_epoch)) return;
    uintptr_t retained, active;
    if (!status_properties(slot, retained, active)) {
        if (slot.status_epoch) {
            slot.status_epoch = 0;
            azn_gap(d, status_retained_routes);
        }
        return;
    }
    if (!status_source_loaded(d, slot, load)) {
        const bool revoked = azn_load_history_revoked(d, load.invalidation_epoch);
        slot.working_origin = {};
        status_working_file(slot, 0);
        if (reloop_state[d].retained.exchange(0, std::memory_order_acq_rel) && !revoked) reloop_loss(d);
        if (slot.status_epoch == load.invalidation_epoch) {
            slot.status_epoch = 0;
            if (!revoked) azn_gap(d, status_retained_routes);
        }
        return;
    }
    reloop_observe(d, slot, retained, load.invalidation_epoch);
    uint32_t rr = unsigned(integer(retained + 8)), wr = unsigned(integer(active + 8));
    uint64_t seed = azn_seed_id(d), cut = azn_cue_loss_cut(d, 0);
    bool same_load = slot.status_epoch == load.invalidation_epoch;
    bool retained_change = rr != slot.status_retained_rev || retained != slot.status_retained;
    bool working_change = wr != slot.status_working_rev;
    if (same_load && !retained_change && !working_change && seed == slot.status_seed && cut == slot.status_loop_cut) return;
    azstatus_native_receipt r {};
    r.kind = AZN_LOOP;
    r.deck = d;
    r.epoch = load.invalidation_epoch;
    r.unit = load.unit_generation;
    r.load_generation = load.load_generation;
    bool memory_tail = copied && copied->state == AZN_MEMORY_LINK_READY && written == active &&
                       retained == slot.status_retained && rr == slot.status_retained_rev + 1u;
    if (same_load && retained_change && !memory_tail && (written != retained || retained != slot.status_retained || rr != slot.status_retained_rev + 1u))
        r.lost_topics |= status_retained_routes;
    if (same_load && working_change && (written != active || wr != slot.status_working_rev + 1u))
        r.lost_topics |= status_loop_route;
    r.b = !same_load || retained_change || seed != slot.status_seed;
    status_points(retained, r.points);
    status_points(active, r.points + 2);
    status_qualify_points(slot, retained, r.points);
    if (reloop_installed.load(std::memory_order_acquire) == 15) r.reloop = 0;
    else if (r.points[0].state == AZN_INVALID || r.points[1].state == AZN_INVALID) r.reloop = AZN_RELOOP_STALE;
    else if (r.points[0].state == AZN_ABSENT || r.points[1].state == AZN_ABSENT) r.reloop = AZN_RELOOP_DENIED;
    else r.reloop = AZN_RELOOP_UNKNOWN; // Complete points are only the panel's first permission gate.
    bool working_source = status_qualify_points(slot, active, r.points + 2);
    /* The native isActiveLoopPlay predicate is current IN validity alone;
     * bounds/rate and retained slot presence are independent. */
    uintptr_t in = active + 0x30;
    r.a = byte(in) && (byte(in + 4) & 31u) && byte(in + 6) && integer(in + 8) &&
          word(in + 16) != UINT64_C(0x7fffffffffffffff);
    r.valid = working_source;
    if (!r.a) r.points[2] = r.points[3] = {AZN_ABSENT, 0, 0};
    if (!same_load || working_change || cut != slot.status_loop_cut) {
        slot.working_origin = {};
        uint64_t next_file = 0;
        if (copied && (copied->state == AZN_HOT_LINK_READY || copied->state == AZN_MEMORY_LINK_READY) && written == active && working_source &&
            r.a && r.points[2].state == AZN_COORD && r.points[3].state == AZN_COORD && !(r.lost_topics & status_loop_route)) {
            slot.working_origin = *copied;
            next_file = file_row;
        }
        status_working_file(slot, next_file);
    }
    r.hot_link = slot.working_origin;
    r.file_row_token = slot.working_file_row;
    if (!r.hot_link.state) r.hot_link.state = working_source && !r.a ? AZN_HOT_LINK_EMPTY : AZN_HOT_LINK_UNKNOWN;
    if (!working_source) r.c = AZN_MODE_STALE;
    else if (!r.a) r.c = AZN_MODE_EMPTY;
    else if (r.points[2].state != AZN_COORD || r.points[3].state != AZN_COORD) r.c = AZN_MODE_STALE;
    else {
        unsigned type = unsigned(integer(active + 0xc)), mode = unsigned(integer(active + 0xc8));
        r.c = AZN_MODE_UNKNOWN;
        if (type == 3 || type == 4) {
            uint32_t beat = unsigned(integer(active + 0xd8));
            if (mode == 0) r.c = AZN_MODE_TIME;
            if (mode == 1) r.c = (beat & 0xffffu) && (beat >> 16) ? AZN_MODE_BEAT : AZN_MODE_STALE;
        }
    }
    azsdk_load_sample end {};
    if (!azsdk_load_read(int(d), &end) || load.invalidation_epoch != end.invalidation_epoch ||
        load.load_generation != end.load_generation || load.unit_generation != end.unit_generation || !status_source_loaded(d, slot, end)) return;
    slot.status_epoch = r.epoch;
    slot.status_seed = seed;
    slot.status_loop_cut = cut;
    slot.status_retained = retained;
    slot.status_retained_rev = rr;
    slot.status_working_rev = wr;
    if (interested) azn_emit(&r);
}

#include "sdk_status_hot.inc"

template <class... Args>
static uintptr_t first_source(uintptr_t source, Args...) noexcept { return source; }
static uintptr_t first_source() noexcept { return 0; }

/* Preserve every incoming AAPCS64 argument used by the original. These
 * methods take integer registers only. No native C++ value crosses the bridge. */
template <unsigned Entry, class... Args>
static void status_writer(void *self, Args... args) {
    auto original = trampoline<void (*)(void *, Args...)>(&status_original[Entry]);
    Slot *slot = processing;
    azsdk_load_sample before {};
    unsigned d = 0;
    bool track = false, hot = false;
    uintptr_t copy_source = 0;
    uint32_t before_copy = 0;
    azn_hot_link origin {};
    uint64_t file_row = 0;
    if (slot && (state().enabled.load(std::memory_order_acquire) & AZEXIT_PASSIVE_STATUS)) {
        d = unsigned(slot - state().decks);
        uintptr_t retained, active;
        uintptr_t object = reinterpret_cast<uintptr_t>(self);
        if (status_properties(*slot, retained, active)) {
            track = object == retained || object == active;
            if (!track && object != editing_hot && (azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_CUES)))
                hot = status_hot_member(*slot, object);
            if ((track || hot) && !azsdk_load_read(int(d), &before)) track = hot = false;
            if (Entry == 0 && track) {
                copy_source = first_source(args...);
                before_copy = unsigned(integer(object + 8));
                if (object == active) origin = copied_hot(*slot, before.load_generation, copy_source);
            }
        }
    }
    ReloopEdit lock_edit(track && reloop_state[d].retained.load(std::memory_order_acquire) == reinterpret_cast<uintptr_t>(self)
                             ? &reloop_state[d]
                             : nullptr);
    original(self, args...);
    lock_edit.end();
    if (track) {
        if (origin.state) {
            const auto after = copied_hot(*slot, before.load_generation, copy_source);
            if (after.stamp != origin.stamp || after.provider != origin.provider || after.slot != origin.slot ||
                unsigned(integer(reinterpret_cast<uintptr_t>(self) + 8)) != before_copy + 1u) origin = {};
        }
#ifdef AZ_STATUS_MEMORY_PRODUCER
        if (Entry == 0 && !origin.state && copy_source && slot->core.load(std::memory_order_acquire))
            origin = azn_memory_copied(d, before.invalidation_epoch, before.load_generation, word(slot->facade + 0x68),
                                       reinterpret_cast<uintptr_t>(self), copy_source, before_copy, &file_row);
#endif
        status_loop_capture(d, *slot, reinterpret_cast<uintptr_t>(self), &before, &origin, file_row);
    }
    if (hot) status_hot_capture(d, *slot, reinterpret_cast<uintptr_t>(self), &before);
}

static bool status_install_writers(State &s, int build, azexit_install_fn install) {
    using A = uintptr_t;
    void *hooks[] = {
        reinterpret_cast<void *>(status_writer<0, A, A>),
        reinterpret_cast<void *>(status_writer<1>),
        reinterpret_cast<void *>(status_writer<2, A, A, A, A>),
        reinterpret_cast<void *>(status_writer<3, A, A, A, A, A>),
        reinterpret_cast<void *>(status_writer<4, A, A, A, A, A, A>),
        reinterpret_cast<void *>(status_writer<5, A>),
        reinterpret_cast<void *>(status_writer<6, A>)};
    const azn_entry *entries = azn_entries[build - 1];
    for (unsigned i = 0; i < 7; ++i)
        if (std::memcmp(reinterpret_cast<const void *>(s.bias + entries[i].address),
                        entries[i].prologue, sizeof entries[i].prologue)) return false;
    for (unsigned i = 0; i < 7; ++i)
        if (!install(s.bias + entries[i].address, hooks[i], &status_original[i]) || !status_original[i]) return false;
    const auto &hot = azn_hot_edit_entries[build - 1];
    uintptr_t edit = s.bias + hot.address;
    if (!std::memcmp(reinterpret_cast<const void *>(edit), hot.prologue, sizeof hot.prologue) &&
        install(edit, reinterpret_cast<void *>(status_hot_edit), &hot_edit_original) && hot_edit_original)
        hot_metadata.store(true, std::memory_order_release);
    return true;
}
