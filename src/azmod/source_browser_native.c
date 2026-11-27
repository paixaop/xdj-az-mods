/* SPDX-License-Identifier: GPL-3.0-only */
/* The EP147 2.00 SOURCE library pane, which also renders Apple Music and Beatport.
 * The additional cell is owned by the model adapter. The controller's native vector
 * and its MediaIDs retain their original size and ownership. Hooks run on the JUCE
 * message thread. The host owns the source selection and the action callbacks. */

#define SBN_MODEL_VT 0x2dc82e0u
#define SBN_ROW_VT 0x2dc8288u
#define SBN_COMPONENT_VT 0x2df4cf8u
#define SBN_SELECT_ITEM 0x1be8a20u
#define SBN_REFRESH_FOCUS 0x1be4450u
#define SBN_WIDGET 0x40
#define SBN_LIST 0x30
#define SBN_ROWS 0x88
#define SBN_NAME 0x110
#define SBN_ICON 0x100
#define SBN_COMPONENT_SIZE 0x228

struct sbn_row {
    uintptr_t vtable;
    unsigned char root[12];
    unsigned kind;
    unsigned char focused;
    unsigned char active;
    unsigned char padding[6];
};

struct sbn_vector {
    uintptr_t begin, end, capacity;
};

_Static_assert(sizeof(struct sbn_row) == 0x20, "EP147 service row size");
_Static_assert(offsetof(struct sbn_row, focused) == 0x18, "EP147 focus flag");
_Static_assert(offsetof(struct sbn_row, active) == 0x19, "EP147 active flag");

static int sdk_source_browser_count(void);
static const char *sdk_source_browser_label(int row);
static uintptr_t sdk_source_browser_icon(int row, uintptr_t (*decode)(const unsigned char *, int));
static int sdk_source_browser_active(int row);
static int sdk_source_browser_is_selected(void);
static void sdk_source_browser_activate(int row, void *model);
static void sdk_source_browser_native_selected(void);

static int (*sbn_original_count)(void *);
static void *(*sbn_original_refresh)(void *, int, int, int, void *);
static void (*sbn_original_click)(void *, int, int, void *);
static void (*sbn_original_focus)(void *, int);
static void (*sbn_original_select_item)(void *);
static void (*sbn_original_refresh_focus)(void *, bool);

static struct {
    void (*construct)(void *, void *);
    void (*set_data)(void *, int, const struct sbn_row *);
    void (*make_string)(void **, const char *);
    void (*drop_string)(void **);
    void (*set_text)(void *, void **, int, void **);
    void (*set_image)(void *, const uintptr_t *);
    uintptr_t (*decode_image)(const unsigned char *, int);
    void (*set_visible)(void *, bool);
    void (*select_row)(void *, int, bool, bool);
    int (*get_selected)(void *, int);
    void (*update_content)(void *);
} sbn_native;

static uintptr_t sbn_row_vtable[9];
static _Atomic int sbn_installed;
static _Thread_local void *sbn_native_focus_model;
static void *sbn_unfocused_model;

static void *sbn_load_pointer(const void *object, size_t offset) {
    void *ptr;
    memcpy(&ptr, (const unsigned char *) object + offset, sizeof ptr);
    return ptr;
}

static void *sbn_list(void *model) {
    unsigned char *widget = sbn_load_pointer(model, SBN_WIDGET);
    return widget ? widget + SBN_LIST : NULL;
}

static int sbn_valid_row_count(const struct sbn_vector *vector) {
    return vector->end >= vector->begin && vector->capacity >= vector->end &&
           (vector->end - vector->begin) % sizeof(struct sbn_row) == 0 &&
           (vector->end - vector->begin) / sizeof(struct sbn_row) <= 256;
}

static int sbn_stock_count(void *model, struct sbn_vector *vector) {
    memcpy(vector, (unsigned char *) model + SBN_ROWS, sizeof *vector);
    if (!sbn_valid_row_count(vector)) return -1;
    return (int) ((vector->end - vector->begin) / sizeof(struct sbn_row));
}

static int sbn_available(void) {
    return atomic_load_explicit(&sbn_installed, memory_order_acquire) ? sdk_source_browser_count() : 0;
}

static int sbn_has_index(int index, int count) {
    return index >= 0 && index < count;
}

static int sbn_extra_index(int index, int count) {
    return count >= 0 && index >= count && index < count + sbn_available();
}

static int sbn_count_hook(void *model) {
    struct sbn_vector vector;
    int count = sbn_stock_count(model, &vector);
    if (count < 0) return sbn_original_count(model);
    return count + (sbn_native_focus_model != model ? sbn_available() : 0);
}

static int sbn_is_component(const void *component) {
    return component && (uintptr_t) sbn_load_pointer(component, 0) == SBN_COMPONENT_VT;
}

static void *sbn_get_component(void *model, void *existing) {
    if (existing) return sbn_is_component(existing) ? existing : NULL;
    void *component = malloc(SBN_COMPONENT_SIZE);
    if (component) sbn_native.construct(component, (unsigned char *) model + 0x38);
    return component;
}

static void sbn_render_name(void *component, const char *label) {
    void *title;
    void *suffix;
    sbn_native.make_string(&title, label);
    sbn_native.make_string(&suffix, "");
    sbn_native.set_text(sbn_load_pointer(component, SBN_NAME), &title, 0, &suffix);
    sbn_native.drop_string(&suffix);
    sbn_native.drop_string(&title);
}

static void sbn_render_icon(void *component, int source) {
    uintptr_t image = sdk_source_browser_icon(source, sbn_native.decode_image);
    void *icon = sbn_load_pointer(component, SBN_ICON);
    if (image)
        sbn_native.set_image(icon, &image);
    sbn_native.set_visible(icon, image != 0);
}

static void *sbn_refresh_source(void *model, int row, int selected, void *existing,
                                int source) {
    const char *label = sdk_source_browser_label(source);
    if (!label) return existing;
    void *component = sbn_get_component(model, existing);
    if (!component) return existing;
    /* Kind 3 lets the native string providers match their normal ABI. The adapter
     * then replaces the stock title and hides the stock provider's icon. */
    const struct sbn_row paint = {
        .vtable = (uintptr_t) sbn_row_vtable,
        .kind = 3,
        .focused = selected != 0 && model != sbn_unfocused_model,
        .active = sdk_source_browser_active(source) != 0,
    };
    sbn_native.set_data(component, row, &paint);
    sbn_render_name(component, label);
    sbn_render_icon(component, source);
    return component;
}

static void *sbn_refresh_stock(void *model, int index, int column, int selected,
                               void *existing, struct sbn_row *row) {
    unsigned char active = row->active;
    if (sdk_source_browser_is_selected()) row->active = 0;
    void *component = sbn_original_refresh(model, index, column, selected, existing);
    row->active = active;
    if (sbn_is_component(component))
        sbn_native.set_visible(sbn_load_pointer(component, SBN_ICON), true);
    return component;
}

static void *sbn_refresh_hook(void *model, int row, int column, int selected, void *existing) {
    struct sbn_vector vector;
    int count = sbn_stock_count(model, &vector);
    if (sbn_extra_index(row, count))
        return sbn_refresh_source(model, row, selected, existing, row - count);
    if (sbn_available() && sbn_has_index(row, count))
        return sbn_refresh_stock(model, row, column, selected, existing,
                                 (struct sbn_row *) vector.begin + row);
    return sbn_original_refresh(model, row, column, selected, existing);
}

static void sbn_update(void *list) {
    if (list) sbn_native.update_content(list);
}

static void sbn_select_source(void *model, void *list, int source) {
    sdk_source_browser_activate(source, model);
    sbn_update(list);
}

static void sbn_clear_selection(void *list) {
    if (!sdk_source_browser_is_selected()) return;
    sdk_source_browser_native_selected();
    sbn_update(list);
}

static void sbn_click_hook(void *model, int row, int column, void *event) {
    struct sbn_vector vector;
    int count = sbn_stock_count(model, &vector);
    void *list = sbn_list(model);
    if (sbn_extra_index(row, count) && list) {
        sbn_unfocused_model = NULL;
        sbn_native.select_row(list, row, false, true);
        sbn_select_source(model, list, row - count);
        return;
    }
    if (!sbn_has_index(row, count)) return;
    sbn_clear_selection(list);
    sbn_original_click(model, row, column, event);
}

static void sbn_clear_stock_focus(struct sbn_row *row, int count) {
    for (int i = 0; i < count; ++i) row[i].focused = 0;
}

static void sbn_stock_focus(void *model, int row) {
    /* The firmware reads getNumRows() and then indexes the native vector directly.
     * Restrict nested count queries to stock bounds while that callback is active. */
    void *saved = sbn_native_focus_model;
    sbn_native_focus_model = model;
    sbn_original_focus(model, row);
    sbn_native_focus_model = saved;
}

static void sbn_focus_hook(void *model, int row) {
    struct sbn_vector vector;
    int count = sbn_stock_count(model, &vector);
    if (sbn_native_focus_model == model || !sbn_available()) {
        sbn_original_focus(model, row);
        return;
    }
    if (sbn_extra_index(row, count))
        sbn_clear_stock_focus((struct sbn_row *) vector.begin, count);
    else
        sbn_stock_focus(model, row);
    sbn_update(sbn_list(model));
}

static void sbn_refresh_focus_hook(void *model, bool focused) {
    struct sbn_vector vector;
    int count = sbn_stock_count(model, &vector);
    void *list = sbn_list(model);
    int row = list ? sbn_native.get_selected(list, 0) : -1;

    /* This native pane-transition path also walks every row reported by getNumRows(). */
    void *saved = sbn_native_focus_model;
    sbn_native_focus_model = model;
    sbn_original_refresh_focus(model, focused);
    sbn_native_focus_model = saved;

    if (!sbn_available() || !list) return;
    sbn_unfocused_model = focused ? NULL : model;
    sbn_update(list);
    if (focused && sbn_extra_index(row, count))
        sbn_native.select_row(list, row, false, true);
    else if (focused && count == 0)
        sbn_native.select_row(list, 0, false, true);
}

static void sbn_select_item_hook(void *model) {
    if (!atomic_load_explicit(&sbn_installed, memory_order_acquire)) {
        sbn_original_select_item(model);
        return;
    }
    struct sbn_vector vector;
    void *list = sbn_list(model);
    if (!list) return;
    int row = sbn_native.get_selected(list, 0);
    int count = sbn_stock_count(model, &vector);
    if (sbn_extra_index(row, count))
        sbn_select_source(model, list, row - count);
    else if (sbn_has_index(row, count)) {
        sbn_clear_selection(list);
        sbn_original_select_item(model);
    }
}

static unsigned sbn_hide_service(const struct sbn_row *row) {
    (void) row;
    return 0;
}

static const struct {
    unsigned slot;
    uintptr_t expected;
    void *hook;
    void **original;
} sbn_slots[] = {
    {5, 0x1be3d98, sbn_refresh_hook, (void **) &sbn_original_refresh},
    {6, 0x1be8ab8, sbn_click_hook, (void **) &sbn_original_click},
    {12, 0x1be5c98, sbn_focus_hook, (void **) &sbn_original_focus},
    {2, 0x1be9bf0, sbn_count_hook, (void **) &sbn_original_count},
};

static int sbn_slots_match(void) {
    for (size_t i = 0; i < sizeof sbn_slots / sizeof sbn_slots[0]; ++i) {
        uintptr_t value;
        if (!img_q(SBN_MODEL_VT + 8 * sbn_slots[i].slot, &value)) return 0;
        if (value != sbn_slots[i].expected) return 0;
    }
    return 1;
}

static int sbn_patch_slots(void) {
    for (size_t i = 0; i < sizeof sbn_slots / sizeof sbn_slots[0]; ++i)
        if (azm_patch_vtable(SBN_MODEL_VT + 8 * sbn_slots[i].slot, sbn_slots[i].hook,
                             sbn_slots[i].original) != 1)
            return 0;
    return 1;
}

static void sbn_configure(void) {
    sbn_native.construct = (void (*)(void *, void *)) 0x1bfdd70;
    sbn_native.set_data = (void (*)(void *, int, const struct sbn_row *)) 0x1bfd818;
    sbn_native.make_string = (void (*)(void **, const char *)) 0x24a1180;
    sbn_native.drop_string = (void (*)(void **)) 0x2469aa0;
    sbn_native.set_text = (void (*)(void *, void **, int, void **)) 0x1bcd2d0;
    sbn_native.set_image = (void (*)(void *, const uintptr_t *)) 0x25f6d40;
    sbn_native.decode_image = mp_image_uncached;
    sbn_native.set_visible = (void (*)(void *, bool)) 0x260af90;
    sbn_native.select_row = (void (*)(void *, int, bool, bool)) 0x2355a30;
    sbn_native.get_selected = (int (*)(void *, int)) 0x2354280;
    sbn_native.update_content = (void (*)(void *)) 0x2351d50;

    /* Preserve the firmware providers which return native String/Image values in x8.
     * The scalar providers can use C and give the row a radio marker with no login UI. */
    sbn_row_vtable[0] = 0x1be9bb8;
    sbn_row_vtable[1] = 0x1be9bc0;
    sbn_row_vtable[2] = 0x1be9c08;
    sbn_row_vtable[3] = (uintptr_t) sbn_hide_service;
    sbn_row_vtable[4] = 0x1be4e18;
    sbn_row_vtable[5] = (uintptr_t) sbn_hide_service;
    sbn_row_vtable[6] = (uintptr_t) sbn_hide_service;
    sbn_row_vtable[7] = (uintptr_t) sbn_hide_service;
    sbn_row_vtable[8] = 0x1be5570;
}

static int sbn_code_matches(uintptr_t address, uint64_t expected) {
    uintptr_t entry;
    return img_q(address, &entry) && entry == expected;
}

static int sbn_code_matches_profile(void) {
    return sbn_code_matches(SBN_SELECT_ITEM, UINT64_C(0xb0000001a9bc7bfd)) &&
           sbn_code_matches(SBN_REFRESH_FOCUS, UINT64_C(0x90008f22a9b47bfd));
}

static int sbn_patch_code(void) {
    return azm_install_hook(SBN_SELECT_ITEM, sbn_select_item_hook,
                            (void **) &sbn_original_select_item) == 1 &&
           azm_install_hook(SBN_REFRESH_FOCUS, sbn_refresh_focus_hook,
                            (void **) &sbn_original_refresh_focus) == 1;
}

static int source_browser_native_install(void) {
    if (atomic_load_explicit(&sbn_installed, memory_order_relaxed)) return 1;
    if (g_ep147_build != EPB_V200) return 0;
    if (!sbn_slots_match() || !sbn_code_matches_profile()) return 0;
    sbn_configure();
    if (!sbn_patch_code()) return 0;
    if (!sbn_patch_slots()) return 0;
    /* A partial install never adds the cell. Its hooks retain the native model bounds. */
    atomic_store_explicit(&sbn_installed, 1, memory_order_release);
    return 1;
}
