/* SPDX-License-Identifier: GPL-3.0-only */
/* The native menu hook installation and label setup. */
static void mp_init_names(void)
{
    bn_set(&g_mp_title, "PLAYLIST", 8);
    g_mp_title_s = (uintptr_t) g_mp_title.text;
    bn_set(&g_mp_body, "REMOVE FROM PLAYLIST", 20);
    g_mp_body_s = (uintptr_t) g_mp_body.text;
    g_mp_b3_on = access(MP_B3_OFF_FLAG, F_OK) != 0;
    bn_set(&g_mp_b3_name, "AZ_COPY_PASTE", 13);
    g_mp_b3_name_s = (uintptr_t) g_mp_b3_name.text;
    bn_set(&g_mp_bm_name, "AZ_MOVE", 7);
    g_mp_bm_name_s = (uintptr_t) g_mp_bm_name.text;
    bn_set(&g_mp_ba_name, "AZ_ADD_TO", 9);
    g_mp_ba_name_s = (uintptr_t) g_mp_ba_name.text;
    bn_set(&g_mp_bt_name, "AZ_SELECT_PL", 12);
    g_mp_bt_name_s = (uintptr_t) g_mp_bt_name.text;
    bn_set(&g_mp_bu_name, "AZ_UNDO", 7);
    g_mp_bu_name_s = (uintptr_t) g_mp_bu_name.text;
    bn_set(&g_mp_bed_name, "AZ_EDIT", 7);
    g_mp_bed_name_s = (uintptr_t) g_mp_bed_name.text;
    bn_set(&g_mp_bnm_name, "AZ_NEW", 6);
    g_mp_bnm_name_s = (uintptr_t) g_mp_bnm_name.text;
    bn_set(&g_mp_brn_name, "AZ_RENAME", 9);
    g_mp_brn_name_s = (uintptr_t) g_mp_brn_name.text;
    bn_set(&g_mp_bs_name, "AZ_MAKE_STEMS", 13);
    g_mp_bs_name_s = (uintptr_t) g_mp_bs_name.text;
    bn_set(&g_mp_bn_name, "AZ_NEW_PLAYLIST", 15);
    g_mp_bn_name_s = (uintptr_t) g_mp_bn_name.text;
    bn_set(&g_mp_bf_name, "AZ_NEW_FOLDER", 13);
    g_mp_bf_name_s = (uintptr_t) g_mp_bf_name.text;
}

static int mp_stock_menu_slots(void)
{
    uintptr_t click = 0, result = 0;
    if (img_q(MP_CLICK_SLOT, &click) && click == MP_CLICK_ORIG &&
        img_q(MP_RESULT_SLOT, &result) && result == MP_RESULT_ORIG)
        return 1;
    mp_log("install: slots hold %lx/%lx, not stock -- popup off\n",
           (unsigned long)click, (unsigned long)result);
    return 0;
}

static void mp_install_title(void)
{
    uintptr_t paint = 0;
    int rc = g_mp_installed && img_q(MP_WIDGET_PAINT_SLOT, &paint) && paint == MP_WIDGET_PAINT_ORIG
        ? azm_patch_vtable(MP_WIDGET_PAINT_SLOT, (void *)mp_paint_hook, (void **)&g_mp_orig_paint)
        : -9;
    mp_log("install: panel title paint=%d\n", rc);
}

static void mp_install_menu_hooks(void)
{
    int r = azm_install_hook(MP_REBUILD, (void *) mp_rebuild_with_plan, (void **) &g_mp_orig_rebuild);
    int p = azm_install_hook(MP_PRODUCER, (void *) mp_producer_hook, (void **) &g_mp_orig_producer);
    int k = r == 1 && p == 1 ? azm_patch_vtable(MP_CLICK_SLOT, (void *) mp_click_with_plan,
                                                (void **) &g_mp_orig_click)
                             : -9;
    int d = k == 1 ? azm_patch_vtable(MP_RESULT_SLOT, (void *) mp_result_hook,
                                      (void **) &g_mp_orig_result)
                   : -9;
    g_mp_installed = d == 1;
    mp_install_title();
    if (g_mp_installed)
        tag_capture_install();
    mp_log("install: rebuild=%d producer=%d click=%d result=%d -> %s\n", r, p, k, d,
           g_mp_installed ? "ON" : "OFF");
}

/* Install over stock slots only. Each action is gated by its own provider. */
static void install_menu_popup(void)
{
    if (AZ_PROBE_ON("/home/root/settings/azmod-menutest.on") ||
        AZ_PROBE_ON("/home/root/settings/azmod-menuprobe.on")) {
        mp_log("install: a menu probe flag is present (it hooks the same functions) -- popup off\n");
        return;
    }
    if (!mp_stock_menu_slots())
        return;
    mp_init_names();
    mp_install_menu_hooks();
}
