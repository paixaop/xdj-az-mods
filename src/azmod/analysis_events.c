/* SPDX-License-Identifier: GPL-3.0-only */
#include "analysis_events.h"
#include <pthread.h>
#define AE_CAPACITY 64u
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t event_sequence;
static az_analysis_complete_v1 events[AE_CAPACITY];
static struct analysis_listener core_listeners[8];

void analysis_events_publish(const az_analysis_complete_v1 *event) {
    pthread_mutex_lock(&event_lock);
    if (event_sequence < UINT64_MAX) {
        az_analysis_complete_v1 *copy = &events[event_sequence % AE_CAPACITY];
        *copy = *event;
        copy->sequence = ++event_sequence;
        copy->size = sizeof *copy;
        copy->version = 1;
        copy->missed = 0;
        copy->reserved = 0;
    }
    pthread_mutex_unlock(&event_lock);
}
int analysis_events_listen(struct analysis_listener *listener, az_analysis_complete_fn fn, void *user) {
    if (!listener || !fn) return AZ_INVALID;
    if (listener->callback) return AZ_LIMIT;
    pthread_mutex_lock(&event_lock);
    *listener = (struct analysis_listener) {.callback = fn, .user = user, .after = event_sequence};
    pthread_mutex_unlock(&event_lock);
    return AZ_OK;
}
static int ae_next(uint64_t after, az_analysis_complete_v1 *out) {
    if (after >= event_sequence) return AZ_NOT_FOUND;
    uint64_t next = after + 1;
    uint64_t first = event_sequence > AE_CAPACITY ? event_sequence - AE_CAPACITY + 1 : 1;
    if (next < first) next = first;
    *out = events[(next - 1) % AE_CAPACITY];
    out->missed = next != after + 1;
    return AZ_OK;
}
int analysis_events_take(struct analysis_listener *listener, az_analysis_complete_v1 *out) {
    if (!listener || !listener->callback || !out) return AZ_INVALID;
    if (pthread_mutex_trylock(&event_lock)) return AZ_BUSY;
    int rc = ae_next(listener->after, out);
    if (!rc) listener->after = out->sequence;
    pthread_mutex_unlock(&event_lock);
    return rc;
}
int analysis_events_core_listen(az_analysis_complete_fn fn, void *user) {
    for (unsigned i = 0; i < 8; ++i)
        if (!core_listeners[i].callback) return analysis_events_listen(&core_listeners[i], fn, user);
    return AZ_LIMIT;
}
static void ae_deliver(struct analysis_listener *listener) {
    if (!listener->callback) return;
    for (unsigned i = 0; i < 16; ++i) {
        az_analysis_complete_v1 event;
        if (analysis_events_take(listener, &event)) break;
        listener->callback(&event, listener->user);
    }
}
void analysis_events_core_tick(void) {
    for (unsigned i = 0; i < 8; ++i) ae_deliver(&core_listeners[i]);
}
