/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_PROVIDER_CATALOG_H
#define AZMOD_SOURCE_PROVIDER_CATALOG_H

#include "source_import.h"

#pragma GCC visibility push(hidden)
/* Create a fresh OneLibrary for a core-owned carrier before mounting it. The
 * supplied key function is the core's native database writer. No donor tracks,
 * playlists, user settings or credentials are used. Worker-only. */
int azsp_catalog_create(const char *root, const struct az_library_sql *sql, int (*key)(void *));
/* On the carrier's owning library worker. Project a validated SDK page into
 * native rows. The optional IDs array receives stable local ID for each row. */
int azsp_catalog_publish(const struct azspi_sql *sql, void *db, const char *provider,
                         const struct azsp_request *request, const struct azsp_result *result,
                         uint32_t ids[AZ_SOURCE_PAGE_MAX]);

/* Read back the provider identity of a native carrier track. */
int azsp_catalog_lookup(const struct azspi_sql *sql, void *db, uint32_t id, char provider[33],
                        az_source_track_v1 *track);

/* Worker entry points for the attached carrier. They borrow the input until
 * return. The native worker is the only thread touching the live catalog. */
int azsp_catalog_apply(const char *provider, const struct azsp_request *request,
                       const struct azsp_result *result, uint32_t ids[AZ_SOURCE_PAGE_MAX]);
int azsp_catalog_track(uint32_t id, char provider[33], az_source_track_v1 *track);

enum azsp_catalog_stream_operation {
    AZSP_CATALOG_STREAM_ANALYZE = 1,
    AZSP_CATALOG_STREAM_BIND,
    AZSP_CATALOG_STREAM_CLEAR,
};

struct azsp_catalog_stream_request {
    uint64_t media;
    uint32_t id;
    const struct st_source *source;
    const char *audio, *dat;
    const struct azspi_record *record;
    uint64_t *analysis_ticket; /* AZSP_CATALOG_STREAM_ANALYZE output. */
};

/* Carrier worker only. Attach and detach exact bindings of an existing row. */
int azsp_catalog_stream_idle(const struct azspi_sql *sql, void *db, uint32_t id);
int azsp_catalog_stream_bind(const struct azspi_sql *sql, void *db, uint32_t id, const struct azspi_record *record);
int azsp_catalog_stream_clear(const struct azspi_sql *sql, void *db, uint32_t id, const char *audio, const char *dat);

/* Storage worker and message-thread wake boundary for the owning job queue. */
int azsp_catalog_stream(enum azsp_catalog_stream_operation operation, const struct azsp_catalog_stream_request *request);
int azsp_catalog_take_wake(void);
#pragma GCC visibility pop

#endif
