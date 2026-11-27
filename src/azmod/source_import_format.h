/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_IMPORT_FORMAT_H
#define AZMOD_SOURCE_IMPORT_FORMAT_H

#include "source_import.h"

/* Storage worker, after native startup. Inspect the actual published audio on
 * the qualified source. On success, duration comes from the native decoder's
 * sample clock and bitrate is the average over the file bytes. */
__attribute__((visibility("hidden"))) int azspi_probe_format(int firmware, const struct st_source *source,
                                                             const char *audio, struct azspi_audio *out);

#endif
