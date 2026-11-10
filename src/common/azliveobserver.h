/* SPDX-License-Identifier: GPL-3.0-only */
/* Private, copied observations. These are unit-scoped callback receipts, not load identity. */
#ifndef AZLIVEOBSERVER_H
#define AZLIVEOBSERVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#define AZSDK_LIVE_NOTHROW noexcept
#else
#define AZSDK_LIVE_NOTHROW
#endif

enum {
    AZSDK_LIVE_PLAY = 1u,
    AZSDK_LIVE_QUANTIZE = 2u,
    AZSDK_LIVE_KEY_LOCK = 4u
};
struct azsdk_live_sample {
    uint64_t unit_generation;
    uint32_t observed_fields, valid_fields;
    /* Exact native play notification bytes. mode_is_2 means PlayerState's mode equals 2;
     * no paused/playing/cueing or source association is invented from these raw flags. */
    int32_t mode_is_2, play_pause, play_aux, quantize, key_lock;
    uint32_t reserved;
    uint64_t revisions[3], observed_ns[3]; /* play, quantize, key lock; independent streams */
};

/* Separate Sync receipt keeps the legacy live sample and its three-element arrays intact.
 * raw_mode preserves all native bits, including malformed values; valid admits only 0/off
 * and 2/on. Other modes, including 1, remain unavailable pending enum evidence. */
struct azsdk_sync_sample {
    uint64_t unit_generation, revision, observed_ns;
    int32_t raw_mode;
    uint32_t valid;
};
/* Native tempo-slider lock is a strict boolean setting, not a playback rate. */
struct azsdk_tempo_lock_sample {
    uint64_t unit_generation, revision, observed_ns;
    int32_t raw_locked;
    uint32_t valid;
};

/* Configure before unit creation. Only the existing EP147 build IDs 1 and 2 are admitted. */
__attribute__((visibility("hidden"))) void azsdk_live_configure(int on, int build_id) AZSDK_LIVE_NOTHROW;
#ifdef AZ_STATUS_NATIVE_PRODUCER
/* Start passive raw play-pause receipt capture. Keep the legacy exports gated
 * by their existing flag. Passive mode registers play and the typed Sync seed. */
__attribute__((visibility("hidden"))) void azsdk_live_status_start(int build_id, int legacy_active) AZSDK_LIVE_NOTHROW;
#endif
/* The caller owns the fully constructed native DjPlayerUnit for the whole call and must
 * check its existing build-specific unit vtable. No native pointer is retained. Returns 1
 * when all five weak observer registrations were submitted, not replay/completion proof. */
__attribute__((visibility("hidden"))) int azsdk_live_register(int deck, void *owned_unit) AZSDK_LIVE_NOTHROW;
/* Retire before the native unit destructor. A borrowed old callback cannot republish. */
__attribute__((visibility("hidden"))) void azsdk_live_retire(int deck) AZSDK_LIVE_NOTHROW;
/* One bounded atomic read. Returns 0 without writing when no current receipt is available.
 * Each observed component has its own validity, revision and callback receipt timestamp. */
__attribute__((visibility("hidden"))) int azsdk_live_read(int deck, struct azsdk_live_sample *out) AZSDK_LIVE_NOTHROW;
/* The independent Sync stream shares unit ownership/retirement with the legacy reader.
 * Returns 1 for a current observed receipt, even if invalid; 0 leaves output unchanged. */
__attribute__((visibility("hidden"))) int azsdk_sync_read(int deck, struct azsdk_sync_sample *out) AZSDK_LIVE_NOTHROW;
__attribute__((visibility("hidden"))) int azsdk_tempo_lock_read(int deck, struct azsdk_tempo_lock_sample *out) AZSDK_LIVE_NOTHROW;
/* Deck bits whose registration was submitted; not firmware/runtime certification. */
__attribute__((visibility("hidden"))) unsigned azsdk_live_capabilities(void) AZSDK_LIVE_NOTHROW;
#ifdef __cplusplus
}
#endif
#undef AZSDK_LIVE_NOTHROW
#endif
