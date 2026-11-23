/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azcueprofile.h"
#include "../common/azcuerecall.h"
#include "../azmod/azmod-mod.h"
#include "sdk_cue_profiles.h"
#include <stdatomic.h>
#include <string.h>
#include "../common/aznative_sync.h"
#include "../common/aznative_c.h"
#if defined(__linux__)
#include <link.h>
#endif
typedef struct State {
    struct azcue_profile profile;
    void *ctor, *dtor;
    _Atomic bool enabled;
    _Atomic uint64_t incarnation;
    _Atomic(void *) managers[4];
    pthread_mutex_t management;
    bool attempted;
} State;
static State storage = {.management = PTHREAD_MUTEX_INITIALIZER};
static State *state(void) { return &storage; }
static uintptr_t word(const void *p) {
    uintptr_t value;
    memcpy(&value, p, sizeof value);
    return value;
}
static bool manager_locators(uint64_t a, uint64_t b, uint64_t c) {
    return a == UINT64_C(0x009d3addf06bd1d7) && (b & ~UINT64_C(255)) == UINT64_C(0xb9d7725fd31fc100) &&
           (c & ~UINT64_C(255)) == UINT64_C(0x3dc18c4946733700) && (b & 255) <= 3 && (b & 255) == (c & 255);
}
static bool constructed_manager(State *s, void *self, uint64_t a, uint64_t b, uint64_t c) {
    return atomic_load_explicit(&s->enabled, memory_order_acquire) && self && manager_locators(a, b, c) &&
           word(self) == s->profile.actor.manager_vptr && word((char *) self + 0x110) == a && word((char *) self + 0x120) == b;
}
static uint64_t next_incarnation(State *s) {
    uint64_t generation = atomic_load_explicit(&s->incarnation, memory_order_relaxed);
    do {
        if (generation == UINT64_MAX) return 0;
    } while (!atomic_compare_exchange_weak_explicit(&s->incarnation, &generation, generation + 1, memory_order_relaxed, memory_order_relaxed));
    return generation + 1;
}
static void ctor_hook(void *self, const uint64_t *settings, const uint64_t *player, const uint64_t *renew, void *config) {
    State *s = state();
    // Copy the locator scalars before stock construction can use temporary storage.
    uint64_t a = settings ? *settings : 0, b = player ? *player : 0, c = renew ? *renew : 0;
    ((void (*)(void *, const uint64_t *, const uint64_t *, const uint64_t *, void *))(s->ctor))(self, settings, player, renew, config);
    if (!constructed_manager(s, self, a, b, c)) return;
    uint64_t generation = next_incarnation(s);
    if (!generation) return;
    const unsigned deck = (unsigned) (b & 255);
    if (azsdk_cue_actor_register(deck, self, generation) == AZ_OK)
        atomic_store_explicit(&(s->managers[deck]), self, memory_order_release);
}
static void dtor_hook(void *self) {
    State *s = state();
    for (unsigned deck = 0; deck < 4; ++deck) {
        void *expected = self;
        if (self && atomic_compare_exchange_strong_explicit(&(s->managers[deck]), &(expected), NULL, memory_order_acq_rel, memory_order_acquire)) {
            azsdk_cue_actor_retire(deck, self);
            azsdk_cue_persist_retire(self);
            azsdk_cue_recall_retire(self);
        }
    }
    ((void (*)(void *))(s->dtor))(self);
}
static __attribute__((unused)) bool bytes(const struct azcue_entry *e) { return e->address && !memcmp(((const void *) (e->address)), e->prologue, 16); }
#if defined(__linux__) && defined(__aarch64__)
typedef struct Main {
    uintptr_t bias;
    bool found;
    const ElfW(Phdr) * headers;
    unsigned count;
} Main;
static bool readable_segment(const ElfW(Phdr) * p, bool execute) {
    return p->p_type == PT_LOAD && (p->p_flags & PF_R) && (!execute || (p->p_flags & PF_X));
}
static bool main_contains(const Main *image, uintptr_t address, size_t size, bool execute) {
    if (address > UINTPTR_MAX - size) return false;
    for (unsigned i = 0; i < image->count; ++i) {
        const ElfW(Phdr) *p = &image->headers[i];
        if (!readable_segment(p, execute)) continue;
        uintptr_t begin = image->bias + p->p_vaddr;
        if (address >= begin && address + size >= address && address + size <= begin + p->p_memsz) return true;
    }
    return false;
}
static __attribute__((unused)) bool valid(const Main *image, const struct azcue_entry *e) {
    return main_contains(image, e->address, 16, true) && bytes(e);
}
static __attribute__((unused)) int main_image(struct dl_phdr_info *p, size_t size, void *opaque) {
    if (p->dlpi_name && p->dlpi_name[0]) return 0;
    (void) size;
    Main *m = opaque;
    m->bias = p->dlpi_addr;
    m->headers = p->dlpi_phdr;
    m->count = p->dlpi_phnum;
    m->found = true;
    return 1;
}
#endif
static bool rebase(uintptr_t *address, uintptr_t bias) {
    if (!*address || *address > UINTPTR_MAX - bias) return false;
    *address += bias;
    return true;
}
static bool rebase_actor(struct azcue_profile *p, uintptr_t bias) {
    uintptr_t *actor[] = {&p->actor.manager_vptr, &p->actor.box_vptr, &p->actor.owner_vptr, &p->actor.task_vptr,
                          &p->actor.memory_producer, &p->actor.post, &p->actor.execute, &p->actor.complete_dtor};
    for (unsigned i = 0; i < sizeof actor / sizeof *actor; ++i)
        if (!rebase(actor[i], bias)) return 0;
    return true;
}
static bool rebase_persist(struct azcue_profile *p, uintptr_t bias) {
    uintptr_t *persist[] = {&p->persist.save, &p->persist.erase, &p->persist.request, &p->persist.handler_request,
                            &p->persist.reception_reply, &p->persist.registered_task, &p->persist.handler_table, &p->persist.reception_table,
                            &p->persist.string_allocate, &p->persist.string_assign, &p->persist.string_destroy, &p->persist.color_construct,
                            &p->persist.memory_helper, &p->persist.content_copy, &p->persist.content_release,
                            &p->persist.vector_append, &p->persist.vector_destroy, &p->persist.content_from_vector, &p->persist.content_publish, &p->persist.hot_helper,
                            &p->persist.hot_content_from_vector, &p->persist.hot_content_publish, &p->persist.cue_copy, &p->persist.cue_destroy,
                            &p->persist.memory_notice};
    for (unsigned i = 0; i < sizeof persist / sizeof *persist; ++i)
        if (!rebase(persist[i], bias)) return 0;
    return true;
}
static bool rebase_entries(struct azcue_entry *entries, unsigned count, uintptr_t bias) {
    for (unsigned i = 0; i < count; ++i)
        if (!rebase(&entries[i].address, bias)) return false;
    return true;
}
static bool rebase_profile_entries(struct azcue_profile *p, uintptr_t bias) {
    return rebase_entries(p->store, sizeof p->store / sizeof *p->store, bias) && rebase_persist(p, bias) &&
           rebase_entries(p->persist_hooks, sizeof p->persist_hooks / sizeof *p->persist_hooks, bias) &&
           rebase_entries(p->persist_metadata, sizeof p->persist_metadata / sizeof *p->persist_metadata, bias) &&
           rebase_entries(p->actor_entries, sizeof p->actor_entries / sizeof *p->actor_entries, bias);
}
int azsdk_cue_profile_get(int build, uintptr_t bias, struct azcue_profile *out) {
    if (!out || build < 1 || build > 2) return 0;
    struct azcue_profile p = cue_profiles[build - 1];
    if (!rebase_actor(&p, bias) || !rebase(&p.ctor.address, bias) ||
        !rebase(&p.complete_dtor.address, bias) || !rebase(&p.deleting_dtor.address, bias)) return 0;
    if (!rebase_profile_entries(&p, bias)) return 0;
    *out = p;
    return 1;
}
#ifdef AZSDK_CUE_LIFECYCLE_TESTING
static const struct azcue_profile *lifecycle_test_profile = NULL;
#endif
#if defined(__linux__) && defined(__aarch64__)
static bool profile_entry_group(const Main *image, const struct azcue_entry *entries, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        if (!valid(image, &entries[i])) return false;
    return true;
}
static bool profile_entries_valid(const Main *image, const struct azcue_profile *p) {
    return valid(image, &p->ctor) && valid(image, &p->complete_dtor) && valid(image, &p->deleting_dtor) &&
           profile_entry_group(image, p->store, sizeof p->store / sizeof *p->store) &&
           profile_entry_group(image, p->persist_hooks, sizeof p->persist_hooks / sizeof *p->persist_hooks) &&
           profile_entry_group(image, p->persist_metadata, sizeof p->persist_metadata / sizeof *p->persist_metadata) &&
           profile_entry_group(image, p->actor_entries, sizeof p->actor_entries / sizeof *p->actor_entries);
}
static bool profile_tables_readable(const Main *image, const struct azcue_profile *p) {
    const uintptr_t tables[] = {p->actor.manager_vptr, p->actor.box_vptr, p->actor.owner_vptr,
                                p->actor.task_vptr, p->persist.handler_table, p->persist.reception_table};
    for (unsigned i = 0; i < sizeof tables / sizeof *tables; i++)
        if (!main_contains(image, tables[i], 64, false)) return false;
    return true;
}
static bool profile_table_bindings(const struct azcue_profile *p) {
    return word((void *) p->actor.manager_vptr) == p->complete_dtor.address &&
           word((void *) (p->actor.manager_vptr + 8)) == p->deleting_dtor.address &&
           word((void *) (p->actor.manager_vptr + 16)) == p->actor.memory_producer &&
           word((void *) (p->actor.box_vptr + 48)) == p->actor.post &&
           word((void *) p->actor.task_vptr) == p->actor.complete_dtor &&
           word((void *) (p->actor.task_vptr + 16)) == p->actor.execute;
}
static __attribute__((unused)) bool profile_verified(const Main *image, const struct azcue_profile *p) {
    return profile_entries_valid(image, p) && profile_tables_readable(image, p) && profile_table_bindings(p);
}
#endif
static bool profile_request_valid(int build, const struct azcue_profile *out) {
    return out && build >= 1 && build <= 2;
}
int azsdk_cue_main_profile(int build, struct azcue_profile *out) {
    if (!profile_request_valid(build, out)) return 0;
#ifdef AZSDK_CUE_LIFECYCLE_TESTING
    if (!lifecycle_test_profile) return 0;
    *out = *lifecycle_test_profile;
    return 1;
#elif defined(__linux__) && defined(__aarch64__)
    // Validation precedes our own hooks. Preserve that verified immutable binding:
    // later actor/lifecycle interception legitimately changes these native bytes.
    static pthread_mutex_t validation_lock = PTHREAD_MUTEX_INITIALIZER;
    static struct azcue_profile validated[2] = {0};
    static bool has_validation[2] = {0};
    AZ_MUTEX_LOCK(guard, validation_lock);
    if (has_validation[build - 1]) {
        *out = validated[build - 1];
        return 1;
    }
    Main image = {0};
    dl_iterate_phdr(main_image, &image);
    struct azcue_profile p = {0};
    if (!image.found || !azsdk_cue_profile_get(build, image.bias, &p)) return 0;
    if (!profile_verified(&image, &p)) return 0;
    validated[build - 1] = p;
    has_validation[build - 1] = true;
    *out = p;
    return 1;
#else
    (void) build;
    (void) out;
    return 0;
#endif
}
struct Install {
    int build;
    azexit_install_fn install;
    int result;
};
static void configure(void *context) {
    struct Install *call = context;
    State *s = state();
    AZ_MUTEX_GUARD(lock, s->management, false);
    if (lock.error || s->attempted) return;
    struct azcue_profile p = {0};
    if (!azsdk_cue_main_profile(call->build, &p)) return;
    s->attempted = true;
    s->profile = p;
    if (!call->install(p.ctor.address, (void *) ctor_hook, &s->ctor) || !s->ctor ||
        !call->install(p.complete_dtor.address, (void *) dtor_hook, &s->dtor) || !s->dtor) return;
    atomic_store_explicit(&s->enabled, true, memory_order_release);
    call->result = 1;
}
int azsdk_cue_lifecycle_configure(int build, azexit_install_fn install) {
    if (!install) return 0;
    struct Install call = {build, install, 0};
    configure(&call);
    return call.result;
}
