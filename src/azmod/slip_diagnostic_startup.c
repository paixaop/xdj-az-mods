/* SPDX-License-Identifier: GPL-3.0-only */
/* Startup adapter: native hooks are core-owned even when no community mod is
 * enabled. The explicit local diagnostic switch is off in normal operation. */
#include "slip_diagnostic/diagnostic.h"
static void slip_diagnostic_log(const char *format, ...) {
    FILE *file = azlog_open("/tmp/azmod-slip-capture.log", "a");
    if (!file) return;
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}
static int slip_diagnostic_worker(void *(*run)(void *), void *user) {
    pthread_attr_t attributes;
    int rc = pthread_attr_init(&attributes);
    if (rc) return rc;
    rc = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t worker;
    if (!rc) rc = az_thread_create(&worker, &attributes, run, user);
    pthread_attr_destroy(&attributes);
    return rc;
}
static int slip_diagnostic_install(uintptr_t address, void *replacement, void **original) {
    return azm_install_hook_at(address, replacement, original, "slip-diagnostic") == 1 ? 0 : -1;
}
static void slip_diagnostic_at_start(void) {
    if (access("/home/root/settings/azmod.slip-capture.on", F_OK) != 0) return;
    static const struct az_slip_diagnostic_host host = {
        slip_diagnostic_log, slip_diagnostic_worker, slip_diagnostic_install
    };
    const char *tag = "";
    if (g_ep147_build == EPB_V130) tag = "ep147-79d43e5de779fc1b";
    if (g_ep147_build == EPB_V200) tag = "ep147-9e9454e357389ece";
    if (az_slip_diagnostic_start(tag, &host)) slip_diagnostic_log("SDK_SLIP_CAPTURE_FAILED v=1 reason=binding_or_install");
}
