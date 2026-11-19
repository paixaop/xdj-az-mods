/* SPDX-License-Identifier: GPL-3.0-only */
/* Host adapter: authenticate the actual external call before forwarding it.
 * No credential, worker, native-controller, or request state lives here. */
#include "beatport_service.h"
#include "beatport_host.h"
static int bpl_caller(const char *id, uintptr_t caller, const char *operation) {
    if (!g_cm_on_ui) return -1;
    cm_mod *c = cm_self(id, caller, operation);
    if (!c) return -1;
    int slot = (int) (c - g_cm);
    return c->state == AZCM_LOADED || (g_sdk_in_init && g_cm_loading == slot) ? slot : -1;
}
static int bpl_configure(const char *id, const az_beatport_login_options *options) {
    int owner = bpl_caller(id, CM_RA(), "beatport.configure");
    return bp_login_configure(owner, g_sdk_in_init && g_cm_loading == owner, options);
}
static int bpl_status(const char *id, az_beatport_login_status *out) {
    return bp_login_status(bpl_caller(id, CM_RA(), "beatport.status"), out);
}
static int bpl_forget(const char *id) {
    return bp_login_forget(bpl_caller(id, CM_RA(), "beatport.forget"));
}
static const az_beatport_login_v1 bpl_api = {
    AZ_BEATPORT_LOGIN_ABI, sizeof(az_beatport_login_v1), bpl_configure, bpl_status, bpl_forget};
static const az_beatport_login_v1 *sdk_beatport_login_service(uint32_t abi) {
    return bp_login_ready() && abi == AZ_BEATPORT_LOGIN_ABI ? &bpl_api : NULL;
}
static void sdk_beatport_owner_tick(void) {
    int owner = bp_login_owner();
    bp_login_set_active(owner >= 0 && owner < g_cm_n && g_cm[owner].state == AZCM_LOADED);
}
static void sdk_beatport_install(void) {
    bp_login_install(g_ep147_build == EPB_V200 && g_compdtor_ok, CM_STATE "/beatport-remember.dat");
}
int bp_host_thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *user) {
    return az_thread_create(thread, attr, fn, user);
}
int bp_host_message_thread(void) { return on_msg_thread(); }
long bp_host_now_ms(void) { return now_ms(); }
uintptr_t bp_host_weak_get(_Atomic uintptr_t *slot) { return wref_get(slot); }
int bp_host_weak_set(_Atomic uintptr_t *slot, uintptr_t value) { return wref_set(slot, value); }
int bp_host_weak_track(_Atomic uintptr_t *slot, int count, int offset) { return wref_track(slot, count, offset); }
int bp_host_showing(uintptr_t widget) { return lb_showing(widget); }
int bp_host_install_tick(const char *name) { return install_msgtick_hook(name); }
int bp_host_install_hook(uintptr_t target, void *hook, void **original) {
    return azm_install_hook(target, hook, original);
}
