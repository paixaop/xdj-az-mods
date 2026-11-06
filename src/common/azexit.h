/* SPDX-License-Identifier: GPL-3.0-only */
/* Private, copied-value bridge between azmod and azstem. No native pointer crosses it. */
#ifndef AZEXIT_H
#define AZEXIT_H
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include "azexitprofile.h"
#ifdef __cplusplus
extern "C" {
#define AZEXIT_NOEXCEPT noexcept
#else
#define AZEXIT_NOEXCEPT
#endif
struct azexit_request {
    uint64_t token, load_generation, deadline_ns;
    uint32_t deck, reserved;
};
/* Versioned scalar extension; seconds are signed boundary/move deltas, while
 * beat lengths are exact native uint16 rational values. Identity stays copied. */
struct azexit_loop_request {
    uint32_t size, operation;
    struct azexit_request identity;
    double position_s, end_s, beats;
};
/* Copied cue request: native identities/containers never cross this boundary. */
struct azexit_cue_request {
    uint32_t size, operation;
    struct azexit_request identity;
    uint64_t expected_revision;
    uint32_t flags, classification, entry_id, rgb;
    double position_s, end_s, beats;
    char comment[128];
};
struct azexit_cue_target {
    uint64_t revision, persistent_commands, revision_commands;
    uint32_t revision_valid, reserved;
};
/* Bounded continued fractions avoid arbitrary rounding into native Beat values.
 * Accept only a rational whose conversion reproduces the caller's double. */
static inline int azexit_pack_beat(double beats, uint32_t *packed) {
    if (!packed || !isfinite(beats) || beats < 1.0 / 65535.0 || beats > 65535.0) return 0;
    double value = beats;
    uint64_t n0 = 0, n1 = 1, d0 = 1, d1 = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!isfinite(value) || value < 0 || value > 65535.0) return 0;
        uint64_t whole = (uint64_t) value;
        uint64_t n = whole * n1 + n0, d = whole * d1 + d0;
        if (n > 65535 || d > 65535) return 0;
        if (n && d && (double) n / (double) d == beats) {
            *packed = (uint32_t) n | ((uint32_t) d << 16);
            return 1;
        }
        double fraction = value - (double) whole;
        if (fraction == 0) return 0;
        n0 = n1;
        n1 = n;
        d0 = d1;
        d1 = d;
        value = 1.0 / fraction;
    }
    return 0;
}
struct azexit_result {
    uint64_t token, load_generation, revision;
    uint32_t outcome;
    int32_t reason;
};
/* Private producer-to-core delivery.  The payload is a copied receipt: no native
 * object, function pointer, or mod-owned address crosses this boundary.  The
 * producer owns the bounded cells until consume() copies one into core storage. */
enum {
    AZEXIT_DELIVERY_VERSION = 2u,
    AZEXIT_DELIVERY_CAPACITY = 32u,
    AZEXIT_DELIVERY_PER_OWNER = 4u,
    AZEXIT_RECEIPT_TERMINAL = 1u,
    AZEXIT_RECEIPT_SETTLED = 2u,
    AZEXIT_RECEIPT_RESOURCES_RETIRED = 3u
};
enum {
    AZEXIT_STOP_DEADLINE = 1u,
    AZEXIT_STOP_CANCEL = 2u,
    AZEXIT_STOP_RETIRE = 4u
};
struct azexit_receipt {
    uint32_t size, version, kind, settled, deck;
    uint64_t token, load_generation, lifetime_generation, revision, observed_ns;
    uint32_t outcome;
    int32_t reason;
};
struct azexit_binding {
    uint32_t size, version, capacity, per_owner;
};
struct azexit_delivery_v2 {
    uint32_t size, version;
    int (*event_fd)(void);
    int (*consume)(struct azexit_receipt *out);
    int (*reclaim)(const struct azexit_receipt *receipt);
    /* Decide under the same arbitration used by the native execution claim.
     * AZ_OK copies the winner, AZ_PENDING retains the stop without a winner. */
    int (*control)(unsigned deck, uint64_t token, uint32_t reasons, uint64_t now_ns,
                   struct azexit_receipt *winner);
    int (*bind)(const struct azexit_binding *core);
    int (*reserve)(const struct azexit_request *request);
    /* Close a token whose dispatch did not acquire any native references. */
    int (*close_local)(const struct azexit_receipt *receipt);
    void (*drive)(void);
    uint64_t (*ready)(unsigned deck);
};
struct az_native_cue_snapshot;
struct azexit_service {
    uint32_t size, version;
    unsigned (*mask)(void);
    int (*target)(unsigned deck, uint64_t *load_generation);
    int (*submit)(const struct azexit_request *request);
    int (*cancel)(unsigned deck, uint64_t token);
    int (*poll)(unsigned deck, struct azexit_result *result);
    int (*release)(unsigned deck, uint64_t token);
    /* Append-only extension. Older peers keep the EXIT-only prefix above. */
    uint64_t (*commands)(void);
    int (*submit_command)(const struct azexit_request *request, uint32_t operation);
    int (*submit_loop)(const struct azexit_loop_request *request);
    int (*submit_cue)(const struct azexit_cue_request *request);
    int (*target_cue)(unsigned deck, struct azexit_cue_target *target);
    /* Optional GUI collection; never invoked from public plugin calls. */
    void (*cue_tick)(void);
    int (*native_cues)(unsigned deck, struct az_native_cue_snapshot *out);
    /* Versioned autonomous delivery, appended after the legacy polling prefix. */
    const struct azexit_delivery_v2 *delivery;
};
#define AZEXIT_SERVICE_V1_SIZE offsetof(struct azexit_service, commands)
#define AZEXIT_SERVICE_COMMAND_SIZE offsetof(struct azexit_service, submit_loop)
#define AZEXIT_SERVICE_LOOP_SIZE offsetof(struct azexit_service, submit_cue)
#define AZEXIT_SERVICE_CUE_SIZE (offsetof(struct azexit_service, cue_tick) + sizeof(((struct azexit_service *) 0)->cue_tick))
#define AZEXIT_SERVICE_CUE_SNAPSHOT_SIZE (offsetof(struct azexit_service, native_cues) + sizeof(((struct azexit_service *) 0)->native_cues))
#define AZEXIT_SERVICE_DELIVERY_SIZE (offsetof(struct azexit_service, delivery) + sizeof(((struct azexit_service *) 0)->delivery))
#define AZEXIT_ENABLE_EXIT 1u
#define AZEXIT_ENABLE_RELOOP 2u
#define AZEXIT_ENABLE_LOOP 4u
#define AZEXIT_ENABLE_CUE 8u
typedef const struct azexit_service *(*azexit_service_fn)(void);
/* Obtained once on the management thread. Functions are process-lifetime. */
__attribute__((visibility("default"))) const struct azexit_service *azstem_exit_service_v1(void) AZEXIT_NOEXCEPT;
/* Called by native owning-thread producers.  It only copies into preallocated
 * cells and signals a descriptor; it never calls the core or waits. */
int azsdk_exit_delivery_publish(const struct azexit_receipt *receipt) AZEXIT_NOEXCEPT;
/* These are companion-internal. Public native work requires a reservation
 * before any owning-thread executor can claim it. */
int azsdk_exit_delivery_claim(const struct azexit_request *request, uint64_t now_ns) AZEXIT_NOEXCEPT;
int azsdk_exit_delivery_continue(const struct azexit_request *request, uint64_t now_ns) AZEXIT_NOEXCEPT;
void azsdk_exit_delivery_wake(void) AZEXIT_NOEXCEPT;
void azsdk_exit_delivery_notify(unsigned deck) AZEXIT_NOEXCEPT;
void azsdk_exit_load_changed(unsigned deck) AZEXIT_NOEXCEPT;
typedef void *(*azexit_install_fn)(uintptr_t, void *, void **);
/* Enable bits above; zero disables dispatch. Configuration is startup-only. */
__attribute__((visibility("hidden"))) int azsdk_exit_configure(int on, int build, azexit_install_fn install) AZEXIT_NOEXCEPT;
/* Called from the existing constructed-unit / pre-destruction native hooks. */
__attribute__((visibility("hidden"))) int azsdk_exit_register(int deck, void *owned_unit) AZEXIT_NOEXCEPT;
__attribute__((visibility("hidden"))) void azsdk_exit_retire(int deck) AZEXIT_NOEXCEPT;
__attribute__((visibility("hidden"))) void azsdk_exit_retire_complete(unsigned deck, uintptr_t unit) AZEXIT_NOEXCEPT;
#ifdef AZSDK_EXIT_TESTING
/* Inject exact nominal tables and a native-entry installer, never bypass guards. */
void azsdk_exit_test_profile(const struct azexit_profile *profile) AZEXIT_NOEXCEPT;
#endif
#ifdef __cplusplus
}
#endif
#undef AZEXIT_NOEXCEPT
#endif
