/* SPDX-License-Identifier: GPL-3.0-only */
/* The core's own questions through the confirm dialog (azcm_confirm: a stick's SSH key waiting
 * to be allowed, a mod's removal), serviced by community_tick on the overlay thread
 * (community_tick extracted out of guest/azmod/community.c, the dialog's queue and servicing from
 * guest/azmod/sdk_ui.c, by run.sh). A deck may have no community mod at all, or only mods that
 * are off, refused or quarantined, so:
 *   - with no mod, and with mods none of which runs, a question the core asks is put on screen
 *     at the next tick and its answer is delivered;
 *   - with a running mod the same, and the mod's own tick still runs (the control for the setup);
 *   - nothing of the mods' own (their ticks, their post_ui queue) runs while none of them does. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "check.h"
#include "azmod-mod.h"
#include "sdk_logic.h"

#define AZMOD_STEM_DECKS 4
#define CM_ROOT "/nonexistent"
#define CM_STARTING CM_ROOT "/.starting"
#define CM_HEALTHY_MS 90000
#define CM_CFG_BUSY_MS 5000
enum {
    AZCM_LOADED = 1,
    AZCM_REFUSED = 3,
    AZCM_FAILED = 4
};
static int g_mod_ticks;
static void mod_tick(void) { g_mod_ticks++; }
static const az_mod_v1 g_mod = {.abi = AZ_MOD_ABI, .size = sizeof(az_mod_v1), .id = "mod-a", .tick = mod_tick};
typedef struct {
    int state, cfg_dirty;
    long cfg_flushed_at;
    const az_mod_v1 *m;
    struct {
        int ready[8];
        char line[8][16];
        unsigned dropped, refused;
    } fq;
} cm_mod;
static cm_mod g_cm[2];
static int g_cm_n, g_cm_live, g_cm_timing, g_cm_healthy, g_cm_logs_on;
static long g_cm_t0, g_now = 1000;
static double g_cm_bpm[AZMOD_STEM_DECKS];
static pthread_mutex_t g_cm_mu = PTHREAD_MUTEX_INITIALIZER;
static long now_ms(void) { return g_now; }
static void dbg(const char *fmt, ...) { (void) fmt; }
static void cm_logf(cm_mod *c, int force, const char *fmt, ...) { (void) c, (void) force, (void) fmt; }
static void cm_cfg_flush(cm_mod *c) { (void) c; }
static int cm_conf_save(void) { return 0; }
static int bpmstate_read(int d, double *v, void *x) {
    (void) d, (void) v, (void) x;
    return 0;
}
static void cm_tempo_poll(void) {
    for (int d = 0; d < AZMOD_STEM_DECKS; ++d) {
        double v = 0.0;
        g_cm_bpm[d] = bpmstate_read(d, &v, NULL) ? v : 0.0;
    }
}
static int g_deck_ticks, g_rows_ticks, g_drains;
static void sdk_deck_tick(long now) {
    (void) now;
    g_deck_ticks++;
}
static void sdk_rows_tick(void) { g_rows_ticks++; }
/* community.c normally gets these from the provider translation units.  The
 * extracted coretick fixture deliberately includes only community_tick, so
 * keep its optional per-frame services self-contained here. */
static void sdk_data_callbacks_tick(void) {}
static int core_analysis_ticks;
static void analysis_events_core_tick(void) { ++core_analysis_ticks; }
static void sdk_analysis_callbacks_tick(void) {}
static void sdk_beatport_owner_tick(void) {}
static int source_browser_ticks;
static void sdk_source_browser_tick(void) { ++source_browser_ticks; }
static int keyboard_up;
static int sdk_text_entry_tick(void) { return keyboard_up; }
static int plan_set_ticks;
static void sdk_plan_set_tick(void) { ++plan_set_ticks; }
static int native_bound, cue_ticks;
static void sdk_cue_tick(void) { ++cue_ticks; }
static int sdk_command_native_bound(void) { return native_bound; }
static void azsdk_slip_collect_v1(void) {}
/* the quarantine's heartbeat (cm_orderly.h has its own suite) */
static long g_rtick_n;
static void cmo_note(long count, long now) { (void) count, (void) now; }
/* the dialog's surface: shown or not */
static int g_shown, g_surface_up;
static void *g_sdk_ovc;
static int sdk_core_can_show(void) { return 1; }
static int sdk_core_surface_show(az_rect r, int on) {
    (void) r;
    g_surface_up = on;
    return 0;
}
static int sd_dlg_open(void);
#define sd_drain(q) (g_drains++)
#include CORETICK_INC
static int sd_dlg_open(void) { /* the layout and painting are the overlay's: here, only "up" */
    g_shown++;
    sdk_core_surface_show((az_rect) {0, 0, 1, 1}, 1);
    g_sd_dlg.up = 1;
    return 0;
}

static int g_answers, g_yes;
static void answered(int yes, void *user) {
    (void) user;
    g_answers++;
    g_yes = yes;
}
/* one question asked by the core, a few ticks, then the user's answer */
static int asked_and_answered(void) {
    g_shown = g_answers = 0;
    g_yes = -1;
    if (azcm_confirm("Remote access", "Allow this key?", "Allow", answered, NULL) != 0)
        return 0;
    for (int i = 0; i < 3; i++, g_now += 40)
        community_tick();
    if (g_shown != 1 || !g_sd_dlg.up)
        return 0;
    sd_dlg_close(1);
    return g_answers == 1 && g_yes == 1 && !g_sd_dlg.up && !g_surface_up;
}

int main(void) {
    ut_suite = "core dialog";
    g_cm_n = 0, g_cm_live = 0;
    CHECK("no community mod at all: the core's question is shown and answered", asked_and_answered());
    CHECK("core analysis completion listeners run without community mods", core_analysis_ticks == 3);
    CHECK("the source browser mailbox is drained on the overlay tick", source_browser_ticks == 3);
    CHECK("Plan Set dispatch waits for a running mod", plan_set_ticks == 0);
    CHECK("... and nothing of the mods' runs", g_mod_ticks == 0 && g_drains == 0 && g_deck_ticks == 0);
    g_cm_n = 2, g_cm_live = 0;
    g_cm[0] = (cm_mod) {.state = AZCM_REFUSED, .m = &g_mod};
    g_cm[1] = (cm_mod) {.state = AZCM_FAILED, .m = &g_mod};
    CHECK("mods installed but none running: the core's question is shown and answered", asked_and_answered());
    CHECK("... the stopped mods' rows are retired, their ticks and queue not run",
          g_rows_ticks > 0 && g_mod_ticks == 0 && g_drains == 0);
    g_cm[0].state = AZCM_LOADED;
    g_cm_live = 1;
    CHECK("a running mod: the question is shown and answered too", asked_and_answered());
    CHECK("Plan Set dispatch runs on the overlay tick", plan_set_ticks == 3);
    CHECK("... and the mod's own tick and queue run (CONTROL for the setup)", g_mod_ticks > 0 && g_drains > 0);
    int before = cue_ticks;
    native_bound = 1;
    g_now += 40;
    community_tick();
    CHECK("bound native command service owns cue observations", cue_ticks == before);
    native_bound = 0;
    g_now += 40;
    community_tick();
    CHECK("unbound companion retains independent cue observations", cue_ticks == before + 1);
    g_shown = g_answers = 0;
    CHECK("a confirmation can queue while the keyboard is in use",
          azcm_confirm("Confirm", "Proceed?", "OK", answered, NULL) == 0);
    keyboard_up = 1;
    community_tick();
    CHECK("the confirmation waits for the keyboard to close", !g_shown && !g_answers);
    keyboard_up = 0;
    community_tick();
    CHECK("the queued confirmation opens after the keyboard closes", g_shown == 1 && g_sd_dlg.up);
    sd_dlg_close(0);
    return ut_summary();
}
