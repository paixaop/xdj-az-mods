/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_host.h"
#include "analysis_presentation.h"
#include <stdatomic.h>
static _Atomic int started;
/* Message-thread presentation cursors, not scheduler or result copies. */
static uint64_t announced[4];

int analysis_host_start(const char *assets, int enabled) {
    if (atomic_load_explicit(&started, memory_order_acquire)) return AZ_OK;
    az_analysis_owner owner = analysis_owner_core_begin();
    if (!owner.generation) return AZ_LIMIT;
    int rc = analysis_runtime_start(assets);
    if (rc != AZ_OK) {
        analysis_owner_core_retire();
        return rc;
    }
    analysis_runtime_enable(owner, enabled);
    atomic_store_explicit(&started, 1, memory_order_release);
    return AZ_OK;
}
void analysis_host_enable(int enabled) {
    if (atomic_load_explicit(&started, memory_order_acquire))
        analysis_runtime_enable(analysis_owner_core(), enabled);
}
static int analysis_notice_ready(unsigned deck, az_analysis_status_v1 *status) {
    if (analysis_runtime_deck_status(deck, status) != AZ_OK) return 0;
    if (!status->request_id || status->request_id == announced[deck]) return 0;
    return status->state >= AZ_ANALYSIS_COMPLETE;
}
static void analysis_notice(unsigned deck, const az_analysis_status_v1 *status,
                            void (*banner)(const char *, int), void (*diagnostic)(const char *)) {
    char text[256];
    announced[deck] = status->request_id;
    snprintf(text, sizeof text, "Deck %u: %s", deck + 1, analysis_result_message(status->result));
    if (status->state == AZ_ANALYSIS_COMPLETE && status->result == AZ_OK && banner) banner(text, 3000);
    if (status->result != AZ_OK && diagnostic) {
        snprintf(text, sizeof text, "Deck %u analysis result %d: %s", deck + 1,
                 status->result, status->diagnostic);
        diagnostic(text);
    }
}
void analysis_host_notices(void (*banner)(const char *, int), void (*diagnostic)(const char *)) {
    if (!atomic_load_explicit(&started, memory_order_acquire)) return;
    for (unsigned deck = 0; deck < 4; ++deck) {
        az_analysis_status_v1 status = {.size = sizeof status};
        if (analysis_notice_ready(deck, &status)) analysis_notice(deck, &status, banner, diagnostic);
    }
}
void analysis_host_shutdown(void) {
    if (!atomic_exchange_explicit(&started, 0, memory_order_acq_rel)) return;
    az_analysis_owner owner = analysis_owner_core();
    analysis_owner_core_retire();
    analysis_runtime_retire(owner);
    analysis_runtime_shutdown();
    analysis_source_retire(owner);
}
