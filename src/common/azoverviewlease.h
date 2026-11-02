/* SPDX-License-Identifier: GPL-3.0-only */
/* Private azmod/azstem boundary. No part of the public SDK ABI. */
#ifndef AZOVERVIEWLEASE_H
#define AZOVERVIEWLEASE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AZOV_LEASE_ABI 1u
#define AZOV_LEASE_PROFILE_UNAVAILABLE 1u
struct azov_owned_entry {
    uintptr_t widget, held_content, native_return, native_stack;
};
struct azov_lease {
    uint64_t token[2]; /* Non-reused generation and bounded broker slot; never a pointer. */
    uintptr_t content_key; /* Identity only; callers must not dereference it. */
    const unsigned char *points; /* Borrowed until finish, including while native code runs. */
    int32_t count;
    uint32_t reserved;
};
struct azov_lease_api {
    uint32_t struct_size, abi_version, flags, reserved;
    int (*begin)(const struct azov_owned_entry *, struct azov_lease *out, size_t out_size);
    int (*install)(const uint64_t token[2], unsigned char *scaled);
    int (*finish)(const uint64_t token[2]);
};
/* Query from azmod.so on the configured message thread only. The helper binds its caller
 * image once; operations recheck caller image and thread. Unknown ABI/size fails unchanged.
 * PROFILE_UNAVAILABLE means begin rejects BEFORE reading widget/Content memory. Supported
 * captures come only from the verified native updater's already-held Content local, with
 * exact entry registers, caller/stack slots, code profile and message-thread checks.
 * A successful finish conditionally restores its own installation, then releases exactly
 * once through the native owner path. It never accepts a caller-supplied object to release. */
int azstem_overview_lease_v1(uint32_t abi, struct azov_lease_api *out, size_t out_size);
/* Constructor-only private wiring, using azstem's existing EP_22043f0 mapping. Disable
 * retires acquisition; it does not abandon existing leases or reset token generations. */
__attribute__((visibility("hidden"))) void azov_lease_configure(int build_id,
                                                                void (*owner_release)(void *, void *));
#ifdef __cplusplus
}
#endif
#endif
