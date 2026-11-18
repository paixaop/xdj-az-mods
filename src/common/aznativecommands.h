/* SPDX-License-Identifier: GPL-3.0-only */
/* Private boundary. No native command backend is certified by this interface. */
#ifndef AZNATIVECOMMANDS_H
#define AZNATIVECOMMANDS_H
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
struct scmd_adapter;
__attribute__((visibility("hidden"))) void azsdk_native_commands_adapter(struct scmd_adapter *out);
__attribute__((visibility("hidden"))) uint64_t azsdk_native_commands_capabilities(void);
/* publish's caller owns the complete native object. Only successful retirement
 * permits destruction; the gate outlives all leases. Admission is bounded and
 * lock-free. Retirement waits for a different thread's lease and must not run
 * in playback/draw callbacks. Admission alone proves no queue or load identity.
 * These atomically accessed scalar fields keep the same C ABI for all callers. */
typedef bool (*az_owner_thread_check)(void *);
struct az_owner_gate {
    unsigned state;
    uint64_t generation, holder;
    pthread_mutex_t control;
    uint64_t last_generation;
    void *owner;
    az_owner_thread_check check;
};
struct az_owner_lease {
    struct az_owner_gate *gate;
    void *owner;
    uint64_t thread, generation;
};
#define AZ_OWNER_GATE_INIT {0, 0, 0, PTHREAD_MUTEX_INITIALIZER, 0, NULL, NULL}
#define AZ_OWNER_GATE(name) struct az_owner_gate name __attribute__((cleanup(az_owner_gate_drop))) = AZ_OWNER_GATE_INIT
#define AZ_OWNER_LEASE(name) struct az_owner_lease name __attribute__((cleanup(az_owner_lease_drop))) = {NULL, NULL, 0, 0}
/* Output is unchanged on failure. Generations and thread identities never wrap. */
__attribute__((visibility("hidden"))) int az_owner_publish(struct az_owner_gate *, void *, az_owner_thread_check, uint64_t *);
__attribute__((visibility("hidden"))) int az_owner_enter(struct az_owner_gate *, struct az_owner_lease *);
/* Reentrant retirement closes admission and returns AZ_BUSY. End the lease and
 * retry before destroying. A stale generation cannot retire a replacement. */
__attribute__((visibility("hidden"))) int az_owner_retire(struct az_owner_gate *, uint64_t);
__attribute__((visibility("hidden"))) uint64_t az_owner_generation(const struct az_owner_gate *);
__attribute__((visibility("hidden"))) bool az_owner_held(const struct az_owner_gate *);
__attribute__((visibility("hidden"))) void az_owner_gate_drop(struct az_owner_gate *);
/* Wrong-thread end leaves the lease held. Owner/generation are hidden on a
 * foreign thread. Keep no native pointer beyond this lease's lifetime. */
__attribute__((visibility("hidden"))) void *az_owner_lease_owner(const struct az_owner_lease *);
__attribute__((visibility("hidden"))) uint64_t az_owner_lease_generation(const struct az_owner_lease *);
__attribute__((visibility("hidden"))) int az_owner_lease_end(struct az_owner_lease *);
__attribute__((visibility("hidden"))) void az_owner_lease_drop(struct az_owner_lease *);
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
uint64_t az_owner_test_thread_token(void);
void az_owner_test_last_thread_token(void);
#endif
#endif
