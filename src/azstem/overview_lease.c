/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azoverviewlease.h"
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#ifndef AZOV_LEASE_UNIT
#include <dlfcn.h>
#if defined(__linux__)
#include <link.h>
#endif
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define AZOV_SLOTS 8u
#define AZOV_RETAIN_ATTEMPTS 8u
#define AZOV_XOCR UINT32_C(0x52434f58)
struct azov_entry {
    uintptr_t content;
    void *cookie;
};
struct azov_slot {
    uint64_t generation;
    uintptr_t content;
    unsigned char *original, *scaled;
};
static struct azov_slot azov_slots[AZOV_SLOTS]; /* Configured message thread only. */
static uint64_t azov_generation;
static pthread_t azov_thread;
static _Atomic int azov_configured;
static int azov_enabled;
static void (*azov_release_owner)(void *, void *);
struct azov_native_profile {
    uintptr_t setter, updater, render_return, updater_return, owner_release, content_vtable;
    uint64_t setter_hash, updater_hash;
};
static int azov_capture_valid(const struct azov_owned_entry *capture,
                              const struct azov_native_profile *profile, uintptr_t lo,
                              uintptr_t hi, uintptr_t here) {
    uintptr_t sp = capture->native_stack;
    if (!profile || capture->native_return != profile->render_return || !capture->widget ||
        capture->held_content < 0x10000 || capture->held_content > UINT64_C(0x0000ffffffffffff) ||
        (capture->held_content & 7u) || (sp & 15u) || sp < lo || sp > hi || hi - sp < 0xd8 ||
        sp <= here || sp - here > 65536) return 0;
    if (*(const uintptr_t *) (sp + 8) != profile->updater_return ||
        *(const uintptr_t *) (sp + 0xd0) != capture->held_content ||
        *(const uintptr_t *) capture->held_content != profile->content_vtable) return 0;
    return 1;
}
#ifndef AZOV_LEASE_UNIT
static void *azov_client_image;
#if defined(__linux__) && defined(__aarch64__)
static const struct azov_native_profile azov_profiles[2] = {
    {0x1dfc710, 0x1c16aa0, 0x1dfc7f0, 0x1c174fc, 0x22043f0, 0x25cd0e0,
     UINT64_C(0x68729b60d1807ca6), UINT64_C(0x6b0805bc2f0d8f1a)},
    {0x1f689b0, 0x1d80220, 0x1f68a90, 0x1d80c7c, 0x2371890, 0x2906700,
     UINT64_C(0x96e12fe1c962d8eb), UINT64_C(0x26153788fdd73e1b)}};
#endif
static const struct azov_native_profile *azov_profile;
static uintptr_t azov_stack_lo, azov_stack_hi;
#endif

/* A native profile must enter an actual ownership/serialization boundary BEFORE returning
 * a Content address. CAS after a raw slot load does not meet this contract. Both verified
 * render entries also carry the updater's ALREADY RETAINED Content in x24. The exact
 * setter and updater retain it through normal execution and EH until this hook unwinds.
 * Only that verified calling context is accepted; a raw UPO slot is never acquired.
 * Unit fixtures supply a real locked owner whose allocation cannot retire before leave;
 * those fixtures validate the broker mechanics, not native profile activation. */
#ifdef AZOV_LEASE_UNIT
extern int azov_test_enter_owned(const struct azov_owned_entry *, struct azov_entry *);
extern void azov_test_leave_owned(struct azov_entry *);
extern int azov_test_trusted(void);
extern int azov_test_profile_ready(void);
extern int azov_test_fail_cas(void);
extern void azov_test_before_publish(void);
#define azov_enter_owned azov_test_enter_owned
#define azov_leave_owned azov_test_leave_owned
static int azov_profile_ready(void) { return azov_test_profile_ready(); }
int azov_test_validate_capture(const struct azov_owned_entry *capture, uintptr_t lo, uintptr_t hi,
                               uintptr_t here, uintptr_t render_return, uintptr_t updater_return,
                               uintptr_t content_vtable) {
    const struct azov_native_profile profile = {0, 0, render_return, updater_return, 0,
                                                content_vtable, 0, 0};
    return azov_capture_valid(capture, &profile, lo, hi, here);
}
#else
static int azov_profile_ready(void) { return azov_profile != NULL; }
static int azov_enter_owned(const struct azov_owned_entry *capture, struct azov_entry *entry) {
    const struct azov_native_profile *profile = azov_profile;
    uintptr_t here = (uintptr_t) &profile;
    /* SP belongs to the actual captured native entry. Its still-live setter frame contains
     * the return to updater; updater's strong holder is setter-SP + 0x60 + 0x70. */
    if (!azov_capture_valid(capture, profile, azov_stack_lo, azov_stack_hi, here)) return 0;
    entry->content = capture->held_content;
    return 1;
}
static void azov_leave_owned(struct azov_entry *entry) { (void) entry; }
#if defined(__linux__) && defined(__aarch64__)
static int azov_main_range(struct dl_phdr_info *info, size_t size, void *argument) {
    (void) size;
    if (info->dlpi_name && *info->dlpi_name) return 0;
    const struct azov_native_profile *profile = argument;
    int setter = 0, updater = 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD || (p->p_flags & (PF_R | PF_X)) != (PF_R | PF_X)) continue;
        if (p->p_vaddr > UINTPTR_MAX - info->dlpi_addr ||
            p->p_memsz > UINTPTR_MAX - info->dlpi_addr - p->p_vaddr) continue;
        uintptr_t lo = info->dlpi_addr + p->p_vaddr, hi = lo + p->p_memsz;
        setter |= profile->setter >= lo && profile->setter <= hi && hi - profile->setter >= 700;
        updater |= profile->updater >= lo && profile->updater <= hi && hi - profile->updater >= 9532;
    }
    return setter && updater ? 1 : 2;
}
static uint64_t azov_hash(uintptr_t address, size_t count) {
    const unsigned char *p = (const unsigned char *) address;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < count; ++i) hash = (hash ^ p[i]) * UINT64_C(1099511628211);
    return hash;
}
static void azov_prepare_profile(int build_id, void (*release)(void *, void *)) {
    const struct azov_native_profile *profile = &azov_profiles[build_id - 1];
    if ((uintptr_t) release != profile->owner_release ||
        dl_iterate_phdr(azov_main_range, (void *) profile) != 1 ||
        azov_hash(profile->setter, 700) != profile->setter_hash ||
        azov_hash(profile->updater, 9532) != profile->updater_hash) return;
    pthread_attr_t attr;
    void *stack;
    size_t size;
    if (pthread_getattr_np(pthread_self(), &attr)) return;
    int result = pthread_attr_getstack(&attr, &stack, &size);
    pthread_attr_destroy(&attr);
    if (result || !stack || size < 0xd8 || (uintptr_t) stack > UINTPTR_MAX - size) return;
    azov_stack_lo = (uintptr_t) stack;
    azov_stack_hi = azov_stack_lo + size;
    azov_profile = profile;
}
#endif
#endif

static int azov_caller_ok(const void *caller) {
    if (!atomic_load_explicit(&azov_configured, memory_order_acquire) ||
        !pthread_equal(pthread_self(), azov_thread)) return 0;
#ifdef AZOV_LEASE_UNIT
    (void) caller;
    return azov_test_trusted();
#else
    Dl_info info;
    return azov_client_image && caller && dladdr(caller, &info) &&
           info.dli_fbase == azov_client_image;
#endif
}
static void azov_drop(uintptr_t content) {
    _Atomic int *rc = (_Atomic int *) (content + 0x0c);
    if (atomic_fetch_sub_explicit(rc, 1, memory_order_acq_rel) == 1)
        azov_release_owner(*(void *const *) (content + 0x18), (void *) content);
}
static struct azov_slot *azov_find(const uint64_t token[2]) {
    if (!token || !token[0] || !token[1] || token[1] > AZOV_SLOTS) return NULL;
    struct azov_slot *slot = &azov_slots[token[1] - 1];
    return slot->content && slot->generation == token[0] ? slot : NULL;
}
static int azov_begin(const struct azov_owned_entry *capture, struct azov_lease *out, size_t size) {
    if (!azov_caller_ok(__builtin_return_address(0)) || !azov_enabled ||
        !azov_profile_ready() || !capture || !out || size < sizeof *out ||
        azov_generation == UINT64_MAX) return 0;
    unsigned index;
    for (index = 0; index < AZOV_SLOTS && azov_slots[index].content; ++index) {}
    if (index == AZOV_SLOTS) return 0;
    struct azov_entry entry = {0, NULL};
    if (!azov_enter_owned(capture, &entry)) return 0;
    uintptr_t content = entry.content;
    int retained = 0;
    if (content && !(content & 7u) &&
        *(const uint32_t *) (content + 0x10) == AZOV_XOCR) {
        _Atomic int *rc = (_Atomic int *) (content + 0x0c);
        int count = atomic_load_explicit(rc, memory_order_acquire);
        /* Leave one further retain for the original renderer; contention selects stock. */
        for (unsigned attempt = 0; attempt < AZOV_RETAIN_ATTEMPTS && count > 0 &&
                                   count < INT_MAX - 1;
             ++attempt) {
#ifdef AZOV_LEASE_UNIT
            if (azov_test_fail_cas()) {
                count = atomic_load_explicit(rc, memory_order_acquire);
                continue;
            }
#endif
            if (atomic_compare_exchange_weak_explicit(rc, &count, count + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                retained = 1;
                break;
            }
        }
    }
    /* The genuine entry owner remains alive through the retain and leave. */
    azov_leave_owned(&entry);
    if (!retained) return 0;
    for (unsigned i = 0; i < AZOV_SLOTS; ++i)
        if (azov_slots[i].content == content) {
            azov_drop(content); /* Same-object recursive rendering uses the outer copy. */
            return 0;
        }
    int count = *(const int *) (content + 0x30);
    unsigned char *points = *(unsigned char **) (content + 0x28);
    if (!points || count <= 0 || count > 2048) {
        azov_drop(content);
        return 0;
    }
    struct azov_slot *slot = &azov_slots[index];
    slot->generation = ++azov_generation;
    slot->content = content;
    slot->original = points;
    slot->scaled = NULL;
    struct azov_lease result = {{slot->generation, index + 1u}, content, points, count, 0};
    memcpy(out, &result, sizeof result);
    return 1;
}
static int azov_install(const uint64_t token[2], unsigned char *scaled) {
    if (!azov_caller_ok(__builtin_return_address(0)) || !scaled) return 0;
    struct azov_slot *slot = azov_find(token);
    if (!slot || slot->scaled || *(unsigned char **) (slot->content + 0x28) != slot->original)
        return 0;
    slot->scaled = scaled;
    *(unsigned char **) (slot->content + 0x28) = scaled;
    return 1;
}
static int azov_finish(const uint64_t token[2]) {
    if (!azov_caller_ok(__builtin_return_address(0))) return 0;
    struct azov_slot *slot = azov_find(token);
    if (!slot) return 0;
    uintptr_t content = slot->content;
    unsigned char **array = (unsigned char **) (content + 0x28);
    if (slot->scaled && *array == slot->scaled) *array = slot->original;
    /* Consume first: reentrant/duplicate releases cannot see an active token. */
    memset(slot, 0, sizeof *slot);
    azov_drop(content);
    return 1;
}
void azov_lease_configure(int build_id, void (*release)(void *, void *)) {
    if (atomic_load_explicit(&azov_configured, memory_order_acquire)) {
        if (pthread_equal(pthread_self(), azov_thread) && (!release || !build_id)) azov_enabled = 0;
        return;
    }
    if ((build_id != 1 && build_id != 2) || !release) return;
#ifndef AZOV_LEASE_UNIT
#if defined(__linux__) && defined(__aarch64__)
    if (syscall(SYS_gettid) != getpid()) return;
#else
    return;
#endif
#endif
    azov_thread = pthread_self();
    azov_release_owner = release;
    azov_enabled = 1;
#if !defined(AZOV_LEASE_UNIT) && defined(__linux__) && defined(__aarch64__)
    azov_prepare_profile(build_id, release);
#endif
#ifdef AZOV_LEASE_UNIT
    azov_test_before_publish();
#endif
    atomic_store_explicit(&azov_configured, 1, memory_order_release);
}
int azstem_overview_lease_v1(uint32_t abi, struct azov_lease_api *out, size_t size) {
    if (!atomic_load_explicit(&azov_configured, memory_order_acquire) ||
        !pthread_equal(pthread_self(), azov_thread) ||
        abi != AZOV_LEASE_ABI || !out || size < sizeof *out) return 0;
#ifdef AZOV_LEASE_UNIT
    if (!azov_test_trusted()) return 0;
#else
    Dl_info info;
    if (!dladdr(__builtin_return_address(0), &info) || !info.dli_fbase || !info.dli_fname) return 0;
    const char *name = strrchr(info.dli_fname, '/');
    if (strcmp(name ? name + 1 : info.dli_fname, "azmod.so")) return 0;
    if (azov_client_image && azov_client_image != info.dli_fbase) return 0;
    azov_client_image = info.dli_fbase;
#endif
    const struct azov_lease_api api = {sizeof api, AZOV_LEASE_ABI,
                                       azov_profile_ready() ? 0u : AZOV_LEASE_PROFILE_UNAVAILABLE, 0,
                                       azov_begin, azov_install, azov_finish};
    memcpy(out, &api, sizeof api);
    return 1;
}
