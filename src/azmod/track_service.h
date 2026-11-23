/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_TRACK_SERVICE_H
#define AZ_TRACK_SERVICE_H
#include "azmod-mod.h"
#include <pthread.h>
#pragma GCC visibility push(hidden)
struct st_data;
void track_service_configure(int enabled);
int track_service_enabled(void);
void track_service_owner_init(int owner);
void track_service_owner_retire(int owner);
int track_service_owner_retired(int owner);
int track_service_event_head(uint64_t *sequence);
int track_service_start_worker(void);
void track_service_wake(void);
void track_service_status_register(void (*jobs)(void));
int track_service_status_started(void);
/* Worker operations; only one worker may drive the provider/cache at a time. */
void track_service_work(void);
int track_service_disk_load(const az_track_ref *, uint32_t, struct st_data *, void *);
int track_service_disk_fresh(const az_track_ref *, const struct st_data *, void *);
int track_service_capabilities(az_data_capabilities *);
int track_service_resolve(int stick, uint32_t content_id, az_track_ref *);
int track_service_request(int owner, const az_track_ref *, uint32_t, uint64_t *);
int track_service_acquire(int owner, uint64_t, const az_track_snapshot **);
int track_service_wave_request(int owner, const az_track_ref *, const az_wave_query *, uint64_t *);
int track_service_wave_acquire(int owner, uint64_t, const az_wave_snapshot **);
int track_service_release(int owner, uint64_t, int cancel);
int track_service_event(int owner, uint64_t, az_data_change *);
/* Host-owned thread trampoline preserves alternate-stack setup. */
int az_storage_thread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
#pragma GCC visibility pop
#endif
