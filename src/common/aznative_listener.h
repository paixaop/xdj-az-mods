/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZNATIVE_LISTENER_H
#define AZNATIVE_LISTENER_H
#include "aznative_c.h"
/* Core listeners contain copied scalar state only. Their native weak owners
 * use the GNU control layout, without allocating or retaining native objects. */
struct az_native_listener;
struct az_native_notification;
struct az_native_notification_server;
struct az_native_listener_vtable {
    void (*destroy)(struct az_native_listener *);
    void (*delete_listener)(struct az_native_listener *);
    void (*notify)(struct az_native_listener *, const struct az_native_notification *);
    void (*notify_route)(struct az_native_listener *, const struct az_native_notification *, unsigned);
};
struct az_native_listener {
    const struct az_native_listener_vtable *vtable;
};
typedef void (*az_native_listener_register_fn)(struct az_native_notification_server *, struct az_native_owner *);
_Static_assert(sizeof(struct az_native_listener) == 8 &&
                   offsetof(struct az_native_listener_vtable, notify_route) == 24,
               "native listener ABI");
static inline void az_listener_destroy(struct az_native_control *control) { free(control); }
static inline void az_listener_dispose(struct az_native_control *control) { (void) control; }
static inline void *az_listener_deleter(struct az_native_control *control, const struct az_native_type_info *type) {
    (void) control;
    (void) type;
    return NULL;
}
#ifdef AZ_NATIVE_LISTENER_TESTING
void *az_listener_test_allocate(size_t size);
#endif
static inline struct az_native_owner az_listener_create(size_t size, const struct az_native_listener_vtable *vtable) {
    static const struct az_native_control_vtable control_vtable = {
        az_listener_dispose, az_listener_destroy, az_listener_dispose,
        az_listener_destroy, az_listener_deleter};
    if (size < sizeof(struct az_native_listener) || size > SIZE_MAX - sizeof(struct az_native_control))
        return (struct az_native_owner) {0};
#ifdef AZ_NATIVE_LISTENER_TESTING
    struct az_native_control *control = az_listener_test_allocate(sizeof *control + size);
#else
    struct az_native_control *control = calloc(1, sizeof *control + size);
#endif
    if (!control) return (struct az_native_owner) {0};
    control->vtable = &control_vtable;
    control->strong = control->weak = 1;
    struct az_native_listener *object = (struct az_native_listener *) (control + 1);
    object->vtable = vtable;
    return (struct az_native_owner) {object, control};
}
static inline void az_listener_register(az_native_listener_register_fn entry, struct az_native_notification_server *server,
                                        const struct az_native_owner *owner) {
    /* The temporary weak owner is released on return and on native unwind. */
    struct az_native_owner weak __attribute__((cleanup(az_native_weak_reset))) = *owner;
    az_native_weak_add(weak.control);
    entry(server, &weak);
}
#endif
