/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by menu_popup.c. Add a standard button to a free position in the
 * native row. The Edit row supplies room when all six main buttons are in use. */
static uintptr_t mp_plan_button, mp_plan_widget, mp_plan_controller;
static int mp_plan_mode;
static bool mp_plan_animating;
static az_plan_set_target_v1 mp_plan_shown;

static int az_plan_set_available(void);
static int az_plan_set_queue(const az_plan_set_target_v1 *target);

static unsigned mp_plan_playlist(const struct plsnap *selection, int type)
{
    if (type == 1)
        return selection->sel_folder ? 0 : selection->sel_pl;
    if (type == 2 || type == MP_TYPE_ADD)
        return selection->open_pl;
    return 0;
}

static int mp_plan_target(uintptr_t controller, az_plan_set_target_v1 *target)
{
    uintptr_t raw_type = 0;
    if (!az_plan_set_available() || !own_q(controller + 0x18, &raw_type))
        return 0;
    int type = (int)(signed char)(raw_type & 0xff);
    struct plsnap selection = plsnap_get();
    *target = (az_plan_set_target_v1){.size = sizeof *target, .version = AZ_DATA_VERSION,
        .playlist_id = mp_plan_playlist(&selection, type)};
    if (!target->playlist_id || !media_mount(selection.any_media, target->mount,
                                              sizeof target->mount))
        return 0;
    if (type == 1)
        snprintf(target->name, sizeof target->name, "%s", selection.sel_title);
    return 1;
}

static const uintptr_t *mp_plan_images(int width)
{
    static uintptr_t sizes[2][3];
    uintptr_t *images = sizes[width == 246];
    if (images[0] || mp_build_plan_images(width, images))
        return images;
    return NULL;
}

static uintptr_t mp_plan_for(uintptr_t controller, uintptr_t widget, uintptr_t first, int width)
{
    const uintptr_t *images = mp_plan_images(width);
    if (!images)
        return 0;
    if (mp_plan_widget == widget && mp_valid(mp_plan_button, MP_TIB_VT0))
        return mp_swap_set(mp_plan_button, 0, images) ? mp_plan_button : 0;
    static struct bn_holder name;
    static uintptr_t text;
    bn_set(&name, "AZ_PLAN_SET", 11);
    text = (uintptr_t)name.text;
    g_mp_mk_single = images;
    mp_plan_button = mp_make_btn(controller, widget, first, &text, NULL, 0);
    g_mp_mk_single = NULL;
    mp_plan_widget = mp_plan_button ? widget : 0;
    return mp_plan_button;
}

static void mp_plan_hide(void)
{
    if (mp_plan_shown.playlist_id && mp_valid(mp_plan_button, MP_TIB_VT0))
        mp_show(mp_plan_button, 0, 0, 0);
    mp_plan_shown = (az_plan_set_target_v1){0};
}

typedef struct { uintptr_t button, widget; } MPPlanPeer;

static bool mp_plan_peer_bounds(MPPlanPeer peer, uintptr_t widget, uintptr_t *xy, uintptr_t *size)
{
    return peer.widget == widget && peer.button && mp_visible(peer.button) &&
        own_q(peer.button + 0x20, xy) && own_q(peer.button + 0x28, size);
}

static bool mp_plan_blocks(uintptr_t xy, uintptr_t size, int x, int width)
{
    int left = (int)(uint32_t)xy, y = (int)(xy >> 32);
    int right = left + (int)(uint32_t)size;
    return y == 36 && left < x + width + 2 && right + 2 > x;
}

static bool mp_plan_room(const MPPlanPeer *buttons, size_t count, uintptr_t widget, int x, int width)
{
    for (size_t i = 0; i < count; ++i) {
        uintptr_t xy = 0, size = 0;
        if (mp_plan_peer_bounds(buttons[i], widget, &xy, &size) &&
            mp_plan_blocks(xy, size, x, width))
            return false;
    }
    return true;
}

static int mp_plan_position(uintptr_t widget, uintptr_t first, uintptr_t second, int width)
{
    const MPPlanPeer buttons[] = {
        {first, widget}, {second, widget},
        {g_mp_b3, g_mp_b3_widget}, {g_mp_bm, g_mp_bm_widget}, {g_mp_bt, g_mp_bt_widget},
        {g_mp_ba, g_mp_ba_widget}, {g_mp_bnm, g_mp_bnm_widget}, {g_mp_bed, g_mp_bed_widget},
        {g_mp_bs, g_mp_bs_widget}, {g_mp_brn, g_mp_brn_widget}
    };
    int slots = width == 210 ? 6 : 5;
    for (int i = slots; i-- > 0;) {
        int x = width == 210 ? MP_SLOT_X(i) : 13 + i * 252;
        if (mp_plan_room(buttons, sizeof buttons / sizeof *buttons, widget, x, width))
            return x;
    }
    return -1;
}

static void mp_plan_show(uintptr_t controller, const az_plan_set_target_v1 *target)
{
    uintptr_t widget = 0, first = 0, second = 0;
    if (!mp_parts(controller, &first, &second) || !own_q(controller + 0xa0, &widget))
        return;
    int width = mp_enabled() ? 210 : 246;
    int x = mp_plan_position(widget, first, second, width);
    if (x < 0)
        return;
    uintptr_t button = mp_plan_for(controller, widget, first, width);
    if (!button)
        return;
    mp_bounds_w(button, x, width);
    mp_plan_shown = *target;
    mp_show(button, 0, 1, 1);
}

static int mp_plan_submenu(void)
{
    return g_mp_newmode ? 1 : g_mp_editmode ? 2 : 0;
}

static void mp_plan_place(uintptr_t controller)
{
    mp_plan_hide();
    mp_plan_mode = mp_plan_submenu();
    az_plan_set_target_v1 target;
    if (!mp_plan_target(controller, &target)) {
        mp_plan_controller = 0;
        return;
    }
    mp_plan_controller = controller;
    if (mp_plan_mode != 1)
        mp_plan_show(controller, &target);
}

static bool mp_plan_panel_open(uintptr_t controller)
{
    uintptr_t widget = 0;
    return mp_ctrl_live(controller) && own_q(controller + 0xa0, &widget) && mp_visible(widget);
}

static void mp_plan_tick(void)
{
    if (!mp_plan_controller)
        return;
    if (!mp_plan_panel_open(mp_plan_controller)) {
        mp_plan_hide();
        mp_plan_controller = 0;
        return;
    }
    if (g_mp_anim.on) {
        mp_plan_animating = true;
        mp_plan_hide();
        return;
    }
    if (mp_plan_animating || mp_plan_mode != mp_plan_submenu()) {
        mp_plan_animating = false;
        mp_plan_place(mp_plan_controller);
    }
}

static bool mp_is_plan_click(uintptr_t arg)
{
    if (!mp_plan_shown.playlist_id || !mp_valid(mp_plan_button, MP_TIB_VT0))
        return false;
    uintptr_t clicked = 0, name = 0;
    return own_q(arg, &clicked) && own_q(mp_plan_button + 0x1d8, &name) && clicked == name;
}

static void mp_plan_activate(uintptr_t controller)
{
    az_plan_set_target_v1 target;
    if (!mp_plan_target(controller, &target))
        return;
    if (target.playlist_id != mp_plan_shown.playlist_id || strcmp(target.mount, mp_plan_shown.mount))
        return;
    mp_close(controller);
    (void)az_plan_set_queue(&target);
}

static void mp_rebuild_with_plan(void *controller)
{
    mp_rebuild_hook(controller);
    mp_plan_place((uintptr_t)controller);
}

static void mp_click_with_plan(void *controller, void *arg)
{
    if (mp_is_plan_click((uintptr_t)arg)) {
        mp_plan_activate((uintptr_t)controller);
        return;
    }
    mp_click_hook(controller, arg);
}
