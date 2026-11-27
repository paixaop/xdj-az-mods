/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_IMPORT_H
#define AZMOD_SOURCE_IMPORT_H

#include "library_sql.h"
#include "source_provider.h"
#include "track_storage.h"

#pragma GCC visibility push(hidden)

struct azspi_audio {
    /* Copied from the native decoder for the exact audio file. */
    uint32_t file_type, sample_rate, bitrate_kbps, duration_ms;
};

struct azspi_asset {
    char relative[AZ_SOURCE_PATH_BYTES];
    uint64_t signature;
};

struct azspi_record {
    struct st_source source;
    az_source_track_v1 track;
    struct azspi_audio format;
    struct azspi_asset audio, analysis[3]; /* DAT, EXT, 2EX */
    uint64_t file_size;
    uint32_t bpm_x100;
};

struct azspi_sql {
    const struct az_library_sql *api;
    int (*bind_text)(void *, int, const char *, int, void (*)(void *));
    int (*autocommit)(void *);
};

/* Storage worker, after native analysis. Require a complete native fileset for
 * this exact USB audio path. Copy a validated record only on success. */
int azspi_inspect(const struct st_source *source, const az_source_track_v1 *track,
                  const struct azspi_audio *format, const char *audio, const char *dat,
                  struct azspi_record *out);

/* Native owning library worker. Verify that the validated assets have retained
 * their path and stat identity before exposing the new row. */
int azspi_current(const struct azspi_record *record);
int azspi_commit(const struct azspi_sql *sql, void *database, const struct azspi_record *record,
                 uint32_t *track_id);

/* Core-owned queue into the destination's native library worker. A poll
 * consumes the terminal result. Cancellation can win only before execution. */
int azspi_submit(const struct azspi_record *record, unsigned media, uint64_t *ticket);
int azspi_poll(uint64_t ticket, uint32_t *track_id);
int azspi_submit_analysis(const struct st_source *source, unsigned media, const char *audio,
                          uint64_t *ticket);
/* On AZ_OK, transfer the native terminal watch to the storage worker. */
int azspi_poll_analysis(uint64_t ticket, uint64_t *analysis_ticket);
int azspi_cancel(uint64_t ticket);
/* Capture the native media number on the server owning this exact physical
 * mount. Pass the captured source unchanged to subsequent requests. */
int azspi_submit_destination(const struct st_source *source, uint64_t *ticket);
int azspi_poll_destination(uint64_t ticket, unsigned *media);

struct azspi_destination {
    struct st_source source;
    unsigned media;
};

/* Worker only. Use the first port with a OneLibrary export, USB1 then USB2. */
int azspi_import_destination(struct azspi_destination *out, int (*cancelled)(void *), void *context);

struct azspi_request {
    const struct st_source *source;
    unsigned media;
    int firmware;
    const char *provider;
    const az_source_track_v1 *track;
    const struct azsp_stage *stage;
    int (*cancelled)(void *);
    void *context;
};

/* Validate a private copy with the native decoder before publishing it on USB.
 * A repeated import of identical bytes reuses the file. Outputs change on success. */
int azspi_publish(const struct azspi_request *request, char relative[AZ_SOURCE_PATH_BYTES],
                  struct azspi_audio *format);

struct azspi_provider_load {
    void *source;
    const char *provider, *staging_root;
    const az_source_track_v1 *track;
    int firmware;
    int (*cancelled)(void *);
    void *context;
};

struct azspi_imported_track {
    unsigned media;
    uint32_t id;
};

/* Storage worker. Materialize one provider track. The returned ID can replace
 * a deferred carrier load using the destination's native Track category. */
int azspi_import_provider(const struct azspi_provider_load *load, struct azspi_imported_track *out);

/* Storage worker. Keep the prepared stage and request valid until return.
 * The resulting ID belongs to the destination's captured native media. */
int azspi_import_stage(const struct azspi_request *request, uint32_t *track_id);

#pragma GCC visibility pop
#endif
