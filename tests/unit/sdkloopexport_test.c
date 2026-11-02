/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile the real two private exports: versions, bounded writes, absent samples and EXIT. */
#include "../common/azdeckinfo.h"
#include "../common/azloopobserver.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NDECK 4
static int g_sdk_loop_on = 1, ready = 1;
static struct azsdk_loop_sample current = {
    .unit_generation = 7, .revision = 12, .observed_ns = 12345, .engaged = 1, .bounds_valid = 1, .in_s = 0.0, .out_s = 2.5};
int azsdk_loop_read(int deck, struct azsdk_loop_sample *out) {
    assert(deck >= 0 && deck < NDECK);
    if (!ready) return 0;
    *out = current;
    return 1;
}
#include LOOP_EXPORT_INC
static void check(int version, int should_fill) {
    _Alignas(struct azstem_loop_observation_v2) unsigned char bytes[128], before[128];
    size_t required = version == 1 ? sizeof(struct azstem_loop_observation_v1) : sizeof(struct azstem_loop_observation_v2);
    for (unsigned n = 0; n < sizeof bytes; n++) {
        memset(bytes, 0xa5, sizeof bytes);
        unsigned header[2] = {n, (unsigned) version};
        memcpy(bytes, header, sizeof header);
        memcpy(before, bytes, sizeof bytes);
        int rc = version == 1 ? azstem_loop_observation_v1(0, (void *) bytes) : azstem_loop_observation_v2(0, (void *) bytes);
        if (n < required || !should_fill) {
            assert(rc == -1 && !memcmp(bytes, before, sizeof bytes));
        } else {
            assert(!rc);
            const struct azstem_loop_observation_v1 *v = (const void *) bytes;
            assert(v->size == n && v->version == (unsigned) version);
            assert(v->unit_generation == 7 && v->revision == 12 && v->observed_ns == 12345 && v->engaged == current.engaged);
            if (version == 1) assert(!v->reserved);
            else {
                const struct azstem_loop_observation_v2 *v2 = (const void *) bytes;
                assert(v2->bounds_valid == current.bounds_valid && v2->in_s == current.in_s && v2->out_s == current.out_s);
            }
            assert(!memcmp(bytes + required, before + required, sizeof bytes - required));
        }
    }
}
int main(void) {
    uint32_t *prefix = malloc(sizeof *prefix);
    assert(prefix);
    *prefix = sizeof *prefix;
    assert(azstem_loop_observation_v1(0, (void *) prefix) == -1 && *prefix == sizeof *prefix);
    assert(azstem_loop_observation_v2(0, (void *) prefix) == -1 && *prefix == sizeof *prefix);
    free(prefix);
    assert(sizeof(struct azstem_loop_observation_v1) == 40);
    assert(sizeof(struct azstem_loop_observation_v2) == 56);
    check(1, 1);
    check(2, 1);
    current.engaged = current.bounds_valid = 0;
    current.in_s = current.out_s = 0;
    check(1, 1);
    check(2, 1);
    ready = 0;
    check(1, 0);
    check(2, 0);
    ready = 1;
    g_sdk_loop_on = 0;
    check(1, 0);
    check(2, 0);
    g_sdk_loop_on = 1;
    struct azstem_loop_observation_v1 v1 = {sizeof v1, 2, 0, 0, 0, 0, 0};
    struct azstem_loop_observation_v2 v2 = {sizeof v2, 1, 0, 0, 0, 0, 0, 0, 0};
    assert(azstem_loop_observation_v1(0, &v1) == -1 && azstem_loop_observation_v2(0, &v2) == -1);
    v1.version = 1;
    v2.version = 2;
    assert(azstem_loop_observation_v1(-1, &v1) == -1 && azstem_loop_observation_v1(4, &v1) == -1);
    assert(azstem_loop_observation_v2(-1, &v2) == -1 && azstem_loop_observation_v2(4, &v2) == -1);
    assert(azstem_loop_observation_v1(0, NULL) == -1 && azstem_loop_observation_v2(0, NULL) == -1);
    puts("sdk loop exports: v1 preserved; v2 bounds, versions, sizes, absent data and EXIT passed");
}
