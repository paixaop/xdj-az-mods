/* SPDX-License-Identifier: GPL-3.0-only */
/* Host invokes only the original-frame boundary double. Entry0/firmware ABI,
 * source ownership and native Slip meaning are not supplied by this fixture. */
#include "capture.h"
#include "../../src/common/aztimestamp.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unwind.h>
typedef struct azce_call azce_call;
typedef struct slip_capture_record slip_capture_record;
typedef struct slip_capture_stats slip_capture_stats;

/* Generate a foreign native unwind from C; the assembly ABI boundary owns cleanup. */
static unsigned exception_deletions;
static void exception_cleanup(_Unwind_Reason_Code reason, struct _Unwind_Exception *value)
{
    (void)reason;
    ++exception_deletions;
    free(value);
}
static __attribute__((noreturn)) void native_unwind(void)
{
    struct _Unwind_Exception *value = calloc(1, sizeof *value);
    assert(value);
    value->exception_class = UINT64_C(0x415a544553544300);
    value->exception_cleanup = exception_cleanup;
    (void)_Unwind_RaiseException(value);
    abort();
}
int fixture_native_try(void (*call)(void *), void *context);

#ifdef SLIP_CAPTURE_FORWARD_ONLY
void *azdiag_slip_entry_originals[AZCE_KIND_COUNT];
int slip_capture_arm(uintptr_t pc)
{
    static uintptr_t armed;
    if (!pc || armed) return 0;
    armed = pc;
    return 1;
}
int slip_capture_read(uint64_t sequence, slip_capture_record *out)
{
    (void)sequence;
    (void)out;
    return 0;
}
void slip_capture_get_stats(slip_capture_stats *out)
{
    if (out) *out = (slip_capture_stats){ 0, 0, 0 };
}
void azdiag_slip_dispatch_call(azce_call *call)
{
    if (call) azdiag_slip_invoke_original(call);
}

#endif

#if defined(__aarch64__) && defined(__linux__) && !defined(SLIP_CAPTURE_HOST_BOUNDARY) && !defined(SLIP_CAPTURE_FORWARD_ONLY)
/* One affected entry only. CFI preserves exception propagation across this
 * real BL caller, entry0 and the qualified invoke-original assembly bridge. */
typedef struct {
    uint64_t low, high;
} ArmResult;
typedef struct {
    uintptr_t receiver;
    uint32_t raw;
    unsigned calls;
    bool throwing;
} ArmCall;
#define ARM_CALL(name) ArmCall name = { .receiver = 1, .raw = 255 }
typedef ArmResult (*ArmFunction)(uintptr_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, ArmCall *);
ArmResult slip_fixture_entry_call(uintptr_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, ArmCall *);
extern char slip_fixture_return_pc;
void azdiag_slip_entry_0(void);
asm(
    ".text\n.p2align 2\n.global slip_fixture_entry_call\n.type slip_fixture_entry_call,%function\n"
    "slip_fixture_entry_call:\n.cfi_startproc\nstp x29,x30,[sp,#-16]!\n.cfi_def_cfa_offset 16\n"
    ".cfi_offset 29,-16\n.cfi_offset 30,-8\nmov x29,sp\n.cfi_def_cfa_register 29\n"
    "bl azdiag_slip_entry_0\n.global slip_fixture_return_pc\nslip_fixture_return_pc:\n"
    "ldp x29,x30,[sp],#16\n.cfi_def_cfa sp,0\n.cfi_restore 29\n.cfi_restore 30\nret\n"
    ".cfi_endproc\n.size slip_fixture_entry_call,.-slip_fixture_entry_call\n");
ArmResult slip_fixture_original(uintptr_t receiver, uint32_t raw, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6, ArmCall *call)
{
    assert(call && receiver == call->receiver && raw == call->raw && errno == EINTR);
    assert(a2 == UINT64_C(0x1234567800000002) && a3 == UINT64_C(0x1234567800000003) &&
           a4 == UINT64_C(0x1234567800000004) && a5 == UINT64_C(0x1234567800000005) && a6 == UINT64_C(0x1234567800000006));
    ++call->calls;
    errno = ERANGE;
    if (call->throwing) native_unwind();
    return (ArmResult){ UINT64_C(0xdeadbeef87654321), UINT64_C(0x123456789abcdef0) };
}
int slip_capture_test_clock_gettime(clockid_t clock, struct timespec *out)
{
    static unsigned tick;
    assert(clock == CLOCK_MONOTONIC);
    errno = EIO;
    out->tv_sec = 1;
    out->tv_nsec = ++tick;
    return 0;
}
static void arm_invoke(ArmCall *call, ArmFunction fn)
{
    errno = EINTR;
    ArmResult result = fn(call->receiver, call->raw, UINT64_C(0x1234567800000002), UINT64_C(0x1234567800000003),
                          UINT64_C(0x1234567800000004), UINT64_C(0x1234567800000005), UINT64_C(0x1234567800000006), call);
    assert(call->calls == 1 && errno == ERANGE && result.low == UINT64_C(0xdeadbeef87654321) && result.high == UINT64_C(0x123456789abcdef0));
}
static void arm_guarded(void *context) { arm_invoke(context, slip_fixture_entry_call); }
int main(int argc, char **argv)
{
    assert(argc == 2);
    azdiag_slip_entry_originals[0] = (void *)slip_fixture_original;
    ARM_CALL(baseline);
    arm_invoke(&baseline, slip_fixture_entry_call);
    puts("SLIP_NATIVE_FORWARD_BASELINE_OK actual ARM entry0 args/results/errno");
    fflush(stdout);
    uintptr_t caller = (uintptr_t)&slip_fixture_return_pc;
    assert(!slip_capture_arm(0) && slip_capture_arm(caller) && !slip_capture_arm(caller));
    const uint32_t values[] = { 0, 1, 255, UINT32_MAX };
    for (unsigned i = 0; i < 4; ++i) {
        ARM_CALL(call);
        call.receiver = i % 2 ? UINTPTR_MAX : 1;
        call.raw = values[i];
        arm_invoke(&call, slip_fixture_entry_call);
        slip_capture_record out = { 0 };
        assert(slip_capture_read(i + 1, &out) && "missing by-value Slip capture record after native forwarding");
        assert(out.sequence == i + 1 && out.observed_ns && out.receiver == call.receiver && out.caller == caller && out.raw == values[i] && !out.reserved);
    }
    ARM_CALL(throwing);
    throwing.throwing = true;
    assert(!fixture_native_try(arm_guarded, &throwing));
    assert(exception_deletions == 1 && throwing.calls == 1 && errno == ERANGE);
    slip_capture_record out = { 0 };
    assert(slip_capture_read(5, &out) && out.raw == 255 && out.caller == caller);
    ARM_CALL(wrong);
    arm_invoke(&wrong, (ArmFunction)azdiag_slip_entry_0);
    slip_capture_stats stats = { 0 };
    slip_capture_get_stats(&stats);
    assert(stats.saved == 5 && stats.rejected == 2 && !slip_capture_read(6, &out));
    printf("PASS slip capture %s (actual AArch64 entry0+original/CFI exception; raw observation only)\n", argv[1]);
}
#else
static const uintptr_t caller = 0x130200;
static _Atomic uint64_t ticks = 1000000000;
static unsigned clock_mode, original_calls;
static bool reentered;
typedef struct {
    azce_call call, before;
    unsigned calls;
    int entry_errno, result_errno;
    bool throwing;
} Frame;
static void frame_init(Frame *frame, uint32_t raw, uintptr_t receiver)
{
    *frame = (Frame){ .entry_errno = EINTR, .result_errno = ERANGE };
    frame->call.kind = AZCE_KIND_MESSAGE;
    frame->call.caller_link = caller;
    frame->call.caller_x21 = UINT64_C(0x8765432198765432);
    for (unsigned i = 0; i < 9; ++i) frame->call.args[i] = UINT64_C(0x9876000000) + i;
    frame->call.args[0] = receiver;
    frame->call.args[1] = UINT64_C(0xfedcba9800000000) | raw;
    frame->call.args[7] = (uintptr_t)frame;
    frame->call.original = UINT64_C(0x12345678);
    for (unsigned i = 0; i < 8; ++i) {
        frame->call.vector_args[i].words[0] = UINT64_C(0xabcdef1234000000) + i;
        frame->call.vector_args[i].words[1] = UINT64_C(0x7890123400000000) + i;
    }
    frame->before = frame->call;
}
#define FRAME(name, raw, receiver) \
    Frame name;                    \
    frame_init(&name, raw, receiver)
static void frame_check_inputs(const Frame *frame)
{
    const azce_call *call = &frame->call, *before = &frame->before;
    assert(!memcmp(call->args, before->args, sizeof call->args));
    assert(!memcmp(call->vector_args, before->vector_args, sizeof call->vector_args));
    assert(call->kind == before->kind && call->reserved == before->reserved && call->original == before->original &&
           call->caller_link == before->caller_link && call->caller_x21 == before->caller_x21 && call->caller_reserved == before->caller_reserved);
}
static void frame_results(const Frame *frame)
{
    frame_check_inputs(frame);
    assert(frame->calls == 1 && errno == frame->result_errno);
    assert(frame->call.result[0] == UINT64_C(0xdeadbeef87654321) && frame->call.result[1] == UINT64_C(0x123456789abcdef0));
    for (unsigned i = 0; i < 4; ++i) assert(frame->call.vector_results[i].words[0] == 100 + i && frame->call.vector_results[i].words[1] == 200 + i);
}
static void frame_invoke(Frame *frame)
{
    frame->before = frame->call;
    errno = frame->entry_errno;
    azdiag_slip_dispatch_call(&frame->call);
    frame_results(frame);
}
static void frame_guarded(void *context)
{
    Frame *frame = context;
    azdiag_slip_dispatch_call(&frame->call);
}
void baseline(void)
{
    FRAME(frame, 255, 1);
    frame_invoke(&frame);
    puts("SLIP_NATIVE_FORWARD_BASELINE_OK args/results/errno preserved");
    fflush(stdout);
}
slip_capture_stats stats(void)
{
    slip_capture_stats s = { 0 };
    slip_capture_get_stats(&s);
    return s;
}
void absent(uint64_t sequence)
{
    slip_capture_record out;
    memset(&out, 0xa5, sizeof out);
    slip_capture_record before = out;
    assert(!slip_capture_read(sequence, &out));
    assert(!memcmp(&out, &before, sizeof out));
}
void azdiag_slip_invoke_original(azce_call *call)
{
    assert(call);
    Frame *frame = (Frame *)call->args[7];
    assert(&frame->call == call && errno == frame->entry_errno);
    frame_check_inputs(frame);
    ++frame->calls;
    ++original_calls;
    call->result[0] = UINT64_C(0xdeadbeef87654321);
    call->result[1] = UINT64_C(0x123456789abcdef0);
    for (unsigned i = 0; i < 4; ++i) {
        call->vector_results[i].words[0] = 100 + i;
        call->vector_results[i].words[1] = 200 + i;
    }
    errno = frame->result_errno;
    if (frame->throwing) native_unwind();
}
int slip_capture_test_clock_gettime(clockid_t clock, struct timespec *out)
{
    assert(clock == CLOCK_MONOTONIC);
    errno = EIO;
    if (clock_mode == 1) return -1;
    if (clock_mode == 2) {
        out->tv_sec = -1;
        out->tv_nsec = 0;
        return 0;
    }
    if (clock_mode == 3) {
        out->tv_sec = 1;
        out->tv_nsec = 1000000000;
        return 0;
    }
    if (clock_mode == 4) {
        out->tv_sec = 0;
        out->tv_nsec = 0;
        return 0;
    }
    if (clock_mode == 5) {
        out->tv_sec = (time_t)((UINTMAX_C(1) << (sizeof(time_t) * CHAR_BIT - 1)) - 1);
        out->tv_nsec = 0;
        return 0;
    }
    if (clock_mode == 6) {
        out->tv_sec = 0;
        out->tv_nsec = 1;
        return 0;
    }
    if (clock_mode == 7 && !reentered) {
        reentered = true;
        FRAME(inner, 1, UINTPTR_MAX);
        frame_invoke(&inner);
    }
    uint64_t ns = atomic_fetch_add(&ticks, 1);
    out->tv_sec = (time_t)(ns / 1000000000);
    out->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static _Atomic bool reader_done;
static _Atomic unsigned reader_checked;
static void *ring_reader(void *context)
{
    (void)context;
    for (unsigned round = 0; round < 20000 && !atomic_load(&reader_done); ++round)
        for (uint64_t sequence = 1; sequence <= 512; ++sequence) {
            slip_capture_record out = { 0 };
            if (!slip_capture_read(sequence, &out)) continue;
            assert(out.sequence == sequence && out.raw == (uint32_t)(sequence * 31337) && out.receiver == UINTPTR_MAX - sequence &&
                   out.caller == caller && !out.reserved && out.observed_ns);
            atomic_fetch_add(&reader_checked, 1);
        }
    return NULL;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    baseline();
    assert(!slip_capture_arm(0));
    assert(slip_capture_arm(caller));
    assert(!slip_capture_arm(caller));
    assert(!slip_capture_arm(caller + 4));
    if (!strcmp(argv[1], "capture")) {
        const uint32_t raw[] = { 0, 1, 255, UINT32_MAX };
        for (unsigned i = 0; i < 4; ++i) {
            FRAME(frame, raw[i], i % 2 ? UINTPTR_MAX : 1);
            frame_invoke(&frame);
            slip_capture_record out = { 0 };
            assert(slip_capture_read(i + 1, &out) && "missing by-value Slip capture record after native forwarding");
            const int observed = compare_timestamps(out.observed_ns, UINT64_C(1000000000) + i, 1000000000, 1000000000, 1);
            assert(out.sequence == i + 1 && observed != AZ_TIME_INVALID && observed == AZ_TIME_EQUAL && out.receiver == frame.call.args[0] && out.caller == caller && out.raw == raw[i] && !out.reserved);
        }
        assert(stats().saved == 4);
        absent(0);
        absent(5);
        absent(UINT64_MAX);
        assert(!slip_capture_read(1, NULL));
        slip_capture_get_stats(NULL);
    } else if (!strcmp(argv[1], "reject")) {
        slip_capture_stats before = stats();
        FRAME(wrong, 255, 1);
        wrong.call.caller_link = caller + 4;
        frame_invoke(&wrong);
        FRAME(reserved, 255, 1);
        reserved.call.reserved = 1;
        frame_invoke(&reserved);
        FRAME(caller_reserved, 255, 1);
        caller_reserved.call.caller_reserved = 1;
        frame_invoke(&caller_reserved);
        FRAME(kind, 255, 1);
        kind.call.kind = AZCE_KIND_SELECTION;
        frame_invoke(&kind);
        assert(stats().saved == 0 && stats().rejected == before.rejected + 4);
        absent(1);
        FRAME(valid, 255, 1);
        frame_invoke(&valid);
        assert(stats().saved == 1);
    } else if (!strcmp(argv[1], "clock")) {
        for (clock_mode = 1; clock_mode <= 5; ++clock_mode) {
            FRAME(frame, 255, 1);
            frame_invoke(&frame);
            assert(!stats().saved);
            absent(1);
        }
        clock_mode = 0;
        FRAME(valid, 255, 1);
        frame_invoke(&valid);
        assert(stats().saved == 1);
        clock_mode = 6;
        FRAME(backwards, 255, 1);
        frame_invoke(&backwards);
        assert(stats().saved == 1 && stats().dropped == 6);
        absent(2);
    } else if (!strcmp(argv[1], "reentrant")) {
        clock_mode = 7;
        FRAME(frame, 255, 1);
        frame_invoke(&frame);
        assert(reentered && stats().saved == 1 && stats().dropped == 1 && original_calls == 3);
        slip_capture_record out = { 0 };
        assert(slip_capture_read(1, &out) && out.raw == 255 && out.receiver == 1);
        absent(2);
    } else if (!strcmp(argv[1], "throws")) {
        FRAME(frame, 255, 1);
        frame.throwing = true;
        errno = frame.entry_errno;
        assert(!fixture_native_try(frame_guarded, &frame));
        assert(exception_deletions == 1);
        frame_results(&frame);
        assert(stats().saved == 1);
        FRAME(second, 255, 1);
        frame_invoke(&second);
        assert(stats().saved == 2);
    } else if (!strcmp(argv[1], "ring")) {
        atomic_store(&reader_done, false);
        atomic_store(&reader_checked, 0);
        for (uint64_t sequence = 1; sequence <= 128; ++sequence) {
            FRAME(frame, (uint32_t)(sequence * 31337), UINTPTR_MAX - sequence);
            frame_invoke(&frame);
        }
        pthread_t reader;
        assert(!pthread_create(&reader, NULL, ring_reader, NULL));
        while (!atomic_load(&reader_checked)) sched_yield();
        for (uint64_t sequence = 129; sequence <= 512; ++sequence) {
            FRAME(frame, (uint32_t)(sequence * 31337), UINTPTR_MAX - sequence);
            frame_invoke(&frame);
            if (!(sequence % 8)) sched_yield();
        }
        atomic_store(&reader_done, true);
        assert(!pthread_join(reader, NULL));
        assert(stats().saved == 512);
        for (uint64_t sequence = 1; sequence <= 384; ++sequence) absent(sequence);
        for (uint64_t sequence = 385; sequence <= 512; ++sequence) {
            slip_capture_record out = { 0 };
            assert(slip_capture_read(sequence, &out) && out.sequence == sequence && out.raw == sequence * 31337);
        }
        for (unsigned i = 0; i < 5; ++i) {
            FRAME(frame, 255, 1);
            frame_invoke(&frame);
        }
        assert(stats().saved == 512 && stats().dropped == 5);
        absent(513);
        absent(UINT64_MAX);
    } else assert(!"unknown fixture mode");
    printf("PASS slip capture %s (host original-boundary double only)\n", argv[1]);
}
#endif
