/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include "../../src/azmod/azmod-mod.h"

typedef struct {
    int state;
} cm_mod;
enum {
    AZCM_LOADED = 1,
    EPB_V200 = 2
};
static cm_mod g_cm[2] = {{AZCM_LOADED}, {AZCM_LOADED}};
static int g_cm_n = 2, g_cm_loading, g_sdk_in_init = 1;
static _Thread_local int g_cm_on_ui = 1;
static int g_ep147_build = EPB_V200, g_compdtor_ok = 1;
static cm_mod *cm_self(const char *id, uintptr_t ra, const char *op) {
    (void) ra;
    (void) op;
    return id && !strcmp(id, "beatport-remember") ? &g_cm[0] : id && !strcmp(id, "other") ? &g_cm[1]
                                                                                          : NULL;
}
#define CM_RA() ((uintptr_t) 0)
static int message_thread = 1;
static long fake_now;
static int on_msg_thread(void) { return message_thread; }
static int widget_showing = 1;
static int lb_showing(uintptr_t widget) { return message_thread && widget && widget_showing; }
static long now_ms(void) { return fake_now; }
static uintptr_t wref_get(_Atomic uintptr_t *slot) { return atomic_load(slot); }
static int wref_set(_Atomic uintptr_t *slot, uintptr_t value) {
    atomic_store(slot, value);
    return 1;
}
static int wref_track(_Atomic uintptr_t *slot, int n, int offset) {
    assert(slot && n == 1 && offset == 0);
    return 1;
}
static int install_msgtick_hook(const char *who) {
    assert(who);
    return 1;
}
static int installs;
static int fake_install(uintptr_t target, void *hook, void **original) {
    assert(target && hook && original);
    *original = (void *) target;
    ++installs;
    return 1;
}
#define azm_install_hook(target, hook, original) fake_install((target), (hook), (original))
static int az_thread_create(pthread_t *t, const pthread_attr_t *a,
                            void *(*f)(void *), void *arg) {
    return pthread_create(t, a, f, arg);
}

/* Aligned storage with the actual offsets consumed by the service. */
union native_object {
    uintptr_t alignment;
    unsigned char bytes[0x240];
};
static union native_object controller, widget, button;
static char user_editor[256], pass_editor[256];
static uintptr_t widget_vtable[1], controller_vtable[1];
static uintptr_t login_name = 0x101010, mask_name = 0x202020;
static int reads, writes, dispatches, ctor_calls, dtor_calls, delete_calls;
static int secondary_destructor_calls[4];
static int retire_after_first_write, replace_after_first_write, read_injection, write_injection;
static _Atomic int fail_write, pause_write, write_entered, release_write;
static char storage_dir[256], storage_path[300];
static void fake_ctor(void *, void *);
static void fake_dtor(void *);
static void fake_delete(void *);
static void fake_dtor8(void *);
static void fake_delete8(void *);
static void fake_dtor48(void *);
static void fake_delete48(void *);
static void fake_dispatch(void *, void *);
/* Bind after the private bpl_string type and typed stand-ins are defined. */
static uintptr_t string_ctor_address, string_dtor_address, editor_set_address;
static key_t no_shared_memory(const char *path, int id) {
    (void) path;
    (void) id;
    return -1;
}
static ssize_t storage_write(int fd, const void *buf, size_t len) {
    if (atomic_load(&fail_write)) {
        errno = ENOSPC;
        return -1;
    }
    if (atomic_load(&pause_write)) {
        atomic_store(&write_entered, 1);
        while (!atomic_load(&release_write)) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    return write(fd, buf, len);
}
#define BP_CRED_WRITE storage_write
#define BPL_TEST
#define BPL_PATH storage_path
#define BPL_CTOR ((uintptr_t) fake_ctor)
#define BPL_DISPATCH ((uintptr_t) fake_dispatch)
#define BPL_DTOR ((uintptr_t) fake_dtor)
#define BPL_DELETE ((uintptr_t) fake_delete)
#define BPL_DTOR8 ((uintptr_t) fake_dtor8)
#define BPL_DELETE8 ((uintptr_t) fake_delete8)
#define BPL_DTOR48 ((uintptr_t) fake_dtor48)
#define BPL_DELETE48 ((uintptr_t) fake_delete48)
#define BPL_WIDGET_VT ((uintptr_t) widget_vtable)
#define BPL_CTRL_VT ((uintptr_t) controller_vtable)
#define BPL_LOGIN_NAME ((uintptr_t) &login_name)
#define BPL_STRING_CTOR string_ctor_address
#define BPL_STRING_DTOR string_dtor_address
#define BPL_EDITOR_SET editor_set_address
#define ftok no_shared_memory
#include "../../src/azmod/beatport_service.c"
#define CM_STATE "/unused-settings"
#include "../../src/azmod/sdk_beatport.c"
#undef ftok

static void set_member(union native_object *object, size_t offset, void *member) {
    memcpy(object->bytes + offset, &member, sizeof member);
}
static void fake_ctor(void *self, void *w) {
    ++ctor_calls;
    *(uintptr_t *) self = BPL_CTRL_VT;
    set_member(self, 0x38, w);
}
static void fake_dtor(void *self) {
    assert(bpl_ctrl != self);
    ++dtor_calls;
}
static void fake_delete(void *self) {
    assert(bpl_ctrl != self);
    ++delete_calls;
}
static void secondary_destructor(void *self, size_t offset, int slot) {
    assert(self == controller.bytes + offset); /* the original receives its base pointer */
    assert(!bpl_ctrl && !wref_get(&bpl_widget));
    ++secondary_destructor_calls[slot];
}
static void fake_dtor8(void *self) { secondary_destructor(self, 8, 0); }
static void fake_delete8(void *self) { secondary_destructor(self, 8, 1); }
static void fake_dtor48(void *self) { secondary_destructor(self, 0x30, 2); }
static void fake_delete48(void *self) { secondary_destructor(self, 0x30, 3); }
static void fake_dispatch(void *self, void *name) {
    assert(self && name);
    ++dispatches;
}
static void fake_string_ctor(bpl_string *out, const char *text) {
    assert(message_thread);
    out->text = text;
}
static void fake_string_dtor(bpl_string *s) {
    (void) s;
    assert(message_thread);
}
static void fake_editor_set(void *editor, const bpl_string *value, int notify) {
    assert(message_thread && notify == 1);
    ++writes;
    assert(strlen(value->text) < sizeof user_editor);
    strcpy(editor, value->text);
    if (write_injection) {
        write_injection = 0;
        assert(bpl_forget("beatport-remember") == 0);
    }
    if (retire_after_first_write && writes == 1)
        wref_set(&bpl_widget, 0);
    if (replace_after_first_write && writes == 1)
        bpl_ctor_hook(&controller, &widget);
}
void bpl_get_text(bpl_string *out, void *editor) {
    assert(message_thread);
    ++reads;
    out->text = editor;
    int injection = read_injection;
    read_injection = 0;
    if (injection == 1)
        assert(bpl_forget("beatport-remember") == 0);
    if (injection == 2) {
        az_beatport_login_options options = bpl_options;
        options.auto_login = 0;
        assert(bpl_configure("beatport-remember", &options) == 0);
    }
}
static az_beatport_login_options options(int auto_login, int remember, int delay) {
    return (az_beatport_login_options) {sizeof(az_beatport_login_options),
                                        auto_login, remember, delay, 1};
}
static void open_screen(void) {
    memset(&controller, 0, sizeof controller);
    memset(&widget, 0, sizeof widget);
    memset(&button, 0, sizeof button);
    user_editor[0] = pass_editor[0] = 0;
    *(uintptr_t *) &widget = BPL_WIDGET_VT;
    set_member(&widget, 0xf0, user_editor);
    set_member(&widget, 0xf8, pass_editor);
    set_member(&widget, 0x118, &button);
    bpl_ctor_hook(&controller, &widget);
}
static void reset_state(void) {
    assert(!bpl_worker_started);
    pthread_mutex_lock(&bpl_mu);
    bpl_owner = 0;
    bpl_have = bpl_error = bpl_job = 0;
    bpl_serial = bpl_done = bpl_control = bpl_seen_control = 0;
    memset(&bpl_saved, 0, sizeof bpl_saved);
    memset(&bpl_candidate, 0, sizeof bpl_candidate);
    bpl_options = options(1, 1, 1);
    pthread_mutex_unlock(&bpl_mu);
    message_thread = g_cm_on_ui = 1;
    g_sdk_in_init = 1;
    g_cm[0].state = AZCM_LOADED;
    atomic_store(&bpl_active, 1);
    reads = writes = dispatches = 0;
    retire_after_first_write = replace_after_first_write = read_injection = write_injection = 0;
    bpl_ctrl = NULL;
    wref_set(&bpl_widget, 0);
    bpl_reset_attempt();
    fake_now = 1000;
    widget_showing = 1;
    open_screen();
}
static void seed_saved(void) {
    bpl_saved = (struct bp_credentials) {"saved@example.invalid", "saved password"};
    bpl_have = 1;
}
static void fill_fields(void) {
    fake_now += 500;
    bp_login_message_tick();
}
static void test_native_state(void) {
    /* A controller can be constructed minutes before its screen is opened.
     * Hidden widgets must not be filled or consume an automatic attempt. */
    reset_state();
    seed_saved();
    widget_showing = 0;
    bp_login_message_tick();
    fake_now += 120000;
    bp_login_message_tick();
    assert(!writes && !dispatches && !bpl_filled && !bpl_presses);
    widget_showing = 1;
    bp_login_message_tick();
    fake_now += 399;
    bp_login_message_tick();
    assert(!writes);
    fake_now += 1;
    bp_login_message_tick();
    assert(writes == 2 && !dispatches);
    widget_showing = 0;
    bp_login_message_tick();
    user_editor[0] = pass_editor[0] = 0; /* native screen reopening resets fields */
    widget_showing = 1;
    bp_login_message_tick();
    fake_now += 400;
    bp_login_message_tick();
    assert(writes == 4 && !bpl_cancelled && !dispatches);

    reset_state();
    seed_saved();
    message_thread = 0;
    fake_now += 5000;
    sdk_beatport_owner_tick();
    bp_login_message_tick();
    bpl_dispatch_hook(&controller, &login_name);
    assert(reads == 0 && writes == 0 && bpl_serial == 0);
    assert(!bpl_filled);
    union native_object other_controller;
    bpl_ctor_hook(&other_controller, &widget);
    assert(bpl_ctrl == &controller); /* background construction is not cached */
    message_thread = 1;
    bp_login_message_tick();
    assert(writes == 2 && !strcmp(user_editor, bpl_saved.user));
    assert(!strcmp(pass_editor, bpl_saved.pass));
    int before = dispatches;
    fake_now += 1001;
    bp_login_message_tick();
    bp_login_message_tick();
    assert(dispatches == before + 1); /* one attempt per screen */

    reset_state();
    seed_saved();
    fake_now += 399;
    bp_login_message_tick();
    assert(writes == 0);
    fake_now += 1;
    bp_login_message_tick();
    assert(writes == 2);
    strcpy(pass_editor, "changed by the DJ");
    fake_now += 2000;
    bp_login_message_tick();
    assert(bpl_cancelled && dispatches == 0);
    strcpy(pass_editor, bpl_saved.pass);
    fake_now += 9000;
    bp_login_message_tick();
    assert(dispatches == 0);

    reset_state();
    seed_saved();
    strcpy(user_editor, "partial typing");
    fill_fields();
    assert(writes == 0 && bpl_cancelled);
    reset_state();
    seed_saved();
    retire_after_first_write = 1;
    fill_fields();
    assert(writes == 1 && dispatches == 0);
    assert(pass_editor[0] == 0);

    /* Reusing the same addresses must not confuse the screen generation check. */
    reset_state();
    seed_saved();
    replace_after_first_write = 1;
    fill_fields();
    assert(writes == 1 && !pass_editor[0] && dispatches == 0);

    reset_state();
    seed_saved();
    bpl_dtor_hook(&controller);
    fill_fields();
    assert(!bpl_ctrl && !wref_get(&bpl_widget) && writes == 0);
    open_screen();
    bpl_delete_hook(&controller);
    fill_fields();
    assert(!bpl_ctrl && !wref_get(&bpl_widget) && writes == 0);
    open_screen();
    wref_set(&bpl_widget, 0); /* component retired before controller */
    fill_fields();
    assert(writes == 0);

    void (*secondary_hooks[])(void *) = {
        bpl_dtor8_hook, bpl_delete8_hook, bpl_dtor48_hook, bpl_delete48_hook};
    for (int i = 0; i < 4; ++i) {
        reset_state();
        seed_saved();
        fill_fields();
        assert(writes == 2);
        secondary_hooks[i](controller.bytes + (i < 2 ? 8 : 0x30));
        fake_now += 10000;
        bp_login_message_tick();
        assert(!bpl_ctrl && !wref_get(&bpl_widget) && writes == 2 && !dispatches);
        assert(secondary_destructor_calls[i] == 1);
    }

    reset_state();
    strcpy(user_editor, "manual@example.invalid");
    strcpy(pass_editor, "manual password");
    bpl_dispatch_hook(&controller, &mask_name);
    assert(reads == 0 && bpl_serial == 0 && !bpl_cancelled);
    *(unsigned *) (button.bytes + 0x1e0) = 1;
    bpl_dispatch_hook(&controller, &login_name);
    assert(reads == 0 && bpl_serial == 0);
    *(unsigned *) (button.bytes + 0x1e0) = 0;
    bpl_dispatch_hook(&controller, &login_name);
    assert(reads == 2 && bpl_serial == 1 && bpl_job == BPL_SAVE);
    assert(!strcmp(bpl_candidate.user, user_editor));
    assert(!strcmp(bpl_candidate.pass, pass_editor));
    reset_state();
    strcpy(user_editor, "manual@example.invalid");
    memset(pass_editor, 'p', 128);
    pass_editor[128] = 0;
    bpl_dispatch_hook(&controller, &login_name);
    assert(bpl_serial == 0);
    pass_editor[127] = 0;
    bpl_dispatch_hook(&controller, &login_name);
    assert(bpl_serial == 1 && strlen(bpl_candidate.pass) == 127);

    reset_state();
    seed_saved();
    g_cm[0].state = 0;
    sdk_beatport_owner_tick();
    fill_fields();
    assert(!atomic_load(&bpl_active) && writes == 0);
}

static void test_accepted_cancellation(void) {
    /* A forget accepted while native getters run must win over capture. */
    reset_state();
    strcpy(user_editor, "manual@example.invalid");
    strcpy(pass_editor, "manual password");
    read_injection = 1;
    bpl_dispatch_hook(&controller, &login_name);
    assert(bpl_job == BPL_FORGET && bpl_serial == 1 && !bpl_candidate.user[0]);
    bpl_dispatch_hook(&controller, &login_name);
    assert(bpl_job == BPL_FORGET && bpl_serial == 1);

    reset_state();
    seed_saved();
    read_injection = 1;
    fill_fields();
    assert(writes == 0 && dispatches == 0 && bpl_job == BPL_FORGET);

    /* setText listeners can accept forget between the two editor updates. */
    reset_state();
    seed_saved();
    write_injection = 1;
    fill_fields();
    assert(writes == 1 && !pass_editor[0] && dispatches == 0);
    assert(bpl_job == BPL_FORGET);

    /* Changing Auto Login during the final equality check prevents submission. */
    reset_state();
    seed_saved();
    fill_fields();
    assert(writes == 2);
    bpl_worker_started = 1; /* only this deterministic configure uses no real worker */
    read_injection = 2;
    fake_now += 2000;
    bp_login_message_tick();
    bpl_worker_started = 0;
    assert(dispatches == 0 && !bpl_options.auto_login);
    fake_now += 10000;
    bp_login_message_tick();
    assert(dispatches == 0);

    reset_state();
    az_beatport_login_options opt = options(1, 1, 1);
    pthread_mutex_lock(&bpl_action_mu);
    assert(bpl_configure("beatport-remember", &opt) == -1);
    assert(bpl_forget("beatport-remember") == -1);
    pthread_mutex_unlock(&bpl_action_mu);
    assert(!bpl_serial && !bpl_control && !bpl_worker_started);
    g_cm_on_ui = 0;
    assert(bpl_configure("beatport-remember", &opt) == -1);
    assert(bpl_forget("beatport-remember") == -1);
    g_cm_on_ui = 1;
    g_sdk_in_init = 0;
    g_cm[0].state = 0;
    assert(bpl_configure("beatport-remember", &opt) == -1);
    assert(bpl_forget("beatport-remember") == -1);
}

static az_beatport_login_status wait_storage(void) {
    az_beatport_login_status status = {.size = sizeof status};
    for (int i = 0; i < 10000; ++i) {
        assert(bpl_status("beatport-remember", &status) == 0);
        if (!status.save_pending)
            return status;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    assert(!"storage worker did not finish");
    return status;
}

static void test_storage_worker(void) {
    reset_state();
    struct bp_credentials first = {"initial@example.invalid", "initial password"};
    assert(bp_credentials_save(storage_path, &first) == 0);
    bpl_owner = -1;
    az_beatport_login_options opt = options(1, 1, 1);
    assert(bpl_configure("unknown", &opt) == -1);
    g_sdk_in_init = 0;
    assert(bpl_configure("beatport-remember", &opt) == -1);
    g_sdk_in_init = 1;
    assert(bpl_configure("beatport-remember", &opt) == 0);
    az_beatport_login_status status = wait_storage();
    assert(status.have_credentials && !status.storage_error);
    assert(!strcmp(status.username, first.user));
    assert(bpl_configure("other", &opt) == -1);
    assert(bpl_forget("other") == -1);
    status.size = 0;
    assert(bpl_status("beatport-remember", &status) == -1);
    opt.max_attempts = 0;
    assert(bpl_configure("beatport-remember", &opt) == -1);
    opt.max_attempts = 1;
    strcpy(user_editor, "new@example.invalid");
    strcpy(pass_editor, "new password");
    bpl_dispatch_hook(&controller, &login_name);
    status = wait_storage();
    assert(status.have_credentials && !status.storage_error);
    struct bp_credentials persisted;
    assert(bp_credentials_load(storage_path, &persisted) == 1);
    assert(!strcmp(persisted.user, user_editor) && !strcmp(persisted.pass, pass_editor));
    atomic_store(&fail_write, 1);
    strcpy(pass_editor, "not persisted yet");
    bpl_dispatch_hook(&controller, &login_name);
    status = wait_storage();
    assert(status.storage_error == ENOSPC && status.have_credentials);
    assert(bp_credentials_load(storage_path, &persisted) == 1);
    assert(!strcmp(persisted.pass, "new password"));
    atomic_store(&fail_write, 0);
    bpl_dispatch_hook(&controller, &login_name);
    status = wait_storage();
    assert(!status.storage_error && status.have_credentials);
    assert(bp_credentials_load(storage_path, &persisted) == 1);
    assert(!strcmp(persisted.pass, "not persisted yet"));

    /* B can commit after C supersedes its request. If C then fails, both the
     * status and saved snapshot must report B, which is what is on disk. */
    atomic_store(&pause_write, 1);
    atomic_store(&write_entered, 0);
    atomic_store(&release_write, 0);
    strcpy(user_editor, "committed@example.invalid");
    strcpy(pass_editor, "committed password");
    bpl_dispatch_hook(&controller, &login_name);
    for (int i = 0; i < 10000 && !atomic_load(&write_entered); ++i) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    assert(atomic_load(&write_entered));
    strcpy(user_editor, "newer@example.invalid");
    strcpy(pass_editor, "newer write fails");
    bpl_dispatch_hook(&controller, &login_name);
    atomic_store(&fail_write, 1); /* B already passed this check before blocking */
    atomic_store(&release_write, 1);
    status = wait_storage();
    assert(status.have_credentials && status.storage_error == ENOSPC);
    assert(!strcmp(status.username, "committed@example.invalid"));
    assert(bp_credentials_load(storage_path, &persisted) == 1);
    assert(!strcmp(persisted.user, status.username));
    assert(!strcmp(persisted.pass, "committed password"));
    pthread_mutex_lock(&bpl_mu);
    assert(!strcmp(bpl_saved.user, persisted.user) && !strcmp(bpl_saved.pass, persisted.pass));
    pthread_mutex_unlock(&bpl_mu);
    atomic_store(&fail_write, 0);

    /* Forget wins even if a previous save is currently blocked in write(). */
    atomic_store(&pause_write, 1);
    atomic_store(&write_entered, 0);
    atomic_store(&release_write, 0);
    strcpy(pass_editor, "save in progress");
    bpl_dispatch_hook(&controller, &login_name);
    for (int i = 0; i < 10000 && !atomic_load(&write_entered); ++i) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    assert(atomic_load(&write_entered));
    assert(bpl_forget("beatport-remember") == 0);
    bpl_dispatch_hook(&controller, &login_name);
    atomic_store(&release_write, 1);
    status = wait_storage();
    assert(!status.have_credentials && !status.storage_error && !status.username[0]);
    assert(bp_credentials_load(storage_path, &persisted) == 0);
}

int main(void) {
    string_ctor_address = (uintptr_t) fake_string_ctor;
    string_dtor_address = (uintptr_t) fake_string_dtor;
    editor_set_address = (uintptr_t) fake_editor_set;
    strcpy(storage_dir, "/tmp/sdk-beatport.XXXXXX");
    assert(mkdtemp(storage_dir));
    assert(snprintf(storage_path, sizeof storage_path, "%s/login.dat", storage_dir) > 0);
    g_ep147_build = 0;
    sdk_beatport_install();
    assert(!sdk_beatport_login_service(AZ_BEATPORT_LOGIN_ABI) && installs == 0);
    g_ep147_build = EPB_V200;
    sdk_beatport_install();
    assert(sdk_beatport_login_service(AZ_BEATPORT_LOGIN_ABI) == &bpl_api && installs == 8);
    assert(!sdk_beatport_login_service(999));
    test_native_state();
    test_accepted_cancellation();
    test_storage_worker();
    assert(rmdir(storage_dir) == 0);
    assert(ctor_calls && dtor_calls && delete_calls);
    puts("Beatport native service and storage worker: passed");
    return 0;
}
