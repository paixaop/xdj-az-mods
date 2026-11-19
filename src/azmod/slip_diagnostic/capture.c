/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "capture.h"
#include "../../common/aztimestamp.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

enum {
    CAPTURE_SLOTS = 128,
    CAPTURE_LIMIT = 512,
    COUNTER_TRIES = 16,
    READ_TRIES = 3
};
struct capture_slot {
    _Atomic uint64_t stamp, observed_ns;
    _Atomic uintptr_t receiver, caller;
    _Atomic uint32_t raw;
};
static struct capture_slot records[CAPTURE_SLOTS];
static _Atomic uintptr_t armed_caller;
static _Atomic unsigned writer;
static _Atomic uint64_t saved, dropped, rejected;
static uint64_t last_ns; /* touched only while holding the nonblocking writer gate */

/* The exact entry bridge's original slot remains live after partial install.
 * This module neither installs an entry nor clears/replaces that original. */
void *azdiag_slip_entry_originals[AZCE_KIND_COUNT];

static void count_loss(_Atomic uint64_t *counter) {
    uint64_t value = atomic_load_explicit(counter, memory_order_relaxed);
    for (unsigned attempt = 0; attempt < COUNTER_TRIES && value != UINT64_MAX; ++attempt)
        if (atomic_compare_exchange_weak_explicit(counter, &value, value + 1,
                                                  memory_order_relaxed, memory_order_relaxed)) return;
    /* Saturate, never wrap or wait. Concurrent diagnostic counter contention
     * can undercount losses; saved remains exact under the writer gate. */
}

int slip_capture_arm(uintptr_t exact_caller_pc) {
    if (!exact_caller_pc) return 0;
    uintptr_t empty = 0;
    return atomic_compare_exchange_strong_explicit(&armed_caller, &empty, exact_caller_pc,
                                                   memory_order_release, memory_order_relaxed);
}

#ifdef SLIP_CAPTURE_TEST_CLOCK
extern int slip_capture_test_clock_gettime(clockid_t, struct timespec *);
#define capture_clock slip_capture_test_clock_gettime
#else
#define capture_clock clock_gettime
#endif
static int capture_timespec_valid(const struct timespec *now) {
    const int nonnegative = compare_timestamps(now->tv_sec, 0, 1, 1000000000, 1);
    const int subsecond = compare_timestamps(now->tv_nsec, 1000000000L, 1000000000, 1000000000, 1);
    if (nonnegative == AZ_TIME_INVALID || nonnegative == AZ_TIME_BEFORE || subsecond == AZ_TIME_INVALID || subsecond != AZ_TIME_BEFORE) return 0;
    return 1;
}

static int capture_time(uint64_t *out) {
    struct timespec now;
    if (capture_clock(CLOCK_MONOTONIC, &now)) return 0;
    if (!capture_timespec_valid(&now)) return 0;
    uint64_t seconds = (uint64_t) now.tv_sec, nanos = (uint64_t) now.tv_nsec;
    const int range = compare_timestamps(seconds, (UINT64_MAX - nanos) / UINT64_C(1000000000), 1, 1000000000, 1);
    if (range == AZ_TIME_INVALID || range == AZ_TIME_AFTER) return 0;
    uint64_t base = seconds * UINT64_C(1000000000);
    *out = base + nanos;
    const int order = compare_timestamps(*out, last_ns, 1000000000, 1000000000, 1);
    return *out && order != AZ_TIME_INVALID && order >= AZ_TIME_EQUAL;
}
#undef capture_clock

static int capture_call_valid(const struct azce_call *call) {
    uintptr_t expected_caller = atomic_load_explicit(&armed_caller, memory_order_acquire);
    return call && expected_caller && call->kind == AZCE_KIND_MESSAGE && !call->reserved &&
           !call->caller_reserved && call->caller_link == expected_caller;
}

static void capture_frame(const struct azce_call *call) {
    if (!capture_call_valid(call)) {
        count_loss(&rejected);
        return;
    }
    unsigned idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&writer, &idle, 1u,
                                                 memory_order_acquire, memory_order_relaxed)) {
        count_loss(&dropped);
        return;
    }
    uint64_t sequence = atomic_load_explicit(&saved, memory_order_relaxed), ns;
    if (sequence >= CAPTURE_LIMIT || !capture_time(&ns)) {
        atomic_store_explicit(&writer, 0u, memory_order_release);
        count_loss(&dropped);
        return;
    }
    ++sequence;
    struct capture_slot *slot = &records[(sequence - 1) % CAPTURE_SLOTS];
    /* All payload fields are atomic. Sequential consistency puts the two
     * sequence stamps around every payload access, including overwrite reads.
     * No sequence ABA is possible within the fixed512-record lifetime. */
    atomic_store(&slot->stamp, sequence * 2 + 1);
    atomic_store(&slot->observed_ns, ns);
    atomic_store(&slot->receiver, (uintptr_t) call->args[0]);
    atomic_store(&slot->caller, (uintptr_t) call->caller_link);
    atomic_store(&slot->raw, (uint32_t) call->args[1]);
    atomic_store(&slot->stamp, sequence * 2);
    last_ns = ns;
    atomic_store(&saved, sequence);
    atomic_store_explicit(&writer, 0u, memory_order_release);
}

void azdiag_slip_dispatch_call(struct azce_call *call) {
    int incoming_errno = errno;
    capture_frame(call);
    errno = incoming_errno;
    /* Gate and every capture operation finish before native forwarding.
     * No post-original work changes its result, errno or exception unwinding. */
    if (call) azdiag_slip_invoke_original(call);
}

int slip_capture_read(uint64_t sequence, struct slip_capture_record *out) {
    if (!out || !sequence || sequence > CAPTURE_LIMIT) return 0;
    const struct capture_slot *slot = &records[(sequence - 1) % CAPTURE_SLOTS];
    for (unsigned attempt = 0; attempt < READ_TRIES; ++attempt) {
        uint64_t before = atomic_load(&slot->stamp);
        if (before != sequence * 2) return 0;
        const struct slip_capture_record value = {
            sequence, atomic_load(&slot->observed_ns), atomic_load(&slot->receiver),
            atomic_load(&slot->caller), atomic_load(&slot->raw), 0};
        if (atomic_load(&slot->stamp) == before) {
            memcpy(out, &value, sizeof value);
            return 1;
        }
    }
    return 0;
}

void slip_capture_get_stats(struct slip_capture_stats *out) {
    if (!out) return;
    const struct slip_capture_stats value = {
        atomic_load(&saved), atomic_load(&dropped), atomic_load(&rejected)};
    memcpy(out, &value, sizeof value);
}
