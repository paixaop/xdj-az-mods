/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/azdeckinfo.h"
#define NDECK 4
static int g_lti_on = 1;
#include OBS_INC
int main(void) {
    uint32_t *prefix = malloc(sizeof *prefix);
    assert(prefix);
    *prefix = sizeof *prefix;
    assert(azstem_deck_observation_v1(0, (void *) prefix) == -1 && *prefix == sizeof *prefix);
    free(prefix);
    struct lti_snap *s = &g_lti_snap[2];
    atomic_store(&s->seq, 2);
    atomic_store(&s->owner, 7);
    atomic_store(&g_lti_tok[2], 7);
    atomic_store(&s->valid, 1);
    atomic_store(&s->observed_ns, 1234);
    __atomic_store_n(&s->id[0], 1, __ATOMIC_RELAXED);
    _Alignas(struct azstem_deck_observation_v1) unsigned char bytes[256];
    struct azstem_deck_observation_v1 *out = (void *) bytes;
    for (unsigned n = 0; n < sizeof *out; n++) {
        memset(bytes, 0xa5, sizeof bytes);
        out->size = n;
        out->version = 1;
        unsigned char before[256];
        memcpy(before, bytes, sizeof bytes);
        assert(azstem_deck_observation_v1(2, out) == -1 && !memcmp(before, bytes, sizeof bytes));
    }
    memset(bytes, 0xa5, sizeof bytes);
    out->size = sizeof *out + 32;
    out->version = 1;
    assert(!azstem_deck_observation_v1(2, out));
    assert(out->loaded == 1 && out->revision == 2 && out->observed_ns == 1234 && out->size == sizeof *out + 32);
    for (unsigned n = sizeof *out; n < sizeof bytes; n++) assert(bytes[n] == 0xa5);
    __atomic_store_n(&s->id[0], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s->id[1], UINT64_C(0xffffffff00000000), __ATOMIC_RELAXED);
    assert(!azstem_deck_observation_v1(2, out) && !out->loaded);
    __atomic_store_n(&s->id[1], UINT64_C(1), __ATOMIC_RELAXED);
    assert(!azstem_deck_observation_v1(2, out) && out->loaded);
    __atomic_store_n(&s->id[1], 0, __ATOMIC_RELAXED);
    atomic_store(&s->seq, 3);
    assert(azstem_deck_observation_v1(2, out) == -1);
    atomic_store(&s->seq, 4);
    atomic_store(&g_lti_tok[2], 9);
    assert(azstem_deck_observation_v1(2, out) == -1);
    atomic_store(&g_lti_tok[2], 7);
    atomic_store(&s->valid, 0);
    assert(azstem_deck_observation_v1(2, out) == -1);
    assert(azstem_deck_observation_v1(-1, out) == -1 && azstem_deck_observation_v1(4, out) == -1);
    out->version = 2;
    assert(azstem_deck_observation_v1(2, out) == -1);
    puts("sdk observation: sizes, guards, timestamps, empty state and stale owners passed");
}
