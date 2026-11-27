/* SPDX-License-Identifier: GPL-3.0-only */
/* The native Search keyboard is shared by playlist naming and custom sources. */
static void nm_key_text(void *str, char *key, size_t capacity) {
    uintptr_t p = 0;
    if (str && own_q((uintptr_t) str, &p) && p > 0x10000) /* the keystroke EP147 passes in */
        own_str(p, key, capacity);
}
static void nm_key(void *str) {
    char k[16] = "";
    nm_key_text(str, k, sizeof k);
    size_t n = strlen(g_nm.name), m = strlen(k);
    if (m && n + m < 64 && (unsigned char) k[0] >= 0x20) {
        memcpy(g_nm.name + n, k, m + 1);
        atomic_store_explicit(&g_nm_dirty, 1, memory_order_release);
    }
}
static int sdk_source_browser_editing(void);
static int sdk_source_browser_key(const char *key);
static int sdk_source_browser_action(int action);
static void sv_key_hook(void *self, void *str) {
    if (nm_naming()) {
        nm_key(str);
    } else if (sdk_source_browser_editing()) {
        char key[16] = "";
        nm_key_text(str, key, sizeof key);
        (void)sdk_source_browser_key(key);
    } else {
        g_orig_sv_key(self, str);
    }
}
static void sv_bs_hook(void *self, int a) {
    if (nm_naming())
        nm_action(a);
    else if (!sdk_source_browser_action(a))
        g_orig_sv_bs(self, a);
}

/* On 2.00 the firmware disables the Search keyboard when no stock media is selected.
 * Its widget enable routine toggles both the Component and all keyboard layouts. */
static uintptr_t source_search_keyboard(uintptr_t view, uintptr_t keyboard_vtable) {
    uintptr_t keyboard = 0, vtable = 0;
    /* SearchView's embedded keyboard controller owns the widget at +0x1928 on 2.00. */
    if (view && own_q(view + 0x1928, &keyboard) && own_q(keyboard, &vtable) &&
        vtable == keyboard_vtable)
        return keyboard;
    return 0;
}

static void source_keyboard_enable_if_needed(uintptr_t keyboard, uintptr_t *owned,
                                             void (*enable)(uintptr_t, int)) {
    uintptr_t flags = 0;
    if (own_q(keyboard + 0xc0, &flags) && (flags & 0x1000u)) {
        enable(keyboard, 1);
        *owned = keyboard;
    }
}

static void source_keyboard_update(uintptr_t keyboard, int editing,
                                   void (*enable)(uintptr_t, int)) {
    static uintptr_t owned;
    if (editing && keyboard) {
        source_keyboard_enable_if_needed(keyboard, &owned, enable);
        return;
    }
    if (owned && owned == keyboard)
        enable(keyboard, 0);
    owned = 0;
}
