/* SPDX-License-Identifier: GPL-3.0-only */
/* Included after the real island installer and source configuration state.
 * Startup-only opt-in dependency; no source/load capability is advertised.
 * The canonical C++ bridge validates/pins the exact loaded module before the
 * first redirect and retains close retirement independently of diagnostics.
 */
#include "../common/azsources.h"

static int sdk_connections_install_at(uintptr_t entry, void *replacement, void **original,
                                       void *context) {
    (void) context;
    return azm_install_island_hook_at(entry, replacement, original, "sdk-connection");
}

static void sdk_connections_at_start(void) {
    if (!g_sdk_sources_on || (!sdk_passive_sources && access("/home/root/settings/azmod.native-connections.on", F_OK) != 0))
        return;
    /* Refusal leaves source diagnostics available but cannot mint connection
     * provenance. There is no retry/reset/uninstall of reachable native hooks.
     * The backend installs close first and admits births only after both
     * genuine originals are published by the installer.
     */
    (void) azsdk_sources_install_connections(sdk_connections_install_at, NULL);
}
