/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "wave-targets.inc"
static void wavewpaint_hook(void) {}
static void *g_orig_wavewpaint, *g_orig_stemref_fwd;
static int g_gridaa_hooked, g_scroll60_ok, g_feedlock_ok, g_vb_hooked;
static int g_wave_hooked, g_wave_vis, g_setvis_hooked, g_compdtor_ok, azmod_stem_native, g_attzero;
static int dedupe_off, grid_off, scroll_off, feed_off, nowave, update_probe, themes;
static int source_row_off, stemnative_off, probe_attzero, compdtor_result = 1;
static int logs, failed_target = -1;
static unsigned seen[WAVE_HOOK_COUNT], serial, hook_order[WAVE_HOOK_COUNT];
static unsigned vb_dtor_order, vb_slider_order, scale_calls, segv_calls;
static unsigned static_order, component_order, source_row_order, master_order, phase_order, theme_calls;
static int fixture_access(const char *path, int mode) {
    assert(mode == F_OK);
    if (strstr(path, "dedupe4.off")) return dedupe_off ? 0 : -1;
    if (strstr(path, "gridaa.off")) return grid_off ? 0 : -1;
    if (strstr(path, "scroll60.off")) return scroll_off ? 0 : -1;
    if (strstr(path, "feedlock.off")) return feed_off ? 0 : -1;
    if (strstr(path, "srcrow.off")) return source_row_off ? 0 : -1;
    if (strstr(path, "stemnative.off")) return stemnative_off ? 0 : -1;
    assert(!"unexpected setting path");
    return -1;
}
static int probe_on(const char *path) {
    if (strstr(path, "nowave.on")) return nowave;
    if (strstr(path, "updhook.on")) return update_probe;
    assert(strstr(path, "attzero.on"));
    return probe_attzero;
}
static int daymode_wanted(void) { return themes; }
static int azm_install_hook(unsigned target, void *hook, void **original) {
    assert(target < WAVE_HOOK_COUNT && hook && original);
    seen[target]++;
    hook_order[target] = ++serial;
    if ((int) target == failed_target) {
        *original = NULL;
        return -1;
    }
    *original = (void *) (uintptr_t) (target + 100);
    return 1;
}
static void dbg(const char *format, ...) { (void) format; }
static FILE *azlog_open(const char *path, const char *mode) {
    assert(path && (!strcmp(mode, "w") || !strcmp(mode, "a")));
    return logs ? tmpfile() : NULL;
}
static void az_install_segv(void) { segv_calls++; }
static int install_wave_scale(void) {
    scale_calls++;
    return 1;
}
static int ui60ui_at_start(void) { return 1; }
static void install_vb_dtor(void) { vb_dtor_order = ++serial; }
static void install_vinyl_slider(void) { vb_slider_order = ++serial; }
#define access fixture_access
#define AZ_PROBE_ON(path) probe_on(path)
#include ROUTING_IMPL_INC

static int install_compdtor_hook(void) {
    component_order = ++static_order;
    return compdtor_result;
}
static void install_srcrow_hook(void) { source_row_order = ++static_order; }
static void install_master_hook(void) { master_order = ++static_order; }
static void install_phasemeter(void) { phase_order = ++static_order; }
static void install_daymode(void) { theme_calls++; }
static void install_scci_hook(void) { theme_calls++; }
static int install_preview_hotcues(void) {
    ++static_order;
    return 1;
}
#define INSTALL(name) \
    static void name(void) { ++static_order; }
INSTALL(install_scrntrace_hook)
INSTALL(install_jogstems)
INSTALL(install_qrlogin_guard)
INSTALL(install_bpmstate_hook)
INSTALL(ds_tempo_install)
INSTALL(ds_models_install)
INSTALL(ds_pad_install)
INSTALL(install_link_members)
INSTALL(install_stemsmark)
INSTALL(install_browse_fields)
INSTALL(install_hidetitle)
INSTALL(install_mlreply_probe)
INSTALL(install_reorder)
INSTALL(install_folderpreview)
INSTALL(install_remotestems)
INSTALL(install_rowcache)
#include "statichooks.inc"
#undef access
static void reset_hooks(void) {
    memset(seen, 0, sizeof seen);
    memset(hook_order, 0, sizeof hook_order);
    serial = scale_calls = segv_calls = 0;
}
static void waveform_installs(void) {
    for (unsigned v = 0; v < 4; v++) {
        reset_hooks();
        dedupe_off = v & 1;
        grid_off = v & 1;
        themes = (v >> 1) & 1;
        scroll_off = v & 1;
        feed_off = (v >> 1) & 1;
        nowave = v & 1;
        update_probe = v == 2;
        logs = v & 1;
        failed_target = v == 2 ? JUCE_SETVISIBLE : -1;
        install_wavestems_hook();
        assert(seen[EP_c85838] == (v == 0)); /* identity hook installs once */
        assert(seen[WAVE_SWITCH] && seen[WAVE_CTOR] && seen[WAVE_MODEGET]);
        assert(seen[WAVE_PAINT] && seen[WAVE_STEMREF]); /* still required with nowave */
        assert(g_orig_stemref_fwd == g_orig_stemref && g_orig_stemref_fwd);
        assert(seen[WAVEVIEW_UPDATE] == !dedupe_off);
        assert(seen[GRID_MAKE] == (!grid_off || themes));
        assert(g_gridaa_hooked == !grid_off);
        assert(seen[SW2D_PAINT] == !nowave && seen[WAVE_OVERVIEW] == !nowave);
        assert(scale_calls == !nowave && segv_calls == !nowave);
        assert(seen[OV_TILE_OWNER] == !nowave && seen[OV_TILE_CTOR] == !nowave);
        assert(seen[JUCE_STARTTIMER] && seen[DISPREFRESH_TIMERCB]);
        assert(seen[FLING_REL] == !scroll_off && seen[RM_PAINTNOW] == !scroll_off);
        assert(g_scroll60_ok == !scroll_off && g_feedlock_ok == !feed_off);
        assert(seen[LOCALPLAYER_TIMERCB] == !feed_off);
        assert((seen[WAVE_UPDATE] != 0) == update_probe && seen[WWB_UPDATE_CALLER]);
        assert(vb_dtor_order < hook_order[VB_SETTER] && hook_order[VB_SETTER] < vb_slider_order);
        assert(g_wave_hooked && g_setvis_hooked == (v != 2));
        if (v == 2) assert(g_wave_vis == -1);
    }
    failed_target = GRID_MAKE;
    grid_off = 0;
    install_wave_grid();
    assert(!g_gridaa_hooked);
}
static void static_installs(void) {
    for (int off = 0; off < 2; off++) {
        static_order = component_order = source_row_order = master_order = phase_order = theme_calls = 0;
        source_row_off = stemnative_off = off;
        compdtor_result = off ? -1 : 1;
        themes = probe_attzero = off;
        logs = off;
        install_static_hooks();
        assert(component_order == 1 && g_compdtor_ok == !off);
        assert(azmod_stem_native == !off && g_attzero == off);
        assert((source_row_order != 0) == !off);
        assert(master_order && master_order < phase_order);
        assert(theme_calls == (off ? 2u : 0u));
    }
}
int main(void) {
    waveform_installs();
    static_installs();
    puts("PASS waveform/static installation: lifetime-first order, independent gates and hook publication");
    return 0;
}
