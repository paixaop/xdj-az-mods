/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"
#include "sdk_logic.h"

enum {
    EPB_V130 = 1,
    EPB_V200 = 2
};
static int g_ep147_build;
static _Atomic long g_viewleave_n;

static int own_q(uintptr_t address, uintptr_t *value) {
    memcpy(value, (void *) address, sizeof *value);
    return 1;
}
static int own_str(uintptr_t address, char *text, size_t size) {
    if (!address)
        return 0;
    snprintf(text, size, "%s", (const char *) address);
    return 1;
}

#include "text_entry_native.c"

enum {
    SDK_SURF_TEXT = 1,
    None = 0,
    False = 0
};

struct az_surface {
    int mod, show, redraw;
    struct {
        uintptr_t id;
        int mapped;
    } w;
};
struct overlay_ctx {
    void *dpy;
    int scr_w, scr_h;
};
static struct az_surface g_sdk_surf[2];
static struct az_surface *g_sdk_focus_surf;
static struct overlay_ctx ctx = {.scr_w = TE_WIDTH, .scr_h = TE_HEIGHT};
static struct overlay_ctx *g_sdk_ovc = &ctx;
static azmod_state g_state;
static struct {
    int up;
} g_sd_dlg;
typedef struct {
    int running;
} cm_mod;
static cm_mod g_cm[1] = {{.running = 1}};
static int on_overlay = 1, screen_owner = -1, create_window = 1;
static long clock_ms = 3000;
static az_surface_v1 modal;

static int sdk_on_overlay(void) { return on_overlay; }
static long now_ms(void) { return clock_ms; }
static int sdk_mod_live(int owner) { return g_cm[owner].running; }
int azmod_cm_screen_owner(const azmod_state *s) {
    assert(s == &g_state);
    return screen_owner;
}
static const az_screen_v1 *cm_scr(int owner) {
    static const az_screen_v1 s;
    return owner < 0 ? NULL : &s;
}
#define CM_RA() 0
static cm_mod *sdk_self(const char *id, uintptr_t address, const char *api, int flags) {
    (void) address;
    assert(!strcmp(api, "text_entry") && flags == 1);
    return id && !strcmp(id, "mod") ? &g_cm[0] : NULL;
}
static int cm_command_code(const cm_mod *c, uintptr_t address) {
    return c == &g_cm[0] && address > 1;
}
static void sdk_modal_init(struct az_surface *p, const az_surface_v1 *desc) {
    assert(p == &g_sdk_surf[SDK_SURF_TEXT]);
    modal = *desc;
}
static void sdk_surf_create_window(struct overlay_ctx *c, struct az_surface *p) {
    assert(c == &ctx);
    p->w.id = create_window ? 17 : None;
}
static int sdk_modal_show(struct az_surface *p, az_rect rect, int on) {
    if (on) {
        assert(rect.y == TE_TOP && rect.w == TE_WIDTH);
        assert(rect.y + rect.h == TE_HEIGHT);
    }
    p->show = on;
    return 0;
}
static void sdk_surf_place_window(struct overlay_ctx *c, struct az_surface *p, int wanted) {
    assert(c == &ctx && p->show && p->w.id && wanted);
    p->w.mapped = 1;
}
static void XSync(void *display, int discard) {
    assert(display == ctx.dpy && !discard && g_sdk_surf[SDK_SURF_TEXT].w.mapped);
}
void azcm_draw_rect(az_draw *d, int x, int y, int w, int h, int width, uint32_t color) {
    (void) d;
    (void) color;
    assert(w > 0 && h > 0 && width > 0 && x >= 0 && y >= TE_TOP);
}
void azcm_fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t color) {
    azcm_draw_rect(d, x, y, w, h, 1, color);
}
int azcm_text_width(const char *text, int size) { return (int) strlen(text) * size; }
uint32_t azcm_color(int role) { return role; }
void azcm_draw_text(az_draw *d, int x, int y, const char *value, int size, uint32_t color) {
    (void) d;
    (void) color;
    assert(x >= 0 && y >= TE_TOP && value && size > 0);
}

#include "sdk_text_entry.c"

static unsigned widget_constructed, widget_hidden;
static int provide_peer = 1;
static void create_widget(void *w) {
    assert(w);
    ++widget_constructed;
}
static void create_controller(void *c, void *w) { assert(c && w); }
static void add_listener(void *c, void *listener) {
    assert(c && *(const void **) listener == &te_listener_vtable);
}
static void set_bounds(void *w, int x, int y, int width, int height) {
    assert(w && !x && y + TE_TOP == TE_KEYS_Y && width == TE_WIDTH && height == TE_HEIGHT - TE_KEYS_Y);
}
static void add_to_desktop(void *w, int flags, void *window) {
    assert(w && !flags && (uintptr_t) window == 17);
    ((unsigned char *) w)[0xc0] = provide_peer;
}
static void set_visible(void *w, int value) { assert(w && value == 1); }
static void controller_visible(void *c, int visible) {
    assert(c == te_controller && te_widget && !visible);
    ++widget_hidden;
}
static const struct te_native_api native = {
    .widget_construct = create_widget,
    .controller_construct = create_controller,
    .controller_visible = controller_visible,
    .add_listener = add_listener,
    .set_bounds = set_bounds,
    .add_to_desktop = add_to_desktop,
    .set_visible = set_visible};

static unsigned callbacks;
static int final_status;
static char accepted[32];

static const char *result(int status, const char *text, void *user) {
    assert(user == &callbacks);
    ++callbacks;
    final_status = status;
    if (status != AZ_OK) {
        assert(!text);
        return NULL;
    }
    assert(text);
    if (!*text)
        return "Enter some text";
    snprintf(accepted, sizeof accepted, "%s", text);
    return NULL;
}

static void send(int kind, int dir, int x, int y) {
    az_event event = {.kind = kind, .dir = dir, .x = x, .y = y};
    assert(modal.on_input(&event, modal.user));
}
static void key(const char *text) { te_native_key(&te_listener, &text); }
static void close_ack(void) {
    assert(!te_surface()->show && atomic_load(&te_native_phase) == TE_CLOSE);
    te_native_tick(&native);
    assert(atomic_load(&te_native_phase) == TE_IDLE);
}
static az_text_entry_v1 request(const char *initial) {
    return (az_text_entry_v1) {.size = sizeof(az_text_entry_v1), .max_bytes = 5, .title = "Title", .initial = initial, .hint = "Hint", .on_result = result, .user = &callbacks};
}

static void test_native_keys(void) {
    g_state.mode = AZ_COMMUNITY;
    az_text_entry_v1 entry = request("abcéx");
    assert(azcm_text_entry(&entry) == AZ_OK);
    assert(azcm_text_entry(&entry) == AZ_BUSY);
    key("x");
    assert(!strcmp(te_text, "abcé"));
    te_native_tick(&native);
    assert(sdk_text_entry_tick() && te_surface()->show);
    modal.on_paint(NULL, (az_rect) {0, TE_TOP, TE_WIDTH, TE_HEIGHT - TE_TOP}, NULL);
    assert(!strcmp(te_text, "abcé"));
    key("x");
    assert(!strcmp(te_text, "abcé"));
    te_native_action(NULL, 0);
    assert(!strcmp(te_text, "abc"));
    key("é");
    assert(!strcmp(te_text, "abcé"));
    te_native_action(NULL, 1);
    te_native_action(NULL, 0);
    key("\n");
    key("");
    te_native_key(NULL, NULL);
    key(NULL);
    assert(!te_text[0]);
    send(AZ_EV_TAP, 0, 1110, 390);
    assert(te_surface()->show && !strcmp(te_dialog.error, "Enter some text"));
    send(AZ_EV_TAP, 0, 2, 390);
    assert(callbacks == 1);
    key("Z");
    assert(sdk_text_entry_tick() && te_surface()->redraw);
    send(AZ_EV_KNOB, 1, 0, 0);
    send(AZ_EV_CLICK, 0, 0, 0);
    assert(callbacks == 2 && final_status == AZ_OK && !strcmp(accepted, "Z"));
    close_ack();
}

static void test_owner_changes(void) {
    az_text_entry_v1 entry = request("");
    g_state.mode = AZ_COMMUNITY;
    assert(sdk_text_entry("mod", &entry) == AZ_INVALID);
    screen_owner = 0;
    assert(sdk_text_entry("mod", &entry) == AZ_OK);
    te_native_tick(&native);
    send(AZ_EV_BACK, 0, 0, 0);
    assert(final_status == AZ_CANCELLED);
    close_ack();

    assert(sdk_text_entry("mod", &entry) == AZ_OK);
    te_native_tick(&native);
    screen_owner = -1;
    assert(!sdk_text_entry_tick());
    close_ack();

    g_state.mode = AZ_CLOSED;
    g_sdk_focus_surf = &g_sdk_surf[0];
    g_sdk_focus_surf->show = 1;
    assert(sdk_text_entry("mod", &entry) == AZ_OK);
    te_native_tick(&native);
    sdk_text_source_changed(&g_cm[0]);
    close_ack();

    assert(sdk_text_entry("mod", &entry) == AZ_OK);
    atomic_fetch_add(&g_viewleave_n, 1);
    assert(!sdk_text_entry_tick() && final_status == AZ_CANCELLED);
    close_ack();

    assert(sdk_text_entry("mod", &entry) == AZ_OK);
    unsigned seen = callbacks;
    g_cm[0].running = 0;
    assert(!sdk_text_entry_tick() && callbacks == seen);
    close_ack();
}

static void test_failures(void) {
    az_text_entry_v1 entry = request("");
    g_state.mode = AZ_COMMUNITY;
    g_ep147_build = 0;
    assert(azcm_text_entry(&entry) == AZ_UNSUPPORTED);
    g_ep147_build = EPB_V130;
    assert(te_native_api() == &te_native_v130);
    g_ep147_build = EPB_V200;
    assert(te_native_api() == &te_native_v200);
    assert(azcm_text_entry(NULL) == AZ_INVALID);
    te_surface()->w.id = None;
    create_window = 0;
    assert(azcm_text_entry(&entry) == AZ_FAILED);
    create_window = 1;
    provide_peer = 0;
    assert(azcm_text_entry(&entry) == AZ_OK);
    te_native_tick(&native);
    assert(!sdk_text_entry_tick() && final_status == AZ_FAILED);
    close_ack();
    assert(azcm_text_entry(&entry) == AZ_OK);
    assert(sdk_text_entry_tick());
    clock_ms += 2001;
    assert(!sdk_text_entry_tick() && final_status == AZ_FAILED);
    close_ack();
}

int main(void) {
    g_ep147_build = EPB_V200;
    test_native_keys();
    test_owner_changes();
    test_failures();
    assert(widget_constructed == 1 && widget_hidden >= 6);
    puts("Native text entry: UTF-8, length limits, validation, ownership, cancellation and failure");
    return 0;
}
