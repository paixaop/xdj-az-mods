/* SPDX-License-Identifier: GPL-3.0-only */
/* Private scalar boundary. This is not a public SDK source/load qualification API. */
#ifndef AZSOURCES_H
#define AZSOURCES_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#define AZS_NOTHROW noexcept
#else
#define AZS_NOTHROW
#endif

enum azsdk_source_result {
    AZS_OK = 1,
    AZS_UNSUPPORTED = 0,
    AZS_INVALID = -1,
    AZS_GUARD = -2,
    AZS_BUSY = -3,
    AZS_LIMIT = -4,
    AZS_UNKNOWN = -5,
    AZS_STALE = -6,
    AZS_AMBIGUOUS = -7
};
enum azsdk_source_policy {
    AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER = 1u
};
enum azsdk_source_flags {
    AZS_OWNER_OBSERVED = 1u,
    AZS_CURRENT = 2u,
    AZS_PHYSICAL = 4u,
    AZS_HISTORY_INCOMPLETE = 8u,
    AZS_MEDIA_AMBIGUOUS = 16u
};
enum azsdk_source_physical_status {
    AZS_PHYSICAL_OK = 0,
    AZS_PHYSICAL_DISABLED = 1,
    AZS_PHYSICAL_PROFILE = 2,
    AZS_PHYSICAL_API = 3,
    AZS_PHYSICAL_VFS = 4,
    AZS_PHYSICAL_METHODS = 5,
    AZS_PHYSICAL_DESCRIPTOR = 6,
    AZS_PHYSICAL_METADATA = 7,
    AZS_PHYSICAL_CHANGED = 8
};
struct azsdk_source_sample {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t owner_epoch; /* Non-reused core identity of actual weak native owners. */
    uint64_t revision; /* Core publication revision, never a native revision. */
    uint64_t observed_ns; /* CLOCK_MONOTONIC receipt on the library worker. */
    uint16_t native_media_id; /* Only the established native TrackID bytes 4..5. */
    uint16_t reserved;
    int32_t physical_status;
    uint64_t device;
    uint64_t inode;
    uint64_t mount_id;
    uint64_t file_size;
    uint64_t physical_observed_ns;
};

/* Configure before hooks start. Build IDs: EPB_V130=1, EPB_V200=2. The first valid build
 * is fixed for this process. Unknown policy bits/builds disable capture. Disable retires
 * current bindings but never resets epochs or ambiguity history. May wait/allocate/hash
 * mapped code; never call from playback/draw callbacks. Physical capture is opt-in and
 * requires the already loaded, exact native SQLCipher module; it never opens a database. */
__attribute__((visibility("hidden"))) int azsdk_sources_configure(int enable, int build_id,
                                                                  unsigned policy) AZS_NOTHROW;
/* Internal C/native forwarding boundary. Copy only the whitelisted message
 * scalars while the actual message is alive, then call original exactly once
 * in that scoped context. This acquires no receiver/chain liveness and assigns
 * no source/load/request identity. The original may destroy the message or
 * propagate a native C++ exception; teardown never reads that message again.
 * A null original is invalid and has no work to forward. */
typedef void (*azsdk_source_message_original)(void *, void *, unsigned);
__attribute__((visibility("hidden"))) void azsdk_sources_forward_owned_message(
    int build_id, void *owned_server, void *owned_message, unsigned parameter,
    azsdk_source_message_original original);
/* ONLY inside a legitimately owned native MusicLibraryServer::onMessage, after the
 * original returns, on that server's normal worker. The complete receiver/chain must
 * remain alive throughout. RTTI, invocation-marker and affinity guards do not acquire
 * liveness or make stale/arbitrary pointers safe. Weak owners are constructed directly
 * from stable native GNU shared_ptr references; no strong native owner is retained.
 * Physical policy permits bounded metadata IO on this library worker only, not on a
 * playback/draw hook. Byte/syscall bounds are not a hard storage-IO latency guarantee.
 * Output may be NULL with size 0. Otherwise size >=8; only its known prefix is copied.
 * Guard/limit/busy failure leaves out unchanged. On AZS_OK inspect physical_status/flags. */
__attribute__((visibility("hidden"))) int azsdk_sources_observe_owned_message(
    void *owned_server, void *out, size_t out_size) AZS_NOTHROW;
/* Scalar-only, try-lock reads. AZS_STALE copies an old record with CURRENT/PHYSICAL clear;
 * UNKNOWN/BUSY/INVALID leave output unchanged. No native pointer is followed or locked. */
__attribute__((visibility("hidden"))) int azsdk_sources_read(uint64_t owner_epoch, void *out,
                                                             size_t out_size) AZS_NOTHROW;
/* Diagnostic only: a media number never establishes source/load association. Once a
 * second owner is seen for a media number, AMBIGUOUS is permanent for this process,
 * including after reclamation. AMBIGUOUS copies a record with owner_epoch=0. */
__attribute__((visibility("hidden"))) int azsdk_sources_candidate(uint16_t native_media_id,
                                                                  void *out, size_t out_size) AZS_NOTHROW;
/* Background/configuration worker only: releases expired/retired weak control blocks.
 * Capture does not evict weak entries. There are 32 epoch and 32 server-binding slots. */
__attribute__((visibility("hidden"))) unsigned azsdk_sources_sweep(void) AZS_NOTHROW;
/* Exact native physical profile was prepared successfully; not runtime certification. */
__attribute__((visibility("hidden"))) int azsdk_sources_physical_ready(void) AZS_NOTHROW;
/* Startup-only internal dependency. Prepares/reuses the exact module proof;
 * binds real forwarding to the one process-lifetime SourceState. The installer
 * publishes each genuine original before redirecting its entry. It must return
 * exactly 1 for an accepted install. Once installed, close retirement continues
 * even when source diagnostics are disabled. No reset/uninstall or public
 * source/load capability is created by a successful return. */
typedef int (*azsdk_source_connection_installer)(uintptr_t, void *, void **, void *);
__attribute__((visibility("hidden"))) int azsdk_sources_install_connections(
    azsdk_source_connection_installer installer, void *context) AZS_NOTHROW;

#ifdef __cplusplus
}
#endif
#undef AZS_NOTHROW
#endif
