/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEMESSAGE_H
#define AZCACHEMESSAGE_H
#include "azcachebirth.h"
#include <string.h>
/* Copied integer request context, never a source/load identity. The caller owns
 * the complete native message and arguments during capture and the server for
 * the forwarded invocation. Bounds checks acquire no native liveness. */
struct azcm_borrow {
    unsigned build;
    const void *server, *message;
    uint32_t parameter;
};
struct azcm_envelope {
    uint16_t kind;
    uint32_t sequence, parameter, present, fields[8];
    unsigned client, selector; /* native vector positions, never deck IDs */
};
static inline bool azcm_canonical(uintptr_t p) { return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u); }
#define AZCM_SCALAR(T, p) ({ T value_;memcpy(&value_,(const void *)(uintptr_t)(p),sizeof value_);value_; })
static inline bool azcm_capture(const struct azcm_borrow *borrow, struct azcm_envelope *output) {
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    (void) borrow;
    (void) output;
    return false;
#else
    if ((borrow->build != 1 && borrow->build != 2) || !borrow->server) return false;
    uintptr_t message = (uintptr_t) borrow->message;
    if (!azcm_canonical(message)) return false;
    struct azcm_envelope value = {0};
    value.kind = AZCM_SCALAR(uint16_t, message);
    if (value.kind != 0x2102 && value.kind != 0x2002 && value.kind != 0x3000) return false;
    if (AZCM_SCALAR(uint32_t, message + 4) != UINT32_C(0x872349ae)) return false;
    value.sequence = AZCM_SCALAR(uint32_t, message + 8);
    value.parameter = borrow->parameter;
    uintptr_t begin = AZCM_SCALAR(uintptr_t, message + 0x28), end = AZCM_SCALAR(uintptr_t, message + 0x30), capacity = AZCM_SCALAR(uintptr_t, message + 0x38);
    /* Conservative observation quota, not a native protocol maximum. At most
     * five argument records are copied, irrespective of the vector length. */
    if (!azcm_canonical(begin) || !azcm_canonical(end) || !azcm_canonical(capacity) || end < begin || capacity < end ||
        (end - begin) % 48 || (capacity - begin) % 48 || (end - begin) / 48 > 4096 || (capacity - begin) / 48 > 4096) return false;
    uintptr_t count = (end - begin) / 48;
    unsigned required = value.kind == 0x3000 ? 4 : 2;
    if (count < required) return false;
    for (unsigned i = 0; i < 8; ++i) {
        if (i >= required && !(value.kind == 0x3000 && i == 7 && count > 7)) continue;
        uintptr_t argument = begin + (uintptr_t) i * 48;
        if (AZCM_SCALAR(uint32_t, argument) != 1) return false;
        value.fields[i] = AZCM_SCALAR(uint32_t, argument + 8);
        value.present |= 1u << i;
    }
    value.client = (value.fields[0] >> 24) & 255u;
    value.selector = (value.fields[0] >> 16) & 255u;
    if (value.selector < 1 || value.selector > 9) return false;
    *output = value;
    return true;
#endif
}
#undef AZCM_SCALAR
struct azcm_frame {
    struct azcm_frame *parent;
    const void *server;
    uintptr_t packet_key;
    struct azcm_envelope value;
    unsigned depth;
    bool valid;
};
extern _Thread_local struct azcm_frame *azcm_head __attribute__((visibility("hidden")));
static inline void azcm_frame_init(struct azcm_frame *f, const struct azcm_borrow *b) {
    *f = (struct azcm_frame) {.parent = azcm_head, .server = b->server, .packet_key = (uintptr_t) b->message};
    f->depth = f->parent ? (f->parent->depth < 32 ? f->parent->depth + 1 : 32) : 1;
    f->valid = (!f->parent || f->parent->depth < 32) && azcm_capture(b, &f->value);
    azcm_head = f; /* Ineligible or overflow calls still mask the older context. */
}
static inline void azcm_frame_drop(struct azcm_frame *f) { azcm_head = f->parent; }
static inline uintptr_t azcm_take_packet_key(const void *server) {
    struct azcm_frame *f = azcm_head;
    if (!f || !f->valid || f->server != server) return 0;
    uintptr_t key = f->packet_key;
    f->packet_key = 0;
    return key;
}
/* Teardown reads no native input; only copied scalars survive its destruction. */
static inline void azcm_forward_owned(const struct azcm_borrow *b, struct azcb_forward original) {
    struct azcm_frame frame __attribute__((cleanup(azcm_frame_drop)));
    azcm_frame_init(&frame, b);
    original.call(original.context);
}
static inline bool azcm_current(const void *server, struct azcm_envelope *out) {
    const struct azcm_frame *f = azcm_head;
    if (!server || !f || !f->valid || f->server != server) return false;
    *out = f->value;
    return true;
}
#endif
