/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NATIVE_ABI_FIXTURE_H
#define NATIVE_ABI_FIXTURE_H
/* C model of the firmware's owned-object ABI, independent of production helpers. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>
struct fixture_control {
    const void *const *vtable;
    int strong, weak;
};
struct fixture_owner {
    void *object;
    struct fixture_control *control;
};
static inline void fixture_weak_drop(struct fixture_owner *p) {
    if (p->control && __atomic_sub_fetch(&p->control->weak, 1, __ATOMIC_ACQ_REL) == 0) free(p->control);
    memset(p, 0, sizeof *p);
}
static inline void fixture_drop(struct fixture_owner *p) {
    if (p->control && __atomic_sub_fetch(&p->control->strong, 1, __ATOMIC_ACQ_REL) == 0) {
        ((void (*)(void *)) p->control->vtable[2])(p->control);
        fixture_weak_drop(p);
    }
    memset(p, 0, sizeof *p);
}
static inline struct fixture_owner fixture_copy(struct fixture_owner p) {
    if (p.control) __atomic_add_fetch(&p.control->strong, 1, __ATOMIC_RELAXED);
    return p;
}
static inline struct fixture_owner fixture_weak(struct fixture_owner p) {
    if (p.control) __atomic_add_fetch(&p.control->weak, 1, __ATOMIC_RELAXED);
    return p;
}
static inline struct fixture_owner fixture_lock(struct fixture_owner p) {
    if (!p.control) return (struct fixture_owner) {0};
    int count = __atomic_load_n(&p.control->strong, __ATOMIC_ACQUIRE);
    while (count && !__atomic_compare_exchange_n(&p.control->strong, &count, count + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    return count ? p : (struct fixture_owner) {0};
}
static inline bool fixture_expired(struct fixture_owner p) {
    return !p.control || __atomic_load_n(&p.control->strong, __ATOMIC_ACQUIRE) == 0;
}
static inline struct fixture_owner fixture_make(size_t size, const void *const *vtable) {
    struct fixture_control *p = calloc(1, sizeof *p + size);
    assert(p);
    p->vtable = vtable;
    p->strong = p->weak = 1;
    return (struct fixture_owner) {p + 1, p};
}
static inline void fixture_destroy(void *p) { free(p); }
static inline void fixture_noop(void *p) { (void) p; }
static inline void *fixture_deleter(void *p, const void *type) {
    (void) p;
    (void) type;
    return NULL;
}
#define FIXTURE_VTABLE(name, dispose) static const void *const name[] = {                                    \
                                          (void *) fixture_noop, (void *) fixture_destroy, (void *) dispose, \
                                          (void *) fixture_destroy, (void *) fixture_deleter}
/* A foreign native unwind in C exercises the same unwinder and cleanup path.
 * The payload is owned until the catch boundary deletes the exception. */
static unsigned fixture_exception_deletions;
static void fixture_exception_cleanup(_Unwind_Reason_Code reason, struct _Unwind_Exception *value) {
    (void) reason;
    ++fixture_exception_deletions;
    free(value);
}
static __attribute__((noreturn, unused)) void fixture_raise(void) {
    struct _Unwind_Exception *value = calloc(1, sizeof *value);
    assert(value);
    value->exception_class = UINT64_C(0x415a544553544300);
    value->exception_cleanup = fixture_exception_cleanup;
    (void) _Unwind_RaiseException(value);
    abort();
}
int az_native_try(void (*call)(void *), void *context);
#endif
