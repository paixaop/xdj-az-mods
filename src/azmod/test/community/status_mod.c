/* SPDX-License-Identifier: GPL-3.0-only */
/* Loaded through the ordinary manifest, approval, dlopen and init path.
 * This mod has no tick, screen, worker, getter, command or private producer. */
#include "azmod-mod.h"
#include <stdio.h>
#define ID "status-watch"
static const az_library_v1 *lib;
static az_deck_subscription handle;
static void changed(const az_deck_status_event_v1 *e, void *user) {
    const az_status_loop_v1 *l = e->view->loop;
    if (!l || e->deck != 1 || e->subscription != handle || user != &handle ||
        e->topics != AZ_STATUS_TOPIC(AZ_STATUS_LOOP) || e->view->load || e->occurrence) __builtin_trap();
    int value = l->header.valid & az_status_field_bit(AZ_STATUS_LOOP_ENGAGED) ? l->engaged : -1;
    printf("LOOP %u %d %llu\n", e->kind, value, (unsigned long long) e->sequence);
    if (value == 1) {
        if (lib->deck_status_release(ID, handle) != AZ_OK ||
            l->engaged_info.info.availability != AZ_AVAIL_READY || l->engaged != 1) __builtin_trap();
    }
}
static int init(const az_core_v1 *core) {
    if (!AZ_HAS(core, library_service)) return -1;
    lib = core->library_service(AZ_LIBRARY_ABI);
    if (!AZ_LIB_HAS(lib, on_deck_status) || !AZ_LIB_HAS(lib, deck_status_release)) return -1;
    const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 2, 0, AZ_STATUS_TOPIC(AZ_STATUS_LOOP)};
    az_deck_subscription untouched = 123;
    if (lib->on_deck_status(ID, &watch, (az_deck_status_fn)(uintptr_t)&handle, NULL, &untouched) != AZ_INVALID ||
        untouched != 123) return -1;
    return lib->on_deck_status(ID, &watch, changed, &handle, &handle) == AZ_OK ? 0 : -1;
}
static const char *const tags[] = {"any", NULL};
static const az_mod_v1 mod = {.abi = AZ_MOD_ABI, .size = sizeof mod, .id = ID, .fw_tags = tags, .init = init};
AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void) { return &mod; }
