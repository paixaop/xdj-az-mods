/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by azmod.c at the firmware's Component::setVisible hook. */

static void setvis_search(long self, int on)
{
    atomic_store_explicit(&g_sv_vis, on, memory_order_release);
    __atomic_store_n(&g_search_on, on, __ATOMIC_RELAXED);
    atomic_store_explicit(&g_sv_obj, (unsigned long)self, memory_order_release);
    if (on)
        nm_view_changed(1);
}

static void setvis_browse(long self, int on)
{
    atomic_store_explicit(&g_bv_obj, (unsigned long)self, memory_order_release);
    atomic_store_explicit(&g_bv_vis, on, memory_order_release);
    if (on)
        atomic_store_explicit(&g_browseview_shown, 1, memory_order_release);
    else
        nm_view_changed(0);
}

static void setvis_log(long self, int on, unsigned long vtable)
{
    /* Width and height share one aligned word in the firmware Component. */
    uintptr_t bounds = 0;
    if (!own_q((uintptr_t)self + 0x28, &bounds))
        return;
    int width = (int)(uint32_t)bounds, height = (int)(uint32_t)(bounds >> 32);
    if (width >= 400 || height >= 300)
        dbg("SETVIS-BIG self=%p vt=%p on=%d %dx%d\n", (void *)self, (void *)vtable, on, width, height);
}

static void setvis_observe(long self, int on, unsigned long vtable)
{
    if (vtable == SEARCHVIEW_VT)
        setvis_search(self, on);
    else if (vtable == BROWSEVIEW_VT)
        setvis_browse(self, on);
    if (self == __atomic_load_n(&g_waveview, __ATOMIC_RELAXED))
        atomic_store_explicit(&g_wave_vis, on, memory_order_relaxed);
    if (atomic_load_explicit(&g_visdbg, memory_order_relaxed))
        setvis_log(self, on, vtable);
}

static void source_keyboard_tick(void);

static void setvis_hook(long self, int on)
{
    unsigned long vtable = self > 0x10000 ? *(unsigned long *)self : 0;
    if (vtable)
        setvis_observe(self, on != 0, vtable);
    g_orig_setvis(self, on);
    if (vtable == SEARCHVIEW_VT)
        source_keyboard_tick();
}
