/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_IMPORT_ANALYZER_H
#define AZMOD_SOURCE_IMPORT_ANALYZER_H

#include "track_storage.h"

#pragma GCC visibility push(hidden)

#define AZSPI_NATIVE_TRACK_BYTES 12u
#define AZSPI_NATIVE_REQUESTER_BYTES 12u

typedef int (*azspi_analyzer_arm_fn)(const uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES],
                                     const uint8_t requester[AZSPI_NATIVE_REQUESTER_BYTES], void *context);

/* Expected physical USB locator for a validated source-relative ASCII audio path.
 * A hash match alone does not establish ownership of an existing fileset. */
int azspi_analyzer_locator(const char *audio, char out[AZ_SOURCE_PATH_BYTES]);

/* Call on the qualified native library server worker, after publishing audio on
 * the current mount and verifying that it can claim the output locator. On
 * AZ_PENDING, raw_id identifies the request passed to the native analyzer queue.
 * The native owner processes it asynchronously. */
int azspi_analyzer_submit(int firmware, void *database, uint16_t media,
                          const struct st_source *source, const char *audio,
                          uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES]);

/* The caller's observer is armed against the native track and requester before
 * AnalyzeManager can schedule the request. An arm failure prevents submission. */
int azspi_analyzer_submit_watched(int firmware, void *database, uint16_t media,
                                  const struct st_source *source, const char *audio,
                                  uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES], azspi_analyzer_arm_fn arm, void *context);

#pragma GCC visibility pop
#endif
