/* SPDX-License-Identifier: GPL-3.0-only */
/* Test-only LD_PRELOAD observer, after core libraries and before libasound.
 * No audio-thread resolution, logging, allocation, waiting or clock reads.
 * Counters describe returned ALSA errors, not distinct audible dropouts.
 * Missing originals make that lane explicitly unsupported. Do not run an
 * acceptance arm unless all seven constructor-resolved lanes are ready. */
#define _GNU_SOURCE
#include "alsa-witness.h"
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

__attribute__((visibility("default"))) struct az_alsa_witness azsdk_alsa_witness_v1 = {
    .magic = AZ_ALSA_MAGIC, .version = 1, .size = sizeof(struct az_alsa_witness), .api_count = AZ_ALSA_APIS
};
static const char *const symbols[AZ_ALSA_APIS] = {
    "snd_pcm_writei", "snd_pcm_writen", "snd_pcm_readi", "snd_pcm_readn",
    "snd_pcm_avail_update", "snd_pcm_wait", "snd_pcm_recover"
};
__attribute__((constructor)) static void initialize(void) {
    const int saved = errno;
    uint64_t ready = 0, missing = 0;
    for (unsigned i = 0; i < AZ_ALSA_APIS; ++i) {
        void *p = dlsym(RTLD_NEXT, symbols[i]);
        atomic_store_explicit(&azsdk_alsa_witness_v1.next_address[i], (uintptr_t)p, memory_order_release);
        if (p) ready |= UINT64_C(1) << i;
        else missing |= UINT64_C(1) << i;
    }
    atomic_store_explicit(&azsdk_alsa_witness_v1.ready, ready, memory_order_release);
    atomic_fetch_or_explicit(&azsdk_alsa_witness_v1.unresolved, missing, memory_order_relaxed);
    atomic_store_explicit(&azsdk_alsa_witness_v1.initialized, 1, memory_order_release);
    errno = saved;
}
static void increment(_Atomic uint64_t *value, unsigned api) {
    if (atomic_fetch_add_explicit(value, 1, memory_order_relaxed) == UINT64_MAX)
        atomic_fetch_or_explicit(&azsdk_alsa_witness_v1.overflow, UINT64_C(1) << api, memory_order_relaxed);
}
static uintptr_t original(unsigned api) {
    uintptr_t p = atomic_load_explicit(&azsdk_alsa_witness_v1.next_address[api], memory_order_acquire);
    if (!p) {
        /* Do not resolve here: even an unexpected early call must not acquire
         * the dynamic loader lock on an audio thread. This run is invalid. */
        atomic_fetch_or_explicit(&azsdk_alsa_witness_v1.unresolved, UINT64_C(1) << api, memory_order_relaxed);
        return 0;
    }
    increment(&azsdk_alsa_witness_v1.api[api].entered, api);
    return p;
}
static void returned(unsigned api, long result) {
    struct az_alsa_count *c = &azsdk_alsa_witness_v1.api[api];
    if (result < 0) increment(&c->negative, api);
    if (result == -EPIPE) increment(&c->epipe, api);
    increment(&c->returned, api);
}
#define FORWARD(type, api) \
    uintptr_t address = original(api); \
    if (!address) return -ENOSYS; \
    type next; \
    _Static_assert(sizeof next == sizeof address, "function pointer ABI"); \
    memcpy(&next, &address, sizeof next)
#define RETURN(api, call) \
    long result = (call); \
    int saved = errno; \
    returned(api, result); \
    errno = saved; \
    return result
typedef long (*interleaved_write)(void *, const void *, unsigned long);
typedef long (*interleaved_read)(void *, void *, unsigned long);
typedef long (*planar)(void *, void **, unsigned long);
typedef long (*available)(void *);
typedef int (*waiter)(void *, int);
typedef int (*recoverer)(void *, int, int);
#define EXPORT __attribute__((visibility("default")))
EXPORT long snd_pcm_writei(void *pcm, const void *buffer, unsigned long frames) {
    FORWARD(interleaved_write, AZ_ALSA_WRITEI); RETURN(AZ_ALSA_WRITEI, next(pcm, buffer, frames));
}
EXPORT long snd_pcm_writen(void *pcm, void **buffer, unsigned long frames) {
    FORWARD(planar, AZ_ALSA_WRITEN); RETURN(AZ_ALSA_WRITEN, next(pcm, buffer, frames));
}
EXPORT long snd_pcm_readi(void *pcm, void *buffer, unsigned long frames) {
    FORWARD(interleaved_read, AZ_ALSA_READI); RETURN(AZ_ALSA_READI, next(pcm, buffer, frames));
}
EXPORT long snd_pcm_readn(void *pcm, void **buffer, unsigned long frames) {
    FORWARD(planar, AZ_ALSA_READN); RETURN(AZ_ALSA_READN, next(pcm, buffer, frames));
}
EXPORT long snd_pcm_avail_update(void *pcm) {
    FORWARD(available, AZ_ALSA_AVAIL); RETURN(AZ_ALSA_AVAIL, next(pcm));
}
EXPORT int snd_pcm_wait(void *pcm, int timeout) {
    FORWARD(waiter, AZ_ALSA_WAIT); RETURN(AZ_ALSA_WAIT, next(pcm, timeout));
}
EXPORT int snd_pcm_recover(void *pcm, int error, int silent) {
    FORWARD(recoverer, AZ_ALSA_RECOVER);
    if (error == -EPIPE) increment(&azsdk_alsa_witness_v1.recover_epipe_inputs, AZ_ALSA_RECOVER);
    RETURN(AZ_ALSA_RECOVER, next(pcm, error, silent));
}
