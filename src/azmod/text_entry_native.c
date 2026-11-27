/* SPDX-License-Identifier: GPL-3.0-only */
/* The stock Wi-Fi keyboard widget and controller in EP147. The widget
 * owns the key graphics and layout, and the controller sends JUCE strings
 * and edit actions to an ISoftwareKeyboard::IListener. All native calls
 * and object lifetimes stay on EP147's message thread. */
enum { TE_IDLE, TE_OPEN, TE_READY, TE_BROKEN, TE_CLOSE };
enum { TE_WIDTH = 1280, TE_HEIGHT = 800, TE_TOP = 74, TE_KEYS_Y = 518 };
static _Atomic int te_native_phase;
static uintptr_t te_parent_window;
static void *te_widget, *te_controller;
static pthread_mutex_t te_text_lock = PTHREAD_MUTEX_INITIALIZER;
static char te_text[AZ_TEXT_ENTRY_MAX_BYTES + 1];
static size_t te_max_bytes;
static unsigned te_revision;

struct te_native_api {
    void (*widget_construct)(void *);
    void (*controller_construct)(void *, void *);
    void (*controller_visible)(void *, int);
    void (*add_listener)(void *, void *);
    void (*set_bounds)(void *, int, int, int, int);
    void (*add_to_desktop)(void *, int, void *);
    void (*set_visible)(void *, int);
};

/* Verified against each stock image's widget and Wi-Fi controller construction
 * sites, listener slot and JUCE Component vtable. Both allocate 0x1b8/0x90. */
static const struct te_native_api te_native_v130 = {
    .widget_construct = (void (*)(void *)) 0x1c598f8,
    .controller_construct = (void (*)(void *, void *)) 0x1c69160,
    .controller_visible = (void (*)(void *, int)) 0x1c691c8,
    .add_listener = (void (*)(void *, void *)) 0x1c5c648,
    .set_bounds = (void (*)(void *, int, int, int, int)) 0x249efe0,
    .add_to_desktop = (void (*)(void *, int, void *)) 0x24ac3a0,
    .set_visible = (void (*)(void *, int)) 0x249daf0,
};
static const struct te_native_api te_native_v200 = {
    .widget_construct = (void (*)(void *)) 0x1dc3420,
    .controller_construct = (void (*)(void *, void *)) 0x1dd2ca0,
    .controller_visible = (void (*)(void *, int)) 0x1dd2d08,
    .add_listener = (void (*)(void *, void *)) 0x1dc6188,
    .set_bounds = (void (*)(void *, int, int, int, int)) 0x260c480,
    .add_to_desktop = (void (*)(void *, int, void *)) 0x2619840,
    .set_visible = (void (*)(void *, int)) 0x260af90,
};

static void te_native_key(void *listener, void *string) {
    (void) listener;
    if (atomic_load_explicit(&te_native_phase, memory_order_acquire) != TE_READY)
        return;
    uintptr_t bytes = 0;
    char key[32];
    if (!string || !own_q((uintptr_t) string, &bytes) || !own_str(bytes, key, sizeof key))
        return;
    size_t len = strlen(key);
    if (!len || (unsigned char) key[0] < 32)
        return;
    pthread_mutex_lock(&te_text_lock);
    size_t used = strlen(te_text);
    if (len <= te_max_bytes - used) {
        memcpy(te_text + used, key, len + 1);
        te_revision++;
    }
    pthread_mutex_unlock(&te_text_lock);
}

static void te_native_action(void *listener, int action) {
    (void) listener;
    if (atomic_load_explicit(&te_native_phase, memory_order_acquire) != TE_READY)
        return;
    pthread_mutex_lock(&te_text_lock);
    size_t len = strlen(te_text);
    if (action == 1) {
        te_text[0] = 0;
        te_revision++;
    } else if (action == 0 && len) {
        do {
            len--;
        } while (len && ((unsigned char) te_text[len] & 0xc0) == 0x80);
        te_text[len] = 0;
        te_revision++;
    }
    pthread_mutex_unlock(&te_text_lock);
}

static const struct {
    void (*key)(void *, void *);
    void (*action)(void *, int);
} te_listener_vtable = {te_native_key, te_native_action};
static const void *te_listener = &te_listener_vtable;

static const struct te_native_api *te_native_api(void) {
    switch (g_ep147_build) {
    case EPB_V130: return &te_native_v130;
    case EPB_V200: return &te_native_v200;
    default: return NULL;
    }
}

static void te_native_hide(const struct te_native_api *api) {
    if (te_widget)
        api->controller_visible(te_controller, 0);
}

static int te_native_show(const struct te_native_api *api) {
    api->set_bounds(te_widget, 0, TE_KEYS_Y - TE_TOP, TE_WIDTH, TE_HEIGHT - TE_KEYS_Y);
    api->add_to_desktop(te_widget, 0, (void *) te_parent_window);
    api->set_visible(te_widget, 1);
    /* Component::hasHeavyweightPeer. A peer is needed for native touch. */
    return (((const unsigned char *) te_widget)[0xc0] & 1) != 0;
}

/* Keep the pair and its dialog parent for the process lifetime. EP147 can have
 * keyboard timers queued in the same JUCE dispatch that observes TE_CLOSE. */
static int te_native_get_widget(const struct te_native_api *api) {
    if (te_widget)
        return 1;
    void *widget = calloc(1, 0x1b8);
    void *controller = calloc(1, 0x90);
    if (!widget || !controller) {
        free(widget);
        free(controller);
        return 0;
    }
    api->widget_construct(widget);
    api->controller_construct(controller, widget);
    api->add_listener(controller, &te_listener);
    te_widget = widget;
    te_controller = controller;
    return 1;
}

static void te_native_tick(const struct te_native_api *api) {
    int phase = atomic_load_explicit(&te_native_phase, memory_order_acquire);
    if (phase == TE_OPEN) {
        int next = te_native_get_widget(api) && te_native_show(api) ? TE_READY : TE_BROKEN;
        atomic_compare_exchange_strong(&te_native_phase, &phase, next);
    } else if (phase == TE_CLOSE) {
        te_native_hide(api);
        atomic_store_explicit(&te_native_phase, TE_IDLE, memory_order_release);
    }
}
