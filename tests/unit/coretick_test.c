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
static const az_mod_v1 g_mod = {AZ_MOD_ABI, sizeof(az_mod_v1), "mod-a", NULL, NULL, mod_tick, NULL};
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
    CHECK("... and the mod's own tick and queue run (CONTROL for the setup)", g_mod_ticks > 0 && g_drains > 0);
    return ut_summary();
}
