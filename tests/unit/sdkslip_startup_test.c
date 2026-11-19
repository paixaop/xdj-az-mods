/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azslipobserver.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static _Atomic int g_azm_late;
static int g_ep147_build, track_disabled, live_disabled, getters_enabled;
static uintptr_t g_ep_img_lo, g_ep_img_hi;
static unsigned installs, getter_installs, patches;
static int accepted = 1;
static int flag_access(const char *path, int mode) {
    assert(mode == F_OK);
    if (!strcmp(path, "/home/root/settings/azmod.track-data.off")) return track_disabled ? 0 : -1;
    if (!strcmp(path, "/home/root/settings/azmod.native-live.off")) return live_disabled ? 0 : -1;
    assert(!strcmp(path, "/home/root/settings/azmod.slip-targets.on"));
    return getters_enabled ? 0 : -1;
}
static int azm_install_island_hook_at(uintptr_t entry, void *replacement, void **original, const char *name) {
    assert(entry == 0x123400 && replacement == (void *) 0x234500 && original && !strcmp(name, "sdk-slip"));
    ++patches;
    *original = (void *) 0x345600;
    return 7;
}
#define access flag_access
#include "../../src/azmod/sdk_slip_startup.c"
#undef access

int azsdk_slip_install(const struct azsdk_slip_native_profile *p, azsdk_slip_installer install, void *context) {
    assert(p->construct_return && p->registration && install == sdk_slip_patch && !context);
    ++installs;
    return accepted;
}
int azsdk_slip_getters_install(const struct azsdk_slip_native_profile *p, azsdk_slip_installer install, void *context) {
    assert(p->producer && install == sdk_slip_patch && !context);
    ++getter_installs;
    return 1;
}
static void reset_startup(int build) {
    g_ep147_build = build;
    assert(ep147_addr_init(build));
    g_ep_img_lo = 0x10000;
    g_ep_img_hi = UINT64_C(0x10000000);
    atomic_store(&g_azm_late, 0);
    track_disabled = live_disabled = getters_enabled = 0;
    accepted = 1;
    installs = getter_installs = 0;
}
static void flag_cases(int build) {
    for (unsigned mask = 0; mask < 16; ++mask) {
        reset_startup(build);
        atomic_store(&g_azm_late, mask & 1);
        track_disabled = mask & 2;
        live_disabled = mask & 4;
        getters_enabled = mask & 8;
        sdk_slip_at_start();
        unsigned allowed = !(mask & 7);
        assert(installs == allowed && getter_installs == (allowed && getters_enabled));
    }
    reset_startup(build);
    accepted = 0;
    getters_enabled = 1;
    sdk_slip_at_start();
    assert(installs == 1 && !getter_installs);
    reset_startup(build);
    g_ep147_build = 0;
    sdk_slip_at_start();
    assert(!installs && !getter_installs);
    reset_startup(build);
    ep147_addr[0] ^= 4;
    sdk_slip_at_start();
    assert(!installs && !getter_installs);
}
static void range_cases(int build) {
    reset_startup(build);
    struct azsdk_slip_native_profile p;
    assert(sdk_slip_target_profile(build, &p));
    uintptr_t starts[] = {p.targets[0], p.targets[1], p.targets[2], p.targets[3], p.owner_scope, p.registration};
    size_t sizes[] = {p.code_bytes[0], p.code_bytes[1], p.code_bytes[2], p.code_bytes[3], p.owner_scope_bytes, p.registration_bytes};
    for (unsigned i = 0; i < sizeof starts / sizeof starts[0]; ++i) {
        reset_startup(build);
        g_ep_img_lo = starts[i] + 1;
        sdk_slip_at_start();
        assert(!installs && !getter_installs);
        reset_startup(build);
        g_ep_img_hi = starts[i] + sizes[i] - 1;
        sdk_slip_at_start();
        assert(!installs && !getter_installs);
    }
    g_ep_img_lo = 100;
    g_ep_img_hi = 200;
    assert(sdk_slip_image_range(100, 100));
    assert(!sdk_slip_image_range(99, 1));
    assert(!sdk_slip_image_range(200, 0));
    assert(!sdk_slip_image_range(101, 100));
    assert(!sdk_slip_image_range(199, SIZE_MAX));
}
int main(void) {
    flag_cases(EPB_V130);
    range_cases(EPB_V130);
    flag_cases(EPB_V200);
    range_cases(EPB_V200);
    void *original = NULL;
    atomic_store(&g_azm_late, 1);
    assert(!sdk_slip_patch(0x123400, (void *) 0x234500, &original, NULL) && !patches && !original);
    atomic_store(&g_azm_late, 0);
    assert(sdk_slip_patch(0x123400, (void *) 0x234500, &original, NULL) == 7 && patches == 1 && original == (void *) 0x345600);
    puts("PASS slip startup: flags, exact runtime profile, image bounds and late patch refusal");
    return 0;
}
