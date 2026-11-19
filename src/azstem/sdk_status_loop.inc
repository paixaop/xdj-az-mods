/* SPDX-License-Identifier: GPL-3.0-only */
/* Included in the PlayerCore owner bridge. Both admitted images update the
 * CueProperty mutation counter at +8 in the leaf methods and the outer kind setter. The
 * working property is the CueContext member at +0x160. Kind zero is a separate
 * retained pointer. No callback notification or UI sampling supplies an edge. */
#include "sdk_status_loop_profiles.h"
static void *status_original[7];
static const uint64_t status_loop_route = AZ_STATUS_TOPIC(AZ_STATUS_LOOP);
#define status_retained_routes (AZ_STATUS_TOPIC(AZ_STATUS_LOOP) | AZ_STATUS_TOPIC(AZ_STATUS_CUES))

static bool status_point_precision(uintptr_t source, unsigned rate, uint64_t us) {
    const int us_range = compare_timestamps(us, UINT64_C(9007199254740992), 1000000, rate, 1);
    if (!word(source + 0x20) || !rate || rate >= 0x7fffffffu ||
        us_range == AZ_TIME_INVALID || us_range > AZ_TIME_EQUAL) return false;
    return true;
}
static azn_point status_point(uintptr_t source) {
    if (empty_position(source)) return (azn_point) {AZN_ABSENT, 0, 0};
    if (!valid_position(source)) return (azn_point) {AZN_INVALID, 0, 0};
    __auto_type rate = (unsigned) (integer(source + 0x30));
    uint64_t samples = word(source + 0x10), us = word(source + 0x38);
    if (!status_point_precision(source, rate, us)) return (azn_point) {AZN_INVALID, 0, 0};
    typedef unsigned __int128 Wide;
    const int consistent = compare_timestamps((Wide) (samples) * 1000000u, (Wide) (us) *rate,
                                              (uint64_t) (rate) * 1000000u, rate, 1);
    if (consistent == AZ_TIME_INVALID || consistent != AZ_TIME_EQUAL) return (azn_point) {AZN_INVALID, 0, 0};
    return (azn_point) {AZN_COORD, rate, ((int64_t) (samples))};
}
static void status_points(uintptr_t property, azn_point *out) {
    out[0] = status_point(property + 0x30);
    out[1] = status_point(property + 0x88);
    const int interval = compare_timestamps(out[1].samples, out[0].samples, out[1].rate, out[0].rate, 1);
    if (out[0].state == AZN_COORD && out[1].state == AZN_COORD &&
        (out[0].rate != out[1].rate || memcmp(((const void *) (property + 0x30)), ((const void *) (property + 0x88)), 12) ||
         interval == AZ_TIME_INVALID || interval != AZ_TIME_AFTER))
        out[1] = (__typeof__(out[1])) {AZN_INVALID, 0, 0};
}
/* Each property endpoint is one committed integer source-sample point. */
static bool status_same_point(const azn_point a, const azn_point b) {
    if (a.state != b.state || a.rate != b.rate) return false;
    if (a.state != AZN_COORD) return true;
    const int order = compare_timestamps(a.samples, b.samples, a.rate, a.rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static bool status_qualify_points(Slot *slot, uintptr_t property, azn_point *out) {
    bool same = true;
    uintptr_t core = atomic_load_explicit(&(slot->core), memory_order_acquire);
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t source = property + (i ? 0x88u : 0x30u);
        if (out[i].state != AZN_ABSENT && byte(source) && (byte(source + 4) & 31u) && byte(source + 6) &&
            integer(source + 8) && memcmp(((const void *) (core + 0x18)), ((const void *) (source)), 12)) {
            out[i] = (__typeof__(out[i])) {AZN_INVALID, 0, 0};
            same = false;
        }
    }
    return same;
}
static bool status_properties(Slot *slot, uintptr_t *retained, uintptr_t *active) {
    if (!coherent(slot) || !set_bindings(slot)) return false;
    uintptr_t context = word(slot->facade + 0x68);
    if (!pointer(context)) return false;
    (*retained) = word(context + 0x18);
    (*active) = context + 0x160;
    __auto_type p = &(state().profile);
    return table((*retained), p->cue_table) && (table((*active), p->cue_table) || table((*active), p->cue_base_table));
}
static bool status_source_loaded(unsigned d, Slot *slot, const azsdk_load_sample load) {
    const uintptr_t core = atomic_load_explicit(&(slot->core), memory_order_acquire);
    unsigned char source[12];
    memcpy(source, ((const void *) (core + 0x18)), sizeof source);
    return azn_load_track_matches(d, load.invalidation_epoch, load.load_generation, source) != 0;
}


#include "sdk_status_reloop.inc"


/* uintptr_t copy from the firmware's indexed uintptr_t–H property is a causal association.
 * Compare the entire bank incarnation and revision on both sides of the copy.
 * An independently constructed property with identical bounds cannot match. */
static bool copied_hot_bank_current(unsigned deck, const HotState *h, uint64_t load) {
    return h->ready && h->load == load && h->serial && h->provider && azn_cue_loss_cut(deck, 1) == h->loss_cut;
}
static bool copied_hot_property(Slot *s, const HotState *h, unsigned i, uintptr_t property) {
    __auto_type profile = &state().profile;
    if (property != h->property[i] || (!table(property, profile->cue_table) && !table(property, profile->cue_base_table)) ||
        (unsigned) integer(property + 8) != h->revision[i]) return false;
    azn_point pair[2];
    status_points(property, pair);
    status_qualify_points(s, property, pair);
    unsigned rgb = (byte(property + 0x10) << 16) | (byte(property + 0x11) << 8) | byte(property + 0x12);
    return status_same_point(pair[0], h->previous[i].in) && status_same_point(pair[1], h->previous[i].out) && rgb == h->previous[i].rgb;
}
static azn_hot_link copied_hot(Slot *s, uint64_t load, uintptr_t source) {
    azn_hot_link link = {0};
    HotState *h = &s->hot;
    if (!source || atomic_flag_test_and_set_explicit(&(h->busy), memory_order_acquire)) return link;
    EXIT_FLAG_GUARD(guard, &h->busy);
    if (!copied_hot_bank_current((unsigned) (s - state().decks), h, load)) return link;
    uintptr_t context = word(s->facade + 0x68);
    for (unsigned i = 0; i < 8; ++i) {
        const uintptr_t property = word(context + 0x20 + i * 8);
        if (!copied_hot_property(s, h, i, property)) return (azn_hot_link) {0};
        if (property == source && h->previous[i].in.state == AZN_COORD && h->previous[i].out.state == AZN_COORD)
            link = (azn_hot_link) {h->provider, h->serial, i + 1, AZN_HOT_LINK_READY};
    }
    return link;
}
static void status_working_file(Slot *slot, uint64_t token) {
    if (slot->working_file_row == token) return;
#ifdef AZ_STATUS_MEMORY_PRODUCER
    if (token) azn_file_row_hold(token, 1);
    if (slot->working_file_row) azn_file_row_hold(slot->working_file_row, 0);
#endif
    slot->working_file_row = token;
}
struct LoopCapture {
    azsdk_load_sample load;
    uintptr_t retained, active, written;
    uint32_t retained_revision, working_revision;
    uint64_t seed, cut;
    bool same_load, retained_change, working_change;
};
static bool status_loop_load(unsigned d, const azsdk_load_sample *before, azsdk_load_sample *load) {
    if (!azsdk_load_read((int) d, load)) return false;
    return !before || (before->load_generation == load->load_generation && before->unit_generation == load->unit_generation &&
                       before->invalidation_epoch == load->invalidation_epoch);
}
static void status_loop_source_lost(unsigned d, Slot *slot, uint64_t epoch) {
    const bool revoked = azn_load_history_revoked(d, epoch);
    slot->working_origin = (azn_hot_link) {0};
    status_working_file(slot, 0);
    if (atomic_exchange_explicit(&(reloop_state[d].retained), 0, memory_order_acq_rel) && !revoked) reloop_loss(d);
    if (slot->status_epoch == epoch) {
        slot->status_epoch = 0;
        if (!revoked) azn_gap(d, status_retained_routes);
    }
}
static bool status_loop_owners(unsigned d, Slot *slot, struct LoopCapture *capture) {
    if (!status_properties(slot, &capture->retained, &capture->active)) {
        if (slot->status_epoch) {
            slot->status_epoch = 0;
            azn_gap(d, status_retained_routes);
        }
        return false;
    }
    if (!status_source_loaded(d, slot, capture->load)) {
        status_loop_source_lost(d, slot, capture->load.invalidation_epoch);
        return false;
    }
    return true;
}
static void status_loop_revisions(unsigned d, Slot *slot, struct LoopCapture *capture) {
    reloop_observe(d, slot, capture->retained, capture->load.invalidation_epoch, -1, 0);
    capture->retained_revision = (unsigned) integer(capture->retained + 8);
    capture->working_revision = (unsigned) integer(capture->active + 8);
    capture->seed = azn_seed_id(d);
    capture->cut = azn_cue_loss_cut(d, 0);
    capture->same_load = slot->status_epoch == capture->load.invalidation_epoch;
    capture->retained_change = capture->retained_revision != slot->status_retained_rev || capture->retained != slot->status_retained;
    capture->working_change = capture->working_revision != slot->status_working_rev;
}
static bool status_loop_unchanged(const Slot *slot, const struct LoopCapture *capture) {
    return capture->same_load && !capture->retained_change && !capture->working_change &&
           capture->seed == slot->status_seed && capture->cut == slot->status_loop_cut;
}
static bool status_loop_memory_tail(const Slot *slot, const struct LoopCapture *capture, const azn_hot_link *copied) {
    return copied && copied->state == AZN_MEMORY_LINK_READY && capture->written == capture->active &&
           capture->retained == slot->status_retained && capture->retained_revision == slot->status_retained_rev + 1u;
}
static bool status_retained_lost(const Slot *slot, const struct LoopCapture *capture, bool memory_tail) {
    return capture->same_load && capture->retained_change && !memory_tail &&
           (capture->written != capture->retained || capture->retained != slot->status_retained || capture->retained_revision != slot->status_retained_rev + 1u);
}
static bool status_working_lost(const Slot *slot, const struct LoopCapture *capture) {
    return capture->same_load && capture->working_change &&
           (capture->written != capture->active || capture->working_revision != slot->status_working_rev + 1u);
}
static void status_loop_receipt_identity(unsigned d, const Slot *slot, const struct LoopCapture *capture,
                                         const azn_hot_link *copied, azstatus_native_receipt *receipt) {
    receipt->kind = AZN_LOOP;
    receipt->deck = d;
    receipt->epoch = capture->load.invalidation_epoch;
    receipt->unit = capture->load.unit_generation;
    receipt->load_generation = capture->load.load_generation;
    bool memory_tail = status_loop_memory_tail(slot, capture, copied);
    if (status_retained_lost(slot, capture, memory_tail)) receipt->lost_topics |= status_retained_routes;
    if (status_working_lost(slot, capture)) receipt->lost_topics |= status_loop_route;
    receipt->b = !capture->same_load || capture->retained_change || capture->seed != slot->status_seed;
}
static unsigned status_loop_reloop(const azn_point points[2]) {
    if (atomic_load_explicit(&(reloop_installed), memory_order_acquire) == 15) return 0;
    if (points[0].state == AZN_INVALID || points[1].state == AZN_INVALID) return AZN_RELOOP_STALE;
    if (points[0].state == AZN_ABSENT || points[1].state == AZN_ABSENT) return AZN_RELOOP_DENIED;
    return AZN_RELOOP_UNKNOWN; // Complete points are only the panel's first permission gate.
}
static bool status_loop_active(uintptr_t in) {
    /* The native isActiveLoopPlay predicate is current IN validity alone;
     * bounds/rate and retained slot presence are independent. */
    return byte(in) && (byte(in + 4) & 31u) && byte(in + 6) && integer(in + 8) && word(in + 16) != UINT64_C(0x7fffffffffffffff);
}
static bool status_loop_points(Slot *slot, const struct LoopCapture *capture, azstatus_native_receipt *receipt) {
    status_points(capture->retained, receipt->points);
    status_points(capture->active, receipt->points + 2);
    status_qualify_points(slot, capture->retained, receipt->points);
    receipt->reloop = status_loop_reloop(receipt->points);
    bool working_source = status_qualify_points(slot, capture->active, receipt->points + 2);
    receipt->a = status_loop_active(capture->active + 0x30);
    receipt->valid = working_source;
    if (!receipt->a) receipt->points[2] = receipt->points[3] = (azn_point) {AZN_ABSENT, 0, 0};
    return working_source;
}
static bool status_loop_link_ready(const azn_hot_link *copied) {
    return copied && (copied->state == AZN_HOT_LINK_READY || copied->state == AZN_MEMORY_LINK_READY);
}
static bool status_loop_copy_qualified(const struct LoopCapture *capture, const azn_hot_link *copied,
                                       bool working_source, const azstatus_native_receipt *receipt) {
    return status_loop_link_ready(copied) && capture->written == capture->active && working_source && receipt->a &&
           receipt->points[2].state == AZN_COORD && receipt->points[3].state == AZN_COORD && !(receipt->lost_topics & status_loop_route);
}
static void status_loop_origin(Slot *slot, const struct LoopCapture *capture, const azn_hot_link *copied,
                               uint64_t file_row, bool working_source, azstatus_native_receipt *receipt) {
    if (!capture->same_load || capture->working_change || capture->cut != slot->status_loop_cut) {
        slot->working_origin = (azn_hot_link) {0};
        uint64_t next_file = 0;
        if (status_loop_copy_qualified(capture, copied, working_source, receipt)) {
            slot->working_origin = *copied;
            next_file = file_row;
        }
        status_working_file(slot, next_file);
    }
    receipt->hot_link = slot->working_origin;
    receipt->file_row_token = slot->working_file_row;
    if (!receipt->hot_link.state) receipt->hot_link.state = working_source && !receipt->a ? AZN_HOT_LINK_EMPTY : AZN_HOT_LINK_UNKNOWN;
}
static unsigned status_property_loop_mode(uintptr_t active) {
    unsigned type = (unsigned) integer(active + 0xc), mode = (unsigned) integer(active + 0xc8);
    if (type != 3 && type != 4) return AZN_MODE_UNKNOWN;
    uint32_t beat = (unsigned) integer(active + 0xd8);
    if (mode == 0) return AZN_MODE_TIME;
    if (mode == 1) return (beat & 0xffffu) && (beat >> 16) ? AZN_MODE_BEAT : AZN_MODE_STALE;
    return AZN_MODE_UNKNOWN;
}
static unsigned status_loop_mode(uintptr_t active, bool working_source, const azstatus_native_receipt *receipt) {
    if (!working_source) return AZN_MODE_STALE;
    if (!receipt->a) return AZN_MODE_EMPTY;
    if (receipt->points[2].state != AZN_COORD || receipt->points[3].state != AZN_COORD) return AZN_MODE_STALE;
    return status_property_loop_mode(active);
}
static bool status_loop_capture_current(unsigned d, Slot *slot, const struct LoopCapture *capture) {
    azsdk_load_sample end = {0};
    if (!azsdk_load_read((int) d, &end) || capture->load.invalidation_epoch != end.invalidation_epoch ||
        capture->load.load_generation != end.load_generation || capture->load.unit_generation != end.unit_generation) return false;
    return status_source_loaded(d, slot, end);
}
static void status_loop_commit(Slot *slot, const struct LoopCapture *capture, const azstatus_native_receipt *receipt) {
    slot->status_epoch = receipt->epoch;
    slot->status_seed = capture->seed;
    slot->status_loop_cut = capture->cut;
    slot->status_retained = capture->retained;
    slot->status_retained_rev = capture->retained_revision;
    slot->status_working_rev = capture->working_revision;
}
static void status_loop_capture(unsigned d, Slot *slot, uintptr_t written, const azsdk_load_sample *before, const azn_hot_link *copied, uint64_t file_row) {
    const bool interested = (azn_interest(d) & status_retained_routes) != 0;
    if (!interested && !written) return;
    if (atomic_flag_test_and_set_explicit(&(slot->status_busy), memory_order_acquire)) {
        azn_gap(d, status_retained_routes);
        return;
    }
    EXIT_FLAG_GUARD(unlock, &slot->status_busy);
    struct LoopCapture capture = {.written = written};
    if (!status_loop_load(d, before, &capture.load) || !status_loop_owners(d, slot, &capture)) return;
    status_loop_revisions(d, slot, &capture);
    if (status_loop_unchanged(slot, &capture)) return;
    azstatus_native_receipt receipt = {0};
    status_loop_receipt_identity(d, slot, &capture, copied, &receipt);
    bool working_source = status_loop_points(slot, &capture, &receipt);
    status_loop_origin(slot, &capture, copied, file_row, working_source, &receipt);
    receipt.c = status_loop_mode(capture.active, working_source, &receipt);
    if (!status_loop_capture_current(d, slot, &capture)) return;
    status_loop_commit(slot, &capture, &receipt);
    if (interested) azn_emit(&receipt);
}


#include "sdk_status_hot.inc"


/* Preserve every incoming AAPCS64 argument used by the original. These
 * methods take integer registers only. No native C++ value crosses the bridge. */
struct StatusWrite {
    Slot *slot;
    azsdk_load_sample before;
    unsigned deck;
    bool track, hot, copying;
    uintptr_t object, copy_source;
    uint32_t before_copy;
    azn_hot_link origin;
    uint64_t file_row;
};
static void status_write_target(struct StatusWrite *write, uintptr_t retained, uintptr_t active) {
    write->track = write->object == retained || write->object == active;
    if (!write->track && write->object != editing_hot && (azn_interest(write->deck) & AZ_STATUS_TOPIC(AZ_STATUS_CUES)))
        write->hot = status_hot_member(write->slot, write->object);
    if ((write->track || write->hot) && !azsdk_load_read((int) write->deck, &write->before)) write->track = write->hot = false;
}
static struct StatusWrite status_begin_write(uintptr_t object, uintptr_t copy_source, bool copying) {
    struct StatusWrite write = {.slot = processing, .object = object, .copying = copying};
    if (!write.slot || !(atomic_load_explicit(&(state().enabled), memory_order_acquire) & AZEXIT_PASSIVE_STATUS)) return write;
    write.deck = (unsigned) (write.slot - state().decks);
    uintptr_t retained, active;
    if (!status_properties(write.slot, &retained, &active)) return write;
    status_write_target(&write, retained, active);
    if (copying && write.track) {
        write.copy_source = copy_source;
        write.before_copy = (unsigned) integer(object + 8);
        if (object == active) write.origin = copied_hot(write.slot, write.before.load_generation, copy_source);
    }
    return write;
}
static void status_confirm_copy(struct StatusWrite *write) {
    if (!write->origin.state) return;
    azn_hot_link after = copied_hot(write->slot, write->before.load_generation, write->copy_source);
    if (after.stamp != write->origin.stamp || after.provider != write->origin.provider || after.slot != write->origin.slot ||
        (unsigned) integer(write->object + 8) != write->before_copy + 1u) write->origin = (azn_hot_link) {0};
}
static void status_memory_copy(struct StatusWrite *write) {
#ifdef AZ_STATUS_MEMORY_PRODUCER
    if (write->copying && !write->origin.state && write->copy_source && atomic_load_explicit(&(write->slot->core), memory_order_acquire))
        write->origin = azn_memory_copied(write->deck, write->before.invalidation_epoch, write->before.load_generation,
                                          word(write->slot->facade + 0x68), write->object, write->copy_source, write->before_copy, &write->file_row);
#else
    (void) write;
#endif
}
static void status_finish_write(struct StatusWrite *write) {
    if (write->track) {
        status_confirm_copy(write);
        status_memory_copy(write);
        status_loop_capture(write->deck, write->slot, write->object, &write->before, &write->origin, write->file_row);
    }
    if (write->hot) status_hot_capture(write->deck, write->slot, write->object, &write->before, NULL);
}
static void status_writer_0(void *self, uintptr_t arg0, uintptr_t arg1) {
    __auto_type original = ((void (*)(void *, uintptr_t, uintptr_t)) trampoline_address(&status_original[0]));
    struct StatusWrite write = status_begin_write((uintptr_t) self, arg0, true);
    ReloopEdit lock_edit __attribute__((cleanup(reloop_edit_drop)));
    reloop_edit_init(&lock_edit, write.track && atomic_load_explicit(&(reloop_state[write.deck].retained), memory_order_acquire) == write.object
                                     ? &reloop_state[write.deck]
                                     : NULL);
    original(self, arg0, arg1);
    reloop_edit_end(&lock_edit);
    status_finish_write(&write);
}
static void status_writer_1(void *self) {
    __auto_type original = ((void (*)(void *)) trampoline_address(&status_original[1]));
    struct StatusWrite write = status_begin_write((uintptr_t) self, 0, false);
    ReloopEdit lock_edit __attribute__((cleanup(reloop_edit_drop)));
    reloop_edit_init(&lock_edit, write.track && atomic_load_explicit(&(reloop_state[write.deck].retained), memory_order_acquire) == write.object
                                     ? &reloop_state[write.deck]
                                     : NULL);
    original(self);
    reloop_edit_end(&lock_edit);
    status_finish_write(&write);
}
static void status_writer_2(void *self, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {
    __auto_type original = ((void (*)(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t)) trampoline_address(&status_original[2]));
    struct StatusWrite write = status_begin_write((uintptr_t) self, 0, false);
    ReloopEdit lock_edit __attribute__((cleanup(reloop_edit_drop)));
    reloop_edit_init(&lock_edit, write.track && atomic_load_explicit(&(reloop_state[write.deck].retained), memory_order_acquire) == write.object
                                     ? &reloop_state[write.deck]
                                     : NULL);
    original(self, arg0, arg1, arg2, arg3);
    reloop_edit_end(&lock_edit);
    status_finish_write(&write);
}
static void status_writer_3(void *self, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4) {
    __auto_type original = ((void (*)(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)) trampoline_address(&status_original[3]));
    struct StatusWrite write = status_begin_write((uintptr_t) self, 0, false);
    ReloopEdit lock_edit __attribute__((cleanup(reloop_edit_drop)));
    reloop_edit_init(&lock_edit, write.track && atomic_load_explicit(&(reloop_state[write.deck].retained), memory_order_acquire) == write.object
                                     ? &reloop_state[write.deck]
                                     : NULL);
    original(self, arg0, arg1, arg2, arg3, arg4);
    reloop_edit_end(&lock_edit);
    status_finish_write(&write);
}
static void status_writer_4(void *self, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, uintptr_t arg5) {
    __auto_type original = ((void (*)(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)) trampoline_address(&status_original[4]));
    struct StatusWrite write = status_begin_write((uintptr_t) self, 0, false);
    ReloopEdit lock_edit __attribute__((cleanup(reloop_edit_drop)));
    reloop_edit_init(&lock_edit, write.track && atomic_load_explicit(&(reloop_state[write.deck].retained), memory_order_acquire) == write.object
                                     ? &reloop_state[write.deck]
                                     : NULL);
    original(self, arg0, arg1, arg2, arg3, arg4, arg5);
    reloop_edit_end(&lock_edit);
    status_finish_write(&write);
}
static void status_writer_5(void *self, uintptr_t arg0) {
    __auto_type original = ((void (*)(void *, uintptr_t)) trampoline_address(&status_original[5]));
    struct StatusWrite write = status_begin_write((uintptr_t) self, 0, false);
    ReloopEdit lock_edit __attribute__((cleanup(reloop_edit_drop)));
    reloop_edit_init(&lock_edit, write.track && atomic_load_explicit(&(reloop_state[write.deck].retained), memory_order_acquire) == write.object
                                     ? &reloop_state[write.deck]
                                     : NULL);
    original(self, arg0);
    reloop_edit_end(&lock_edit);
    status_finish_write(&write);
}
static void status_writer_6(void *self, uintptr_t arg0) {
    __auto_type original = ((void (*)(void *, uintptr_t)) trampoline_address(&status_original[6]));
    struct StatusWrite write = status_begin_write((uintptr_t) self, 0, false);
    ReloopEdit lock_edit __attribute__((cleanup(reloop_edit_drop)));
    reloop_edit_init(&lock_edit, write.track && atomic_load_explicit(&(reloop_state[write.deck].retained), memory_order_acquire) == write.object
                                     ? &reloop_state[write.deck]
                                     : NULL);
    original(self, arg0);
    reloop_edit_end(&lock_edit);
    status_finish_write(&write);
}

static bool status_install_writers(State *s, int build, azexit_install_fn install) {

    void *hooks[] = {
        ((void *) (status_writer_0)),
        ((void *) (status_writer_1)),
        ((void *) (status_writer_2)),
        ((void *) (status_writer_3)),
        ((void *) (status_writer_4)),
        ((void *) (status_writer_5)),
        ((void *) (status_writer_6))};
    const struct azn_entry *entries = azn_entries[build - 1];
    for (unsigned i = 0; i < 7; ++i)
        if (!status_entry_matches(s->bias + entries[i].address, entries[i].prologue,
                                  sizeof entries[i].prologue, status_original[i])) return false;
    for (unsigned i = 0; i < 7; ++i)
        if (!status_install_entry(s->bias + entries[i].address, hooks[i], &status_original[i], install)) return false;
    __auto_type hot = &(azn_hot_edit_entries[build - 1]);
    uintptr_t edit = s->bias + hot->address;
    if (status_entry_matches(edit, hot->prologue, sizeof hot->prologue, hot_edit_original) &&
        status_install_entry(edit, ((void *) (status_hot_edit)), &hot_edit_original, install))
        atomic_store_explicit(&(hot_metadata), true, memory_order_release);
    return true;
}
