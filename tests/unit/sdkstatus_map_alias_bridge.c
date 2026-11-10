/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise real GUI wrappers while the native motion fixture renders another deck. */
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include "azmod-mod.h"
#include "../common/azstatus_source.h"
#include "sdk_status_map_source.c"
static uintptr_t model[120], pack[40], phrase[9], grid[9], phrases[7], beats[7];
static void (*render_other_deck)(void);
static unsigned char retain(uintptr_t object) { return object != 0; }
static unsigned char release(uintptr_t object) { assert(object); return 0; }
static int prove(uintptr_t owner, uintptr_t p, uintptr_t gh, uintptr_t gc, uintptr_t ph, uintptr_t pc) {
    assert(owner == (uintptr_t)model + 0x268 && p == (uintptr_t)pack);
    assert(gh == (uintptr_t)grid && gc == (uintptr_t)beats && ph == (uintptr_t)phrase && pc == (uintptr_t)phrases);
    return 1; /* An already witnessed second deck retains this exact same pair. */
}
static void retire(uintptr_t owner) { assert(owner == (uintptr_t)model + 0x268); }
static uint64_t begin(uintptr_t owner, uintptr_t p) {
    assert(owner == (uintptr_t)model + 0x268 && (!p || p == (uintptr_t)pack));
    return azn_map_begin_mask(1); /* The other deck still owns the shared pack. */
}
static uint64_t native_set(uintptr_t m, const uintptr_t *ph, const uintptr_t *gh) {
    assert(m == (uintptr_t)model);
    model[0x2c8/8] = *ph; model[0x2d0/8] = *gh;
    if (render_other_deck) render_other_deck();
    return 0xabcde;
}
static uint64_t native_clear(uintptr_t m) {
    assert(m == (uintptr_t)model);
    model[0x2c8/8] = model[0x2d0/8] = 0;
    if (render_other_deck) render_other_deck();
    return 0xbabce;
}
void gui_alias_run(unsigned mode, void (*render)(void)) {
    static struct sm_profile profile;
    static struct azn_maps_api api;
    memset(&profile, 0, sizeof profile);
    profile.set_return = 100; profile.clear_return = 120; profile.owner_vptr = 901; profile.pack_vptr = 902;
    profile.functions[2].address = (uintptr_t)retain; profile.functions[3].address = (uintptr_t)release;
    sm_profile = &profile;
    api.size = sizeof api; api.version = 1; api.prove_pair = prove; api.retire_pair = retire;
    api.begin_pair = begin; api.end_pair = azn_map_end_mask;
    sm_original[0] = native_set; sm_original[1] = native_clear;
    atomic_store(&sm_maps, &api); atomic_store(&sm_ready, 1);
    model[0x268/8] = profile.owner_vptr; pack[0] = profile.pack_vptr;
    pack[0x120/8] = (uintptr_t)phrase; pack[0xd8/8] = (uintptr_t)grid;
    phrase[0x28/8] = (uintptr_t)phrases; grid[0x28/8] = (uintptr_t)beats;
    render_other_deck = 0;
    assert(sm_set_forward((uintptr_t)model, pack+0x120/8, pack+0xd8/8, 100) == 0xabcde);
    if (mode == 1) assert(sm_clear_forward((uintptr_t)model, 120) == 0xbabce);
    render_other_deck = render;
    if (mode == 2) assert(sm_clear_forward((uintptr_t)model, 120) == 0xbabce);
    else assert(sm_set_forward((uintptr_t)model, pack+0x120/8, pack+0xd8/8, 100) == 0xabcde);
    render_other_deck = 0;
    assert(sm_clear_forward((uintptr_t)model, 120) == 0xbabce);
}
