/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azstatus_native.h"
#include "../common/azstatus_memory.h"
#include "../common/azloadobserver.h"
#include "../azmod/azmod-mod.h"
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

enum {
    AZN_CAPACITY = 256,
    AZN_LANES = 21,
    AZN_HOT_LANE = 5
};
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2, "native status requires lock-free 64-bit receipts");
struct azn_cell {
    uint64_t generation, order;
    struct azstatus_native_receipt receipt;
};
struct azn_lane {
    _Atomic uint64_t head, tail, writer, pending_loss, generation;
    struct azn_cell cells[AZN_CAPACITY];
};
static struct {
    _Atomic uint64_t order, seed, interest, raw_state;
    _Atomic unsigned memory_requested;
    _Atomic unsigned caps;
    struct azn_lane lanes[AZN_LANES]; /* lifecycle, cue owner, LoadedTrackInfo, motion, play receipt */
    struct azn_hot_details hot_details[AZN_CAPACITY]; /* sealed by the corresponding hot lane cell */
} azn_decks[4];
#ifdef AZSDK_STATUS_LANE_TEST
extern void azn_test_reserved(unsigned deck, unsigned kind);
extern void azn_test_scan(unsigned deck, unsigned lane) __attribute__((weak));
#endif
static _Atomic int azn_fd_plus_one;
static void azn_wake(void) {
    int fd = atomic_load_explicit(&azn_fd_plus_one, memory_order_acquire) - 1;
    if (fd >= 0) {
        uint64_t one = 1;
        ssize_t result = write(fd, &one, sizeof one);
        if (result < 0 && errno == EINTR) {
            result = write(fd, &one, sizeof one);
        }
        (void) result;
    }
}
void azn_notify(void) { azn_wake(); }
static void azn_record_gap(struct azn_lane *q, uint64_t topics) {
    /* An unsealed competing write from this same lane cannot straddle a
     * recovery cut. Include its routes in the gap if it must be discarded. */
    topics |= atomic_load_explicit(&q->writer, memory_order_acquire);
    atomic_fetch_or_explicit(&q->pending_loss, topics, memory_order_release);
    azn_wake();
}
void azn_map_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[13],
                                 AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) |
                                     AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
}
uint64_t azn_reloop_cut(unsigned deck) {
    return deck < 4 ? atomic_load_explicit(&azn_decks[deck].lanes[19].generation, memory_order_acquire) : 0;
}
void azn_reloop_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[19], AZ_STATUS_TOPIC(AZ_STATUS_LOOP));
}
void azn_hot_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[5], AZ_STATUS_TOPIC(AZ_STATUS_CUES));
}
void azn_rate_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[14], AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
}
void azn_transport_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[17], AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
}
void azn_key_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[10], AZ_STATUS_TOPIC(AZ_STATUS_KEY));
}
void azn_sync_role_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[15], AZ_STATUS_TOPIC(AZ_STATUS_SYNC));
}
void azn_slip_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[11], AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
}
void azn_duration_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[20],
                                 AZ_STATUS_TOPIC(AZ_STATUS_METADATA) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION));
}
void azn_end_gap(unsigned deck) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[18],
                                 AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION));
}
void azn_motion_gap(unsigned deck, int reposition) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[3], AZ_STATUS_TOPIC(AZ_STATUS_POSITION) |
                                                                AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION) |
                                                                (reposition ? AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) : 0));
}
struct azn_route {
    unsigned lane;
    uint64_t topics;
};
/* Receipt kinds and loss lanes share one routing definition. Unlisted kinds
 * have no topics and cannot reserve a writer. */
static const struct azn_route azn_routes[] = {
    [AZN_LOAD] = {0, AZ_STATUS_ALL_V1},
    [AZN_LOOP] = {1, AZ_STATUS_TOPIC(AZ_STATUS_LOOP)},
    [AZN_METADATA] = {2, AZ_STATUS_TOPIC(AZ_STATUS_METADATA)},
    [AZN_MOTION] = {3, AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION)},
    [AZN_PLAY] = {4, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)},
    [AZN_HOT_CUES] = {AZN_HOT_LANE, AZ_STATUS_TOPIC(AZ_STATUS_CUES)},
    [AZN_STEMS] = {6, AZ_STATUS_TOPIC(AZ_STATUS_STEMS)},
    [AZN_QUANTIZE] = {7, AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS)},
    [AZN_KEY_LOCK] = {8, AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS)},
    [AZN_SYNC] = {9, AZ_STATUS_TOPIC(AZ_STATUS_SYNC)},
    [AZN_KEY_SHIFT] = {10, AZ_STATUS_TOPIC(AZ_STATUS_KEY)},
    [AZN_SLIP_HEAD] = {11, AZ_STATUS_TOPIC(AZ_STATUS_POSITION)},
    [AZN_MEMORY] = {12, AZ_STATUS_TOPIC(AZ_STATUS_CUES)},
    [AZN_MAPS] = {13, AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION)},
    [AZN_RATE] = {14, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)},
    [AZN_SYNC_ROLE] = {15, AZ_STATUS_TOPIC(AZ_STATUS_SYNC)},
    [AZN_SLIDER_LOCK] = {16, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO)},
    [AZN_TRANSPORT_STATE] = {17, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT)},
    [AZN_END] = {18, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION)},
    [AZN_RELOOP] = {19, AZ_STATUS_TOPIC(AZ_STATUS_LOOP)},
    [AZN_DURATION] = {20, AZ_STATUS_TOPIC(AZ_STATUS_METADATA) | AZ_STATUS_TOPIC(AZ_STATUS_POSITION)}};
void azn_setting_gap(unsigned deck, unsigned kind) {
    if (deck >= 4) return;
    if (kind != AZN_QUANTIZE && kind != AZN_KEY_LOCK && kind != AZN_SYNC && kind != AZN_SLIDER_LOCK) return;
    const struct azn_route *route = &azn_routes[kind];
    azn_record_gap(&azn_decks[deck].lanes[route->lane], route->topics);
}
void azn_gap(unsigned deck, uint64_t topics) {
    if (deck < 4) azn_record_gap(&azn_decks[deck].lanes[1], topics);
}
static uint64_t azn_receipt_topics(const struct azstatus_native_receipt *r, uint64_t topics) {
    if (r->kind == AZN_LOOP && r->b) return topics | AZ_STATUS_TOPIC(AZ_STATUS_CUES);
    if (r->kind != AZN_MOTION) return topics;
    if (r->a == AZN_WRAP || r->a == AZN_JUMP || r->a == AZN_SEEK || r->history_only)
        topics |= AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT);
    return topics;
}
static int azn_receipt_timestamp(struct azstatus_native_receipt *r) {
    if (r->observed_ns) return 1;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    r->observed_ns = (uint64_t) ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
    return 1;
}
static int azn_offer_failed(struct azn_lane *q, uint64_t topics) {
    azn_record_gap(q, topics);
    atomic_store_explicit(&q->writer, 0, memory_order_release);
    return 0;
}
static int azn_seal_receipt(struct azn_lane *q, uint64_t pos, uint64_t generation,
                            struct azstatus_native_receipt *r, const struct azn_hot_receipt *hot) {
    r->size = hot ? sizeof *hot : sizeof *r;
    r->version = 1;
    struct azn_cell *c = &q->cells[pos % AZN_CAPACITY];
    c->receipt = *r;
    if (hot) azn_decks[r->deck].hot_details[pos % AZN_CAPACITY] = hot->details;
    c->generation = generation;
#ifdef AZSDK_STATUS_LANE_TEST
    azn_test_reserved(r->deck, r->kind);
#endif
    c->order = atomic_fetch_add_explicit(&azn_decks[r->deck].order, 1, memory_order_release);
    /* The reservation stays private until the entire cell is sealed. A writer
     * parked here never blocks draining another lane or announcing final loss. */
    atomic_store_explicit(&q->head, pos + 1, memory_order_release);
    atomic_store_explicit(&q->writer, 0, memory_order_release);
    azn_wake();
    return 1;
}
static int azn_offer_receipt(struct azstatus_native_receipt *r, const struct azn_hot_receipt *hot) {
    if (r->deck >= 4) return 0;
    if (r->kind >= sizeof azn_routes / sizeof *azn_routes || !azn_routes[r->kind].topics) return 0;
    const struct azn_route *route = &azn_routes[r->kind];
    const unsigned index = route->lane;
    const uint64_t topics = azn_receipt_topics(r, route->topics);
    struct azn_lane *q = &azn_decks[r->deck].lanes[index];
    uint64_t generation = atomic_load_explicit(&q->generation, memory_order_acquire);
    uint64_t idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&q->writer, &idle, topics, memory_order_acq_rel, memory_order_acquire)) {
        azn_record_gap(q, topics | idle);
        return 0;
    }
    uint64_t pos = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (pos == UINT64_MAX || pos - atomic_load_explicit(&q->tail, memory_order_acquire) >= AZN_CAPACITY ||
        atomic_load_explicit(&q->pending_loss, memory_order_acquire)) return azn_offer_failed(q, topics);
    if (!azn_receipt_timestamp(r)) return azn_offer_failed(q, topics);
    return azn_seal_receipt(q, pos, generation, r, hot);
}
int azn_offer(struct azstatus_native_receipt *r) { return azn_offer_receipt(r, NULL); }
void azn_emit(struct azstatus_native_receipt *r) { (void) azn_offer(r); }
void azn_emit_hot(struct azn_hot_receipt *r) { (void) azn_offer_receipt(&r->base, r); }
static int azn_copy_receipt(struct azstatus_native_receipt *out, const struct azstatus_native_receipt *value) {
    size_t size = out->size < sizeof *value ? out->size : sizeof *value;
    memcpy(out, value, size);
    out->size = (uint32_t) size;
    return 1;
}
static int azn_cell_readable(const struct azn_cell *cell, uint64_t generation, size_t capacity) {
    if (cell->generation != generation) return 0;
#ifdef AZ_STATUS_MAPS_PRODUCER
    if (cell->receipt.kind == AZN_MAPS && capacity < offsetof(struct azstatus_native_receipt, map) + sizeof cell->receipt.map) return 0;
#endif
#ifdef AZ_STATUS_MEMORY_PRODUCER
    if (cell->receipt.kind == AZN_MEMORY && capacity < offsetof(struct azstatus_native_receipt, memory_serial) + sizeof cell->receipt.memory_serial) return 0;
#endif
    (void) capacity;
    return 1;
}
static void azn_release_cell(const struct azn_cell *cell) {
    /* This reservation resumed after its scoped recovery cut. Its storage
     * was never reused while the producer still owned it. */
#ifdef AZ_STATUS_MEMORY_PRODUCER
    if (cell->receipt.kind == AZN_MEMORY && cell->receipt.memory_pin)
        azn_memory_service()->release(cell->receipt.memory_pin);
#endif
#ifdef AZ_STATUS_MAPS_PRODUCER
    if (cell->receipt.kind == AZN_MAPS && azn_maps_service()) azn_maps_service()->release(&cell->receipt.map);
#endif
    (void) cell;
}
struct azn_selection {
    struct azn_lane *lane;
    uint64_t order, position;
};
static void azn_select_cell(struct azn_selection *selected, struct azn_lane *lane, uint64_t pos) {
    const struct azn_cell *cell = &lane->cells[pos % AZN_CAPACITY];
    if (!selected->lane || cell->order < selected->order)
        *selected = (struct azn_selection) {lane, cell->order, pos};
}
static int azn_copy_gap(unsigned deck, unsigned index, struct azn_lane *lane, struct azstatus_native_receipt *out) {
    static const unsigned kinds[AZN_LANES] = {
        [3] = AZN_MOTION, [5] = AZN_HOT_CUES, [7] = AZN_QUANTIZE, [8] = AZN_KEY_LOCK, [9] = AZN_SYNC, [10] = AZN_KEY_SHIFT, [11] = AZN_SLIP_HEAD, [12] = AZN_MEMORY, [13] = AZN_MAPS, [14] = AZN_RATE, [15] = AZN_SYNC_ROLE, [16] = AZN_SLIDER_LOCK, [17] = AZN_TRANSPORT_STATE, [18] = AZN_END, [19] = AZN_RELOOP, [20] = AZN_DURATION};
    atomic_fetch_add_explicit(&lane->generation, 1, memory_order_acq_rel);
    uint64_t loss = atomic_exchange_explicit(&lane->pending_loss, 0, memory_order_acq_rel);
    atomic_fetch_add_explicit(&azn_decks[deck].seed, 1, memory_order_release);
    const struct azstatus_native_receipt gap = {.size = sizeof gap, .version = 1, .kind = AZN_GAP, .deck = deck, .lost_topics = loss, .b = kinds[index]};
    return azn_copy_receipt(out, &gap);
}
/* -1 retries this lane after a writer sealed a previously unseen entry.
 * 1 is a delivered gap; 0 leaves the earliest readable cell in selected. */
static int azn_scan_lane(unsigned deck, unsigned index, struct azstatus_native_receipt *out, struct azn_selection *selected) {
#ifdef AZSDK_STATUS_LANE_TEST
    if (azn_test_scan) azn_test_scan(deck, index);
#endif
    struct azn_lane *lane = &azn_decks[deck].lanes[index];
    uint64_t pos = atomic_load_explicit(&lane->tail, memory_order_relaxed);
    uint64_t end = atomic_load_explicit(&lane->head, memory_order_acquire);
    uint64_t generation = atomic_load_explicit(&lane->generation, memory_order_relaxed);
    while (pos < end) {
        struct azn_cell *cell = &lane->cells[pos % AZN_CAPACITY];
        if (azn_cell_readable(cell, generation, out->size)) {
            azn_select_cell(selected, lane, pos);
            break;
        }
        azn_release_cell(cell);
        atomic_store_explicit(&lane->tail, ++pos, memory_order_release);
    }
    if (pos != end || !atomic_load_explicit(&lane->pending_loss, memory_order_acquire)) return 0;
    /* Consume any entry sealed before the failing writer declared loss.
     * An in-flight entry on this lane is already named in pending_loss. */
    if (atomic_load_explicit(&lane->head, memory_order_acquire) != end) return -1;
    return azn_copy_gap(deck, index, lane, out);
}
static int azn_scan_lanes(unsigned deck, struct azstatus_native_receipt *out, struct azn_selection *selected) {
    *selected = (struct azn_selection) {NULL, UINT64_MAX, 0};
    for (unsigned i = 0; i < AZN_LANES; ++i) {
        int result = azn_scan_lane(deck, i, out, selected);
        if (result > 0) return 1;
        if (result < 0) --i;
    }
    return 0;
}
static int azn_take_memory_seed(unsigned deck, struct azstatus_native_receipt *out) {
#ifdef AZ_STATUS_MEMORY_PRODUCER
    if (out->size >= offsetof(struct azstatus_native_receipt, memory_serial) + sizeof out->memory_serial &&
        atomic_exchange_explicit(&azn_decks[deck].memory_requested, 0, memory_order_acq_rel)) {
        struct azstatus_native_receipt seed = {.size = sizeof seed, .version = 1};
        if (azn_memory_service()->seed(deck, &seed)) return azn_copy_receipt(out, &seed);
    }
#else
    (void) deck;
    (void) out;
#endif
    return 0;
}
static int azn_take_selected(unsigned deck, const struct azn_selection *selected, struct azstatus_native_receipt *out) {
    const struct azstatus_native_receipt *row = &selected->lane->cells[selected->position % AZN_CAPACITY].receipt;
    int hot = row->kind == AZN_HOT_CUES && row->size == sizeof(struct azn_hot_receipt) && out->size >= row->size;
    azn_copy_receipt(out, row);
    if (hot) {
        struct azn_hot_receipt *packet = (struct azn_hot_receipt *) out;
        packet->details = azn_decks[deck].hot_details[selected->position % AZN_CAPACITY];
        out->size = sizeof *packet;
    }
    atomic_store_explicit(&selected->lane->tail, selected->position + 1, memory_order_release);
    return 1;
}
static int azn_take_ordered(unsigned deck, struct azstatus_native_receipt *out) {
    uint64_t cut = atomic_load_explicit(&azn_decks[deck].order, memory_order_acquire);
    struct azn_selection selected;
    if (azn_scan_lanes(deck, out, &selected)) return 1;
    if (!selected.lane) return azn_take_memory_seed(deck, out);
    /* If this candidate was sealed during the scan, a preceding commit on an
     * already visited lane may now be visible. Acquiring the candidate's head
     * observes prior commits at a common source. Scan once more while its
     * slot is still owned by the reader, without waiting for any writer. */
    if (selected.order >= cut) {
        if (azn_scan_lanes(deck, out, &selected)) return 1;
        if (!selected.lane) return azn_take_memory_seed(deck, out);
    }
    return azn_take_selected(deck, &selected, out);
}
static int azn_take(unsigned deck, struct azstatus_native_receipt *out) {
    if (deck >= 4 || !out || out->size < offsetof(struct azstatus_native_receipt, content_id) || out->version != 1) return 0;
#ifdef AZ_STATUS_MEMORY_PRODUCER
    azn_memory_maintenance();
#endif
    return azn_take_ordered(deck, out);
}
uint64_t azn_interest(unsigned deck) {
    return deck < 4 ? atomic_load_explicit(&azn_decks[deck].interest, memory_order_acquire) : 0;
}
uint64_t azn_seed_id(unsigned deck) {
    return atomic_load_explicit(&azn_decks[deck].seed, memory_order_acquire);
}
uint64_t azn_cue_loss_cut(unsigned deck, int hot) {
    return deck < 4 ? atomic_load_explicit(&azn_decks[deck].lanes[hot ? 5 : 1].generation, memory_order_acquire) : 0;
}
static void azn_set_interest(unsigned deck, uint64_t topics) {
    if (deck >= 4) return;
    /* After enrollment keep the qualified reducer baseline fresh for later subscribers. */
    uint64_t prior = atomic_fetch_or_explicit(&azn_decks[deck].interest, topics, memory_order_acq_rel);
    if (topics & ~prior) atomic_fetch_add_explicit(&azn_decks[deck].seed, 1, memory_order_release);
#ifdef AZ_STATUS_MAPS_PRODUCER
    if ((topics & ~prior) & (AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) |
                             AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION))) {
        const struct azn_maps_api *maps = azn_maps_service();
        if (maps) maps->seed(deck);
    }
#endif
#ifdef AZ_STATUS_MEMORY_PRODUCER
    if ((topics & ~prior) & AZ_STATUS_TOPIC(AZ_STATUS_CUES)) {
        atomic_store_explicit(&azn_decks[deck].memory_requested, 1, memory_order_release);
        azn_wake();
    }
#endif
}
static int azn_eventfd(void) {
    int current = atomic_load_explicit(&azn_fd_plus_one, memory_order_acquire);
    if (current) return current - 1;
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) return -1;
    if (!atomic_compare_exchange_strong_explicit(&azn_fd_plus_one, &current, fd + 1,
                                                 memory_order_release, memory_order_acquire)) {
        close(fd);
        return current - 1;
    }
    azn_wake(); /* cover commits made before attaching the eventfd */
    return fd;
}
static uint32_t azn_capabilities(unsigned deck) {
    if (deck >= 4) return 0;
    uint64_t raw = atomic_load_explicit(&azn_decks[deck].raw_state, memory_order_acquire);
    return atomic_load_explicit(&azn_decks[deck].caps, memory_order_acquire) |
           (raw && !(raw & 1) ? AZN_CAP_PLAY_RAW : 0);
}
void azn_raw_state(unsigned deck, uint64_t generation, int retired) {
    if (deck >= 4 || !generation || generation > UINT64_MAX / 2) return;
    uint64_t next = 2 * generation + (retired != 0);
    uint64_t old = atomic_load_explicit(&azn_decks[deck].raw_state, memory_order_acquire);
    /* An old notification completing after retire cannot rearm the stream. */
    while (old < next)
        if (atomic_compare_exchange_weak_explicit(&azn_decks[deck].raw_state, &old, next,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            azn_wake();
            return;
        }
}
void azn_capability(unsigned deck, unsigned flags, int ready) {
    if (deck >= 4) return;
    unsigned prior = ready ? atomic_fetch_or_explicit(&azn_decks[deck].caps, flags, memory_order_release) : atomic_fetch_and_explicit(&azn_decks[deck].caps, ~flags, memory_order_release);
    if (ready ? (prior & flags) != flags : (prior & flags) != 0) azn_wake();
}
static int azn_load_seed(unsigned deck, struct azstatus_native_receipt *out) {
    struct azsdk_load_sample load;
    if (!out || out->version != 1 || out->size < offsetof(struct azstatus_native_receipt, content_id)) return 0;
    if (!azsdk_load_read((int) deck, &load)) {
        struct azstatus_native_receipt empty;
        return azn_empty_seed(deck, &empty) && azn_copy_receipt(out, &empty);
    }
    struct azstatus_native_receipt seed = {.size = sizeof seed, .version = 1, .kind = AZN_LOAD, .deck = deck, .a = AZN_LOADED, .valid = 1, .attempt = load.native_request_id, .unit = load.unit_generation, .epoch = load.invalidation_epoch, .load_generation = load.load_generation, .observed_ns = load.observed_ns, .source_pin = azn_load_source_pin(deck, load.invalidation_epoch, load.load_generation), .source_kind = azn_load_kind(deck, load.invalidation_epoch, load.load_generation)};
    seed.original_bpm_state = azn_load_original_bpm(deck, load.unit_generation, load.invalidation_epoch,
                                                    load.load_generation, &seed.original_bpm);
    return azn_copy_receipt(out, &seed);
}
#include "sdk_status_stems.inc"
#include "../common/azanalysis_native.h"
__attribute__((visibility("default"))) const struct azstatus_native_v1 *azstem_status_service_v1(void) {
    static const struct azstatus_native_v1 api = {sizeof api, 1, azn_eventfd, azn_take, azn_load_seed, azn_set_interest, azn_capabilities, azn_metadata_seed, azn_register_source_capture, azn_stem_bind, azn_stems_seed, azn_install_settings,
#ifdef AZ_STATUS_MEMORY_PRODUCER
                                                  azn_memory_service,
#else
                                                  NULL,
#endif
#ifdef AZ_STATUS_MAPS_PRODUCER
                                                  azn_maps_service,
#else
                                                  NULL,
#endif
                                                  azn_install_terminal,
#ifdef AZ_STATUS_MEMORY_PRODUCER
                                                  azn_file_service,
#else
                                                  NULL,
#endif
                                                  azn_load_track_matches,
                                                  azn_analysis_sources};
    return &api;
}
