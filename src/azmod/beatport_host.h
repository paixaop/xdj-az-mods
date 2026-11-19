/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_BEATPORT_HOST_H
#define AZ_BEATPORT_HOST_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#pragma GCC visibility push(hidden)
/* Narrow native operations supplied by azmod's existing host. Weak slots use
 * its single destructor registry; native hooks use its original install chain.
 * No host state or native object is exported to an SDK client. */
int bp_host_thread_create(pthread_t *, const pthread_attr_t *, void *(*) (void *), void *);
int bp_host_message_thread(void);
long bp_host_now_ms(void);
uintptr_t bp_host_weak_get(_Atomic uintptr_t *);
int bp_host_weak_set(_Atomic uintptr_t *, uintptr_t);
int bp_host_weak_track(_Atomic uintptr_t *, int, int);
int bp_host_showing(uintptr_t);
int bp_host_install_tick(const char *);
int bp_host_install_hook(uintptr_t, void *, void **);
#pragma GCC visibility pop
#endif
