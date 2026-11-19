/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual placement and XTEST admission functions, with an observed X boundary. */
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdk_surface_logic.h"
#include "sdk_logic.h"
struct overlay_ctx {
    int need_raise;
};
#include SURFACE_TYPES_INC
static struct az_surface g_sdk_surf[SDK_SURF_ALL];
static struct az_surface *g_sdk_focus_surf;
static int g_sdk_focus, g_sdk_view, g_four_deck;
static int live = 1, create_failed, created, placed, shown, passive;
static int sdk_mod_live(int mod) {
    assert(mod == 3);
    return live;
}
static Window ov_create_window(struct overlay_ctx *c, XSetWindowAttributes *a, int x, int y, int w, int h, int pass) {
    (void) c;
    assert(a->override_redirect == True && a->background_pixmap == None);
    assert(a->event_mask == (ButtonPressMask | ButtonReleaseMask | ExposureMask));
    assert(x == -16 && y == -16 && w == 1 && h == 1);
    ++created;
    passive = pass;
    return create_failed ? None : 42;
}
static void ov_window_place(struct overlay_ctx *c, struct ov_window *w, int x, int y, int width, int height) {
    (void) c;
    ++placed;
    w->x = x;
    w->y = y;
    w->w = width;
    w->h = height;
}
static void ov_window_show(struct overlay_ctx *c, struct ov_window *w) {
    (void) c;
    ++shown;
    w->mapped = w->wanted;
}
static int extension, queries;
Bool XTestQueryExtension(Display *dpy, int *event, int *error, int *major, int *minor) {
    assert(dpy == (Display *) (uintptr_t) 1);
    (void) event;
    (void) error;
    (void) major;
    (void) minor;
    ++queries;
    return extension;
}
int XTestFakeMotionEvent(Display *dpy, int screen, int x, int y, unsigned long delay) {
    (void) dpy;
    (void) screen;
    (void) x;
    (void) y;
    (void) delay;
    return 1;
}
int XTestFakeButtonEvent(Display *dpy, unsigned button, Bool down, unsigned long delay) {
    (void) dpy;
    (void) button;
    (void) down;
    (void) delay;
    return 1;
}
static int (*p_xtest_motion)(Display *, int, int, int, unsigned long);
static int (*p_xtest_button)(Display *, unsigned int, int, unsigned long);
#include SURFACE_IMPL_INC
static int input(const az_event *event, void *user) {
    (void) event;
    (void) user;
    return 1;
}
static void placement(void) {
    struct overlay_ctx context = {0};
    struct az_surface *surface = &g_sdk_surf[0];
    *surface = (struct az_surface) {.used = 1, .mod = 3, .d = {.layer = AZ_LAYER_OVER_EP147, .views = AZ_VIEW_BIT(AZ_VIEW_DECK)}, .r = {4, 8, 30, 40}, .show = 1};
    g_sdk_view = AZ_VIEW_DECK;
    sdk_overlay_place(&context, AZ_LAYER_TOP);
    assert(!created && !placed && !shown);
    create_failed = 1;
    sdk_overlay_place(&context, AZ_LAYER_OVER_EP147);
    assert(created == 1 && !placed && !shown && surface->w.id == None);
    create_failed = 0;
    sdk_overlay_place(&context, AZ_LAYER_OVER_EP147);
    assert(created == 2 && passive && placed == 1 && shown == 1 && surface->w.id == 42);
    assert(surface->redraw && context.need_raise && surface->w.mapped && surface->w.wanted);
    assert(surface->w.x == 4 && surface->w.y == 8 && surface->w.w == 30 && surface->w.h == 40);
    context.need_raise = surface->redraw = 0;
    sdk_overlay_place(&context, AZ_LAYER_OVER_EP147);
    assert(created == 2 && placed == 2 && shown == 2 && !surface->redraw && !context.need_raise);
    surface->r.w = 70;
    sdk_overlay_place(&context, AZ_LAYER_OVER_EP147);
    assert(surface->redraw && surface->w.w == 70 && !context.need_raise);
    g_sdk_focus_surf = surface;
    g_sdk_focus = 1;
    live = 0;
    sdk_overlay_place(&context, AZ_LAYER_OVER_EP147);
    assert(!surface->w.wanted && !surface->w.mapped && !g_sdk_focus_surf && !g_sdk_focus);
    live = 1;
    surface->show = 0;
    sdk_overlay_place(&context, AZ_LAYER_OVER_EP147);
    assert(!surface->w.wanted);
    surface->show = 1;
    surface->d.views = AZ_VIEW_BIT(AZ_VIEW_SEARCH);
    sdk_overlay_place(&context, AZ_LAYER_OVER_EP147);
    assert(!surface->w.wanted);
    surface->w.id = None;
    surface->d.views = AZ_VIEW_BIT(AZ_VIEW_DECK);
    surface->d.on_input = input;
    sdk_overlay_place(&context, AZ_LAYER_OVER_EP147);
    assert(created == 3 && !passive && surface->w.mapped && context.need_raise);
}
static void play_state(void) {
    struct sd_play state = {0};
    assert(sd_play_step(&state, -1, 100, 0) == -1);
    assert(sd_play_step(&state, 1, 100, 0) == -1);
    assert(sd_play_step(&state, 1, 100, 30) == 0);
    assert(sd_play_step(&state, 1, 2000, 60) == 1);
    assert(sd_play_step(&state, 1, 2000, 60 + SD_PLAY_HOLD_MS) == 1);
    assert(sd_play_step(&state, 1, 2000, 61 + SD_PLAY_HOLD_MS) == 0);
    assert(sd_play_step(&state, 1, 0, 700) == 0);
    assert(sd_play_step(&state, 0, -1, 800) == 0 && !state.have);
    assert(sd_play_step(&state, 1, 0, 830) == -1);
    assert(sd_play_step(&state, 1, -1, 860) == -1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    extension = !strcmp(argv[1], "available");
    assert(sdk_xtest_load((Display *) (uintptr_t) 1) == extension);
    assert(sdk_xtest_load((Display *) (uintptr_t) 1) == extension && queries == 1);
    assert((p_xtest_motion != NULL) == extension && (p_xtest_button != NULL) == extension);
    placement();
    play_state();
    puts("PASS surface placement: layers, create retry, geometry, redraw/raise, owner/view visibility, focus and XTEST admission");
    return 0;
}
