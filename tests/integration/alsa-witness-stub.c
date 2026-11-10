/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent next provider for the preload regression; never used by firmware. */
#include "alsa-witness.h"
#include <errno.h>
#include <stddef.h>
struct stub_call { unsigned calls; void *pcm, *buffer; unsigned long frames; int argument, silent, entry_errno; };
static struct stub_call calls[AZ_ALSA_APIS];
static long results[AZ_ALSA_APIS];
static int errors[AZ_ALSA_APIS];
void witness_stub_set(unsigned api, long result, int error) { results[api] = result; errors[api] = error; }
const struct stub_call *witness_stub_call(unsigned api) { return &calls[api]; }
static long call(unsigned api, void *pcm, void *buffer, unsigned long frames, int argument, int silent) {
    unsigned n = calls[api].calls + 1;
    calls[api] = (struct stub_call){n, pcm, buffer, frames, argument, silent, errno};
    errno = errors[api];
    return results[api];
}
long snd_pcm_writei(void *p, const void *b, unsigned long n) { return call(AZ_ALSA_WRITEI, p, (void *)b, n, 0, 0); }
#ifndef AZ_ALSA_PARTIAL_STUB
long snd_pcm_writen(void *p, void **b, unsigned long n) { return call(AZ_ALSA_WRITEN, p, b, n, 0, 0); }
#endif
long snd_pcm_readi(void *p, void *b, unsigned long n) { return call(AZ_ALSA_READI, p, b, n, 0, 0); }
long snd_pcm_readn(void *p, void **b, unsigned long n) { return call(AZ_ALSA_READN, p, b, n, 0, 0); }
long snd_pcm_avail_update(void *p) { return call(AZ_ALSA_AVAIL, p, NULL, 0, 0, 0); }
int snd_pcm_wait(void *p, int n) { return (int)call(AZ_ALSA_WAIT, p, NULL, 0, n, 0); }
int snd_pcm_recover(void *p, int e, int s) { return (int)call(AZ_ALSA_RECOVER, p, NULL, 0, e, s); }
