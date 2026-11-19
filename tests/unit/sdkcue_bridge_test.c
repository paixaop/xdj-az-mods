#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
/* SPDX-License-Identifier: GPL-3.0-only */
/* Public cue snapshot/tick routing against the actual core command bridge. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CM_RA() ((uintptr_t) 1)
static int st_mod_owner(const char *id, uintptr_t caller, const char *service) {
    (void) id;
    (void) caller;
    (void) service;
    return -1;
}
#include "sdk_command_service.c"
#include "sdk_command.c"

static unsigned ticks, snapshots, expected_deck;
static int snapshot_status;
static void native_tick(void) { ticks++; }
static int native_snapshot(unsigned deck, struct az_native_cue_snapshot *out) {
    assert(deck == expected_deck);
    snapshots++;
    if (snapshot_status != AZ_OK) return snapshot_status;
    uint32_t size = out->size;
    az_native_cue_snapshot value = {.size = size, .version = AZ_DATA_VERSION, .deck = deck, .count = 1, .native_load_generation = 31, .revision = 17};
    value.entries[0].size = sizeof value.entries[0];
    value.entries[0].version = AZ_DATA_VERSION;
    value.entries[0].classification = AZ_CUE_MEMORY;
    value.entries[0].entry_id = 9;
    size_t bytes = size < sizeof value ? size : sizeof value;
    memcpy(out, &value, bytes);
    return AZ_OK;
}
int main(void) {
    az_native_cue_snapshot out = {.size = sizeof out}, before = out;
    sdk_cue_tick();
    assert(!ticks && sdk_native_cues(0, &out) == AZ_UNSUPPORTED && !memcmp(&out, &before, sizeof out));
    struct azexit_service service = {.size = sizeof service, .version = 1, .cue_tick = native_tick, .native_cues = native_snapshot};
    atomic_store(&g_sdk_exit_service, &service);
    assert(sdk_native_cues(-1, &out) == AZ_INVALID && sdk_native_cues(4, &out) == AZ_INVALID);
    assert(sdk_native_cues(0, NULL) == AZ_INVALID && !snapshots);
    out.size = AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE - 1;
    assert(sdk_native_cues(0, &out) == AZ_INVALID && !snapshots);
    out.size = sizeof out;
    service.size = AZEXIT_SERVICE_CUE_SIZE - 1;
    sdk_cue_tick();
    assert(!ticks && sdk_native_cues(0, &out) == AZ_UNSUPPORTED);
    service.size = AZEXIT_SERVICE_CUE_SIZE;
    sdk_cue_tick();
    assert(ticks == 1 && sdk_native_cues(0, &out) == AZ_UNSUPPORTED);
    service.size = sizeof service;
    service.cue_tick = NULL;
    sdk_cue_tick();
    assert(ticks == 1);
    service.native_cues = NULL;
    assert(sdk_native_cues(0, &out) == AZ_UNSUPPORTED && !snapshots);
    service.native_cues = native_snapshot;
    expected_deck = 2;
    snapshot_status = AZ_STALE;
    before = out;
    assert(sdk_native_cues(2, &out) == AZ_STALE && !memcmp(&out, &before, sizeof out));
    snapshot_status = AZ_OK;
    out.size = AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE;
    out.source_rate_hz = 12345;
    assert(sdk_native_cues(2, &out) == AZ_OK && snapshots == 2);
    assert(out.size == AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE && out.version == AZ_DATA_VERSION && out.deck == 2 && out.count == 1);
    assert(out.revision == 17 && out.native_load_generation == 31 && out.entries[0].entry_id == 9);
    assert(out.source_rate_hz == 12345);
    atomic_store(&g_sdk_exit_service, NULL);
    puts("PASS cue bridge: prefix admission, unavailable services, caller preservation and native snapshot/tick routing");
    return 0;
}
