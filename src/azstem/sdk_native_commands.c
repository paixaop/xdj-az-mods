/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aznativecommands.h"
#include "../common/aznative_c.h"
#include "../common/aznative_sync.h"
#include "../azmod/sdk_command_engine.h"
#include <sched.h>
#include <stdatomic.h>
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "native admission must not use a hidden lock");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2,
               "native generation must not use a hidden lock");
enum {
    Live = 1,
    Reader = 2,
    Retiring = 4
};
static uint64_t next_thread_token;
static uint64_t thread_cookie(void) {
    static _Thread_local uint64_t cookie;
    if (cookie) return cookie;
    uint64_t expected = __atomic_load_n(&next_thread_token, __ATOMIC_RELAXED);
    if (expected == UINT64_MAX) return 0;
    /* A TLS address/pthread_t can be reused. One CAS bounds first-use work. */
    if (!__atomic_compare_exchange_n(&next_thread_token, &expected, expected + 1, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 0;
    cookie = expected + 1;
    return cookie;
}
void *az_owner_lease_owner(const struct az_owner_lease *lease) {
    return lease->gate && lease->thread == thread_cookie() ? lease->owner : NULL;
}
uint64_t az_owner_lease_generation(const struct az_owner_lease *lease) {
    return az_owner_lease_owner(lease) ? lease->generation : 0;
}
int az_owner_lease_end(struct az_owner_lease *lease) {
    if (!lease->gate || lease->thread != thread_cookie()) return AZ_INVALID;
    struct az_owner_gate *gate = lease->gate;
    *lease = (struct az_owner_lease) {0};
    __atomic_store_n(&gate->holder, 0, __ATOMIC_RELEASE);
    __atomic_fetch_and(&gate->state, ~(unsigned) Reader, __ATOMIC_RELEASE);
    return AZ_OK;
}
void az_owner_lease_drop(struct az_owner_lease *lease) {
    /* Losing the only release handle on another thread would deadlock teardown. */
    if (lease->gate && az_owner_lease_end(lease) != AZ_OK) abort();
}
uint64_t az_owner_generation(const struct az_owner_gate *gate) {
    return __atomic_load_n(&gate->generation, __ATOMIC_ACQUIRE);
}
bool az_owner_held(const struct az_owner_gate *gate) {
    uint64_t thread = thread_cookie();
    return thread && __atomic_load_n(&gate->holder, __ATOMIC_ACQUIRE) == thread;
}
void az_owner_gate_drop(struct az_owner_gate *gate) {
    uint64_t current = az_owner_generation(gate);
    if (current && az_owner_retire(gate, current) != AZ_OK) abort();
    if (pthread_mutex_destroy(&gate->control)) abort();
}
int az_owner_publish(struct az_owner_gate *gate, void *owner, az_owner_thread_check check, uint64_t *generation) {
    if (!owner || !check || !generation) return AZ_INVALID;
    AZ_MUTEX_GUARD(lock, gate->control, false);
    if (lock.error) return AZ_FAILED;
    if (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE)) return AZ_BUSY;
    if (gate->last_generation == UINT64_MAX) return AZ_LIMIT;
    gate->owner = owner;
    gate->check = check;
    uint64_t current = ++gate->last_generation;
    __atomic_store_n(&gate->generation, current, __ATOMIC_RELEASE);
    __atomic_store_n(&gate->state, Live, __ATOMIC_RELEASE);
    *generation = current;
    return AZ_OK;
}
struct OwnerCheck {
    struct az_owner_gate *gate;
    int status;
};
static void check_owner(void *opaque) {
    struct OwnerCheck *call = opaque;
    struct az_owner_gate *gate = call->gate;
    if (!gate->check(gate->owner)) call->status = AZ_INVALID;
    else if (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & Retiring) call->status = AZ_STALE;
}
int az_owner_enter(struct az_owner_gate *gate, struct az_owner_lease *out) {
    if (out->gate) return AZ_BUSY;
    uint64_t thread = thread_cookie();
    if (!thread) return AZ_BUSY;
    unsigned expected = Live;
    if (!__atomic_compare_exchange_n(&gate->state, &expected, Live | Reader, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return expected & Reader ? AZ_BUSY : AZ_UNSUPPORTED;
    /* Native pointers are read only after admission. Retirement cannot change
     * them until the reader bit is released. */
    __atomic_store_n(&gate->holder, thread, __ATOMIC_RELEASE);
    *out = (struct az_owner_lease) {gate, gate->owner, thread, az_owner_generation(gate)};
    struct OwnerCheck call = {gate, AZ_OK};
    int status = az_native_try(check_owner, &call) ? call.status : AZ_FAILED;
    if (status != AZ_OK) az_owner_lease_end(out);
    return status;
}
int az_owner_retire(struct az_owner_gate *gate, uint64_t expected_generation) {
    if (!expected_generation) return AZ_INVALID;
    uint64_t thread = thread_cookie();
    if (thread && __atomic_load_n(&gate->holder, __ATOMIC_ACQUIRE) == thread) {
        if (az_owner_generation(gate) != expected_generation) return AZ_STALE;
        __atomic_fetch_or(&gate->state, Retiring, __ATOMIC_ACQ_REL);
        return AZ_BUSY;
    }
    AZ_MUTEX_GUARD(lock, gate->control, false);
    if (lock.error) return AZ_FAILED;
    if (az_owner_generation(gate) != expected_generation) return AZ_STALE;
    __atomic_fetch_or(&gate->state, Retiring, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & Reader) sched_yield();
    gate->owner = NULL;
    gate->check = NULL;
    __atomic_store_n(&gate->generation, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gate->state, 0, __ATOMIC_RELEASE);
    return AZ_OK;
}
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
uint64_t az_owner_test_thread_token(void) { return thread_cookie(); }
void az_owner_test_last_thread_token(void) { __atomic_store_n(&next_thread_token, UINT64_MAX - 1, __ATOMIC_RELAXED); }
#endif
static int unavailable_arguments(void *context, const az_command_request *request, const struct scmd_target *target) {
    (void) context;
    (void) request;
    (void) target;
    return AZ_UNSUPPORTED;
}
static int unavailable_begin(void *context, const az_command_request *request, struct scmd_target *target) {
    (void) context;
    (void) request;
    (void) target;
    return AZ_UNSUPPORTED;
}
static int unavailable_dispatch(void *context, const az_command_request *request, uint64_t token, struct scmd_completion *completion) {
    (void) context;
    (void) request;
    (void) token;
    (void) completion;
    return AZ_UNSUPPORTED;
}
static void unavailable_end(void *context, uint32_t deck) {
    (void) context;
    (void) deck;
}
void azsdk_native_commands_adapter(struct scmd_adapter *out) {
    if (out) *out = (struct scmd_adapter) {NULL, unavailable_arguments, unavailable_begin, unavailable_dispatch, unavailable_end};
}
uint64_t azsdk_native_commands_capabilities(void) { return 0; }
