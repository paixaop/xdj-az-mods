/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "alsa-witness.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct stub_call { unsigned calls; void *pcm, *buffer; unsigned long frames; int argument, silent, entry_errno; };
static const char *const names[AZ_ALSA_APIS] = {"snd_pcm_writei", "snd_pcm_writen", "snd_pcm_readi", "snd_pcm_readn", "snd_pcm_avail_update", "snd_pcm_wait", "snd_pcm_recover"};
static void *functions[AZ_ALSA_APIS];
static int token, storage[8];
static long invoke(unsigned api, int error) {
    if (api == AZ_ALSA_WRITEI) return ((long (*)(void *, const void *, unsigned long))functions[api])(&token, storage, 73);
    if (api == AZ_ALSA_WRITEN || api == AZ_ALSA_READN) return ((long (*)(void *, void **, unsigned long))functions[api])(&token, (void **)storage, 73);
    if (api == AZ_ALSA_READI) return ((long (*)(void *, void *, unsigned long))functions[api])(&token, storage, 73);
    if (api == AZ_ALSA_AVAIL) return ((long (*)(void *))functions[api])(&token);
    if (api == AZ_ALSA_WAIT) return ((int (*)(void *, int))functions[api])(&token, 987);
    return ((int (*)(void *, int, int))functions[api])(&token, error, 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    struct az_alsa_witness *w = dlsym(RTLD_DEFAULT, "azsdk_alsa_witness_v1");
    void (*set)(unsigned, long, int) = dlsym(RTLD_DEFAULT, "witness_stub_set");
    const struct stub_call *(*last)(unsigned) = dlsym(RTLD_DEFAULT, "witness_stub_call");
    assert(w && set && last && w->magic == AZ_ALSA_MAGIC && w->size == sizeof *w && atomic_load(&w->initialized));
    for (unsigned i = 0; i < AZ_ALSA_APIS; ++i) { functions[i] = dlsym(RTLD_DEFAULT, names[i]); assert(functions[i]); }
    if (!strcmp(argv[1], "unsupported")) {
        assert(atomic_load(&w->ready) == (AZ_ALSA_READY & ~(UINT64_C(1) << AZ_ALSA_WRITEN)));
        assert(atomic_load(&w->unresolved) == (UINT64_C(1) << AZ_ALSA_WRITEN));
        errno = EDOM;
        assert(invoke(AZ_ALSA_WRITEN, 0) == -ENOSYS && errno == EDOM);
        assert(!last(AZ_ALSA_WRITEN)->calls && !atomic_load(&w->api[AZ_ALSA_WRITEN].returned));
        puts("PASS missing original is explicitly unsupported, never a zero-error measurement"); return 0;
    }
    assert(atomic_load(&w->ready) == AZ_ALSA_READY && !atomic_load(&w->unresolved));
    if (!strcmp(argv[1], "overflow")) {
        atomic_store(&w->api[AZ_ALSA_WRITEI].returned, UINT64_MAX);
        set(AZ_ALSA_WRITEI, 73, EDOM);
        assert(invoke(AZ_ALSA_WRITEI, 0) == 73 && errno == EDOM);
        assert(atomic_load(&w->overflow) == (UINT64_C(1) << AZ_ALSA_WRITEI));
        assert(last(AZ_ALSA_WRITEI)->calls == 1);
        puts("PASS counter exhaustion is sticky and original still called exactly once"); return 0;
    }
    assert(!strcmp(argv[1], "forward"));
    const long values[] = {73, 7, 0, -EPIPE, -EAGAIN};
    for (unsigned api = 0; api < AZ_ALSA_APIS; ++api) {
        for (unsigned test = 0; test < sizeof values / sizeof values[0]; ++test) {
            set(api, values[test], ERANGE + (int)test);
            errno = EDOM;
            long rc = invoke(api, test == 2 ? -EPIPE : -EINTR);
            assert(rc == values[test] && errno == ERANGE + (int)test);
            const struct stub_call *c = last(api);
            assert(c->calls == test + 1 && c->pcm == &token && c->entry_errno == EDOM);
            if (api <= AZ_ALSA_READN) assert(c->buffer == storage && c->frames == 73);
            if (api == AZ_ALSA_WAIT) assert(c->argument == 987);
            if (api == AZ_ALSA_RECOVER) assert(c->argument == (test == 2 ? -EPIPE : -EINTR) && c->silent == 1);
        }
        assert(atomic_load(&w->api[api].entered) == 5 && atomic_load(&w->api[api].returned) == 5);
        assert(atomic_load(&w->api[api].negative) == 2 && atomic_load(&w->api[api].epipe) == 1);
    }
    assert(atomic_load(&w->recover_epipe_inputs) == 1 && !atomic_load(&w->overflow));
    puts("PASS seven real preload paths: exact arguments/one call/return/errno, short/zero/EPIPE/other errors; successful EPIPE recovery counted separately");
}
