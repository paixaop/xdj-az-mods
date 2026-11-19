/* SPDX-License-Identifier: GPL-3.0-only */
/* Included after the real island installer and source configuration state.
 * Startup-only opt-in dependency; no source/load capability is advertised.
 * The canonical C bridge validates/pins the exact loaded module before the
 * first redirect and retains close retirement independently of diagnostics.
 */
#include "../common/azsources.h"
#include "../common/azconnectionhooks.h"

static int sdk_connections_install_open(uintptr_t entry, az_native_sql_open replacement, az_native_sql_open *original,
                                        void *context) {
    (void) context;
    return azm_install_island_hook_at(entry, (void *) replacement, (void **) original, "sdk-connection");
}
static int sdk_connections_install_close(uintptr_t entry, az_native_sql_close replacement, az_native_sql_close *original,
                                         void *context) {
    (void) context;
    return azm_install_island_hook_at(entry, (void *) replacement, (void **) original, "sdk-connection");
}

static void sdk_connections_at_start(void) {
    if (!g_sdk_sources_on || (!sdk_passive_sources && access("/home/root/settings/azmod.native-connections.on", F_OK) != 0))
        return;
    /* Refusal leaves source diagnostics available but cannot mint connection
     * provenance. There is no retry/reset/uninstall of reachable native hooks.
     * The backend installs close first and admits births only after both
     * genuine originals are published by the installer.
     */
    (void) azsdk_sources_install_connections((struct az_connection_installer) {
        sdk_connections_install_open, sdk_connections_install_close, NULL});
}
