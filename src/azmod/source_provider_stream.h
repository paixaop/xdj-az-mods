/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_PROVIDER_STREAM_H
#define AZMOD_SOURCE_PROVIDER_STREAM_H

#include "source_provider.h"

#pragma GCC visibility push(hidden)

struct azsp_stream;

/* Storage worker. Audio must be staged inside the current carrier root. The
 * lease retains the stage independently of the caller's reference. A non-NULL
 * lease on error holds work that still needs retirement. */
int azsp_stream_prepare(int firmware, uint64_t carrier_media, uint32_t content_id,
                        const az_source_track_v1 *track, struct azsp_stage *stage,
                        int (*cancelled)(void *), void *context, struct azsp_stream **out);

const char *azsp_stream_path(const struct azsp_stream *stream);

/* After the analyzer releases the exact raw request, revoke the carrier row
 * before asking whether native load and preview readers have also released it.
 * A false predicate leaves the detached binding and lease available for retry.
 * AZ_OK consumes the lease. */
int azsp_stream_release_when(struct azsp_stream *stream, int (*last_reader)(void *), void *context);

/* Storage worker, after the last native consumer released this exact load.
 * AZ_OK consumes the lease. A retryable failure leaves ownership with the
 * caller so the audio remains available as long as native code can use it. */
int azsp_stream_release(struct azsp_stream *stream);

#pragma GCC visibility pop
#endif
