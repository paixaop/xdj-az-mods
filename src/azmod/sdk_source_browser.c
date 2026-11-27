/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by community.c. Native selection and mixer events publish a view;
 * only the overlay thread calls the registered browser. */
#include "source_browser_buttons.h"
#include "source_provider.h"

static _Atomic(cm_mod *) g_source_browser_owner;
static _Atomic(cm_mod *) g_source_provider_owner;
static _Atomic(const struct azsp_native *) g_source_provider_native;
static _Atomic(cm_mod *) g_source_browser_registered[CM_MAX];
static cm_mod *g_source_browser_delivered;
static _Atomic int g_source_browser_pending = SOURCE_BROWSER_NO_VIEW;
static _Atomic int g_source_browser_shown;
static _Atomic int g_source_browser_over_source;
static _Atomic int g_source_browser_native_view;
static _Atomic unsigned g_source_browser_button;
static int g_source_browser_install_result;
static pthread_mutex_t g_source_browser_query_lock = PTHREAD_MUTEX_INITIALIZER;

static int cmi_png_fit(unsigned char *, int, uint32_t, uint32_t);

static int sdk_source_native_valid(const struct azsp_native *native) {
    return native && native->select && native->clear && native->storage_modes;
}

int azsp_bind_native(const struct azsp_native *native) {
    if (!sdk_source_native_valid(native))
        return AZ_INVALID;
    const struct azsp_native *expected = NULL;
    return atomic_compare_exchange_strong(&g_source_provider_native, &expected, native) ? AZ_OK : AZ_BUSY;
}

static const struct azsp_native *sdk_source_provider_native(void) {
    return atomic_load_explicit(&g_source_provider_native, memory_order_acquire);
}

enum az_source_storage_mode azsp_source_storage_mode(const void *source) {
    const cm_mod *owner = source;
    return atomic_load_explicit(&owner->source_provider_mode, memory_order_acquire);
}

int azsp_request_source(void *source, const struct azsp_request *request, struct azsp_result *result) {
    cm_mod *owner = source; /* Token given to the adapter by its select callback. */
    pthread_mutex_lock(&owner->source_provider_worker);
    int status = AZ_STALE;
    if (cm_enter(owner)) {
        status = azsp_run(&owner->source_provider, request, result);
        cm_leave(owner);
    } else
        memset(result, 0, sizeof(*result));
    pthread_mutex_unlock(&owner->source_provider_worker);
    return status;
}

static int sdk_source_stage_file(void *source, const az_source_track_v1 *track, struct azsp_stage *stage) {
    struct azsp_request request = {.operation = AZSP_PREPARE_TRACK, .value.prepare.track = *track};
    snprintf(request.value.prepare.directory, sizeof(request.value.prepare.directory), "%s",
             azsp_stage_directory(stage));
    struct azsp_result result;
    int status = azsp_request_source(source, &request, &result);
    if (status == AZ_OK)
        azsp_stage_set_path(stage, &result);
    return status;
}

int azsp_prepare_source(void *source, const az_source_track_v1 *track, const char *parent,
                        enum az_source_storage_mode mode, struct azsp_stage **out) {
    struct azsp_stage *stage;
    *out = NULL;
    int status = azsp_stage_create(parent, mode, &stage);
    if (status != AZ_OK)
        return status;
    status = sdk_source_stage_file(source, track, stage);
    if (status != AZ_OK)
        azsp_stage_release(stage);
    else
        *out = stage;
    return status;
}

static cm_mod *sdk_source_provider_selected(void) {
    return atomic_load_explicit(&g_source_provider_owner, memory_order_acquire);
}

static cm_mod *sdk_source_browser_live(void) {
    cm_mod *owner = atomic_load_explicit(&g_source_browser_owner, memory_order_acquire);
    return owner && cm_running(owner) ? owner : NULL;
}

static cm_mod *sdk_source_browser_registered(int index) {
    cm_mod *owner = atomic_load_explicit(&g_source_browser_registered[index], memory_order_acquire);
    return owner && cm_running(owner) ? owner : NULL;
}

static cm_mod *sdk_source_browser_at(int row) {
    for (int i = 0; i < CM_MAX; ++i) {
        cm_mod *owner = sdk_source_browser_registered(i);
        if (owner && row-- == 0)
            return owner;
    }
    return NULL;
}

static int sdk_source_browser_count(void) {
    int count = 0;
    for (int i = 0; i < CM_MAX; ++i)
        count += sdk_source_browser_registered(i) != NULL;
    return count;
}

static const char *sdk_source_browser_label(int row) {
    cm_mod *owner = sdk_source_browser_at(row);
    return owner ? owner->source_browser_name : NULL;
}

static uintptr_t sdk_source_browser_icon(int row, uintptr_t (*decode)(const unsigned char *, int)) {
    cm_mod *owner = sdk_source_browser_at(row);
    if (!owner || !owner->source_browser_png)
        return 0;
    if (!owner->source_browser_image_decoded) {
        owner->source_browser_image_decoded = 1;
        owner->source_browser_image = decode(owner->source_browser_png, owner->source_browser_png_size);
    }
    return owner->source_browser_image;
}

static int sdk_source_browser_is_selected(void) {
    return sdk_source_browser_live() || sdk_source_provider_selected();
}

static int sdk_source_browser_active(int row) {
    cm_mod *owner = sdk_source_browser_at(row);
    return owner && (owner == sdk_source_browser_live() || owner == sdk_source_provider_selected());
}

static void sdk_source_browser_queue(int view) {
    atomic_store_explicit(&g_source_browser_pending, view, memory_order_release);
}

static void sdk_source_browser_native_queue(int view, unsigned button) {
    atomic_store_explicit(&g_source_browser_native_view, view, memory_order_release);
    atomic_store_explicit(&g_source_browser_button, button, memory_order_release);
    sdk_source_browser_queue(view);
}

static void sdk_source_browser_native_selected(void) {
    if (atomic_exchange_explicit(&g_source_provider_owner, NULL, memory_order_acq_rel))
        sdk_source_provider_native()->clear();
    if (atomic_exchange_explicit(&g_source_browser_owner, NULL, memory_order_acq_rel))
        sdk_source_browser_native_queue(AZ_SOURCE_VIEW_HIDDEN, 0);
}

static void sdk_source_provider_select(cm_mod *owner, void *model) {
    sdk_source_browser_native_selected();
    if (sdk_source_provider_native()->select(owner, model) == AZ_OK)
        atomic_store_explicit(&g_source_provider_owner, owner, memory_order_release);
}

static void sdk_source_browser_activate(int row, void *model) {
    cm_mod *owner = sdk_source_browser_at(row);
    if (!owner)
        return;
    if (owner->source_provider.size) {
        sdk_source_provider_select(owner, model);
        return;
    }
    if (sdk_source_provider_selected())
        sdk_source_browser_native_selected();
    atomic_store_explicit(&g_source_browser_over_source, 1, memory_order_release);
    atomic_store_explicit(&g_source_browser_owner, owner, memory_order_release);
    if (owner->source_browser_query) {
        sdk_source_browser_native_queue(AZ_SOURCE_VIEW_BROWSE, 0x40u);
    } else {
        atomic_store_explicit(&g_source_browser_native_view, AZ_SOURCE_VIEW_HIDDEN, memory_order_release);
        sdk_source_browser_queue(AZ_SOURCE_VIEW_BROWSE);
    }
}

static int sdk_source_browser_reveal_native(void) {
    return atomic_load_explicit(&g_source_browser_shown, memory_order_acquire) &&
           __atomic_load_n(&g_sdk_focus, __ATOMIC_ACQUIRE) &&
           atomic_load_explicit(&g_source_browser_over_source, memory_order_acquire);
}

static void sdk_source_browser_native_tab(unsigned rising) {
    /* The custom surface can cover SOURCE's screen signature. Track a native
     * tab change from the button that reached firmware. */
    if (rising & SOURCE_BROWSER_NATIVE_TABS)
        atomic_store_explicit(&g_source_browser_over_source, 0, memory_order_release);
}

static unsigned sdk_source_browser_synthetic_key(unsigned buttons) {
    static unsigned held;
    static long until;
    if (buttons) {
        held = 0;
        return buttons;
    }
    unsigned requested = atomic_exchange_explicit(&g_source_browser_button, 0, memory_order_acq_rel);
    long time = now_ms();
    if (requested) {
        held = requested;
        until = time + 240;
    }
    return time < until ? held : 0;
}

static unsigned sdk_source_browser_route_native(unsigned buttons, unsigned rising) {
    int view = source_browser_key_view(rising);
    if (view != SOURCE_BROWSER_NO_VIEW)
        sdk_source_browser_native_queue(view, 0);
    return sdk_source_browser_synthetic_key(buttons);
}

static int sdk_source_browser_native_owner(int selected) {
    cm_mod *owner = selected ? sdk_source_browser_live() : NULL;
    return owner && owner->source_browser_query;
}

static unsigned sdk_source_browser_route(struct source_browser_buttons *keys, unsigned buttons,
                                         int selected) {
    unsigned previous = keys->down;
    int view;
    int native = sdk_source_browser_native_owner(selected);
    unsigned result = source_browser_route_keys(keys, buttons, selected && !native,
                                                sdk_source_browser_reveal_native(), &view);
    if (native)
        return sdk_source_browser_route_native(result, buttons & ~previous);
    sdk_source_browser_native_tab(result & ~previous);
    if (view != SOURCE_BROWSER_NO_VIEW)
        sdk_source_browser_queue(view);
    return result;
}

static int sdk_source_browser_tracks_active(void) {
    int view = atomic_load_explicit(&g_source_browser_native_view, memory_order_acquire);
    return (view == AZ_SOURCE_VIEW_BROWSE && atomic_load_explicit(&g_bv_vis, memory_order_acquire)) ||
           (view == AZ_SOURCE_VIEW_SEARCH && atomic_load_explicit(&g_sv_vis, memory_order_acquire));
}

static int sdk_source_browser_store_buttons(uint8_t *frame, unsigned buttons, unsigned actions,
                                            int native) {
    if (buttons == frame[32] && actions == frame[33])
        return 0;
    if (buttons && buttons != frame[32] && native)
        frame[6] = 0;
    if (!buttons && !actions)
        frame[6] = 0x08;
    frame[32] = (uint8_t) buttons;
    frame[33] = (uint8_t) actions;
    return 1;
}

static int sdk_source_browser_filter(uint8_t *frame) {
    static struct source_browser_buttons keys; /* mixer reader owns the held bits */
    int selected = wd_stall <= 200u && sdk_source_browser_live();
    unsigned buttons = sdk_source_browser_route(&keys, frame[32], selected);
    unsigned actions = frame[33];
    if (selected && sdk_source_browser_tracks_active())
        actions &= ~0x6fu; /* The native background still has its previous source's selection. */
    return sdk_source_browser_store_buttons(frame, buttons, actions,
                                            sdk_source_browser_native_owner(selected));
}

static int sdk_source_browser_view_ready(cm_mod *owner, int view) {
    if (owner && owner->source_browser_query) {
        if (view == AZ_SOURCE_VIEW_SEARCH)
            return atomic_load_explicit(&g_sv_vis, memory_order_acquire);
        if (view == AZ_SOURCE_VIEW_BROWSE)
            return atomic_load_explicit(&g_bv_vis, memory_order_acquire);
    }
    return 1;
}

static int sdk_source_browser_wait_view(cm_mod *owner, int view) {
    if (sdk_source_browser_view_ready(owner, view))
        return 0;
    int pending = SOURCE_BROWSER_NO_VIEW;
    atomic_compare_exchange_strong_explicit(&g_source_browser_pending, &pending, view,
                                            memory_order_release, memory_order_relaxed);
    return 1;
}

static void sdk_source_browser_notify(cm_mod *owner, enum az_source_view view) {
    if (!owner || !cm_running(owner))
        return;
    sdk_text_source_changed(owner);
    owner->source_browser_view(view, owner->source_browser_user);
}

static void sdk_source_browser_views(void) {
    int view = atomic_exchange_explicit(&g_source_browser_pending, SOURCE_BROWSER_NO_VIEW,
                                        memory_order_acq_rel);
    if (view == SOURCE_BROWSER_NO_VIEW)
        return;
    cm_mod *owner = sdk_source_browser_live();
    if (!owner)
        view = AZ_SOURCE_VIEW_HIDDEN;
    cm_mod *previous = g_source_browser_delivered;
    if (previous && previous != owner) {
        sdk_source_browser_notify(previous, AZ_SOURCE_VIEW_HIDDEN);
        g_source_browser_delivered = NULL;
    }
    if (sdk_source_browser_wait_view(owner, view))
        return;
    atomic_store_explicit(&g_source_browser_shown, view != AZ_SOURCE_VIEW_HIDDEN, memory_order_release);
    sdk_source_browser_notify(owner, (enum az_source_view) view);
    g_source_browser_delivered = owner;
}

static cm_mod *sdk_source_browser_editor(void) {
    if (atomic_load_explicit(&g_source_browser_native_view, memory_order_acquire) != AZ_SOURCE_VIEW_SEARCH)
        return NULL;
    if (!atomic_load_explicit(&g_sv_vis, memory_order_acquire))
        return NULL;
    cm_mod *owner = sdk_source_browser_live();
    return owner && owner->source_browser_query ? owner : NULL;
}

static int sdk_source_browser_editing(void) {
    return sdk_source_browser_editor() != NULL;
}

static int sdk_source_browser_key(const char *key) {
    cm_mod *owner = sdk_source_browser_editor();
    if (!owner)
        return 0;
    pthread_mutex_lock(&g_source_browser_query_lock);
    size_t used = strlen(owner->source_browser_search), size = strlen(key);
    if (size && used + size < sizeof(owner->source_browser_search)) {
        memcpy(owner->source_browser_search + used, key, size + 1);
        owner->source_browser_search_dirty = 1;
    }
    pthread_mutex_unlock(&g_source_browser_query_lock);
    return 1;
}

static void sdk_source_browser_trim(cm_mod *owner, int action) {
    char *text = owner->source_browser_search;
    size_t end = strlen(text);
    if (action == 0 && end) {
        --end;
        while (end && ((unsigned char) text[end] & 0xc0) == 0x80)
            --end;
    } else if (action == 1) {
        end = 0;
    }
    text[end] = '\0';
    owner->source_browser_search_dirty = 1;
}

static int sdk_source_browser_action(int action) {
    cm_mod *owner = sdk_source_browser_editor();
    if (!owner)
        return 0;
    pthread_mutex_lock(&g_source_browser_query_lock);
    sdk_source_browser_trim(owner, action);
    pthread_mutex_unlock(&g_source_browser_query_lock);
    return 1;
}

static void sdk_source_browser_query(cm_mod *owner) {
    char text[sizeof(owner->source_browser_search)];
    if (pthread_mutex_trylock(&g_source_browser_query_lock))
        return;
    int changed = owner->source_browser_search_dirty;
    if (changed)
        memcpy(text, owner->source_browser_search, sizeof(text));
    owner->source_browser_search_dirty = 0;
    pthread_mutex_unlock(&g_source_browser_query_lock);
    if (changed)
        owner->source_browser_query(text, owner->source_browser_user);
}

static void sdk_source_browser_tick(void) {
    sdk_source_browser_views();
    cm_mod *owner = sdk_source_browser_editor();
    if (owner)
        sdk_source_browser_query(owner);
}

#include "source_browser_native.c"

static int sdk_source_browser_init_owner(const cm_mod *owner) {
    return owner && g_sdk_in_init && (owner - g_cm) == g_cm_loading;
}

static int sdk_source_browser_validate(cm_mod *owner, const az_source_browser_v1 *browser) {
    if (!browser || browser->size < offsetof(az_source_browser_v1, on_query) ||
        !browser->name || !browser->on_view)
        return 0;
    size_t len = strnlen(browser->name, sizeof(owner->source_browser_name));
    return len && len < sizeof(owner->source_browser_name) &&
           cm_command_code(owner, (uintptr_t) browser->on_view);
}

static int sdk_source_copy_icon(cm_mod *owner, const unsigned char *png, uint32_t bytes) {
    if (!bytes)
        return AZ_OK;
    if (!png || bytes > 16384)
        return AZ_INVALID;
    unsigned char *image = malloc(bytes);
    if (!image)
        return AZ_FAILED;
    memcpy(image, png, bytes);
    int size = cmi_png_fit(image, (int) bytes, 128, 128);
    if (size <= 0) {
        free(image);
        return AZ_INVALID;
    }
    owner->source_browser_png = image;
    owner->source_browser_png_size = size;
    return AZ_OK;
}

static int sdk_source_browser_copy_icon(cm_mod *owner, const az_source_browser_v1 *browser) {
    return browser->size < sizeof(*browser) ? AZ_OK : sdk_source_copy_icon(owner, browser->icon_png, browser->icon_bytes);
}

static int sdk_source_browser_has_query(const az_source_browser_v1 *browser) {
    return browser->size >= offsetof(az_source_browser_v1, icon_png) && browser->on_query;
}

static int sdk_source_browser_query_valid(cm_mod *owner, const az_source_browser_v1 *browser) {
    if (!sdk_source_browser_has_query(browser))
        return AZ_OK;
    if (!(g_nm_hooked & 1) || !install_msgtick_hook("Source Search keyboard"))
        return AZ_UNSUPPORTED;
    if (!cm_command_code(owner, (uintptr_t) browser->on_query))
        return AZ_INVALID;
    return AZ_OK;
}

static int sdk_source_browser_install_once(void) {
    if (!g_source_browser_install_result)
        g_source_browser_install_result = source_browser_native_install() ? 1 : -1;
    return g_source_browser_install_result > 0;
}

static void sdk_source_publish_owner(cm_mod *owner, const char *name) {
    snprintf(owner->source_browser_name, sizeof(owner->source_browser_name), "%s", name);
    __atomic_store_n(&owner->sticky, 1, __ATOMIC_RELEASE);
    atomic_store_explicit(&g_source_browser_registered[owner - g_cm], owner, memory_order_release);
}

static void sdk_source_browser_publish(cm_mod *owner, const az_source_browser_v1 *browser) {
    owner->source_browser_view = browser->on_view;
    owner->source_browser_user = browser->user;
    owner->source_browser_query = sdk_source_browser_has_query(browser) ? browser->on_query : NULL;
    sdk_source_publish_owner(owner, browser->name);
}

static int sdk_source_register_browser(const char *id, const az_source_browser_v1 *browser) {
    cm_mod *owner = cm_self(id, CM_RA(), "register_source_browser");
    if (!sdk_source_browser_init_owner(owner) || !sdk_source_browser_validate(owner, browser))
        return AZ_INVALID;
    if (atomic_load_explicit(&g_source_browser_registered[owner - g_cm], memory_order_acquire))
        return AZ_BUSY;
    if (!sdk_source_browser_install_once())
        return AZ_UNSUPPORTED;
    int status = sdk_source_browser_query_valid(owner, browser);
    if (status != AZ_OK)
        return status;
    status = sdk_source_browser_copy_icon(owner, browser);
    if (status != AZ_OK)
        return status;
    sdk_source_browser_publish(owner, browser);
    return AZ_OK;
}

static int sdk_source_show_view(const char *id, enum az_source_view view) {
    static const unsigned buttons[] = {0x80u, 0x40u, 0x08u};
    if ((unsigned) view >= sizeof(buttons) / sizeof(buttons[0]))
        return AZ_INVALID;
    cm_mod *owner = cm_self(id, CM_RA(), "source_show_view");
    if (!owner || !owner->source_browser_query)
        return AZ_INVALID;
    if (owner != sdk_source_browser_live())
        return AZ_STALE;
    sdk_source_browser_native_queue(view, buttons[view]);
    return AZ_OK;
}

static int sdk_source_provider_code(cm_mod *owner, const az_source_provider_v1 *provider) {
    return cm_command_code(owner, (uintptr_t) provider->list_playlists) &&
           cm_command_code(owner, (uintptr_t) provider->playlist_tracks) &&
           cm_command_code(owner, (uintptr_t) provider->search) &&
           cm_command_code(owner, (uintptr_t) provider->prepare_track);
}

static int sdk_source_provider_publish(cm_mod *owner, const az_source_provider_v1 *provider) {
    if (pthread_mutex_init(&owner->source_provider_worker, NULL))
        return AZ_FAILED;
    int status = sdk_source_copy_icon(owner, provider->icon_png, provider->icon_bytes);
    if (status != AZ_OK) {
        pthread_mutex_destroy(&owner->source_provider_worker);
        return status;
    }
    owner->source_provider = *provider;
    owner->source_provider.name = owner->source_browser_name;
    owner->source_provider.icon_png = owner->source_browser_png;
    owner->source_provider.icon_bytes = owner->source_browser_png_size;
    atomic_init(&owner->source_provider_mode, provider->storage_mode);
    sdk_source_publish_owner(owner, provider->name);
    return AZ_OK;
}

static int sdk_source_supports_mode(enum az_source_storage_mode mode) {
    const struct azsp_native *native = sdk_source_provider_native();
    return native && (native->storage_modes & azsp_storage_flag(mode));
}

static int sdk_source_register_provider(const char *id, const az_source_provider_v1 *provider) {
    cm_mod *owner = cm_self(id, CM_RA(), "register_source_provider");
    if (!sdk_source_browser_init_owner(owner) || !azsp_provider_valid(provider))
        return AZ_INVALID;
    if (!sdk_source_provider_code(owner, provider))
        return AZ_INVALID;
    if (atomic_load_explicit(&g_source_browser_registered[owner - g_cm], memory_order_acquire))
        return AZ_BUSY;
    if (!sdk_source_supports_mode(provider->storage_mode) || !sdk_source_browser_install_once())
        return AZ_UNSUPPORTED;
    return sdk_source_provider_publish(owner, provider);
}

static int sdk_source_provider_running(const cm_mod *owner) {
    return owner && cm_running(owner) && owner->source_provider.size;
}

static int sdk_source_set_storage_mode(const char *id, enum az_source_storage_mode mode) {
    cm_mod *owner = cm_self(id, CM_RA(), "source_set_storage_mode");
    if (!sdk_source_provider_running(owner) || !g_cm_on_ui || !azsp_storage_flag(mode))
        return AZ_INVALID;
    if (!sdk_source_supports_mode(mode))
        return AZ_UNSUPPORTED;
    atomic_store_explicit(&owner->source_provider_mode, mode, memory_order_release);
    return AZ_OK;
}

static const az_source_v1 *sdk_source_service(uint32_t abi) {
    static const az_source_v1 browser = {.abi = AZ_SOURCE_ABI, .size = sizeof(browser), .register_browser = sdk_source_register_browser, .show_view = sdk_source_show_view};
    static const az_source_v1 provider = {.abi = AZ_SOURCE_ABI, .size = sizeof(provider), .register_browser = sdk_source_register_browser, .show_view = sdk_source_show_view, .register_provider = sdk_source_register_provider, .set_storage_mode = sdk_source_set_storage_mode};
    if (abi != AZ_SOURCE_ABI || g_ep147_build != EPB_V200)
        return NULL;
    return sdk_source_provider_native() ? &provider : &browser;
}
