/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EPB_V130 = 1, EPB_V200 = 2 };
static int g_ep147_build;
static int img_q(uintptr_t at, uintptr_t *out);
static int azm_patch_vtable(uintptr_t at, void *hook, void **original);
static int azm_install_hook(uintptr_t at, void *hook, void **original);
static uintptr_t mp_image_uncached(const unsigned char *png, int size);
#include "source_browser_native.c"

static unsigned char model[0xa0];
static unsigned char widget[0x100];
static struct sbn_row stock[] = {
    {.vtable = SBN_ROW_VT, .kind = 3, .focused = 1, .active = 1},
    {.vtable = SBN_ROW_VT, .kind = 4},
};

static const char *label;
static int active, selected, icon_visible, row_focus, row_active;
static int count_fallback, clicked, selected_item, activations, cleared, updates;
static int constructed, dropped, focus_calls, slot_calls, bad_signature, refuse_patch;
static char title_out[64];
static int native_icon;
static int native_title;
static uintptr_t painted_image;

static void set_pointer(void *object, size_t offset, void *value) {
    memcpy((unsigned char *) object + offset, &value, sizeof value);
}

static void set_stock(struct sbn_row *begin, size_t used, size_t capacity) {
    struct sbn_vector vector = {(uintptr_t) begin, (uintptr_t) (begin + used),
                                (uintptr_t) (begin + capacity)};
    memcpy(model + SBN_ROWS, &vector, sizeof vector);
}

static int sdk_source_browser_count(void) { return label != NULL; }
static const char *sdk_source_browser_label(int row) { return row == 0 ? label : NULL; }
static int sdk_source_browser_active(int row) { return row == 0 && active; }
static uintptr_t mp_image_uncached(const unsigned char *png, int size) {
    assert(size == 1 && png[0] == 'P');
    return 0x1000;
}
static uintptr_t sdk_source_browser_icon(int row, uintptr_t (*decode)(const unsigned char *, int)) {
    assert(row == 0);
    static const unsigned char png[] = {'P'};
    return decode(png, sizeof png);
}
static int sdk_source_browser_is_selected(void) { return active; }
static void sdk_source_browser_activate(int row, void *model) { assert(row == 0 && model); ++activations; active = 1; }
static void sdk_source_browser_native_selected(void) { ++cleared; active = 0; }

static int img_q(uintptr_t at, uintptr_t *out) {
    if (at == SBN_SELECT_ITEM) {
        *out = bad_signature ? 0 : UINT64_C(0xb0000001a9bc7bfd);
        return 1;
    }
    if (at == SBN_REFRESH_FOCUS) {
        *out = UINT64_C(0x90008f22a9b47bfd);
        return 1;
    }
    for (size_t i = 0; i < sizeof sbn_slots / sizeof *sbn_slots; ++i) {
        if (at == SBN_MODEL_VT + 8 * sbn_slots[i].slot) {
            *out = sbn_slots[i].expected;
            return 1;
        }
    }
    return 0;
}

static int azm_patch_vtable(uintptr_t at, void *hook, void **original) {
    assert(at > SBN_MODEL_VT && hook && original);
    ++slot_calls;
    return !refuse_patch;
}

static int azm_install_hook(uintptr_t at, void *hook, void **original) {
    assert((at == SBN_SELECT_ITEM || at == SBN_REFRESH_FOCUS) && hook && original);
    return 1;
}

static int count_mock(void *self) {
    assert(self == model);
    return ++count_fallback;
}

static void *refresh_mock(void *self, int index, int column, int focused, void *existing) {
    assert(self == model && column == 1 && focused == 0);
    assert(index >= 0 && index < 2);
    row_active = stock[index].active;
    return existing;
}

static void click_mock(void *self, int index, int column, void *event) {
    assert(self == model && index == 1 && column == 1 && !event);
    assert(active == 0);
    ++clicked;
}

static void focus_mock(void *self, int index) {
    assert(self == model && sbn_count_hook(model) == 2);
    assert(index >= 0 && index < 2);
    stock[index].focused = 1;
    ++focus_calls;
}

static void select_item_mock(void *self) {
    assert(self == model);
    assert(!active);
    ++selected_item;
}

static void refresh_focus_mock(void *self, bool focused) {
    assert(self == model && sbn_count_hook(model) <= 2);
    if (!focused) selected = -1;
}

static void construct_mock(void *component, void *context) {
    assert(context == model + 0x38);
    set_pointer(component, 0, (void *) SBN_COMPONENT_VT);
    set_pointer(component, SBN_NAME, &native_title);
    set_pointer(component, SBN_ICON, &native_icon);
    ++constructed;
}

static void set_data_mock(void *component, int index, const struct sbn_row *row) {
    assert(sbn_is_component(component));
    assert(index == 0 || index == 2);
    assert(row->vtable == (uintptr_t) sbn_row_vtable);
    assert(row->kind == 3 && !row->root[0]);
    assert(!((unsigned (*)(const struct sbn_row *))sbn_row_vtable[3])(row));
    row_focus = row->focused;
    row_active = row->active;
}

static void make_string_mock(void **result, const char *label_text) {
    *result = (void *) label_text;
}

static void drop_string_mock(void **string) {
    assert(*string);
    ++dropped;
}

static void set_text_mock(void *native, void **title, int flags, void **suffix) {
    assert(native == &native_title && flags == 0 && *(const char *) *suffix == '\0');
    snprintf(title_out, sizeof title_out, "%s", (const char *) *title);
}

static void set_visible_mock(void *native, bool visible) {
    assert(native == &native_icon);
    icon_visible = visible;
}

static void set_image_mock(void *native, const uintptr_t *image) {
    assert(native == &native_icon);
    painted_image = *image;
}

static void select_row_mock(void *list, int index, bool no_scroll, bool deselect) {
    assert(list == widget + SBN_LIST && !no_scroll && deselect);
    selected = index;
    sbn_focus_hook(model, index);
}

static int get_selected_mock(void *list, int offset) {
    assert(list == widget + SBN_LIST && offset == 0);
    return selected;
}

static void update_content_mock(void *list) {
    assert(list == widget + SBN_LIST);
    ++updates;
}

static void setup(void) {
    set_pointer(model, SBN_WIDGET, widget);
    set_stock(stock, 2, 2);
    sbn_original_count = count_mock;
    sbn_original_refresh = refresh_mock;
    sbn_original_click = click_mock;
    sbn_original_focus = focus_mock;
    sbn_original_select_item = select_item_mock;
    sbn_original_refresh_focus = refresh_focus_mock;
    sbn_native.construct = construct_mock;
    sbn_native.set_data = set_data_mock;
    sbn_native.make_string = make_string_mock;
    sbn_native.drop_string = drop_string_mock;
    sbn_native.set_text = set_text_mock;
    sbn_native.set_visible = set_visible_mock;
    sbn_native.set_image = set_image_mock;
    sbn_native.select_row = select_row_mock;
    sbn_native.get_selected = get_selected_mock;
    sbn_native.update_content = update_content_mock;
}

static void test_install(void) {
    g_ep147_build = EPB_V130;
    assert(!source_browser_native_install() && !slot_calls);
    g_ep147_build = EPB_V200;
    bad_signature = 1;
    assert(!source_browser_native_install() && !slot_calls);
    bad_signature = 0;
    refuse_patch = 1;
    assert(!source_browser_native_install() && slot_calls == 1);
    refuse_patch = 0;
    assert(source_browser_native_install() && slot_calls == 5);
    assert(source_browser_native_install() && slot_calls == 5);
}

static void test_model_and_paint(void) {
    assert(sbn_count_hook(model) == 2);
    label = "YouTube";
    assert(sbn_count_hook(model) == 3);
    void *cell = sbn_refresh_hook(model, 2, 1, 1, NULL);
    assert(cell && icon_visible && painted_image == 0x1000 && row_focus && !row_active);
    assert(strcmp(title_out, label) == 0 && constructed == 1 && dropped == 2);
    assert(sbn_refresh_hook(model, 0, 1, 0, cell) == cell);
    assert(icon_visible && row_active && stock[0].active == 1);
    active = 1;
    assert(sbn_refresh_hook(model, 0, 1, 0, cell) == cell);
    assert(icon_visible && !row_active && stock[0].active == 1);
    assert(sbn_refresh_hook(model, 2, 1, 0, cell) == cell);
    assert(icon_visible && !row_focus && row_active && constructed == 1);
    sbn_refresh_focus_hook(model, false);
    assert(sbn_refresh_hook(model, 2, 1, 1, cell) == cell && !row_focus);
    sbn_refresh_focus_hook(model, true);
    assert(sbn_refresh_hook(model, 2, 1, 1, cell) == cell && row_focus);
    uintptr_t other_cell = 0;
    assert(sbn_refresh_hook(model, 2, 1, 1, &other_cell) == &other_cell);
    free(cell);
    active = 0;
}

static void test_activation(void) {
    sbn_focus_hook(model, 1);
    assert(focus_calls == 1 && sbn_count_hook(model) == 3);
    sbn_click_hook(model, 2, 1, NULL);
    assert(selected == 2 && activations == 1 && !clicked && active);
    assert(!stock[0].focused && !stock[1].focused);
    sbn_click_hook(model, 1, 1, NULL);
    assert(clicked == 1 && cleared == 1 && !active);
    sbn_select_item_hook(model);
    assert(activations == 2 && active && !selected_item);
    selected = 1;
    sbn_select_item_hook(model);
    assert(!active && cleared == 2 && selected_item == 1);
    label = NULL;
    selected = 2;
    sbn_select_item_hook(model);
    sbn_click_hook(model, 2, 1, NULL);
    assert(sbn_count_hook(model) == 2 && activations == 2);
    assert(clicked == 1 && selected_item == 1 && updates >= 5);
}

static void test_empty_and_invalid_vector(void) {
    label = "YouTube";
    set_stock(stock, 0, 2);
    assert(sbn_count_hook(model) == 1);
    sbn_refresh_focus_hook(model, true);
    assert(selected == 0);
    void *cell = sbn_refresh_hook(model, 0, 1, 1, NULL);
    assert(cell && row_focus && !row_active);
    free(cell);
    sbn_click_hook(model, 0, 1, NULL);
    assert(activations == 3 && clicked == 1);
    set_stock(stock, 2, 1);
    assert(sbn_count_hook(model) == 1 && count_fallback == 1);
    sbn_click_hook(model, 0, 1, NULL);
    assert(activations == 3 && clicked == 1);
}

int main(void) {
    test_install();
    setup();
    test_model_and_paint();
    test_activation();
    test_empty_and_invalid_vector();
    puts("Native SOURCE browser row registration, rendering, focus and activation passed");
}
