/* SPDX-License-Identifier: GPL-3.0-only */
#include "azmod-mod.h"
#include "track_storage.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int running;
    az_plan_set_fn plan_set_fn;
    void *plan_set_user;
} cm_mod;

static cm_mod g_cm[2];
static int g_sdk_in_init, g_cm_loading, g_cm_on_ui, callback_owned = 1;
static int this_owner, usb_connected[2] = {1, 1};
static uint64_t usb_generation[2] = {40, 41}, usb_library[2] = {60, 61};
#define CM_RA() ((uintptr_t)1)

static cm_mod *cm_self(const char *id, uintptr_t address, const char *service)
{
    assert(address && service);
    return !strcmp(id, "planner") ? &g_cm[this_owner] : NULL;
}
static int cm_running(const cm_mod *c) { return c->running; }

static int cm_called_by(cm_mod *c, uintptr_t callback)
{
    assert(c && callback);
    return callback_owned;
}

int st_source_read(int stick, struct st_source *out)
{
    assert(stick >= 0 && stick < 2);
    if (!usb_connected[stick])
        return AZ_STALE;
    *out = (struct st_source){.ref = {.source_generation = usb_generation[stick], .library_id = usb_library[stick]}};
    snprintf(out->mount, sizeof out->mount, "/media/usb/%d", stick + 1);
    return AZ_OK;
}

enum { SQLDB_CONN = 0x10, NATIVE_DB = 0x1000 };
static unsigned bound_media;
static int mlserver_chain(void *server, uintptr_t *svc, uintptr_t *lib, uintptr_t *db, unsigned *media)
{
    assert(server);
    *svc = *lib = *db = NATIVE_DB;
    *media = bound_media;
    return !bound_media;
}
static int own_q(uintptr_t address, uintptr_t *out);
static int cp_mount_of(void *connection, char *mount, size_t size)
{
    assert(connection == (void *)NATIVE_DB);
    return snprintf(mount, size, "/media/usb/%u", bound_media) < (int)size;
}
#include "playlist_media.c"
#include "sdk_plan_set.c"

enum { CONTROLLER = 0x1000, WIDGET = 0x2000, BUTTON = 0x3000, FIRST = 0x4000,
       TAPPED = 0x5000, CLICKED_NAME = 0x6000, MP_TIB_VT0 = 11,
       MP_TYPE_ADD = 8,
       MP_REBUILD = 20, MP_PRODUCER, MP_CLICK_SLOT, MP_RESULT_SLOT, MP_WIDGET_PAINT_SLOT };
enum { MP_CLICK_ORIG = 31, MP_RESULT_ORIG, MP_WIDGET_PAINT_ORIG };
struct plsnap { unsigned sel_folder, sel_pl, open_pl, any_media; char sel_title[96]; };
static struct plsnap highlight;
static unsigned mode, rebuilds, stock_clicks, closes, delivered, shown, image_failure, allocation_failure;
static unsigned bad_slot, hook_failure, tag_installs, loaded_images, probe;
static int g_mp_installed, g_mp_b3_on;
static const uintptr_t *g_mp_mk_single;
static int g_mp_newmode, g_mp_editmode, edit_enabled, panel_open = 1;
static int other_slots, button_x, button_width;
static struct { int on; } g_mp_anim;
#define MP_SLOT_X(i) (5 + (i) * 212)

#define PEER(name, i) static uintptr_t name = FIRST + (i) * 0x100, name##_widget = WIDGET
PEER(g_mp_b3, 2);
PEER(g_mp_bm, 3);
PEER(g_mp_bt, 4);
PEER(g_mp_ba, 5);
PEER(g_mp_bnm, 6);
PEER(g_mp_bed, 7);
PEER(g_mp_bs, 8);
PEER(g_mp_brn, 9);

static int peer_index(uintptr_t button) { return (int)(button - FIRST) / 0x100; }

static int mp_visible(uintptr_t handle)
{
    if (handle == WIDGET)
        return panel_open;
    if (!other_slots)
        return false;
    return g_mp_editmode ? handle == g_mp_brn : peer_index(handle) < other_slots;
}

static int mp_enabled(void) { return edit_enabled; }
static bool mp_ctrl_live(uintptr_t controller) { return controller == CONTROLLER; }

struct bn_holder { char text[32]; };
#define DECLARE_NAME(base) static struct bn_holder base; static uintptr_t base##_s
DECLARE_NAME(g_mp_title);
DECLARE_NAME(g_mp_body);
DECLARE_NAME(g_mp_b3_name);
DECLARE_NAME(g_mp_bm_name);
DECLARE_NAME(g_mp_ba_name);
DECLARE_NAME(g_mp_bt_name);
DECLARE_NAME(g_mp_bu_name);
DECLARE_NAME(g_mp_bed_name);
DECLARE_NAME(g_mp_bnm_name);
DECLARE_NAME(g_mp_brn_name);
DECLARE_NAME(g_mp_bs_name);
DECLARE_NAME(g_mp_bn_name);
DECLARE_NAME(g_mp_bf_name);
#define MP_B3_OFF_FLAG "/fixture/nonexistent"
#define AZ_PROBE_ON(path) ((void)(path), probe)
static void (*g_mp_orig_rebuild)(void *);
static void (*g_mp_orig_producer)(void *);
static void (*g_mp_orig_click)(void *, void *);
static void (*g_mp_orig_result)(void *, int);
static void (*g_mp_orig_paint)(void *, void *);

static void bn_set(struct bn_holder *n, const char *text, size_t len)
{
    assert(len < sizeof n->text);
    memcpy(n->text, text, len);
    n->text[len] = 0;
}
static int own_q(uintptr_t address, uintptr_t *out)
{
    switch (address) {
    case NATIVE_DB + SQLDB_CONN: *out = NATIVE_DB; return 1;
    case CONTROLLER + 0x18: *out = mode; return 1;
    case CONTROLLER + 0xa0: *out = WIDGET; return 1;
    case TAPPED:
    case BUTTON + 0x1d8: *out = CLICKED_NAME; return 1;
    default: break;
    }
    if (address >= FIRST && address <= g_mp_brn + 0x28) {
        int within = (int)(address - FIRST) % 0x100;
        if (within == 0x20) {
            int index = g_mp_editmode ? 2 : peer_index(address);
            *out = (UINT64_C(36) << 32) | (uintptr_t)MP_SLOT_X(index);
            return 1;
        }
        if (within == 0x28) {
            *out = 210;
            return 1;
        }
    }
    return 0;
}
static int img_q(uintptr_t address, uintptr_t *out)
{
    if (address == bad_slot)
        return 0;
    switch (address) {
    case MP_CLICK_SLOT: *out = MP_CLICK_ORIG; return 1;
    case MP_RESULT_SLOT: *out = MP_RESULT_ORIG; return 1;
    case MP_WIDGET_PAINT_SLOT: *out = MP_WIDGET_PAINT_ORIG; return 1;
    default: return 0;
    }
}
static struct plsnap plsnap_get(void) { return highlight; }
static const unsigned char ML_NEWPL_W210_normal[] = {210, 0};
static const unsigned char ML_NEWPL_W210_down[] = {210, 1};
static const unsigned char ML_NEWPL_W210_disabled[] = {210, 2};
static const unsigned char ML_NEWPL_normal[] = {246, 0};
static const unsigned char ML_NEWPL_down[] = {246, 1};
static const unsigned char ML_NEWPL_disabled[] = {246, 2};
#define MP_FT_INK {255, 240, 180}
static int font_missing, art_width[12], released_images;
static uint8_t art[12][80 * 246 * 4];

static uintptr_t mp_image_uncached(const unsigned char *src, int size)
{
    assert(size == 2 && loaded_images + 1 < 12);
    if (image_failure == (unsigned)src[1] + 1)
        return 0;
    uintptr_t image = ++loaded_images;
    art_width[image] = src[0];
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < src[0]; ++x) {
            uint8_t *pixel = art[image] + (y * src[0] + x) * 4;
            memset(pixel, (x % 8 + y % 8) * 4 + src[1], 3);
            pixel[3] = 255;
        }
    return image;
}
static void mp_release(uintptr_t image) { released_images += image != 0; }
static uint8_t *mp_pixels_w(uintptr_t image, int *stride, int width)
{
    if (!image)
        return NULL;
    assert(art_width[image] == width);
    *stride = width * 4;
    return art[image];
}

struct mp_gl { uint32_t cp; int x; };
typedef struct mock_glyph {
    int bitmap_left, bitmap_top;
    struct { unsigned rows, width; int pitch; const uint8_t *buffer; } bitmap;
} *FT_GlyphSlot;
static int mp_ft_ready(void) { return !font_missing; }
static int mp_lay(const uint32_t *text, int n, struct mp_gl *glyphs, double *unused)
{
    (void)unused;
    for (int i = 0; i < n; ++i)
        glyphs[i] = (struct mp_gl){.cp = text[i], .x = i * 10};
    return n;
}
static int mp_ink(const struct mp_gl *glyphs, int n, int *low, int *high)
{
    *low = glyphs[0].x;
    *high = glyphs[n - 1].x + 1;
    return n > 0;
}
static FT_GlyphSlot mp_glyph(uint32_t cp)
{
    static const uint8_t pixels[] = {255, 128, 128, 255};
    static struct mock_glyph glyph = {.bitmap_top = 2, .bitmap = {2, 2, 2, pixels}};
    return cp == ' ' ? NULL : &glyph;
}

static int mp_valid(uintptr_t handle, uintptr_t type)
{
    return handle == BUTTON && type == MP_TIB_VT0;
}
static int mp_swap_set(uintptr_t button, int slot, const uintptr_t images[3])
{
    assert(button == BUTTON && slot == 0 && images[0] && images[1] && images[2]);
    return 1;
}
static uintptr_t mp_make_btn(uintptr_t controller, uintptr_t widget, uintptr_t first,
                             uintptr_t *name, const int *sets, int count)
{
    assert(controller == CONTROLLER && widget == WIDGET && first == FIRST);
    assert(name && *name && !sets && !count && g_mp_mk_single && g_mp_mk_single[1]);
    return allocation_failure ? 0 : BUTTON;
}
static void mp_show(uintptr_t button, int index, int enabled, int visible)
{
    assert(button == BUTTON && index == 0 && enabled == visible);
    shown = (unsigned)visible;
}
static int mp_parts(uintptr_t controller, uintptr_t *first, uintptr_t *second)
{
    if (controller != CONTROLLER)
        return 0;
    *first = FIRST;
    *second = FIRST + 0x100;
    return 1;
}
static void mp_bounds_w(uintptr_t button, int x, int w)
{
    assert(button == BUTTON);
    button_x = x;
    button_width = w;
}
static void mp_close(uintptr_t controller)
{
    assert(controller == CONTROLLER);
    ++closes;
}
static void mp_rebuild_hook(void *controller)
{
    assert((uintptr_t)controller == CONTROLLER);
    ++rebuilds;
}
static void mp_click_hook(void *controller, void *arg)
{
    assert((uintptr_t)controller == CONTROLLER);
    (void)arg;
    ++stock_clicks;
}
static void mp_producer_hook(void *unused) { (void)unused; }
static void mp_result_hook(void *unused, int value) { (void)unused; (void)value; }
static void mp_paint_hook(void *unused, void *graphics) { (void)unused; (void)graphics; }
static void mp_log(const char *format, ...) { (void)format; }
static int azm_install_hook(uintptr_t address, void *hook, void **original)
{
    assert(hook && original);
    return address == hook_failure ? -1 : 1;
}
static int azm_patch_vtable(uintptr_t address, void *hook, void **original)
{
    return azm_install_hook(address, hook, original);
}
static void tag_capture_install(void) { ++tag_installs; }

#include "menu_plan_set_art.c"
#include "menu_plan_set.c"
#include "menu_popup_install.c"

static az_plan_set_target_v1 received;
static void accept(const az_plan_set_target_v1 *target, void *user)
{
    assert(user == &received && closes > delivered);
    ++delivered;
    received = *target;
}

static void installation(void)
{
    probe = 1;
    install_menu_popup();
    assert(!g_mp_installed && !tag_installs);
    probe = 0;
    bad_slot = MP_CLICK_SLOT;
    install_menu_popup();
    assert(!g_mp_installed && !tag_installs);
    bad_slot = MP_WIDGET_PAINT_SLOT;
    hook_failure = MP_CLICK_SLOT;
    install_menu_popup();
    assert(!g_mp_installed && !tag_installs);
    hook_failure = 0;
    install_menu_popup();
    assert(g_mp_installed && tag_installs == 1);
    bad_slot = 0;
    install_menu_popup();
    assert(g_mp_installed && tag_installs == 2);
}

static void registration(void)
{
    assert(sdk_on_plan_set("planner", accept, &received) == AZ_INVALID);
    g_sdk_in_init = 1;
    callback_owned = 0;
    assert(sdk_on_plan_set("planner", accept, &received) == AZ_INVALID);
    callback_owned = 1;
    assert(sdk_on_plan_set("foreign", accept, &received) == AZ_INVALID);
    assert(sdk_on_plan_set("planner", accept, &received) == AZ_OK);
    assert(!az_plan_set_available());
    g_cm[0].running = 1;
    assert(sdk_on_plan_set("planner", accept, &received) == AZ_BUSY);
    g_sdk_in_init = 0;
}

static void build_panel(void)
{
    mp_rebuild_with_plan((void *)CONTROLLER);
}
static void tap_plan(void)
{
    mp_click_with_plan((void *)CONTROLLER, (void *)TAPPED);
}

static void check_target_source(const az_plan_set_target_v1 *target, unsigned stick)
{
    az_plan_set_source_v1 source = {.size = sizeof source, .version = AZ_DATA_VERSION};
    assert(sdk_plan_set_resolve("planner", target, &source) == AZ_OK);
    assert(source.stick == stick && source.source_generation == usb_generation[stick]);
    assert(source.library_id == usb_library[stick]);
}

static void browser_actions(void)
{
    mode = 1;
    highlight = (struct plsnap){.sel_pl = 23, .any_media = 2, .sel_title = "Party"};
    image_failure = 2;
    build_panel();
    assert(!shown && released_images == 1);
    image_failure = 0;
    allocation_failure = 1;
    build_panel();
    assert(!shown);
    allocation_failure = 0;
    build_panel();
    assert(shown && rebuilds == 3 && button_width == 246 && button_x == 1021);
    assert(art[2][(29 * 246 + 30) * 4] == 255 && art[3][(29 * 246 + 30) * 4] == 240);
    assert(art[2][(45 * 246 + 108) * 4] == 255);
    mp_click_with_plan((void *)CONTROLLER, NULL);
    assert(stock_clicks == 1 && !closes);
    ++highlight.sel_pl;
    tap_plan();
    assert(!closes && !delivered);
    --highlight.sel_pl;
    tap_plan();
    assert(closes == 1 && !delivered);
    assert(!az_plan_set_queue(&mp_plan_shown));
    sdk_plan_set_tick();
    assert(delivered == 1 && received.playlist_id == 23 && !strcmp(received.name, "Party"));
    check_target_source(&received, 1);
    highlight.sel_folder = 1;
    build_panel();
    assert(!shown);
    mode = 2;
    highlight = (struct plsnap){.open_pl = 44, .any_media = 1};
    build_panel();
    assert(shown);
    tap_plan();
    sdk_plan_set_tick();
    assert(delivered == 2 && received.playlist_id == 44 && !received.name[0]);
    check_target_source(&received, 0);
    mode = MP_TYPE_ADD;
    highlight.any_media = 0;
    build_panel();
    assert(!shown);
    mode = 3;
    build_panel();
    assert(!shown);
    g_cm[0].running = 0;
    assert(!az_plan_set_queue(&received));
    sdk_plan_set_tick();
    assert(delivered == 2);
    g_cm[0].running = 1;
}

static void native_row(void)
{
    edit_enabled = 1;
    mode = 2;
    highlight = (struct plsnap){.open_pl = 63, .any_media = 1};
    other_slots = 5;
    font_missing = 1;
    build_panel();
    assert(!shown);
    font_missing = 0;
    build_panel();
    assert(shown && button_x == MP_SLOT_X(5) && button_width == 210);
    other_slots = 6;
    build_panel();
    assert(!shown);
    g_mp_editmode = 1;
    g_mp_anim.on = 1;
    mp_plan_tick();
    assert(!shown);
    g_mp_anim.on = 0;
    mp_plan_tick();
    assert(shown && button_x == MP_SLOT_X(5) && button_width == 210);
    g_mp_editmode = 0;
    mp_plan_tick();
    assert(!shown);
    g_mp_newmode = 1;
    other_slots = 5;
    mp_plan_tick();
    assert(!shown);
    g_mp_newmode = 0;
    mp_plan_tick();
    assert(shown);
    panel_open = 0;
    mp_plan_tick();
    assert(!shown && !mp_plan_controller);
    mp_plan_tick();
}

static void resolution(void)
{
    az_plan_set_source_v1 source = {.size = sizeof source, .version = AZ_DATA_VERSION};
    az_plan_set_target_v1 target = received;
    g_cm_on_ui = 1;
    assert(sdk_plan_set_resolve("planner", &target, &source) == AZ_INVALID);
    g_cm_on_ui = 0;
    assert(sdk_plan_set_resolve("planner", NULL, &source) == AZ_INVALID);
    assert(sdk_plan_set_resolve("planner", &target, NULL) == AZ_INVALID);
    assert(sdk_plan_set_resolve("foreign", &target, &source) == AZ_INVALID);
    assert(sdk_plan_set_resolve("planner", &target, &source) == AZ_OK);
    assert(source.stick == 0 && source.source_generation == 40 && source.library_id == 60);
    usb_connected[0] = 0;
    assert(sdk_plan_set_resolve("planner", &target, &source) == AZ_STALE);
    usb_connected[0] = 1;
    memset(target.mount, '/', sizeof target.mount);
    assert(sdk_plan_set_resolve("planner", &target, &source) == AZ_INVALID);
}

static void bind_native(unsigned media, uintptr_t server)
{
    bound_media = media;
    char mount[128];
    if (mm_record((void *)server, mount))
        assert(mount[0] == '/');
}

static void replacement(void)
{
    az_plan_set_source_v1 source = {.size = sizeof source, .version = AZ_DATA_VERSION};
    az_plan_set_target_v1 old = received;
    mode = 2;
    highlight = (struct plsnap){.open_pl = old.playlist_id, .any_media = 1};
    build_panel();
    assert(shown);

    ++usb_generation[0];
    ++usb_library[0];
    assert(sdk_plan_set_resolve("planner", &old, &source) == AZ_STALE);
    bind_native(1, 3);
    unsigned closed_before = closes;
    tap_plan();
    assert(closes == closed_before);
    build_panel();
    tap_plan();
    sdk_plan_set_tick();
    assert(sdk_plan_set_resolve("planner", &received, &source) == AZ_OK);
    assert(source.source_generation == usb_generation[0] && source.library_id == usb_library[0]);
    assert(sdk_plan_set_resolve("planner", &old, &source) == AZ_STALE);
}

static void recycled_servers(void)
{
    az_plan_set_target_v1 copied = received;
    for (unsigned i = 0; i < MM_N; ++i)
        bind_native(3 + i, 10 + i);
    char mount[128];
    assert(!media_mount(1, mount, sizeof mount));
    check_target_source(&copied, 0);
}

int main(void)
{
    installation();
    registration();
    bind_native(1, 1);
    bind_native(1, 1);
    bind_native(2, 2);
    char mount[128];
    assert(media_mount(1, mount, sizeof mount) && !strcmp(mount, "/media/usb/1"));
    browser_actions();
    resolution();
    replacement();
    native_row();
    recycled_servers();
    puts("Native Plan Set action tests passed");
    return 0;
}
