/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZNATIVE_C_H
#define AZNATIVE_C_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

/* The native GNU shared/weak ABI is two words. The control block starts with
 * its vptr followed by the 32-bit strong and weak counts. A strong owner holds
 * one implicit weak reference. Only copy from an already-owned native scope.
 * may_alias permits these ABI views over native fields of another nominal type. */
struct az_native_control;
struct az_native_type_info;
struct az_native_control_vtable {
    void (*complete_destroy)(struct az_native_control *);
    void (*deleting_destroy)(struct az_native_control *);
    void (*dispose)(struct az_native_control *);
    void (*destroy)(struct az_native_control *);
    void *(*deleter)(struct az_native_control *, const struct az_native_type_info *);
};
_Static_assert(offsetof(struct az_native_control_vtable, dispose) == 16 &&
                   offsetof(struct az_native_control_vtable, destroy) == 24,
               "native control methods ABI");
struct __attribute__((may_alias)) az_native_control {
    const struct az_native_control_vtable *vtable;
    int strong, weak;
};
struct __attribute__((may_alias)) az_native_owner {
    void *object;
    struct az_native_control *control;
};
_Static_assert(sizeof(struct az_native_owner) == 16, "native owner pair ABI");
_Static_assert(offsetof(struct az_native_control, strong) == 8 &&
                   offsetof(struct az_native_control, weak) == 12,
               "GNU atomic control ABI");
static inline void az_native_weak_add(struct az_native_control *c) {
    if (c) __atomic_fetch_add(&c->weak, 1, __ATOMIC_RELAXED);
}
static inline bool az_native_alive(const struct az_native_control *c) {
    return c && __atomic_load_n(&c->strong, __ATOMIC_RELAXED) > 0;
}
static inline void az_native_weak_drop(struct az_native_control *c) {
    if (c && __atomic_fetch_sub(&c->weak, 1, __ATOMIC_ACQ_REL) == 1)
        c->vtable->destroy(c);
}
static inline void az_native_strong_drop(struct az_native_control *c) {
    if (c && __atomic_fetch_sub(&c->strong, 1, __ATOMIC_ACQ_REL) == 1) {
        c->vtable->dispose(c);
        az_native_weak_drop(c);
    }
}
static inline void az_native_owner_reset(struct az_native_owner *p) {
    struct az_native_control *c = p->control;
    *p = (struct az_native_owner) {0};
    az_native_strong_drop(c);
}
static inline void az_native_weak_reset(struct az_native_owner *p) {
    struct az_native_control *c = p->control;
    *p = (struct az_native_owner) {0};
    az_native_weak_drop(c);
}
static inline struct az_native_owner az_native_lock(const struct az_native_owner *p) {
    if (!p->control) return (struct az_native_owner) {0};
    int count = __atomic_load_n(&p->control->strong, __ATOMIC_RELAXED);
    while (count && !__atomic_compare_exchange_n(&p->control->strong, &count, count + 1,
                                                 true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
    return count ? *p : (struct az_native_owner) {0};
}
static inline void az_native_owner_copy(struct az_native_owner *dst, const struct az_native_owner *src) {
    struct az_native_owner next = *src;
    if (next.control) __atomic_fetch_add(&next.control->strong, 1, __ATOMIC_RELAXED);
    az_native_owner_reset(dst);
    *dst = next;
}
static inline void az_native_weak_copy(struct az_native_owner *dst, const struct az_native_owner *src) {
    struct az_native_owner next = *src;
    az_native_weak_add(next.control);
    az_native_weak_reset(dst);
    *dst = next;
}
/* A C aggregate larger than two words uses the native indirect-result register
 * (x8 on AAPCS64; the first argument on SysV x86-64). The firmware writes only
 * its documented one- or two-word prefix. Never read the unused carrier bytes.
 * These are caller adapters, not native entry points: returning this carrier
 * to an eight-byte firmware destination would overwrite that destination. */
struct native_result_storage {
    uintptr_t first;
    uintptr_t second;
    unsigned char unused[8];
};
_Static_assert(sizeof(struct native_result_storage) == 24, "indirect native result ABI");

static inline void az_native_errno_restore(int *saved) { errno = *saved; }
#define AZ_NATIVE_ERRNO(name) int name __attribute__((cleanup(az_native_errno_restore))) = errno
#endif
