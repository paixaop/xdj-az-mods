/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_host.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static int available, enabled, start_result, bind_result;
static unsigned logs, started, bound, phase;
static char last_diagnostic[128];
static void (*azmod_analysis_enable)(int);
static int (*azmod_analysis_status)(unsigned, az_analysis_status_v1 *);
static void (*azmod_vocal_lines_config)(unsigned, int);
static int vocal_result, vocal_starts, vocal_configs, runtime_loads, vocal_loads;
static void (*load_observer)(unsigned, uint64_t, uint64_t, int, const az_track_ref *);
int vocal_lines_start(void) {
    ++vocal_starts;
    return vocal_result;
}
void vocal_lines_config(unsigned gap, int on) {
    assert(gap == 2000 && on == enabled);
    ++vocal_configs;
}
static void azf_vocal_lines_changed(int value, void *user) {
    (void) value;
    (void) user;
    azmod_vocal_lines_config(2000, enabled);
}
void vocal_lines_load(unsigned d, uint64_t o, uint64_t e, int l) {
    assert(d == 2 && o == 7 && e == 9 && l == 1);
    ++vocal_loads;
}
enum {
    AZF_TRACK_ANALYSIS = 1
};
static int az_on(int id) {
    assert(id == AZF_TRACK_ANALYSIS);
    return enabled;
}
static void analysis_diagnostic(const char *text) {
    assert(text);
    snprintf(last_diagnostic, sizeof last_diagnostic, "%s", text);
    ++logs;
}
static int usbmods_find(char *out, size_t size) {
    if (!available) return 0;
    assert(snprintf(out, size, "/media/usb2/MODS") > 0);
    return 1;
}
int analysis_host_start(const char *assets, int on) {
    assert(!strcmp(assets, "/media/usb2/MODS/analysis/assets") && on == enabled);
    ++started;
    return start_result;
}
void analysis_host_enable(int on) { enabled = on; }
void analysis_runtime_load(unsigned d, uint64_t o, uint64_t e, int l, const az_track_ref *t) {
    assert(d == 2 && o == 7 && e == 9 && l == 1 && !t);
    ++runtime_loads;
}
int analysis_runtime_deck_status(unsigned d, az_analysis_status_v1 *s) {
    (void) d;
    (void) s;
    return AZ_OK;
}
static int sdk_status_analysis_bind(void (*observer)(unsigned, uint64_t, uint64_t, int, const az_track_ref *)) {
    load_observer = observer;
    ++bound;
    return bind_result;
}
static void sdk_status_start(void) {
    assert(phase == 0);
    phase = 1;
}
static void cm_services_start(void) {
    assert(phase == 1);
    phase = 2;
}
#include "../../src/azmod/analysis_startup.c"
int main(void) {
    char path[128];
    assert(core_analysis_assets(path, sizeof path) == AZ_NOT_FOUND);
    core_analysis_start();
    assert(logs == 1 && !started && !bound && !azmod_analysis_status);
    assert(strstr(last_diagnostic, "core startup failed"));
    available = 1;
    assert(core_analysis_assets(path, 3) == AZ_LIMIT);
    start_result = AZ_FAILED;
    core_analysis_start();
    assert(logs == 2 && started == 1 && !bound && !azmod_analysis_enable);
    assert(strstr(last_diagnostic, "core startup failed"));
    char result_text[32];
    snprintf(result_text, sizeof result_text, "(%d)", AZ_FAILED);
    assert(strstr(last_diagnostic, result_text));
    start_result = AZ_OK;
    bind_result = AZ_UNSUPPORTED;
    core_analysis_start();
    assert(logs == 3 && bound == 1 && azmod_analysis_enable && !azmod_analysis_status);
    assert(strstr(last_diagnostic, "automatic load observation failed"));
    snprintf(result_text, sizeof result_text, "(%d)", AZ_UNSUPPORTED);
    assert(strstr(last_diagnostic, result_text));
    bind_result = AZ_OK;
    enabled = 1;
    core_services_init();
    assert(phase == 2 && started == 3 && bound == 2 && logs == 3);
    assert(azmod_analysis_status == analysis_runtime_deck_status);
    assert(vocal_starts == 1 && vocal_configs == 1);
    load_observer(2, 7, 9, 1, NULL);
    assert(runtime_loads == 1 && vocal_loads == 1);
    vocal_result = AZ_FAILED;
    core_vocal_lines_start();
    assert(vocal_starts == 2 && vocal_configs == 1 && logs == 4);
    assert(strstr(last_diagnostic, "vocal lines startup failed"));
    azmod_analysis_enable(0);
    assert(!enabled);
    puts("analysis startup: core ordering, asset location, and explicit failure passed");
}
