/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZNATIVE_LISTENER_H
#define AZNATIVE_LISTENER_H
#include "aznative_c.h"
/* Core listeners contain copied scalar state only. Their native weak owners
 * use the GNU control layout, without allocating or retaining native objects. */
static inline void az_listener_destroy(void *control) { free(control); }
static inline void az_listener_dispose(void *control) { (void) control; }
static inline void *az_listener_deleter(void *control, const void *type) {
    (void) control;
    (void) type;
    return NULL;
}
#ifdef AZ_NATIVE_LISTENER_TESTING
void *az_listener_test_allocate(size_t size);
#endif
static inline struct az_native_owner az_listener_create(size_t size, const void *const *vtable) {
    static const void *const control_vtable[] = {
        (void *) az_listener_dispose, (void *) az_listener_destroy, (void *) az_listener_dispose,
        (void *) az_listener_destroy, (void *) az_listener_deleter};
    if (size < sizeof vtable || size > SIZE_MAX - sizeof(struct az_native_control))
        return (struct az_native_owner) {0};
#ifdef AZ_NATIVE_LISTENER_TESTING
    struct az_native_control *control = az_listener_test_allocate(sizeof *control + size);
#else
    struct az_native_control *control = calloc(1, sizeof *control + size);
#endif
    if (!control) return (struct az_native_owner) {0};
    control->vtable = control_vtable;
    control->strong = control->weak = 1;
    void *object = control + 1;
    *(const void *const **) object = vtable;
    return (struct az_native_owner) {object, control};
}
static inline void az_listener_register(uintptr_t entry, uintptr_t server, const struct az_native_owner *owner) {
    /* The temporary weak owner is released on return and on native unwind. */
    struct az_native_owner weak __attribute__((cleanup(az_native_weak_reset))) = *owner;
    az_native_weak_add(weak.control);
    ((void (*)(void *, struct az_native_owner *)) entry)((void *) server, &weak);
}
#endif
