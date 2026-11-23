/* SPDX-License-Identifier: GPL-3.0-only */
/* Included after sdk_data_callbacks.c. Registration and delivery are UI-only. */
#include "analysis_events.h"
static struct {
    struct analysis_listener listener;
    az_analysis_owner owner;
} g_analysis_callbacks[CM_MAX];
static int sdk_on_analysis_complete(const char *id, az_analysis_complete_fn fn, void *user) {
    int slot = sdk_callback_owner(id, CM_RA(), (uintptr_t) fn, "analysis.on_complete");
    if (slot < 0) return AZ_INVALID;
    az_analysis_owner owner = analysis_owner_get((unsigned) slot);
    if (!analysis_owner_current(owner)) return AZ_STALE;
    if (g_analysis_callbacks[slot].owner.generation != owner.generation)
        g_analysis_callbacks[slot].listener = (struct analysis_listener) {0};
    int rc = analysis_events_listen(&g_analysis_callbacks[slot].listener, fn, user);
    if (!rc) g_analysis_callbacks[slot].owner = owner;
    return rc;
}
static int sdk_analysis_listener_active(int slot) {
    return g_cm[slot].state == AZCM_LOADED && analysis_owner_current(g_analysis_callbacks[slot].owner);
}
static void sdk_analysis_deliver(int slot) {
    struct analysis_listener *listener = &g_analysis_callbacks[slot].listener;
    if (!listener->callback) return;
    for (unsigned n = 0; n < 16 && sdk_analysis_listener_active(slot); ++n) {
        az_analysis_complete_v1 event;
        if (analysis_events_take(listener, &event)) break;
        listener->callback(&event, listener->user);
    }
}
static void sdk_analysis_callbacks_tick(void) {
    for (int slot = 0; slot < g_cm_n; ++slot) sdk_analysis_deliver(slot);
}
