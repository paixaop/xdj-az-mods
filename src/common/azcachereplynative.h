/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEREPLYNATIVE_H
#define AZCACHEREPLYNATIVE_H
#include "azcachebirth.h"
#include "azcachereply.h"
#include "azcacheentry.h"
/* Same owned Reception invocation only. Stocker is opaque comparison context,
 * never a permission to dereference or reacquire it. PackID and RequestID are
 * distinct native types; neither is a source/track/player load generation. */
struct azcr_callback_receipt {
    struct azcr_receipt selected;
    uint32_t pack_id;
    uintptr_t borrowed_stocker;
};
static inline void azcr_callback_receipt_drop(struct azcr_callback_receipt *r) { azcr_receipt_drop(&r->selected); }
static inline void azcr_callback_receipt_copy(struct azcr_callback_receipt *dst, const struct azcr_callback_receipt *src) {
    azcr_receipt_copy(&dst->selected, &src->selected);
    dst->pack_id = src->pack_id;
    dst->borrowed_stocker = src->borrowed_stocker;
}
__attribute__((visibility("hidden"))) void azcr_forward_ticket_owned(struct azce_call *, struct azcb_forward);
__attribute__((visibility("hidden"))) void azcr_forward_reception_owned(struct azce_call *, struct azcb_forward);
__attribute__((visibility("hidden"))) bool azcr_current_callback_owned(struct azcr_callback_receipt *);
#endif
