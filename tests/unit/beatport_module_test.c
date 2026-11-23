/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "beatport_service.h"
#include "beatport_host.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <unistd.h>

static int message_thread, allow_track, allow_tick, allow_hook, thread_error;
static unsigned hooks, threads;
int bp_host_message_thread(void) { return message_thread; }
long bp_host_now_ms(void) { return 0; }
uintptr_t bp_host_weak_get(_Atomic uintptr_t *slot) { return atomic_load(slot); }
int bp_host_weak_set(_Atomic uintptr_t *slot, uintptr_t value) {
    atomic_store(slot, value);
    return 1;
}
int bp_host_weak_track(_Atomic uintptr_t *slot, int count, int offset) {
    assert(slot && count == 1 && !offset);
    return allow_track;
}
int bp_host_showing(uintptr_t widget) {
    (void) widget;
    return 0;
}
int bp_host_install_tick(const char *name) {
    assert(name);
    return allow_tick;
}
int bp_host_install_hook(uintptr_t target, void *hook, void **original) {
    assert(target && hook && original);
    ++hooks;
    *original = (void *) target;
    return allow_hook;
}
int bp_host_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *user) {
    ++threads;
    return thread_error ? thread_error : pthread_create(thread, attr, fn, user);
}
key_t ftok(const char *path, int id) {
    (void) path;
    (void) id;
    return (key_t) -1;
}
static az_beatport_login_status await_storage(void) {
    for (unsigned i = 0; i < 1000; ++i) {
        az_beatport_login_status status = {.size = sizeof status};
        assert(bp_login_status(0, &status) == 0);
        if (!status.save_pending) return status;
        usleep(1000);
    }
    assert(!"credential storage failed to finish");
    return (az_beatport_login_status) {0};
}
static void installation(const char *path) {
    bp_login_install(0, path);
    bp_login_install(1, NULL);
    assert(!hooks && !bp_login_ready());
    bp_login_install(1, path);
    assert(!hooks);
    allow_track = 1;
    bp_login_install(1, path);
    assert(!hooks);
    allow_tick = 1;
    bp_login_install(1, path);
    assert(hooks == 6 && !bp_login_ready());
    allow_hook = 1;
    bp_login_install(1, path);
    assert(hooks == 14 && bp_login_ready());
}
int main(int argc, char **argv) {
    assert(argc == 2);
    installation(argv[1]);
    az_beatport_login_options options = {.size = sizeof options, .remember = 1, .max_attempts = 1};
    assert(bp_login_owner() == -1);
    assert(bp_login_configure(-1, 1, &options) == -1);
    assert(bp_login_configure(0, 0, &options) == -1);
    assert(bp_login_configure(0, 1, NULL) == -1);
    thread_error = EAGAIN;
    assert(bp_login_configure(0, 1, &options) == -1 && threads == 1);
    thread_error = 0;
    assert(bp_login_configure(0, 1, &options) == 0 && threads == 2);
    assert(bp_login_owner() == 0);
    assert(bp_login_configure(1, 1, &options) == -1);
    assert(bp_login_forget(-1) == -1 && bp_login_forget(1) == -1);
    az_beatport_login_status status = await_storage();
    assert(!status.have_credentials && !status.storage_error);
    assert(bp_login_status(1, &status) == -1 && bp_login_status(0, NULL) == -1);
    bp_login_set_active(1);
    bp_login_message_tick();
    message_thread = 1;
    bp_login_message_tick(); /* no retained widget; no firmware dereference */
    bp_login_set_active(0);
    bp_login_message_tick();
    assert(bp_login_forget(0) == 0);
    status = await_storage();
    assert(!status.have_credentials && !status.storage_error);
    puts("Beatport independent module: startup, admission, storage, and inactivity passed");
}
