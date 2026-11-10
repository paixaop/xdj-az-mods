/* SPDX-License-Identifier: GPL-3.0-only */
/* Included in the community host. Status has its own UI signal, including
 * when track tickets and all command services are disabled. */
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <poll.h>
#include "sdk_status_engine.c"

static int ds_event_fd = -1, ds_timer_fd = -1;
static void ds_wake(void) {
    uint64_t one = 1;
    if (ds_event_fd >= 0) {
        ssize_t n;
        do n = write(ds_event_fd, &one, sizeof one);
        while (n < 0 && errno == EINTR);
        /* EAGAIN means the descriptor is already readable. */
    }
}
static void ds_deadline(uint64_t ns) {
    struct itimerspec timer = {0};
    timer.it_value.tv_sec = ns / UINT64_C(1000000000);
    timer.it_value.tv_nsec = ns % UINT64_C(1000000000);
    if (ds_timer_fd >= 0) (void) timerfd_settime(ds_timer_fd, TFD_TIMER_ABSTIME, &timer, NULL);
}
static int ds_live(int owner) {
    return owner >= 0 && owner < g_cm_n && g_cm[owner].state == AZCM_LOADED;
}
static void sdk_status_ui_drain(void) {
    if (!ds.enabled) return;
    uint64_t value;
    ssize_t event_read = read(ds_event_fd, &value, sizeof value);
    ssize_t timer_read = read(ds_timer_fd, &value, sizeof value);
    if (event_read < 0 && timer_read < 0) return;
    ds_drain(8);
}
#ifndef SDK_PROC_IMPL
#include "sdk_status_store.c"
#include "sdk_status_native.c"
#include "sdk_status_gui.c"
#include "sdk_status_metrics.c"
#else
/* The normal-loader fixture has no EP147 units or libraries. Inject receipts
 * at the reducer boundary; all registration, pinning and UI delivery is real. */
static void ds_gui_interest(uint64_t topics) { (void) topics; }
static void ds_native_start(void) {}
static void ds_start_metrics(void) {}
static void ds_native_interest(unsigned d, uint64_t t) {
    (void) d;
    (void) t;
}
static void sdk_status_store_interest(unsigned d, uint64_t t) {
    (void) d;
    (void) t;
}
static void sdk_status_store_binding(unsigned d, uint64_t e, const az_track_ref *r, uint64_t c) {
    (void) d;
    (void) e;
    (void) r;
    (void) c;
}
#endif
static void sdk_status_start(void) {
    if (ds.enabled || access(CM_ROOT "/azmod.deck-status.off", F_OK) == 0) return;
#ifndef SDK_PROC_IMPL
    if (g_ep147_build != EPB_V130 && g_ep147_build != EPB_V200) return;
#endif
    ds_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ds_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ds_event_fd < 0 || ds_timer_fd < 0) {
        if (ds_event_fd >= 0) close(ds_event_fd);
        if (ds_timer_fd >= 0) close(ds_timer_fd);
        ds_event_fd = ds_timer_fd = -1;
        return;
    }
    ds.enabled = 1;
    ds_native_start();
    ds_start_metrics();
}
static int sdk_status_code(cm_mod *mod, uintptr_t address) {
    if (!cm_called_by(mod, address)) return 0;
    for (unsigned i = 0; i < mod->nsegments; ++i) {
        const ElfW(Phdr) *ph = &mod->segments[i];
        uintptr_t start = mod->image_base + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) && address >= start &&
            address - start < ph->p_memsz) return 1;
    }
    return 0;
}
static int sdk_status_owner(const char *id, uintptr_t caller) {
    if (!g_cm_on_ui || g_cm_in_frame) return -1;
    cm_mod *c = cm_self(id, caller, "deck status");
    if (!c || !sdk_status_code(c, caller)) return -1;
    int owner = (int) (c - g_cm);
    return (ds_live(owner) || (g_sdk_in_init && owner == g_cm_loading)) ? owner : -1;
}
static int sdk_on_deck_status(const char *id, const az_deck_watch_v1 *watch, az_deck_status_fn fn,
                              void *user, az_deck_subscription *out) {
    int owner = sdk_status_owner(id, CM_RA());
    if (owner < 0 || !sdk_status_code(&g_cm[owner], (uintptr_t) fn)) return AZ_INVALID;
    return ds_register(owner, watch, fn, user, out);
}
static int sdk_deck_status_release(const char *id, az_deck_subscription handle) {
    return ds_release(sdk_status_owner(id, CM_RA()), handle);
}
