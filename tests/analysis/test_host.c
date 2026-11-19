/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_host.h"
#include "analysis_presentation.h"
#include <assert.h>
#include <string.h>

static int start_result, starts, enables, last_enabled, retired, joined, reclaimed;
static unsigned queries, banners, diagnostics;
static az_analysis_owner accepted;
static az_analysis_status_v1 observations[4];
static int replies[4];
int analysis_runtime_start(const char *assets) {
    assert(!strcmp(assets, "/usb/MODS/analysis/assets"));
    ++starts;
    assert(analysis_owner_current(analysis_owner_core()));
    return start_result;
}
void analysis_runtime_enable(az_analysis_owner owner, int enabled) {
    assert(analysis_owner_current(owner) == !!enabled);
    accepted = owner;
    ++enables;
    last_enabled = enabled;
}
int analysis_runtime_deck_status(unsigned deck, az_analysis_status_v1 *out) {
    assert(deck < 4 && out->size == sizeof *out);
    ++queries;
    if (replies[deck] == AZ_OK) *out = observations[deck];
    return replies[deck];
}
void analysis_runtime_retire(az_analysis_owner owner) {
    assert(owner.generation == accepted.generation && !analysis_owner_current(owner));
    ++retired;
}
void analysis_runtime_shutdown(void) {
    assert(retired > joined);
    ++joined;
}
void analysis_source_retire(az_analysis_owner owner) {
    assert(owner.generation == accepted.generation && !analysis_owner_current(owner));
    ++reclaimed;
}
static void banner(const char *text, int duration) {
    assert(strstr(text, "Track analysis ready") && duration == 3000);
    ++banners;
}
static void diagnostic(const char *text) {
    assert(strstr(text, "analysis result") && strstr(text, "native failure"));
    ++diagnostics;
}
static void terminal(unsigned deck, uint64_t id, uint32_t state, int result) {
    observations[deck] = (az_analysis_status_v1) {.size = sizeof(az_analysis_status_v1),
                                                  .request_id = id,
                                                  .state = state,
                                                  .result = result};
    strcpy(observations[deck].diagnostic, "native failure");
}
static void startup_and_retirement(void) {
    analysis_host_enable(1);
    analysis_host_notices(banner, diagnostic);
    analysis_host_shutdown();
    assert(!starts && !enables && !queries && !joined);
    start_result = AZ_FAILED;
    assert(analysis_host_start("/usb/MODS/analysis/assets", 1) == AZ_FAILED);
    assert(!analysis_owner_current(analysis_owner_core()));
    start_result = AZ_OK;
    assert(analysis_host_start("/usb/MODS/analysis/assets", 0) == AZ_OK);
    assert(starts == 2 && enables == 1 && !last_enabled);
    assert(!analysis_owner_current(accepted));
    assert(analysis_host_start("/usb/MODS/analysis/assets", 1) == AZ_OK);
    assert(starts == 2 && enables == 1);
    analysis_host_enable(1);
    assert(enables == 2 && last_enabled);
    az_analysis_owner before = accepted;
    analysis_host_enable(1);
    assert(enables == 2 && analysis_owner_current(before));
    analysis_host_enable(0);
    assert(enables == 3 && !last_enabled && !analysis_owner_current(before));
    assert(reclaimed == 1);
    analysis_host_enable(0);
    assert(enables == 3 && reclaimed == 1);
    analysis_host_enable(1);
    assert(enables == 4 && last_enabled && accepted.generation != before.generation);
    assert(!analysis_owner_current(before) && analysis_owner_current(accepted));
}
static void result_delivery(void) {
    terminal(0, 1, AZ_ANALYSIS_RUNNING, AZ_PENDING);
    terminal(1, 2, AZ_ANALYSIS_SAVED, AZ_PENDING);
    terminal(2, 3, AZ_ANALYSIS_COMPLETE, AZ_OK);
    terminal(3, 4, AZ_ANALYSIS_FAILED, AZ_FAILED);
    replies[2] = AZ_BUSY;
    analysis_host_notices(banner, diagnostic);
    assert(!banners && diagnostics == 1 && queries == 4);
    replies[2] = AZ_OK;
    analysis_host_notices(banner, diagnostic);
    assert(banners == 1 && diagnostics == 1 && queries == 8);
    analysis_host_notices(banner, diagnostic);
    assert(banners == 1 && diagnostics == 1);
    terminal(0, 5, AZ_ANALYSIS_CANCELLED, AZ_STALE);
    terminal(1, 6, AZ_ANALYSIS_COMPLETE, AZ_OK);
    analysis_host_notices(NULL, NULL);
    analysis_host_notices(banner, diagnostic);
    assert(banners == 1 && diagnostics == 1); /* consumed even without a renderer */
    terminal(0, 7, AZ_ANALYSIS_CANCELLED, AZ_STALE);
    analysis_host_notices(banner, diagnostic);
    assert(banners == 1 && diagnostics == 2); /* cancellation never celebrates saved data */
}
static void reused_analysis_notices(void) {
    unsigned before_banners = banners, before_diagnostics = diagnostics;
    terminal(0, 8, AZ_ANALYSIS_COMPLETE, AZ_NOT_FOUND);
    observations[0].diagnostic[0] = '\0';
    analysis_host_notices(banner, diagnostic);
    analysis_host_notices(banner, diagnostic);
    assert(banners == before_banners && diagnostics == before_diagnostics);
    terminal(0, 9, AZ_ANALYSIS_FAILED, AZ_NOT_FOUND);
    analysis_host_notices(banner, diagnostic);
    assert(banners == before_banners && diagnostics == before_diagnostics + 1);
    analysis_host_notices(banner, diagnostic);
    assert(banners == before_banners && diagnostics == before_diagnostics + 1);
}
static void presentation(void) {
    struct {
        unsigned state;
        int result;
        const char *text;
    } cases[] = {
        {0, AZ_OK, "Waiting"}, {AZ_ANALYSIS_QUEUED, AZ_OK, "Checking"}, {AZ_ANALYSIS_RUNNING, AZ_PENDING, "Analysing"}, {AZ_ANALYSIS_SAVED, AZ_PENDING, "waiting for library"}, {AZ_ANALYSIS_CANCELLED, AZ_OK, "cancelled"}, {AZ_ANALYSIS_COMPLETE, AZ_OK, "ready"}, {AZ_ANALYSIS_COMPLETE, AZ_NOT_FOUND, "ready"}, {AZ_ANALYSIS_FAILED, AZ_UNSUPPORTED, "Unsupported"}, {AZ_ANALYSIS_FAILED, AZ_STALE, "changed"}, {AZ_ANALYSIS_FAILED, AZ_FAILED, "failed"}};
    assert(strstr(analysis_status_message(NULL, AZ_BUSY), "Updating"));
    assert(strstr(analysis_status_message(NULL, AZ_UNSUPPORTED), "unavailable"));
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        az_analysis_status_v1 status = {.state = cases[i].state, .result = cases[i].result};
        assert(strstr(analysis_status_message(&status, AZ_OK), cases[i].text));
    }
}
int main(void) {
    startup_and_retirement();
    result_delivery();
    reused_analysis_notices();
    presentation();
    uint64_t previous = accepted.generation;
    analysis_host_shutdown();
    assert(retired == 1 && joined == 1 && reclaimed == 2);
    unsigned before = queries;
    analysis_host_notices(banner, diagnostic);
    analysis_host_shutdown();
    assert(queries == before && joined == 1);
    assert(analysis_host_start("/usb/MODS/analysis/assets", 1) == AZ_OK);
    assert(accepted.generation != previous);
    analysis_host_shutdown();
    assert(retired == 2 && joined == 2 && reclaimed == 3);
    puts("analysis host: lifecycle, copied status, and presentation passed");
}
