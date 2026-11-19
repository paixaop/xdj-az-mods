/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEREPLY_H
#define AZCACHEREPLY_H
#include "azcachebirth.h"
/* Private synchronous handoff from an owned Ticket's actual tag2 weak receiver.
 * RequestID's 16 bytes remain opaque. No raw Ticket/packet or strong Reception
 * survives the call. The entry bridge supplies ORIGINAL X21 and LR; its caller
 * must verify the native module and exact slot13 return site independently.
 * TLS routes invocations but proves neither native ownership nor code identity. */
struct azcr_request {
    uint64_t request_id[2];
    uint32_t sequence;
    uint16_t reply_kind;
};
struct azcr_promotion {
    uintptr_t control, caller_link;
};
struct azcr_receipt {
    struct azcr_request request;
    struct az_native_owner reception; /* weak */
};
static inline void azcr_receipt_drop(struct azcr_receipt *r) { az_native_weak_reset(&r->reception); }
static inline void azcr_receipt_copy(struct azcr_receipt *dst, const struct azcr_receipt *src) {
    if (dst == src) return;
    az_native_weak_copy(&dst->reception, &src->reception);
    dst->request = src->request;
}
struct azcr_scope {
    struct az_native_owner reception; /* genuine weak copy, never promoted at entry */
    struct azcr_request request;
    uintptr_t return_pc;
    bool used, active;
};
extern _Thread_local struct azcr_scope *azcr_current __attribute__((visibility("hidden")));
static inline void azcr_scope_init(struct azcr_scope *s, const struct az_native_owner *receiver,
                                   const struct azcr_request *request, uintptr_t return_pc) {
    *s = (struct azcr_scope) {.request = *request, .return_pc = return_pc};
    az_native_weak_copy(&s->reception, receiver);
}
static inline void azcr_scope_drop(struct azcr_scope *s) { az_native_weak_reset(&s->reception); }
struct azcr_forward_scope {
    struct azcr_scope *scope, *previous;
};
static inline struct azcr_forward_scope azcr_forward_begin(struct azcr_scope *s) {
    struct azcr_forward_scope f = {s, azcr_current};
    s->active = !s->used;
    s->used = true;
    azcr_current = s;
    return f;
}
static inline void azcr_forward_end(struct azcr_forward_scope *f) {
    f->scope->active = false;
    azcr_current = f->previous;
}
static inline void azcr_forward_owned(struct azcr_scope *s, struct azcb_forward original) {
    struct azcr_forward_scope frame __attribute__((cleanup(azcr_forward_end))) = azcr_forward_begin(s);
    original.call(original.context);
}
static inline int azcr_forward_result(struct azcr_scope *s, int (*original)(void *), void *context) {
    struct azcr_forward_scope frame __attribute__((cleanup(azcr_forward_end))) = azcr_forward_begin(s);
    return original(context);
}
static inline bool azcr_scope_matches(const struct azcr_scope *s, const struct azcr_promotion *native, const void *borrowed_receiver) {
    return azcr_current == s && s->active && s->return_pc && native->caller_link == s->return_pc && native->control && borrowed_receiver &&
           s->reception.object == borrowed_receiver && (uintptr_t) s->reception.control == native->control;
}
static inline bool azcr_receive_owned(struct azcr_scope *s, const struct azcr_promotion *native,
                                      const void *borrowed_receiver, struct azcr_receipt *out) {
    if (!azcr_scope_matches(s, native, borrowed_receiver)) return false;
    /* Native already owns this receiver during its callback. Promotion ends
     * before the caller forwards; output retains only a checked weak owner. */
    struct az_native_owner lease __attribute__((cleanup(az_native_owner_reset))) = az_native_lock(&s->reception);
    if (!lease.object || lease.object != borrowed_receiver || (uintptr_t) lease.control != native->control) return false;
    az_native_weak_copy(&out->reception, &lease);
    out->request = s->request;
    return true;
}
static inline bool azcr_receive_current_owned(const struct azcr_promotion *native, const void *receiver, struct azcr_receipt *out) {
    return azcr_current && azcr_receive_owned(azcr_current, native, receiver, out);
}
/* SDK stack identity prevents even an identical nested Ticket from borrowing
 * the outer callback's copied receipt. It names no native object. */
static inline const void *azcr_current_invocation(void) { return azcr_current; }
#endif
