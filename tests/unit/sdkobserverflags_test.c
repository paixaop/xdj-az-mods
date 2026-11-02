/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual native observer startup declarations and exports, queried during configuration. */
#include "../common/azdeckinfo.h"
#include "../common/azloopobserver.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define NDECK 4
static const struct azsdk_loop_sample loop_sample = {
    .unit_generation = 7, .revision = 11, .observed_ns = 123, .engaged = 1, .bounds_valid = 1, .in_s = 0, .out_s = 2.5};
static const struct azsdk_live_sample live_sample = {
    .unit_generation = 9, .observed_fields = 7, .valid_fields = 7, .mode_is_2 = 1, .play_pause = 0, .play_aux = 1, .quantize = 0, .key_lock = 1, .revisions = {12, 23, 34}, .observed_ns = {123, 234, 345}};
int azsdk_loop_read(int deck, struct azsdk_loop_sample *out) {
    assert(deck >= 0 && deck < NDECK);
    *out = loop_sample;
    return 1;
}
int azsdk_live_read(int deck, struct azsdk_live_sample *out) {
    assert(deck >= 0 && deck < NDECK);
    *out = live_sample;
    return 1;
}
unsigned azsdk_loop_capabilities(void) { return 3; }
unsigned azsdk_live_capabilities(void) { return 5; }
#include OBSERVER_FLAGS_INC
static _Atomic int start;
static void *configure(void *unused) {
    (void) unused;
    while (!atomic_load(&start)) {}
    for (unsigned i = 0; i < 20000; ++i) {
        g_sdk_loop_on = (int) (i & 1);
        g_sdk_live_on = (int) ((i >> 1) & 1);
    }
    return NULL;
}
static void *readers(void *live_only) {
    while (!atomic_load(&start)) {}
    /* Each reader touches one flag only. The other flag cannot accidentally
     * synchronize a deliberately restored non-atomic mutation with its writer. */
    for (unsigned i = 0; i < 10000; ++i) {
        if (live_only) {
            unsigned mask = azstem_live_observer_mask_v1();
            assert(mask == 0 || mask == 5);
            struct azstem_live_observation_v1 live = {.size = sizeof live, .version = 1}, before = live;
            int rc = azstem_live_observation_v1(i & 3, &live);
            assert(rc == 0 || rc == -1);
            if (rc) assert(!memcmp(&live, &before, sizeof live));
            else assert(!memcmp(&live.sample, &live_sample, sizeof live_sample));
        } else {
            unsigned mask = azstem_loop_observer_mask_v1();
            assert(mask == 0 || mask == 3);
            struct azstem_loop_observation_v1 loop1 = {.size = sizeof loop1, .version = 1}, before1 = loop1;
            struct azstem_loop_observation_v2 loop2 = {.size = sizeof loop2, .version = 2}, before2 = loop2;
            int a = azstem_loop_observation_v1(i & 3, &loop1);
            int b = azstem_loop_observation_v2(i & 3, &loop2);
            assert(a == 0 || a == -1);
            assert(b == 0 || b == -1);
            if (a) assert(!memcmp(&loop1, &before1, sizeof loop1));
            else assert(loop1.unit_generation == 7 && loop1.revision == 11 && loop1.engaged == 1);
            if (b) assert(!memcmp(&loop2, &before2, sizeof loop2));
            else assert(loop2.unit_generation == 7 && loop2.bounds_valid == 1 && loop2.in_s == 0 && loop2.out_s == 2.5);
        }
    }
    return NULL;
}
int main(void) {
    assert(!azstem_loop_observer_mask_v1() && !azstem_live_observer_mask_v1());
    pthread_t writer, first, second;
    assert(!pthread_create(&writer, NULL, configure, NULL));
    assert(!pthread_create(&first, NULL, readers, NULL));
    assert(!pthread_create(&second, NULL, readers, &second));
    atomic_store(&start, 1);
    assert(!pthread_join(writer, NULL) && !pthread_join(first, NULL) && !pthread_join(second, NULL));
    g_sdk_loop_on = g_sdk_live_on = 0;
    assert(!azstem_loop_observer_mask_v1() && !azstem_live_observer_mask_v1());
    puts("sdk observer flags: actual startup declarations, concurrent configuration and private exporter reads passed");
}
