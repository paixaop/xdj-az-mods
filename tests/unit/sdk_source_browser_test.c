/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "azmod-mod.h"

typedef struct {
    int sticky, running;
    az_source_provider_v1 source_provider;
    _Atomic enum az_source_storage_mode source_provider_mode;
    pthread_mutex_t source_provider_worker;
    char source_browser_name[32];
    void (*source_browser_view)(enum az_source_view, void *);
    void *source_browser_user;
    void (*source_browser_query)(const char *, void *);
    unsigned char *source_browser_png;
    int source_browser_png_size;
    uintptr_t source_browser_image;
    int source_browser_image_decoded;
    char source_browser_search[128];
    int source_browser_search_dirty;
} cm_mod;

enum {
    EPB_V200 = 2,
    CM_MAX = 3
};
static int g_ep147_build = EPB_V200;
static unsigned wd_stall;
static int g_sdk_focus;
static cm_mod g_cm[CM_MAX];
static int g_sdk_in_init = 1;
static int g_cm_on_ui = 1;
static int g_cm_loading;
static int g_nm_hooked = 1;
static int install_msgtick_hook(const char *who) { assert(*who); return 1; }
static _Atomic int g_bv_vis, g_sv_vis;
static _Atomic int g_browseview_shown, g_visdbg, g_wave_vis;
static _Atomic unsigned long g_bv_obj, g_sv_obj;
static long g_waveview;
static int g_search_on, naming_event, forwarded, visibility_logs, readable;
static int keyboard_transitions;
enum { SEARCHVIEW_VT = 1, BROWSEVIEW_VT = 2 };

static void nm_view_changed(int search) { naming_event = search + 1; }
static void forward_visibility(long self, int on) { (void)self; forwarded = on + 1; }
static void (*g_orig_setvis)(long, int) = forward_visibility;
static int own_q(uintptr_t ptr, uintptr_t *value) {
    if (readable) memcpy(value, (const void *)ptr, sizeof *value);
    return readable;
}
static void dbg(const char *format, ...) { assert(format); ++visibility_logs; }
static void source_keyboard_tick(void) { ++keyboard_transitions; }
#include "view_visibility.c"
static long test_time;

static long now_ms(void) { return test_time; }
static int cmi_png_fit(unsigned char *data, int size, uint32_t width, uint32_t height) {
    assert(width == 128 && height == 128);
    return size == 2 && data[0] == 'P' ? size : 0;
}
static uintptr_t mp_image_uncached(const unsigned char *data, int size) {
    assert(size == 2 && data[0] == 'P');
    return 0x1234;
}

#define CM_RA() 0
static int cm_running(const cm_mod *owner) { return owner->running; }
static int cm_enter(cm_mod *owner) { return cm_running(owner); }
static void cm_leave(cm_mod *owner) { assert(owner->running); }
static cm_mod *cm_self(const char *id, uintptr_t caller, const char *operation) {
    (void) caller;
    assert(!strcmp(operation, "register_source_browser") || !strcmp(operation, "source_show_view") ||
           !strcmp(operation, "register_source_provider") || !strcmp(operation, "source_set_storage_mode"));
    if (!strcmp(id, "youtube")) return &g_cm[0];
    if (!strcmp(id, "other")) return &g_cm[1];
    return !strcmp(id, "provider") ? &g_cm[2] : NULL;
}
static int cm_command_code(const cm_mod *owner, uintptr_t ptr) {
    return owner >= g_cm && owner < g_cm + CM_MAX && ptr != 1;
}

static int img_q(uintptr_t address, uintptr_t *entry) {
    (void) address;
    (void) entry;
    return 0;
}
static int azm_install_hook(uintptr_t address, void *hook, void **original) {
    (void) address;
    (void) hook;
    (void) original;
    return 0;
}
static int azm_patch_vtable(uintptr_t address, void *hook, void **original) {
    return azm_install_hook(address, hook, original);
}

static void sdk_text_source_changed(const cm_mod *owner)
{
    (void)owner;
}

#include "sdk_source_browser.c"

static struct { char name[64]; } g_nm;
static _Atomic int g_nm_dirty;
static int naming, native_keys, native_actions;
static int nm_naming(void) { return naming; }
static void nm_action(int action) { assert(action == 1); g_nm.name[0] = '\0'; }
static void own_str(uintptr_t address, char *output, size_t capacity) {
    snprintf(output, capacity, "%s", (const char *)address);
}
static void stock_key(void *self, void *text) { assert(!self && text); ++native_keys; }
static void stock_action(void *self, int action) { assert(!self && action == 0); ++native_actions; }
static void (*g_orig_sv_key)(void *, void *) = stock_key;
static void (*g_orig_sv_bs)(void *, int) = stock_action;
#include "browse_keyboard.c"

static void press_key(const char *text) { sv_key_hook(NULL, &text); }

enum { RAW = 1, MENU = 2, FILTER = 4 };
static int observed, reorder_calls, knob_anchored;
static uint8_t raw_key, knob_anchor;
static _Atomic int g_reorder_enabled;

static void frame_crc_stamp(uint8_t *frame, size_t length) {
    assert(length == 96);
    frame[length] = frame[32] ^ frame[34];
}
static int frame_crc_ok(const uint8_t *frame, size_t length) {
    assert(length == 96);
    return frame[length] == (frame[32] ^ frame[34]);
}
static void bus_capture_mixer_in(const uint8_t *frame, ssize_t length) {
    (void) frame;
    (void) length;
}
static void bus_capture_mixer(const uint8_t *frame, ssize_t length) {
    bus_capture_mixer_in(frame, length);
}
static void mixer_drum_tone(const uint8_t *frame, ssize_t length) {
    bus_capture_mixer_in(frame, length);
}
static void mixer_stem_eq(const uint8_t *frame, ssize_t length) {
    bus_capture_mixer_in(frame, length);
}
static void mixer_fx_disarm(const uint8_t *frame, ssize_t length) {
    bus_capture_mixer_in(frame, length);
}
static void mixer_xpad_drum(const uint8_t *frame, ssize_t length, uint64_t cfg) {
    (void) cfg;
    bus_capture_mixer_in(frame, length);
}
static int mixer_browse_knob(const uint8_t *frame, int *delta) {
    *delta = frame[34];
    return knob_anchored;
}
static int bus_cfg_reorder(uint64_t cfg) { return cfg != 0; }
static void mixer_reorder_gesture(uint8_t *frame, int delta) {
    assert(frame[34] == delta);
    ++reorder_calls;
}
static int mixer_watchdog(void) { return 0; }
static void mixer_menu_buttons(const uint8_t *frame, int crc_in, int menu_open) {
    assert(!menu_open);
    assert(!crc_in || frame_crc_ok(frame, 96));
    observed |= MENU;
}
static void sdk_filter_raw(int bus, const uint8_t *frame, int length) {
    assert(bus == AZ_BUS_MIXER_IN && length >= 98);
    assert(!observed);
    observed |= RAW;
    raw_key = frame[32];
}
static int sdk_filter_run(int bus, uint8_t *frame, int length) {
    assert(bus == AZ_BUS_MIXER_IN && length >= 98);
    assert(observed == (RAW | MENU));
    observed |= FILTER;
    frame[34] += 1;
    return 1;
}

#include "mixer_input.c"

static enum az_source_view last_view;
static int draws;
static void on_view(enum az_source_view view, void *user) {
    assert(user == &draws);
    last_view = view;
    ++draws;
}

static void expect_view(enum az_source_view view) {
    int previous = draws;
    sdk_source_browser_tick();
    assert(draws == previous + 1 && last_view == view);
    sdk_source_browser_tick();
    assert(draws == previous + 1);
}

static void registration(void) {
    assert(!sdk_source_service(AZ_SOURCE_ABI + 1));
    g_ep147_build = 1;
    assert(!sdk_source_service(AZ_SOURCE_ABI));
    g_ep147_build = EPB_V200;
    const az_source_v1 *api = sdk_source_service(AZ_SOURCE_ABI);
    assert(AZ_SOURCE_HAS(api, register_browser));
    char name[] = "YouTube";
    az_source_browser_v1 descriptor = {.size = sizeof(descriptor), .name = name,
                                       .on_view = on_view, .user = &draws};
    assert(api->register_browser("wrong", &descriptor) == AZ_INVALID);
    assert(api->register_browser("youtube", NULL) == AZ_INVALID);
    g_sdk_in_init = 0;
    assert(api->register_browser("youtube", &descriptor) == AZ_INVALID);
    g_sdk_in_init = 1;
    assert(api->register_browser("youtube", &descriptor) == AZ_UNSUPPORTED);
    atomic_store(&sbn_installed, 1); /* replace the firmware install with the native fixture's result */
    g_source_browser_install_result = 0;
    assert(api->register_browser("youtube", &descriptor) == AZ_OK);
    assert(api->register_browser("youtube", &descriptor) == AZ_BUSY);
    name[0] = 'X';
    assert(!sdk_source_browser_label(0));
    sdk_source_browser_activate(0, NULL);
    assert(!sdk_source_browser_is_selected() && g_cm[0].sticky);
    g_cm[0].running = 1;
    assert(!strcmp(sdk_source_browser_label(0), "YouTube"));
}

static void frame(unsigned buttons, unsigned other, unsigned expected) {
    uint8_t data[98] = {[6] = 1, [32] = buttons, [33] = other};
    assert(sdk_source_browser_filter(data) == (expected != buttons));
    assert(data[32] == expected && data[33] == other);
    assert(data[6] == (expected != buttons && !expected && !other ? 0x08 : 1));
}

static void routing(void) {
    frame(0x40, 0, 0x40);
    frame(0, 0, 0);
    sdk_source_browser_activate(0, NULL);
    expect_view(AZ_SOURCE_VIEW_BROWSE);
    frame(0x08, 0, 0);
    expect_view(AZ_SOURCE_VIEW_SEARCH);
    frame(0x08, 1, 0);
    frame(0, 0, 0);
    frame(0x40, 0, 0);
    expect_view(AZ_SOURCE_VIEW_BROWSE);
    frame(0x40 | 0x20, 0, 0x20);
    expect_view(AZ_SOURCE_VIEW_HIDDEN);
    frame(0, 0, 0);
    sdk_source_browser_activate(0, NULL);
    g_sdk_focus = 1;
    frame(0x08, 0, 0);
    expect_view(AZ_SOURCE_VIEW_SEARCH);
    frame(0x80, 0, 0);
    expect_view(AZ_SOURCE_VIEW_HIDDEN);
    frame(0x80, 0, 0);
    frame(0, 0, 0);
    frame(0x80, 0, 0x80);
    expect_view(AZ_SOURCE_VIEW_HIDDEN);
    sdk_source_browser_native_selected();
    expect_view(AZ_SOURCE_VIEW_HIDDEN);
    frame(0x40, 0, 0x40);
    frame(0, 0, 0);
    frame(0x08, 0, 0x08);
    frame(0, 0, 0);
    sdk_source_browser_activate(0, NULL);
    sdk_source_browser_native_selected();
    sdk_source_browser_tick();
    wd_stall = 201;
    sdk_source_browser_activate(0, NULL);
    frame(0x40, 0, 0x40);
}

static void delivery(void) {
    uint8_t data[98] = {[32] = 0x08, [34] = 1};
    wd_stall = 0;
    bus_mixer_frame(data, sizeof(data), 1);
    assert(!observed && !reorder_calls && data[32] == 0x08);
    bus_mixer_frame(data, 8, 1);
    bus_mixer_frame(data, 30, 1);
    assert(!observed && !reorder_calls);
    bus_mixer_frame(data, 34, 1);
    assert(observed == MENU && data[32] == 0x08);
    observed = 0;
    frame_crc_stamp(data, 96);
    knob_anchored = 1;
    knob_anchor = 19;
    bus_mixer_frame(data, sizeof(data), 1);
    assert(observed == (RAW | MENU | FILTER) && raw_key == 0x08);
    assert(!data[32] && data[34] == 20 && frame_crc_ok(data, 96) && !reorder_calls);
    expect_view(AZ_SOURCE_VIEW_SEARCH);
    observed = 0;
    knob_anchored = 0;
    bus_mixer_frame(data, sizeof(data), 1);
    assert(reorder_calls == 1 && frame_crc_ok(data, 96));
}

static int native_draws, edits;
static enum az_source_view native_view;
static char query[128];

static void on_native_view(enum az_source_view view, void *user) {
    assert(user == &native_draws);
    native_view = view;
    ++native_draws;
}

static void on_query(const char *value, void *user) {
    assert(user == &native_draws);
    snprintf(query, sizeof query, "%s", value);
    ++edits;
}

static void native_routing(void) {
    const az_source_v1 *api = sdk_source_service(AZ_SOURCE_ABI);
    assert(AZ_SOURCE_HAS(api, show_view));
    unsigned char png[] = {'x', 0};
    az_source_browser_v1 descriptor = {.size = sizeof descriptor, .name = "Other",
        .on_view = on_native_view, .user = &native_draws, .on_query = on_query,
        .icon_png = png, .icon_bytes = sizeof png};
    sdk_source_browser_native_selected();
    sdk_source_browser_tick();
    g_cm_loading = 1;
    assert(api->register_browser("other", &descriptor) == AZ_INVALID);
    g_nm_hooked = 0;
    assert(api->register_browser("other", &descriptor) == AZ_UNSUPPORTED);
    g_nm_hooked = 1;
    png[0] = 'P';
    assert(api->register_browser("other", &descriptor) == AZ_OK);
    png[0] = 'x';
    g_cm[1].running = 1;
    assert(sdk_source_browser_count() == 2 && !strcmp(sdk_source_browser_label(1), "Other"));
    assert(!sdk_source_browser_icon(0, mp_image_uncached));
    assert(sdk_source_browser_icon(1, mp_image_uncached) == 0x1234);
    assert(sdk_source_browser_icon(1, mp_image_uncached) == 0x1234);
    assert(api->show_view("other", AZ_SOURCE_VIEW_BROWSE) == AZ_STALE);

    sdk_source_browser_activate(1, NULL);
    sdk_source_browser_tick();
    assert(!native_draws && sdk_source_browser_active(1) && !sdk_source_browser_active(0));
    uint8_t idle[98] = {[6] = 8};
    assert(sdk_source_browser_filter(idle) && idle[32] == 0x40 && !idle[6]);
    test_time += 300;
    frame(0, 0, 0);
    g_bv_vis = 1;
    sdk_source_browser_tick();
    assert(native_draws == 1 && native_view == AZ_SOURCE_VIEW_BROWSE);
    assert(!sdk_source_browser_editing() && !sdk_source_browser_action(0));

    frame(0x08, 0, 0x08);
    sdk_source_browser_tick();
    assert(native_draws == 1);
    g_bv_vis = 0;
    g_sv_vis = 1;
    sdk_source_browser_tick();
    assert(native_view == AZ_SOURCE_VIEW_SEARCH && sdk_source_browser_editing());
    readable = 1;
    press_key("a");
    press_key("é");
    sdk_source_browser_tick();
    assert(edits == 1 && !strcmp(query, "aé"));
    sv_bs_hook(NULL, 0);
    sdk_source_browser_tick();
    assert(edits == 2 && !strcmp(query, "a"));
    assert(sdk_source_browser_action(1));
    sdk_source_browser_tick();
    assert(edits == 3 && !query[0]);

    uint8_t load[98] = {[33] = 0x08};
    assert(sdk_source_browser_filter(load) && !load[33] && load[6] == 8);
    assert(api->show_view("other", AZ_SOURCE_VIEW_HIDDEN) == AZ_OK);
    sdk_source_browser_tick();
    assert(native_view == AZ_SOURCE_VIEW_HIDDEN && !sdk_source_browser_editing());
    sdk_source_browser_activate(0, NULL);
    expect_view(AZ_SOURCE_VIEW_BROWSE);
    assert(native_view == AZ_SOURCE_VIEW_HIDDEN && sdk_source_browser_active(0));
}

static void stock_keyboard(void) {
    press_key("a");
    sv_bs_hook(NULL, 0);
    assert(native_keys == 1 && native_actions == 1);
    naming = 1;
    readable = 1;
    sv_key_hook(NULL, NULL);
    press_key("House");
    assert(g_nm_dirty && !strcmp(g_nm.name, "House"));
    press_key("\n");
    assert(!strcmp(g_nm.name, "House"));
    sv_bs_hook(NULL, 1);
    assert(!g_nm.name[0] && native_keys == 1 && native_actions == 1);
    readable = 0;
    naming = 0;
}

static int keyboard_enabled;
static void enable_keyboard(uintptr_t keyboard, int enabled) {
    assert(keyboard);
    keyboard_enabled = enabled ? 1 : -1;
}

static void custom_keyboard_without_media(void) {
    unsigned long widget[26] = {0};
    unsigned long view[0x1930 / sizeof(unsigned long)] = {0};
    view[0x1928 / sizeof(unsigned long)] = (uintptr_t)widget;
    widget[0] = 0x100;
    widget[24] = 0x1000;
    readable = 1;
    assert(!source_search_keyboard(0, 0x100));
    assert(!source_search_keyboard((uintptr_t)view, 0x101));
    uintptr_t keyboard = source_search_keyboard((uintptr_t)view, 0x100);
    assert(keyboard == (uintptr_t)widget);
    source_keyboard_update(keyboard, 1, enable_keyboard);
    assert(keyboard_enabled == 1);
    widget[24] = 0;
    source_keyboard_update((uintptr_t)widget, 1, enable_keyboard);
    assert(keyboard_enabled == 1);
    source_keyboard_update((uintptr_t)widget, 0, enable_keyboard);
    assert(keyboard_enabled == -1);
    source_keyboard_update(0, 0, enable_keyboard);
}

static void firmware_visibility(void) {
    unsigned long object[6] = {SEARCHVIEW_VT};
    readable = 0;
    setvis_hook((long)object, 1);
    assert(g_sv_vis && g_search_on && naming_event == 2 && forwarded == 2);
    setvis_hook((long)object, 0);
    assert(!g_sv_vis && !g_search_on && forwarded == 1 && keyboard_transitions == 2);
    object[0] = BROWSEVIEW_VT;
    setvis_hook((long)object, 1);
    assert(g_bv_vis && g_browseview_shown && g_bv_obj == (unsigned long)object);
    setvis_hook((long)object, 0);
    assert(!g_bv_vis && naming_event == 1);
    g_visdbg = 1;
    object[0] = 3;
    object[5] = 401;
    g_waveview = (long)object;
    setvis_hook(0, 3);
    assert(forwarded == 4);
    setvis_hook((long)object, 1);
    assert(!visibility_logs && g_wave_vis);
    readable = 1;
    setvis_hook((long)object, 0);
    assert(visibility_logs == 1 && !g_wave_vis);
}

static void *native_source;
static int select_error, core_callbacks;

static int select_provider(void *source, void *model) {
    assert(source == &g_cm[2] && model == g_cm);
    if (select_error) return AZ_FAILED;
    native_source = source;
    return AZ_OK;
}

static void clear_provider(void) {
    assert(native_source);
    native_source = NULL;
}

static int provider_playlists(void *user, uint32_t offset, az_source_playlist_v1 *rows,
                              uint32_t capacity, az_source_page_v1 *page) {
    assert(user == &core_callbacks && !offset && capacity == 1);
    rows[0] = (az_source_playlist_v1) {.id = "playlist-42", .name = "Collections"};
    page->count = 1;
    ++core_callbacks;
    return AZ_OK;
}

static int provider_tracks(void *user, const char *id, uint32_t offset,
                           az_source_track_v1 *rows, uint32_t capacity, az_source_page_v1 *page) {
    (void) user;
    (void) id;
    (void) offset;
    (void) rows;
    (void) capacity;
    (void) page;
    return AZ_NOT_FOUND;
}

static int provider_file(void *user, const az_source_track_v1 *track, const char *directory,
                         char *path, uint32_t capacity) {
    assert(user == &core_callbacks && !strcmp(track->id, "chosen"));
    assert((unsigned) snprintf(path, capacity, "%s/song.wav", directory) < capacity);
    FILE *file = fopen(path, "w");
    assert(file && fputs("media", file) >= 0 && fclose(file) == 0);
    return !strcmp(track->title, "Refused") ? AZ_FAILED : AZ_OK;
}

static void provider_registration(void) {
    static const struct azsp_native native = {.storage_modes = 3, .select = select_provider, .clear = clear_provider};
    assert(!AZ_SOURCE_HAS(sdk_source_service(AZ_SOURCE_ABI), register_provider));
    assert(azsp_bind_native(NULL) == AZ_INVALID);
    assert(azsp_bind_native(&native) == AZ_OK);
    assert(azsp_bind_native(&native) == AZ_BUSY);
    const az_source_v1 *api = sdk_source_service(AZ_SOURCE_ABI);
    assert(AZ_SOURCE_HAS(api, register_provider) && AZ_SOURCE_HAS(api, set_storage_mode));
    char name[] = "Provider";
    unsigned char png[] = {'x', 0};
    az_source_provider_v1 provider = {.size = sizeof provider, .name = name, .user = &core_callbacks, .storage_mode = AZ_SOURCE_STREAMING, .list_playlists = provider_playlists, .playlist_tracks = provider_tracks, .search = provider_tracks, .prepare_track = provider_file, .icon_png = png, .icon_bytes = sizeof png};
    assert(api->register_provider("provider", &provider) == AZ_INVALID);
    g_cm_loading = 2;
    assert(api->register_provider("provider", &provider) == AZ_INVALID);
    png[0] = 'P';
    assert(api->register_provider("provider", &provider) == AZ_OK);
    assert(api->register_provider("provider", &provider) == AZ_BUSY);
    assert(sdk_source_browser_count() == 2);
    g_cm[2].running = 1;
    name[0] = 'x';
    png[0] = 'x';
    assert(sdk_source_browser_count() == 3 && !strcmp(sdk_source_browser_label(2), "Provider"));
    assert(sdk_source_browser_icon(2, mp_image_uncached) == 0x1234);
}

static void provider_prepare(void) {
    char parent[] = "/tmp/az-source-load.XXXXXX";
    assert(mkdtemp(parent));
    const az_source_v1 *api = sdk_source_service(AZ_SOURCE_ABI);
    const az_source_track_v1 track = {.id = "chosen", .title = "Selected track"};
    enum az_source_storage_mode admitted = azsp_source_storage_mode(native_source);
    assert(admitted == AZ_SOURCE_STREAMING);
    assert(api->set_storage_mode("provider", AZ_SOURCE_IMPORT) == AZ_OK);
    struct azsp_stage *stage;
    assert(azsp_prepare_source(native_source, &track, parent, admitted, &stage) == AZ_OK);
    assert(azsp_stage_mode(stage) == AZ_SOURCE_STREAMING);
    char path[AZ_SOURCE_PATH_BYTES];
    snprintf(path, sizeof(path), "%s", azsp_stage_path(stage));
    azsp_stage_retain(stage); /* decoder and analysis retain their own references */
    azsp_stage_release(stage);
    assert(access(path, R_OK) == 0);
    azsp_stage_release(stage);
    assert(access(path, F_OK) != 0);
    admitted = azsp_source_storage_mode(native_source);
    assert(azsp_prepare_source(native_source, &track, parent, admitted, &stage) == AZ_OK);
    assert(azsp_stage_mode(stage) == AZ_SOURCE_IMPORT);
    azsp_stage_release(stage);
    az_source_track_v1 failure = {.id = "chosen", .title = "Refused"};
    assert(azsp_prepare_source(native_source, &failure, parent, admitted, &stage) == AZ_FAILED && !stage);
    assert(azsp_prepare_source(native_source, &track, "relative", admitted, &stage) == AZ_INVALID && !stage);
    assert(rmdir(parent) == 0);
    assert(azsp_prepare_source(native_source, &track, parent, admitted, &stage) == AZ_FAILED && !stage);
    assert(api->set_storage_mode("provider", AZ_SOURCE_STREAMING) == AZ_OK);
}

static void provider_routing(void) {
    const az_source_v1 *api = sdk_source_service(AZ_SOURCE_ABI);
    select_error = 1;
    sdk_source_browser_activate(2, g_cm);
    expect_view(AZ_SOURCE_VIEW_HIDDEN);
    assert(!native_source && !sdk_source_browser_active(2));
    select_error = 0;
    sdk_source_browser_activate(2, g_cm);
    assert(native_source && sdk_source_browser_active(2));
    assert(!sdk_source_browser_editing());
    frame(0x48, 0x6f, 0x48);
    frame(0, 0, 0);
    struct azsp_result result;
    const struct azsp_request request = {.operation = AZSP_PLAYLISTS, .capacity = 1};
    assert(azsp_request_source(native_source, &request, &result) == AZ_OK);
    assert(result.page.count == 1 && !strcmp(result.value.playlists[0].id, "playlist-42"));
    assert(azsp_source_storage_mode(native_source) == AZ_SOURCE_STREAMING);
    assert(api->set_storage_mode("other", AZ_SOURCE_IMPORT) == AZ_INVALID);
    assert(api->set_storage_mode("provider", 0) == AZ_INVALID);
    g_cm_on_ui = 0;
    assert(api->set_storage_mode("provider", AZ_SOURCE_IMPORT) == AZ_INVALID);
    g_cm_on_ui = 1;
    assert(api->set_storage_mode("provider", AZ_SOURCE_IMPORT) == AZ_OK);
    assert(azsp_source_storage_mode(native_source) == AZ_SOURCE_IMPORT);
    assert(api->set_storage_mode("provider", AZ_SOURCE_STREAMING) == AZ_OK);
    assert(azsp_source_storage_mode(native_source) == AZ_SOURCE_STREAMING);
    provider_prepare();
    sdk_source_browser_native_selected();
    assert(!native_source && !sdk_source_browser_is_selected());
    g_cm[2].running = 0;
    assert(azsp_request_source(&g_cm[2], &request, &result) == AZ_STALE);
    assert(!result.page.count && core_callbacks == 1 && sdk_source_browser_count() == 2);
    free(g_cm[2].source_browser_png);
    assert(!pthread_mutex_destroy(&g_cm[2].source_provider_worker));
}

int main(void) {
    registration();
    routing();
    delivery();
    stock_keyboard();
    native_routing();
    custom_keyboard_without_media();
    firmware_visibility();
    provider_registration();
    provider_routing();
    puts("Source browser registration and physical button routing passed");
    return 0;
}
