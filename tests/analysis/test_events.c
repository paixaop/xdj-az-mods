/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_events.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
static atomic_int done;
static unsigned calls;
static void received(const az_analysis_complete_v1 *event, void *user) {
    (void) user;
    assert(event->request_id && event->available_components == AZ_ANALYSIS_VOCALS);
    ++calls;
}
static void *produce(void *unused) {
    (void) unused;
    for (uint64_t i = 1; i <= 1000; ++i) {
        az_analysis_complete_v1 event = {.request_id = i, .load_epoch = i + 7, .available_components = AZ_ANALYSIS_VOCALS};
        analysis_events_publish(&event);
        sched_yield();
    }
    atomic_store(&done, 1);
    return NULL;
}
int main(void) {
    struct analysis_listener listener = {0};
    assert(analysis_events_listen(NULL, received, NULL) == AZ_INVALID);
    assert(analysis_events_listen(&listener, NULL, NULL) == AZ_INVALID);
    assert(analysis_events_listen(&listener, received, NULL) == AZ_OK);
    assert(analysis_events_listen(&listener, received, NULL) == AZ_LIMIT);
    az_analysis_complete_v1 event;
    assert(analysis_events_take(NULL, &event) == AZ_INVALID);
    assert(analysis_events_take(&listener, NULL) == AZ_INVALID);
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, produce, NULL));
    uint64_t last = 0;
    for (;;) {
        int rc = analysis_events_take(&listener, &event);
        if (rc == AZ_OK) {
            assert(event.sequence > last && event.sequence == event.request_id);
            assert(event.load_epoch == event.request_id + 7 && event.version == 1);
            if (!event.missed) assert(event.sequence == last + 1);
            last = event.sequence;
        } else {
            assert(rc == AZ_BUSY || rc == AZ_NOT_FOUND);
            if (atomic_load(&done) && last == 1000) break;
            sched_yield();
        }
    }
    assert(!pthread_join(producer, NULL));
    for (unsigned i = 0; i < 8; ++i) assert(analysis_events_core_listen(received, NULL) == AZ_OK);
    assert(analysis_events_core_listen(received, NULL) == AZ_LIMIT);
    analysis_events_core_tick();
    assert(!calls);
    event = (az_analysis_complete_v1) {.request_id = 1001, .available_components = AZ_ANALYSIS_VOCALS};
    analysis_events_publish(&event);
    analysis_events_core_tick();
    assert(calls == 8);
    analysis_events_core_tick();
    assert(calls == 8);
    puts("Analysis events: concurrent ordered publication, gap detection, independent core listeners passed");
}
