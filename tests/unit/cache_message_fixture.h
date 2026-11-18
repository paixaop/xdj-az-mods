/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CACHE_MESSAGE_FIXTURE_H
#define CACHE_MESSAGE_FIXTURE_H
#include <stdint.h>
#include <stdlib.h>
/* Independent retained native wire layout; storage belongs to the caller. */
struct message_arg {
    uint32_t tag, pad, value;
    unsigned char tail[36];
} __attribute__((aligned(8)));
struct fixture_message {
    uint16_t kind, reserved;
    uint32_t magic, sequence, pad;
    uint64_t unused[3];
    struct message_arg *begin, *end, *capacity;
};
_Static_assert(sizeof(struct fixture_message) == 64 && sizeof(struct message_arg) == 48, "native message prefix");
static inline struct fixture_message *message_new(unsigned context, uint32_t sequence) {
    struct fixture_message *p = calloc(1, sizeof *p);
    if (!p) abort();
    p->kind = 0x2102;
    p->magic = 0x872349ae;
    p->sequence = sequence;
    p->begin = calloc(2, sizeof *p->begin);
    if (!p->begin) abort();
    p->end = p->capacity = p->begin + 2;
    p->begin[0].tag = p->begin[1].tag = 1;
    p->begin[0].value = (context << 24) | ((context + 1) << 16);
    p->begin[1].value = 117;
    return p;
}
static inline void message_delete(struct fixture_message *p) {
    if (p) {
        free(p->begin);
        free(p);
    }
}
#endif
