/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_source.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static void *unbound_worker(void *slot) {
    assert(!analysis_owner_for_caller(*(unsigned *) slot).generation);
    return NULL;
}
int main(void) {
    assert(!analysis_owner_get(32).generation);
    assert(!analysis_owner_begin(32).generation);
    assert(!analysis_owner_retire(32).generation);
    assert(!analysis_owner_current((az_analysis_owner) {1, 0, 0}));
    assert(!analysis_owner_current((az_analysis_owner) {1, 1, AZ_ANALYSIS_OWNER_CORE}));
    assert(!analysis_owner_current((az_analysis_owner) {1, 32, AZ_ANALYSIS_OWNER_MOD}));
    assert(!analysis_owner_current(analysis_owner_get(0)));
    az_analysis_owner core = analysis_owner_core_begin();
    az_analysis_owner old = analysis_owner_begin(0);
    az_analysis_owner other = analysis_owner_begin(1);
    assert(analysis_owner_current(core) && analysis_owner_current(old) && analysis_owner_current(other));
    unsigned slot = 0;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, unbound_worker, &slot));
    assert(!pthread_join(thread, NULL));
    analysis_owner_thread_bind(old);
    assert(analysis_owner_for_caller(0).generation == old.generation);
    assert(!analysis_owner_for_caller(1).generation);
    az_analysis_owner retired = analysis_owner_retire(0);
    assert(retired.generation == old.generation && !analysis_owner_current(old));
    assert(analysis_owner_current(core) && analysis_owner_current(other));
    az_analysis_owner replacement = analysis_owner_begin(0);
    assert(replacement.generation != old.generation);
    assert(!analysis_owner_current(analysis_owner_for_caller(0)));
    assert(!pthread_create(&thread, NULL, unbound_worker, &slot));
    assert(!pthread_join(thread, NULL));
    analysis_owner_thread_bind(replacement);
    assert(analysis_owner_current(analysis_owner_for_caller(0)));
    analysis_owner_core_retire();
    assert(!analysis_owner_current(core) && !analysis_owner_core().generation);
    assert(analysis_owner_current(replacement) && analysis_owner_current(other));
    assert(analysis_owner_core_begin().generation != core.generation);
    puts("Analysis owner lifetimes: qualified workers, replacement epochs and core isolation passed");
}
