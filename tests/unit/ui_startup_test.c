/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define AZS_OK 0
#define AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER 1u
static int g_sdk_sources_on, sdk_passive_sources, g_ep147_build, g_azm_inert = 1;
static int is_target, address_ok, identified = 200, status_off, registry_ok = 1;
static int physical_ready = 1, cache_ready = 1, library_ok = 1, log_on;
static int patch_result = 1, thread_failure;
static unsigned created, detached, last_policy, configured, disabled;
static char calls[8192];
static void mark(const char *name) {
    size_t used = strlen(calls), n = strlen(name);
    assert(used + n + 2 < sizeof calls);
    memcpy(calls + used, name, n);
    memcpy(calls + used + n, " ", 2);
}
#define STEP(name) \
    static void name(void) { mark(#name); }
STEP(ep147_late_init)
STEP(ep_img_init)
STEP(sdk_connections_at_start)
STEP(sdk_cache_at_start)
STEP(sdk_slip_at_start)
STEP(msg_thread_note)
STEP(bt_uart_recover)
STEP(install_browse_columns)
STEP(install_browse_edit)
STEP(install_clock_native)
STEP(install_bt_mods)
STEP(install_font_size)
STEP(install_streaming_key)
STEP(install_key_pane)
STEP(install_spin_back)
STEP(install_crash_reporter)
STEP(load_settings_early)
STEP(wz_restore)
STEP(install_banner)
STEP(install_daymode_early)
STEP(install_menu_probe)
STEP(install_touchaccum)
STEP(install_menu_popup)
STEP(pngpatch_apply)
STEP(install_static_hooks)
STEP(sdk_beatport_install)
STEP(azm_hooks_go_late)
static int is_ep147(void) {
    mark("process");
    return is_target;
}
static int ep147_identify(const char *path, char *md5, long *size) {
    assert(!strcmp(path, "/proc/self/exe"));
    strcpy(md5, "0123456789abcdef0123456789abcdef");
    *size = 123;
    mark("identify");
    return identified;
}
static int ep147_addr_init(int build) {
    assert(build == identified);
    mark("addresses");
    return address_ok;
}
static const char *ep147_build_name(int build) {
    assert(build == identified);
    return "fixture";
}
static FILE *azlog_open(const char *path, const char *mode) {
    assert(!strcmp(path, "/tmp/azmod-build.log") && !strcmp(mode, "a"));
    return log_on ? tmpfile() : NULL;
}
static int fixture_access(const char *path, int mode) {
    assert(mode == F_OK);
    if (!strcmp(path, "/home/root/settings/azmod.deck-status.off")) return status_off ? 0 : -1;
    assert(!strcmp(path, "/home/root/settings/azmod.track-data.on") ||
           !strcmp(path, "/home/root/settings/azmod.native-sources.on") ||
           !strcmp(path, "/home/root/settings/azmod.native-source-files.on"));
    return -1;
}
static int azsdk_sources_configure(int enable, int build, unsigned policy) {
    assert(build == identified);
    last_policy = policy;
    if (enable) {
        configured++;
        mark("registry");
    } else {
        disabled++;
        mark("disable_registry");
    }
    return registry_ok ? AZS_OK : -1;
}
static int azsdk_sources_physical_ready(void) {
    mark("physical");
    return physical_ready;
}
static int azsdk_cache_capture_ready(void) {
    mark("cache");
    return cache_ready;
}
typedef int (*join_patch)(uintptr_t, uintptr_t, void *, void **, int);
static int azjs_install(int build, join_patch patch) {
    assert(build == identified && patch);
    mark("capture");
    return 1;
}
static int sdk_status_map_source_install(int build, join_patch patch) {
    assert(build == identified && patch);
    mark("maps");
    return 1;
}
static int sdk_status_save_install(int build, join_patch patch) {
    assert(build == identified && patch);
    mark("save");
    return 1;
}
static int azm_patch_vtable(uintptr_t at, void *hook, void **original) {
    assert(at && hook && original);
    mark("vtable");
    if (patch_result == 1) *original = (void *) *(uintptr_t *) at;
    return patch_result;
}
static int azm_install_hook_at(uintptr_t at, void *hook, void **original, const char *tag) {
    assert(at && hook && original && !strcmp(tag, "status-source"));
    mark("entry");
    if (patch_result == 1) *original = (void *) at;
    return patch_result;
}
static void azalt_this_thread(int unused) {
    assert(!unused);
    mark("altstack");
}
static int install_library_hooks(void) {
    mark("library");
    return library_ok;
}
static void *cr_watch(void *arg) { return arg; }
static void *overlay_thread(void *arg) { return arg; }
static int az_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    assert(!attr && !arg);
    created++;
    assert(fn == (created == 1 ? cr_watch : overlay_thread));
    mark(created == 1 ? "crash_thread" : "overlay_thread");
    if (thread_failure) return 1;
    *thread = (pthread_t) created;
    return 0;
}
static int fixture_detach(pthread_t thread) {
    assert(thread == (pthread_t) created);
    detached++;
    return 0;
}
#define access fixture_access
#define pthread_detach fixture_detach
#include ROUTING_IMPL_INC
#undef access
#undef pthread_detach

static void reset(void) {
    calls[0] = 0;
    created = detached = configured = disabled = 0;
    g_azm_inert = 1;
    g_ep147_build = g_sdk_sources_on = sdk_passive_sources = 0;
}
static void before(const char *a, const char *b) {
    const char *pa = strstr(calls, a), *pb = strstr(calls, b);
    assert(pa && pb && pa < pb);
}
static void constructor_paths(void) {
    reset();
    azmod_init();
    assert(!strcmp(calls, "process ") && g_azm_inert && !created);
    is_target = 1;
    reset();
    azmod_init();
    assert(!strcmp(calls, "process identify addresses ") && g_azm_inert && !created);
    address_ok = 1;
    for (unsigned variant = 0; variant < 6; variant++) {
        reset();
        status_off = variant == 1;
        registry_ok = variant != 2;
        physical_ready = variant != 3;
        cache_ready = variant != 4;
        library_ok = variant != 5;
        thread_failure = variant & 1;
        log_on = variant & 1;
        azmod_init();
        assert(!g_azm_inert && g_ep147_build == identified && created == 2);
        assert(detached == (thread_failure ? 0u : 2u));
        assert(configured == (status_off ? 0u : 1u));
        assert(sdk_passive_sources == !status_off);
        assert(g_sdk_sources_on == (!status_off && registry_ok && library_ok));
        assert(disabled == (variant == 5));
        if (!status_off && registry_ok && library_ok) assert(last_policy == AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER);
        assert((strstr(calls, "capture ") != NULL) == (!status_off && registry_ok && physical_ready && cache_ready));
        assert((strstr(calls, "maps ") != NULL) == !status_off);
        assert((strstr(calls, "save ") != NULL) == !status_off);
        before("ep147_late_init ", "ep_img_init ");
        before("sdk_connections_at_start ", "sdk_cache_at_start ");
        before("sdk_slip_at_start ", "msg_thread_note ");
        before("msg_thread_note ", "bt_uart_recover ");
        before("install_browse_columns ", "install_browse_edit ");
        before("altstack ", "install_crash_reporter ");
        before("install_crash_reporter ", "crash_thread ");
        before("load_settings_early ", "install_static_hooks ");
        before("wz_restore ", "install_banner ");
        before("install_static_hooks ", "azm_hooks_go_late ");
        before("sdk_beatport_install ", "azm_hooks_go_late ");
        before("azm_hooks_go_late ", "overlay_thread ");
    }
}
static void patch_paths(void) {
    uintptr_t slot = 0x1234;
    void *original = NULL;
    assert(!sdk_source_join_patch((uintptr_t) &slot, 0x9999, (void *) 7, &original, 1));
    assert(!original);
    patch_result = -1;
    assert(!sdk_source_join_patch((uintptr_t) &slot, slot, (void *) 7, &original, 1));
    assert(!original);
    patch_result = 1;
    assert(sdk_source_join_patch((uintptr_t) &slot, slot, (void *) 7, &original, 1));
    assert(original == (void *) slot);
    assert(sdk_source_join_patch(0x4321, 0, (void *) 7, &original, 0));
    assert(original == (void *) 0x4321);
    patch_result = -1;
    assert(!sdk_source_join_patch(0x4321, 0, (void *) 7, &original, 0));
}
int main(void) {
    constructor_paths();
    patch_paths();
    puts("PASS core startup: process/build admission, source failure isolation and hook/thread order");
    return 0;
}
