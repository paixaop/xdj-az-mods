/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZNATIVE_SYNC_H
#define AZNATIVE_SYNC_H
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
struct az_mutex_guard {
    pthread_mutex_t *mutex;
    bool held;
    int error;
};
static inline struct az_mutex_guard az_mutex_acquire(pthread_mutex_t *mutex, bool try_only) {
    int error = try_only ? pthread_mutex_trylock(mutex) : pthread_mutex_lock(mutex);
    return (struct az_mutex_guard) {mutex, !error, error};
}
static inline void az_mutex_release(struct az_mutex_guard *guard) {
    if (guard->held) {
        guard->held = false;
        if (pthread_mutex_unlock(guard->mutex)) abort();
    }
}
static inline void az_mutex_reacquire(struct az_mutex_guard *guard) {
    if (!guard->held) {
        guard->error = pthread_mutex_lock(guard->mutex);
        guard->held = !guard->error;
        if (guard->error) abort();
    }
}
#define AZ_MUTEX_GUARD(name, mutex, try_only) \
    struct az_mutex_guard name __attribute__((cleanup(az_mutex_release))) = az_mutex_acquire(&(mutex), try_only)
/* Uncaught failure had terminate semantics at the original noexcept boundary. */
#define AZ_MUTEX_LOCK(name, mutex)      \
    AZ_MUTEX_GUARD(name, mutex, false); \
    if (name.error) abort()
#endif
