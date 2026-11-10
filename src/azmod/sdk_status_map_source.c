/* SPDX-License-Identifier: GPL-3.0-only */
/* TrackStructureData's pair setter is called with the two strong members of
 * the active PlayMusicPack. PhraseMeterWidget consumes this exact pair to map
 * its section indexes through the grid's sample positions, offset and clock. */
#include <pthread.h>
#include "../common/azstatus_maps.h"
#include "../common/azstatus_native.h"
struct sm_target {
    uintptr_t address;
    uint32_t words[4];
};
struct sm_profile {
    uintptr_t set_return, clear_return, owner_vptr, pack_vptr;
    struct sm_target functions[6];
};
static const struct sm_profile sm_profiles[] = {
    {0x1f9ba68, 0x1f9b95c, 0x2ddb648, 0x2612530, {{0x1f538a8, {0xa9b77bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4}}, {0x1f54a20, {0xa9b67bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3}}, {0x6b0c98, {0xa9bb7bfd, 0x5289eb03, 0x91003002, 0x910003fd}}, {0x6b0db8, {0xa9bc7bfd, 0x5289eb01, 0x72aa4861, 0x910003fd}}, {0x22043f0, {0xa9b77bfd, 0x910003fd, 0x910203e8, 0xa90153f3}}, {0x1fa1418, {0xa9be7bfd, 0xd00071c1, 0x91192021, 0x910003fd}}}},
    {0x2108618, 0x210850c, 0x318ea38, 0x294c9b0, {{0x20bfc20, {0xa9b77bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4}}, {0x20c0d98, {0xa9b67bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3}}, {0x6f2728, {0xa9bb7bfd, 0x5289eb03, 0x91003002, 0x910003fd}}, {0x6f2848, {0xa9bc7bfd, 0x5289eb01, 0x72aa4861, 0x910003fd}}, {0x2371890, {0xa9b77bfd, 0x910003fd, 0x910203e8, 0xa90153f3}}, {0x210eb68, {0xa9be7bfd, 0x90008401, 0x9128e021, 0x910003fd}}}}};
static const struct sm_profile *sm_profile;
static _Atomic int sm_ready;
/* The facade may complete before either holder gets its first content. Keep
 * a counted pack until this GUI owner clears or changes its assignment. All
 * later lookups require a fresh legitimate firmware borrow of that same owner.
 * Neither the GUI address nor an unpinned content address is a retained lease. */
static struct {
    uintptr_t owner, pack;
} sm_bindings[16];
static pthread_mutex_t sm_binding_mu = PTHREAD_MUTEX_INITIALIZER;
static void *sm_original[5];
static _Atomic unsigned sm_seen[8]; /* setter, accepted ABI, ready data, submitted, clear, phrase/grid update, busy */
struct sm_continuations {
    uintptr_t slot[2], target[2];
};
static const struct sm_continuations sm_updates[] = {
    {{0x2ddb988, 0x2ddb9b8}, {0x1f9d170, 0x1f9d1a0}},
    {{0x318ed90, 0x318edc0}, {0x210a9f8, 0x210aa28}}};
static _Atomic(const struct azn_maps_api *) sm_maps;
static uintptr_t sm_ptr(uintptr_t object, unsigned offset) {
    return __atomic_load_n((const uintptr_t *) (object + offset), __ATOMIC_ACQUIRE);
}
static const struct azn_maps_api *sm_service(void) {
    const struct azn_maps_api *maps = atomic_load_explicit(&sm_maps, memory_order_acquire);
    if (maps) return maps;
    azstatus_native_fn get = (azstatus_native_fn) dlsym(RTLD_DEFAULT, "azstem_status_service_v1");
    const struct azstatus_native_v1 *s = get ? get() : NULL;
    if (s && s->version == 1 && s->size >= offsetof(struct azstatus_native_v1, maps) + sizeof s->maps && s->maps) {
        maps = s->maps();
        if (maps && maps->version == 1 && maps->size >= offsetof(struct azn_maps_api, retire_pair) + sizeof maps->retire_pair &&
            maps->prove_pair && maps->retire_pair) {
            atomic_store_explicit(&sm_maps, maps, memory_order_release);
            return maps;
        }
    }
    return NULL;
}
static void sm_drop(uintptr_t content) {
    if (content && ((unsigned char (*)(uintptr_t)) sm_profile->functions[3].address)(content))
        ((void (*)(uintptr_t, uintptr_t)) sm_profile->functions[4].address)(sm_ptr(content, 0x18), content);
}
/* Exactly the grant/release used by both native pack setters, under a strong
 * holder on the GUI writer. The firmware's reader obtains the content through
 * the same guard and nonzero native count. Never follow a saved raw key. */
static uintptr_t sm_take(uintptr_t holder) {
    if (!holder) return 0;
    uintptr_t content = sm_ptr(holder, 0x28);
    if (!content || !((unsigned char (*)(uintptr_t)) sm_profile->functions[2].address)(content)) return 0;
    if (sm_ptr(holder, 0x28) == content) return content;
    sm_drop(content);
    return 0;
}
static int sm_retain(uintptr_t object) {
    return object && ((unsigned char (*)(uintptr_t)) sm_profile->functions[2].address)(object);
}
static void sm_binding(uintptr_t owner, uintptr_t pack) {
    uintptr_t old = 0;
    pthread_mutex_lock(&sm_binding_mu);
    unsigned slot = 16;
    for (unsigned i = 0; i < 16; i++) {
        if (sm_bindings[i].owner == owner) {
            slot = i;
            break;
        }
        if (!sm_bindings[i].owner && slot == 16) slot = i;
    }
    if (slot < 16 && sm_bindings[slot].pack != pack) {
        if (pack && !sm_retain(pack)) pack = 0;
        old = sm_bindings[slot].pack;
        sm_bindings[slot].owner = pack ? owner : 0;
        sm_bindings[slot].pack = pack;
    }
    pthread_mutex_unlock(&sm_binding_mu);
    sm_drop(old);
}
static uintptr_t sm_bound_pack(uintptr_t owner) {
    uintptr_t pack = 0;
    pthread_mutex_lock(&sm_binding_mu);
    for (unsigned i = 0; i < 16; i++)
        if (sm_bindings[i].owner == owner && sm_retain(sm_bindings[i].pack)) {
            pack = sm_bindings[i].pack;
            break;
        }
    pthread_mutex_unlock(&sm_binding_mu);
    return pack;
}
static uint64_t sm_begin(const struct azn_maps_api *maps, uintptr_t owner, uintptr_t pack) {
    return maps && maps->size >= offsetof(struct azn_maps_api, end_pair) + sizeof maps->end_pair &&
                   maps->begin_pair && maps->end_pair
               ? maps->begin_pair(owner, pack)
               : 0;
}
struct sm_scope {
    const struct azn_maps_api *maps;
    uint64_t ticket;
    uintptr_t pack;
};
static void sm_leave(struct sm_scope *scope) {
    if (scope->ticket) scope->maps->end_pair(scope->ticket);
    sm_drop(scope->pack);
}
static int sm_owner(uintptr_t owner) {
    return owner && sm_ptr(owner, 0) == sm_profile->owner_vptr;
}
static void sm_publish(const struct azn_maps_api *maps, uintptr_t owner, uintptr_t pack,
                       uintptr_t ph, uintptr_t gh) {
    if (sm_ptr(owner, 0x60) != ph || sm_ptr(owner, 0x68) != gh ||
        sm_ptr(pack, 0x120) != ph || sm_ptr(pack, 0xd8) != gh) return;
    uintptr_t gc = sm_take(gh), pc = sm_take(ph);
    if ((gc || pc) && (!gh || sm_ptr(gh, 0x28) == gc) && (!ph || sm_ptr(ph, 0x28) == pc)) {
        atomic_fetch_add(&sm_seen[2], 1);
        if (maps->prove_pair(owner, pack, gh, gc, ph, pc)) atomic_fetch_add(&sm_seen[3], 1);
    }
    sm_drop(pc);
    sm_drop(gc);
}
static uint64_t sm_set_forward(uintptr_t model, const uintptr_t *phrase, const uintptr_t *grid, uintptr_t caller) {
    const struct azn_maps_api *maps = NULL;
    uintptr_t owner = model + 0x268, pack = (uintptr_t) grid - 0xd8;
    atomic_fetch_add(&sm_seen[0], 1);
    int qualified = atomic_load_explicit(&sm_ready, memory_order_acquire) && caller == sm_profile->set_return &&
                    (uintptr_t) phrase == pack + 0x120 && sm_owner(owner) && sm_ptr(pack, 0) == sm_profile->pack_vptr;
    if (qualified) {
        atomic_fetch_add(&sm_seen[1], 1);
        maps = sm_service();
    }
    /* Both supported native setters change GUI references and listeners only.
     * The native service serializes pair publication and retirement; another
     * deck retaining this pack has no analysis mutation to bracket here. */
    if (maps && (sm_ptr(owner, 0x60) != sm_ptr((uintptr_t) phrase, 0) ||
                 sm_ptr(owner, 0x68) != sm_ptr((uintptr_t) grid, 0))) maps->retire_pair(owner);
    uint64_t result = ((uint64_t (*)(uintptr_t, const uintptr_t *, const uintptr_t *)) sm_original[0])(model, phrase, grid);
    if (qualified) {
        sm_binding(owner, pack);
        if (maps) sm_publish(maps, owner, pack, sm_ptr((uintptr_t) phrase, 0), sm_ptr((uintptr_t) grid, 0));
    }
    return result;
}
static uint64_t sm_set(uintptr_t model, const uintptr_t *phrase, const uintptr_t *grid) {
    return sm_set_forward(model, phrase, grid, (uintptr_t) __builtin_return_address(0));
}
static uint64_t sm_clear_forward(uintptr_t model, uintptr_t caller) {
    uintptr_t owner = model + 0x268;
    const struct azn_maps_api *maps = NULL;
    int qualified = atomic_load_explicit(&sm_ready, memory_order_acquire) && caller == sm_profile->clear_return && sm_owner(owner);
    if (qualified) maps = sm_service();
    /* Clear releases this GUI owner's references. A deck that still owns the
     * same pack can continue using the independently pinned pair. */
    if (qualified) {
        atomic_fetch_add(&sm_seen[4], 1);
        if (maps) maps->retire_pair(owner);
        sm_binding(owner, 0);
    }
    uint64_t result = ((uint64_t (*)(uintptr_t)) sm_original[1])(model);
    return result;
}
static uint64_t sm_clear(uintptr_t model) {
    return sm_clear_forward(model, (uintptr_t) __builtin_return_address(0));
}
/* Both AsyncTask bodies first compare their borrowed holder to the GUI
 * owner's matching slot. A callback from the replaced holder changes nothing. */
static uint64_t sm_update(uintptr_t task, unsigned index) {
    uintptr_t owner = sm_ptr(task, 0x18), changed = sm_ptr(task, 0x20);
    unsigned offset = index ? 0x68 : 0x60;
    uintptr_t pack = 0;
    const struct azn_maps_api *maps = NULL;
    if (atomic_load_explicit(&sm_ready, memory_order_acquire) && sm_owner(owner) && changed &&
        sm_ptr(owner, offset) == changed) {
        atomic_fetch_add(&sm_seen[5 + index], 1);
        pack = sm_bound_pack(owner);
        if (pack) maps = sm_service();
    }
    struct sm_scope scope __attribute__((cleanup(sm_leave))) = {maps, sm_begin(maps, owner, pack), pack};
    uint64_t result = ((uint64_t (*)(uintptr_t)) sm_original[2 + index])(task);
    if (maps && sm_ptr(owner, offset) == changed)
        sm_publish(maps, owner, pack, sm_ptr(owner, 0x60), sm_ptr(owner, 0x68));

    return result;
}
static uint64_t sm_phrase_update(uintptr_t task) { return sm_update(task, 0); }
static uint64_t sm_grid_update(uintptr_t task) { return sm_update(task, 1); }
/* The model destructor releases the two members directly, without calling
 * the ordinary clear setter. Retire our lease before the stock owner dies. */
static uint64_t sm_destroy(uintptr_t model) {
    if (atomic_load_explicit(&sm_ready, memory_order_acquire)) {
        uintptr_t owner = model + 0x268;
        const struct azn_maps_api *maps = atomic_load_explicit(&sm_maps, memory_order_acquire);
        if (maps) maps->retire_pair(owner);
        sm_binding(owner, 0);
    }
    return ((uint64_t (*)(uintptr_t)) sm_original[4])(model);
}
static int sdk_status_map_source_install(int build, azjs_installer install) {
    if ((build != 1 && build != 2) || !install) return 0;
    sm_profile = &sm_profiles[build - 1];
    for (unsigned i = 0; i < 6; i++) {
        const struct sm_target *fn = &sm_profile->functions[i];
        if (memcmp((void *) fn->address, fn->words, sizeof fn->words)) return 0;
    }
    const struct sm_continuations *updates = &sm_updates[build - 1];
    for (unsigned i = 0; i < 2; i++)
        if (*(const uintptr_t *) updates->slot[i] != updates->target[i]) return 0;
    uintptr_t destroy = sm_profile->functions[5].address;
    if (!install(destroy, destroy, sm_destroy, &sm_original[4], 0) || !sm_original[4]) return 0;
    void *hooks[] = {sm_set, sm_clear, sm_phrase_update, sm_grid_update};
    for (unsigned i = 0; i < 2; i++) {
        uintptr_t at = sm_profile->functions[i].address;
        if (!install(at, at, hooks[i], &sm_original[i], 0) || !sm_original[i]) return 0;
    }
    for (unsigned i = 0; i < 2; i++)
        if (!install(updates->slot[i], updates->target[i], hooks[2 + i], &sm_original[2 + i], 1) || !sm_original[2 + i]) return 0;
    atomic_store_explicit(&sm_ready, 1, memory_order_release);
    return 1;
}
