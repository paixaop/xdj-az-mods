/* SPDX-License-Identifier: GPL-3.0-only */
/* Beatport login service. Included by community.c. Native objects belong only
 * to EP147's message thread; the overlay exchanges copies under bpl_mu.
 * The worker never holds that mutex while accessing the settings partition. */
#include "beatport_credentials.h"
#include <sys/ipc.h>
#include <sys/shm.h>

#ifndef BPL_PATH
#define BPL_PATH CM_STATE "/beatport-remember.dat"
#endif
#ifndef BPL_CTOR
#define BPL_CTOR 0x1b8e3a8u
#define BPL_DISPATCH 0x1b8dfa8u
#define BPL_DTOR 0x1b8e988u
#define BPL_DELETE 0x1b8ef30u
#define BPL_DTOR8 0x1b8f110u
#define BPL_DELETE8 0x1b8ed48u
#define BPL_DTOR48 0x1b8f2e8u
#define BPL_DELETE48 0x1b8eb60u
#define BPL_WIDGET_VT 0x2d8d5e8u
#define BPL_CTRL_VT 0x2d91bd0u
#define BPL_LOGIN_NAME 0x3feee98u
#define BPL_STRING_CTOR 0x24a1180u
#define BPL_STRING_DTOR 0x2469aa0u
#define BPL_EDITOR_SET 0x2647670u
#endif

enum {
    BPL_LOAD = 1,
    BPL_SAVE,
    BPL_FORGET
};
static pthread_mutex_t bpl_mu = PTHREAD_MUTEX_INITIALIZER;
/* Accepted cancellation and LOG IN dispatch have a single ordering. API calls
 * refuse a busy action instead of blocking the overlay behind native code. */
static pthread_mutex_t bpl_action_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bpl_cv = PTHREAD_COND_INITIALIZER;
static int bpl_ready, bpl_owner = -1, bpl_worker_started;
static _Atomic int bpl_active;
static az_beatport_login_options bpl_options;
static struct bp_credentials bpl_saved, bpl_candidate;
static int bpl_have, bpl_error, bpl_job;
static uint64_t bpl_serial, bpl_done, bpl_control;

/* Message thread only, except the wref slot which the component destructor
 * clears. All complete/deleting destructor variants invalidate the controller,
 * including independently compiled secondary-base adjustors. */
static _Atomic uintptr_t bpl_widget;
static void *bpl_ctrl;
static uint64_t bpl_seen_control;
static uint64_t bpl_screen;
static long bpl_opened_ms, bpl_press_ms;
static int bpl_filled, bpl_cancelled, bpl_presses, bpl_visible;
static struct bp_credentials bpl_filled_with;
static void (*bpl_orig_ctor)(void *, void *);
static void (*bpl_orig_dispatch)(void *, void *);
static void (*bpl_orig_dtor)(void *);
static void (*bpl_orig_delete)(void *);
static void (*bpl_orig_dtor8)(void *);
static void (*bpl_orig_delete8)(void *);
static void (*bpl_orig_dtor48)(void *);
static void (*bpl_orig_delete48)(void *);

typedef struct {
    const char *text;
} bpl_string;
#ifndef BPL_TEST
#include "../common/aznative_c.h"
static void bpl_get_text(bpl_string *out, void *editor) {
    az_native_result_word((void *) (uintptr_t) 0x263f4e0, out, editor);
}
#else
extern void bpl_get_text(bpl_string *, void *);
#endif

static void bpl_unlock_action(pthread_mutex_t **mutex) { pthread_mutex_unlock(*mutex); }
static int bpl_caller_live(cm_mod *c) {
    return g_cm_on_ui && c && (c->state == AZCM_LOADED || (g_sdk_in_init && g_cm_loading == c - g_cm));
}

static void bpl_reset_attempt(void) {
    bpl_filled = bpl_cancelled = bpl_presses = 0;
    bpl_press_ms = 0;
    bp_credentials_clear(&bpl_filled_with, sizeof bpl_filled_with);
}

static void bpl_queue_locked(int job, const struct bp_credentials *creds) {
    bp_credentials_clear(&bpl_candidate, sizeof bpl_candidate);
    if (creds) bpl_candidate = *creds;
    bpl_job = job;
    ++bpl_serial;
    bpl_error = 0;
    pthread_cond_signal(&bpl_cv);
}

static int bpl_storage_operation(int job, struct bp_credentials *value) {
    if (job == BPL_LOAD) return bp_credentials_load(BPL_PATH, value);
    if (job == BPL_FORGET) return bp_credentials_forget(BPL_PATH);
    return bp_credentials_save(BPL_PATH, value);
}
static void bpl_storage_result(uint64_t serial, int job, int rc, int error, const struct bp_credentials *value) {
    /* A superseded operation may still have committed; describe that result. */
    if (!error) {
        bp_credentials_clear(&bpl_saved, sizeof bpl_saved);
        bpl_have = job == BPL_SAVE || (job == BPL_LOAD && rc == 1);
        if (bpl_have) bpl_saved = *value;
    }
    if (serial == bpl_serial) {
        bpl_done = serial;
        bpl_error = error;
        bp_credentials_clear(&bpl_candidate, sizeof bpl_candidate);
    }
}
static void *bpl_worker(void *unused) {
    (void) unused;
    pthread_mutex_lock(&bpl_mu);
    for (;;) {
        while (bpl_done == bpl_serial) pthread_cond_wait(&bpl_cv, &bpl_mu);
        uint64_t serial = bpl_serial;
        int job = bpl_job;
        struct bp_credentials value = bpl_candidate;
        pthread_mutex_unlock(&bpl_mu);
        errno = 0;
        int rc = bpl_storage_operation(job, &value);
        int error = rc < 0 ? (errno ? errno : EIO) : 0;
        pthread_mutex_lock(&bpl_mu);
        bpl_storage_result(serial, job, rc, error, &value);
        bp_credentials_clear(&value, sizeof value);
    }
    return NULL;
}
static int bpl_option_flag(int value) { return value == 0 || value == 1; }
static int bpl_options_valid(const az_beatport_login_options *options) {
    return options && options->size >= sizeof *options && bpl_option_flag(options->auto_login) &&
           bpl_option_flag(options->remember) && options->delay_sec >= 0 && options->delay_sec <= 600 &&
           options->max_attempts >= 1 && options->max_attempts <= 4;
}
static int bpl_owner_admitted(int owner) {
    return bpl_owner >= 0 ? bpl_owner == owner : g_sdk_in_init && g_cm_loading == owner;
}
static int bpl_start_worker(void) {
    if (bpl_worker_started) return 1;
    pthread_t thread;
    if (az_thread_create(&thread, NULL, bpl_worker, NULL)) return 0;
    pthread_detach(thread);
    bpl_worker_started = 1;
    bpl_queue_locked(BPL_LOAD, NULL);
    return 1;
}
static int bpl_configure(const char *id, const az_beatport_login_options *o) {
    if (!g_cm_on_ui) return -1;
    cm_mod *c = cm_self(id, CM_RA(), "beatport.configure");
    if (!bpl_caller_live(c) || !bpl_ready || !bpl_options_valid(o)) return -1;
    if (pthread_mutex_trylock(&bpl_action_mu)) return -1;
    pthread_mutex_t *action __attribute__((cleanup(bpl_unlock_action))) = &bpl_action_mu;
    pthread_mutex_lock(&bpl_mu);
    int owner = (int) (c - g_cm);
    if (!bpl_owner_admitted(owner) || !bpl_start_worker()) {
        pthread_mutex_unlock(&bpl_mu);
        return -1;
    }
    bpl_owner = owner;
    bpl_options = *o;
    ++bpl_control;
    pthread_mutex_unlock(&bpl_mu);
    return 0;
}

static int bpl_status(const char *id, az_beatport_login_status *out) {
    if (!g_cm_on_ui) return -1;
    cm_mod *c = cm_self(id, CM_RA(), "beatport.status");
    if (!bpl_caller_live(c) || !out || out->size < sizeof *out) return -1;
    pthread_mutex_lock(&bpl_mu);
    if (bpl_owner != c - g_cm) {
        pthread_mutex_unlock(&bpl_mu);
        return -1;
    }
    *out = (az_beatport_login_status) {.size = sizeof *out,
                                       .have_credentials = bpl_have,
                                       .save_pending = bpl_serial != bpl_done,
                                       .storage_error = bpl_error};
    if (bpl_have) memcpy(out->username, bpl_saved.user, sizeof out->username);
    pthread_mutex_unlock(&bpl_mu);
    return 0;
}

static int bpl_forget(const char *id) {
    if (!g_cm_on_ui) return -1;
    cm_mod *c = cm_self(id, CM_RA(), "beatport.forget");
    if (!bpl_caller_live(c) || pthread_mutex_trylock(&bpl_action_mu)) return -1;
    pthread_mutex_t *action __attribute__((cleanup(bpl_unlock_action))) = &bpl_action_mu;
    pthread_mutex_lock(&bpl_mu);
    if (bpl_owner != c - g_cm) {
        pthread_mutex_unlock(&bpl_mu);
        return -1;
    }
    ++bpl_control;
    bpl_queue_locked(BPL_FORGET, NULL);
    pthread_mutex_unlock(&bpl_mu);
    return 0;
}

static const az_beatport_login_v1 bpl_api = {
    AZ_BEATPORT_LOGIN_ABI, sizeof(az_beatport_login_v1),
    bpl_configure, bpl_status, bpl_forget};
static const az_beatport_login_v1 *sdk_beatport_login_service(uint32_t abi) {
    return bpl_ready && abi == AZ_BEATPORT_LOGIN_ABI ? &bpl_api : NULL;
}

/* Overlay thread: no native objects touched. Disabling/suspending the owning
 * mod stops native work even when its own tick callback is no longer called. */
static void sdk_beatport_owner_tick(void) {
    pthread_mutex_lock(&bpl_mu);
    int enabled = bpl_owner >= 0 && bpl_owner < g_cm_n &&
                  g_cm[bpl_owner].state == AZCM_LOADED;
    atomic_store_explicit(&bpl_active, enabled, memory_order_release);
    pthread_mutex_unlock(&bpl_mu);
}

static void *bpl_member(void *object, size_t offset) {
    return *(void **) ((char *) object + offset);
}
static void *bpl_live_widget(void) {
    if (!on_msg_thread()) return NULL;
    void *w = (void *) wref_get(&bpl_widget);
    if (!w || !bpl_ctrl || *(uintptr_t *) w != BPL_WIDGET_VT ||
        *(uintptr_t *) bpl_ctrl != BPL_CTRL_VT || bpl_member(bpl_ctrl, 0x38) != w)
        return NULL;
    return w;
}
/* -1 invalid/too long; 0 empty; 1 complete. Never truncate UTF-8 or passwords. */
static int bpl_editor_text_valid(const char *text, size_t n) {
    if (n >= 128) return 0;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char) text[i] < 32 || (unsigned char) text[i] == 127) return 0;
    return 1;
}
static int bpl_editor_read(void *editor, char out[128]) {
    if (!editor) return -1;
    bpl_string s = {0};
    bpl_get_text(&s, editor);
    size_t n = s.text ? strnlen(s.text, 128) : 0;
    int ok = bpl_editor_text_valid(s.text, n);
    if (ok) {
        if (n) memcpy(out, s.text, n);
        out[n] = 0;
    } else out[0] = 0;
    ((void (*)(bpl_string *)) BPL_STRING_DTOR)(&s);
    return ok ? (n ? 1 : 0) : -1;
}
static int bpl_read(void *w, struct bp_credentials *value) {
    int u = bpl_editor_read(bpl_member(w, 0xf0), value->user);
    int p = bpl_editor_read(bpl_member(w, 0xf8), value->pass);
    if (u < 0 || p < 0) return -1;
    return u && p ? 1 : (!u && !p ? 0 : -1);
}
static int bpl_is_login(void *w) {
    void *button = bpl_member(w, 0x118);
    return button && *(unsigned *) ((char *) button + 0x1e0) == 0;
}
static void bpl_editor_write(void *editor, const char *text) {
    bpl_string s;
    ((void (*)(bpl_string *, const char *)) BPL_STRING_CTOR)(&s, text);
    ((void (*)(void *, const bpl_string *, int)) BPL_EDITOR_SET)(editor, &s, 1);
    ((void (*)(bpl_string *)) BPL_STRING_DTOR)(&s);
}

static void bpl_ctor_hook(void *self, void *widget) {
    bpl_orig_ctor(self, widget);
    if (!on_msg_thread()) return;
    bpl_ctrl = self;
    ++bpl_screen;
    wref_set(&bpl_widget, (uintptr_t) widget);
    bpl_opened_ms = now_ms();
    bpl_visible = lb_showing((uintptr_t) widget);
    bpl_reset_attempt();
}
static void bpl_retire(void *self) {
    if (self == bpl_ctrl) {
        ++bpl_screen;
        bpl_ctrl = NULL;
        bpl_visible = 0;
        wref_set(&bpl_widget, 0);
        bpl_reset_attempt();
    }
}
static void bpl_dtor_hook(void *self) {
    bpl_retire(self);
    bpl_orig_dtor(self);
}
static void bpl_delete_hook(void *self) {
    bpl_retire(self);
    bpl_orig_delete(self);
}
static void bpl_dtor8_hook(void *self) {
    bpl_retire((char *) self - 8);
    bpl_orig_dtor8(self);
}
static void bpl_delete8_hook(void *self) {
    bpl_retire((char *) self - 8);
    bpl_orig_delete8(self);
}
static void bpl_dtor48_hook(void *self) {
    bpl_retire((char *) self - 0x30);
    bpl_orig_dtor48(self);
}
static void bpl_delete48_hook(void *self) {
    bpl_retire((char *) self - 0x30);
    bpl_orig_delete48(self);
}

static int bpl_remember_enabled(void) {
    return bpl_options.remember && !(bpl_job == BPL_FORGET && bpl_serial != bpl_done);
}
static int bpl_credentials_changed(const struct bp_credentials *value) {
    return !bpl_have || bpl_error || strcmp(value->user, bpl_saved.user) || strcmp(value->pass, bpl_saved.pass);
}
static void bpl_remember_fields(void *widget) {
    pthread_mutex_lock(&bpl_mu);
    uint64_t control = bpl_control;
    int capture = bpl_remember_enabled();
    pthread_mutex_unlock(&bpl_mu);
    struct bp_credentials value = {0};
    if (capture && bpl_read(widget, &value) == 1) {
        pthread_mutex_lock(&bpl_mu);
        if (control == bpl_control && bpl_remember_enabled() && bpl_credentials_changed(&value)) bpl_queue_locked(BPL_SAVE, &value);
        pthread_mutex_unlock(&bpl_mu);
    }
    bp_credentials_clear(&value, sizeof value);
}
static void bpl_dispatch_hook(void *self, void *name) {
    if (on_msg_thread() && atomic_load_explicit(&bpl_active, memory_order_acquire) &&
        name && *(uintptr_t *) name == *(uintptr_t *) BPL_LOGIN_NAME) {
        bpl_cancelled = 1;
        void *widget = bpl_live_widget();
        if (self == bpl_ctrl && widget && lb_showing((uintptr_t) widget) && bpl_is_login(widget)) bpl_remember_fields(widget);
    }
    bpl_orig_dispatch(self, name);
}
static int bpl_auto_control(uint64_t control) {
    return control == bpl_control && bpl_options.auto_login && bpl_have && !bpl_error &&
           bpl_serial == bpl_done && atomic_load_explicit(&bpl_active, memory_order_acquire);
}
static int bpl_auto_current(uint64_t control, uint64_t screen) {
    pthread_mutex_lock(&bpl_mu);
    int ok = bpl_auto_control(control);
    pthread_mutex_unlock(&bpl_mu);
    void *widget = bpl_live_widget();
    return ok && screen == bpl_screen && !bpl_cancelled && widget && lb_showing((uintptr_t) widget);
}

static void bpl_submit(uint64_t control, uint64_t screen) {
    if (pthread_mutex_trylock(&bpl_action_mu)) return;
    /* C cleanup also runs during C++ unwinding (-fexceptions). */
    pthread_mutex_t *action __attribute__((cleanup(bpl_unlock_action))) = &bpl_action_mu;
    if (!bpl_auto_current(control, screen)) return;
    ++bpl_presses;
    bpl_press_ms = now_ms() + 8000;
    bpl_orig_dispatch(bpl_ctrl, (void *) BPL_LOGIN_NAME);
}

struct bpl_attempt {
    struct bp_credentials saved, current;
    az_beatport_login_options options;
    uint64_t control, screen;
};
static int bpl_visible_entry(void *widget) {
    int showing = widget && lb_showing((uintptr_t) widget);
    if (showing != bpl_visible) {
        bpl_visible = showing;
        ++bpl_screen;
        bpl_reset_attempt();
        bpl_opened_ms = now_ms();
    }
    return showing;
}
static int bpl_capture_attempt(struct bpl_attempt *attempt) {
    pthread_mutex_lock(&bpl_mu);
    if (bpl_seen_control != bpl_control) {
        bpl_seen_control = bpl_control;
        bpl_reset_attempt();
    }
    int eligible = bpl_have && !bpl_error && bpl_serial == bpl_done;
    attempt->saved = bpl_saved;
    attempt->options = bpl_options;
    attempt->control = bpl_control;
    attempt->screen = bpl_screen;
    pthread_mutex_unlock(&bpl_mu);
    return eligible;
}
static int bpl_fill_attempt(void *widget, struct bpl_attempt *attempt, long now) {
    if (bpl_filled) return 1;
    int fields = bpl_read(widget, &attempt->current);
    if (!bpl_auto_current(attempt->control, attempt->screen)) return 0;
    if (fields != 0) {
        bpl_cancelled = 1;
        return 0;
    }
    bpl_filled = 1;
    bpl_filled_with = attempt->saved;
    bpl_editor_write(bpl_member(widget, 0xf0), attempt->saved.user);
    /* Native listeners may retire the widget between writes. */
    if (!bpl_auto_current(attempt->control, attempt->screen)) return 0;
    bpl_editor_write(bpl_member(widget, 0xf8), attempt->saved.pass);
    if (!bpl_auto_current(attempt->control, attempt->screen)) return 0;
    bpl_press_ms = now + (long) attempt->options.delay_sec * 1000;
    return 1;
}
static void bpl_attempt_press(struct bpl_attempt *attempt, long now) {
    if (bpl_presses >= attempt->options.max_attempts || now < bpl_press_ms) return;
    void *widget = bpl_live_widget();
    if (!widget || !bpl_is_login(widget) || !*(uintptr_t *) BPL_LOGIN_NAME) return;
    if (bpl_read(widget, &attempt->current) != 1 || strcmp(attempt->current.user, bpl_filled_with.user) ||
        strcmp(attempt->current.pass, bpl_filled_with.pass)) {
        bpl_cancelled = 1;
        return;
    }
    bpl_submit(attempt->control, attempt->screen);
}
static void bpl_auto_attempt(void *widget, struct bpl_attempt *attempt, int eligible) {
    if (!eligible || !attempt->options.auto_login || bpl_cancelled || !widget || !bpl_is_login(widget)) return;
    long now = now_ms();
    if (now - bpl_opened_ms < 400) return;
    if (bpl_fill_attempt(widget, attempt, now)) bpl_attempt_press(attempt, now);
}
static void sdk_beatport_msg_tick(void) {
    if (!bpl_ready || !on_msg_thread()) return;
    if (!atomic_load_explicit(&bpl_active, memory_order_acquire)) {
        bpl_reset_attempt();
        bpl_visible = 0;
        return;
    }
    /* Construction occurs offscreen; each visible entry starts its own delay. */
    void *widget = bpl_live_widget();
    if (!bpl_visible_entry(widget)) return;
    struct bpl_attempt attempt = {0};
    int eligible = bpl_capture_attempt(&attempt);
    bpl_auto_attempt(widget, &attempt, eligible);
    bp_credentials_clear(&attempt.saved, sizeof attempt.saved);
    bp_credentials_clear(&attempt.current, sizeof attempt.current);
}

/* Native hooks go in before EP147 starts. The secondary-base destructors are
 * independently compiled bodies, not branches through the primary entries.
 * All six must be tracked before retaining any controller. */
static int bpl_install_destructors(void) {
    struct {
        uintptr_t address;
        void (*hook)(void *);
        void (**original)(void *);
    } entries[] = {
        {BPL_DTOR, bpl_dtor_hook, &bpl_orig_dtor}, {BPL_DELETE, bpl_delete_hook, &bpl_orig_delete}, {BPL_DTOR8, bpl_dtor8_hook, &bpl_orig_dtor8}, {BPL_DELETE8, bpl_delete8_hook, &bpl_orig_delete8}, {BPL_DTOR48, bpl_dtor48_hook, &bpl_orig_dtor48}, {BPL_DELETE48, bpl_delete48_hook, &bpl_orig_delete48}};
    int complete = 1;
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++)
        if (azm_install_hook(entries[i].address, (void *) entries[i].hook, (void **) entries[i].original) != 1) complete = 0;
    return complete;
}
static void bpl_retire_cipher_segment(void) {
    key_t key = ftok("/etc", 0x14);
    struct shmid_ds ds;
    int shmid = key == (key_t) -1 ? -1 : shmget(key, 0, 0);
    if (shmid >= 0 && shmctl(shmid, IPC_STAT, &ds) == 0 && ds.shm_nattch == 0 &&
        ds.shm_segsz == 0x340 && ds.shm_perm.mode == 0600) (void) shmctl(shmid, IPC_RMID, NULL);
}
static void sdk_beatport_install(void) {
    if (g_ep147_build != EPB_V200 || !g_compdtor_ok ||
        !wref_track(&bpl_widget, 1, 0) || !install_msgtick_hook("Beatport login")) return;
    if (!bpl_install_destructors()) return;
    int ctor = azm_install_hook(BPL_CTOR, (void *) bpl_ctor_hook, (void **) &bpl_orig_ctor);
    int dispatch = azm_install_hook(BPL_DISPATCH, (void *) bpl_dispatch_hook, (void **) &bpl_orig_dispatch);
    bpl_ready = ctor == 1 && dispatch == 1;
    bpl_retire_cipher_segment();
}
