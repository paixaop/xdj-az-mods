/* SPDX-License-Identifier: GPL-3.0-only */
/* Overlay dialog and SDK ownership. The native keyboard is a child of the
 * modal's X window and receives its own normal EP147 touch events. */
static struct {
    int owner, selected;
    long deadline, view_leave;
    struct az_surface *source;
    const az_screen_v1 *screen;
    const char *(*result)(int, const char *, void *);
    void *user;
    unsigned revision;
    char title[128], submit[48], hint[160], error[160];
} te_dialog;

static struct az_surface *te_surface(void) {
    return &g_sdk_surf[SDK_SURF_TEXT];
}

static int te_owner_live(void) {
    return te_dialog.owner < 0 || sdk_mod_live(te_dialog.owner);
}

static int te_context_live(void) {
    if (te_dialog.view_leave != atomic_load_explicit(&g_viewleave_n, memory_order_relaxed))
        return 0;
    if (te_dialog.source)
        return te_dialog.source->show && g_state.mode == AZ_CLOSED;
    if (te_dialog.owner < 0)
        return g_state.mode == AZ_COMMUNITY;
    return azmod_cm_screen_owner(&g_state) == te_dialog.owner &&
           cm_scr(te_dialog.owner) == te_dialog.screen;
}

static void te_snapshot(char text[AZ_TEXT_ENTRY_MAX_BYTES + 1], unsigned *revision) {
    pthread_mutex_lock(&te_text_lock);
    memcpy(text, te_text, sizeof te_text);
    *revision = te_revision;
    pthread_mutex_unlock(&te_text_lock);
}

static void te_close(int status) {
    const char *(*result)(int, const char *, void *) = te_dialog.result;
    void *user = te_dialog.user;
    te_dialog.result = NULL;
    atomic_store_explicit(&te_native_phase, TE_CLOSE, memory_order_release);
    sdk_modal_show(te_surface(), (az_rect) {0}, 0);
    if (status != AZ_OK && te_owner_live())
        result(status, NULL, user);
}

static void te_submit(void) {
    if (atomic_load_explicit(&te_native_phase, memory_order_acquire) != TE_READY)
        return;
    if (!te_owner_live() || !te_context_live()) {
        te_close(AZ_CANCELLED);
        return;
    }
    char text[sizeof te_text];
    te_snapshot(text, &te_dialog.revision);
    const char *error = te_dialog.result(AZ_OK, text, te_dialog.user);
    if (error) {
        snprintf(te_dialog.error, sizeof te_dialog.error, "%s", error);
        te_surface()->redraw = 1;
    } else {
        te_close(AZ_OK);
    }
}

static az_rect te_button(int index) {
    return (az_rect) {780 + index * 238, 365, 214, 66};
}

static void te_draw_button(az_draw *d, int index, const char *label) {
    az_rect b = te_button(index);
    int selected = te_dialog.selected == index;
    if (selected)
        azcm_fill_rect(d, b.x, b.y, b.w, b.h, azcm_color(AZ_COL_HI));
    else
        azcm_draw_rect(d, b.x, b.y, b.w, b.h, 2, azcm_color(AZ_COL_TEXT_DIM));
    int x = b.x + (b.w - azcm_text_width(label, 24)) / 2;
    azcm_draw_text(d, x, b.y + 15, label, 24,
                   azcm_color(selected ? AZ_COL_HI_TEXT : AZ_COL_TEXT));
}

static const char *te_fit_value(const char *text) {
    while (*text && azcm_text_width(text, 32) > TE_WIDTH - 128) {
        do {
            text++;
        } while (((unsigned char) *text & 0xc0) == 0x80);
    }
    return text;
}

static void te_paint(az_draw *d, az_rect area, void *user) {
    (void) area;
    (void) user;
    char text[sizeof te_text];
    unsigned revision;
    te_snapshot(text, &revision);
    azcm_draw_text(d, 48, 110, te_dialog.title, 32, azcm_color(AZ_COL_TEXT));
    azcm_draw_rect(d, 48, 196, TE_WIDTH - 96, 68, 2, azcm_color(AZ_COL_HI));
    azcm_draw_text(d, 64, 206, te_fit_value(text), 32, azcm_color(AZ_COL_TEXT));
    const char *note = te_dialog.error[0] ? te_dialog.error : te_dialog.hint;
    azcm_draw_text(d, 48, 278, note, 22, azcm_color(AZ_COL_TEXT_DIM));
    te_draw_button(d, 0, "Cancel");
    te_draw_button(d, 1, te_dialog.submit);
}

static int te_button_hit(const az_event *event) {
    for (int i = 0; i < 2; i++) {
        az_rect b = te_button(i);
        if (event->x >= b.x && event->x < b.x + b.w &&
            event->y >= b.y && event->y < b.y + b.h)
            return i;
    }
    return -1;
}

static int te_input(const az_event *event, void *user) {
    (void) user;
    if (!te_dialog.result)
        return 1;
    int state, hit = -1;
    switch (event->kind) {
    case AZ_EV_KNOB: state = SC_IN_KNOB; break;
    case AZ_EV_CLICK: state = SC_IN_CLICK; break;
    case AZ_EV_BACK: state = SC_IN_BACK; break;
    case AZ_EV_TAP:
        state = SC_IN_TAP;
        hit = te_button_hit(event);
        break;
    default: return 1;
    }
    int answer = sc_input(&te_dialog.selected, state, event->dir, hit);
    if (answer == 0)
        te_close(AZ_CANCELLED);
    else if (answer == 1)
        te_submit();
    else
        te_surface()->redraw = 1;
    return 1;
}

static int te_supports_prompt(void) {
    return te_native_api() && g_sdk_ovc &&
           g_sdk_ovc->scr_w == TE_WIDTH && g_sdk_ovc->scr_h == TE_HEIGHT;
}

static int te_entry_valid(const az_text_entry_v1 *entry) {
    return entry && entry->size >= sizeof *entry && entry->on_result && entry->title &&
           entry->max_bytes && entry->max_bytes <= AZ_TEXT_ENTRY_MAX_BYTES;
}

static void te_prefill(const az_text_entry_v1 *entry) {
    pthread_mutex_lock(&te_text_lock);
    const char *initial = entry->initial ? entry->initial : "";
    size_t len = strnlen(initial, entry->max_bytes + 1);
    if (len > entry->max_bytes) {
        len = entry->max_bytes;
        while (len && ((unsigned char) initial[len] & 0xc0) == 0x80)
            len--;
    }
    memset(te_text, 0, sizeof te_text);
    memcpy(te_text, initial, len);
    te_max_bytes = entry->max_bytes;
    te_revision++;
    pthread_mutex_unlock(&te_text_lock);
}

static int te_overlay_open(void) {
    static const az_surface_v1 desc = {
        .size = sizeof desc, .on_paint = te_paint, .on_input = te_input};
    struct az_surface *surface = te_surface();
    sdk_modal_init(surface, &desc);
    if (surface->w.id == None)
        sdk_surf_create_window(g_sdk_ovc, surface);
    if (surface->w.id == None)
        return AZ_FAILED;
    az_rect full = {0, TE_TOP, TE_WIDTH, TE_HEIGHT - TE_TOP};
    if (sdk_modal_show(surface, full, 1) != 0)
        return AZ_FAILED;
    /* A touch can open the dialog before this tick drains the browse knob.
     * Map it now so the input router keeps focus on the new modal. */
    sdk_surf_place_window(g_sdk_ovc, surface, 1);
    /* The parent XID must exist before the other X connection uses it. */
    XSync(g_sdk_ovc->dpy, False);
    te_parent_window = surface->w.id;
    atomic_store_explicit(&te_native_phase, TE_OPEN, memory_order_release);
    return AZ_OK;
}

static void te_prepare(int owner, struct az_surface *source, const az_text_entry_v1 *entry) {
    memset(&te_dialog, 0, sizeof te_dialog);
    te_dialog.owner = owner;
    te_dialog.source = source;
    te_dialog.screen = source ? NULL : cm_scr(owner);
    te_dialog.view_leave = atomic_load_explicit(&g_viewleave_n, memory_order_relaxed);
    te_dialog.deadline = now_ms() + 2000;
    te_dialog.result = entry->on_result;
    te_dialog.user = entry->user;
    snprintf(te_dialog.title, sizeof te_dialog.title, "%s", entry->title);
    snprintf(te_dialog.submit, sizeof te_dialog.submit, "%s", entry->submit ? entry->submit : "OK");
    snprintf(te_dialog.hint, sizeof te_dialog.hint, "%s", entry->hint ? entry->hint : "");
    te_prefill(entry);
}

static int te_request(int owner, struct az_surface *source, const az_text_entry_v1 *entry) {
    if (!sdk_on_overlay() || !te_entry_valid(entry))
        return AZ_INVALID;
    if (!te_supports_prompt())
        return AZ_UNSUPPORTED;
    if (te_dialog.result || g_sd_dlg.up || atomic_load(&te_native_phase) != TE_IDLE)
        return AZ_BUSY;
    te_prepare(owner, source, entry);
    int status = te_overlay_open();
    if (status != AZ_OK)
        te_dialog.result = NULL;
    return status;
}

static struct az_surface *te_mod_surface(int owner) {
    struct az_surface *source = g_sdk_focus_surf;
    return source && source->mod == owner && source->show ? source : NULL;
}

static int sdk_text_entry(const char *id, const az_text_entry_v1 *entry) {
    cm_mod *mod = sdk_self(id, CM_RA(), "text_entry", 1);
    if (!mod || !te_entry_valid(entry))
        return AZ_INVALID;
    if (!cm_command_code(mod, (uintptr_t) entry->on_result))
        return AZ_INVALID;
    int owner = (int) (mod - g_cm);
    struct az_surface *source = te_mod_surface(owner);
    if (!source && azmod_cm_screen_owner(&g_state) != owner)
        return AZ_INVALID;
    return te_request(owner, source, entry);
}

int azcm_text_entry(const az_text_entry_v1 *entry) {
    return te_request(-1, NULL, entry);
}

static int te_open_failed(int phase) {
    if (phase != TE_OPEN)
        return phase == TE_BROKEN;
    int order = compare_timestamps(now_ms(), te_dialog.deadline, 1000, 1000, 1);
    return order == AZ_TIME_EQUAL || order == AZ_TIME_AFTER || order == AZ_TIME_INVALID;
}

static int sdk_text_entry_tick(void) {
    if (!te_dialog.result)
        return 0;
    if (!te_owner_live() || !te_context_live()) {
        te_close(AZ_CANCELLED);
        return 0;
    }
    if (te_open_failed(atomic_load(&te_native_phase))) {
        te_close(AZ_FAILED);
        return 0;
    }
    pthread_mutex_lock(&te_text_lock);
    if (te_revision != te_dialog.revision) {
        te_dialog.revision = te_revision;
        te_surface()->redraw = 1;
    }
    pthread_mutex_unlock(&te_text_lock);
    return 1;
}

static void sdk_text_source_changed(const cm_mod *mod) {
    if (te_dialog.result && te_dialog.source && te_dialog.owner == mod - g_cm)
        te_close(AZ_CANCELLED);
}
