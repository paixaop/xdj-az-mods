/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_PROVIDER_H
#define AZMOD_SOURCE_PROVIDER_H

#include "azmod-mod.h"

enum azsp_operation {
    AZSP_PLAYLISTS,
    AZSP_PLAYLIST_TRACKS,
    AZSP_SEARCH,
    AZSP_PREPARE_TRACK
};

struct azsp_request {
    enum azsp_operation operation;
    uint32_t offset, capacity;
    union {
        char playlist_id[AZ_SOURCE_ID_BYTES];
        char query[AZ_SOURCE_QUERY_BYTES];
        struct {
            az_source_track_v1 track;
            char directory[AZ_SOURCE_PATH_BYTES];
        } prepare;
    } value;
};

struct azsp_result {
    az_source_page_v1 page;
    union {
        az_source_playlist_v1 playlists[AZ_SOURCE_PAGE_MAX];
        az_source_track_v1 tracks[AZ_SOURCE_PAGE_MAX];
        char path[AZ_SOURCE_PATH_BYTES];
    } value;
};

struct azsp_stage;
/* The adapter snapshots the mode when a native load is admitted and selects a
 * staging parent on the corresponding storage. Release the final reference off
 * message/audio threads, after the import or all native file users have settled. */
int azsp_stage_create(const char *parent, enum az_source_storage_mode mode, struct azsp_stage **out);
void azsp_stage_retain(struct azsp_stage *stage);
void azsp_stage_release(struct azsp_stage *stage);
const char *azsp_stage_directory(const struct azsp_stage *stage);
const char *azsp_stage_path(const struct azsp_stage *stage);
enum az_source_storage_mode azsp_stage_mode(const struct azsp_stage *stage);
void azsp_stage_set_path(struct azsp_stage *stage, const struct azsp_result *result);

/* Private worker boundary. The caller keeps the provider alive, serializes calls
 * per provider and owns request cancellation and native message delivery. An
 * adapter may use the output only when the return value is AZ_OK. */
int azsp_provider_valid(const az_source_provider_v1 *provider);
unsigned azsp_storage_flag(enum az_source_storage_mode mode);
int azsp_run(const az_source_provider_v1 *provider, const struct azsp_request *request,
             struct azsp_result *result);

/* A firmware profile with process lifetime installs this after its native library and load
 * adapters are ready. Source tokens are opaque for the life of the process.
 * SOURCE selection methods run on the firmware message thread. */
struct azsp_native {
    unsigned storage_modes; /* Each ready mode's azsp_storage_flag(). */
    int (*select)(void *source, void *source_list_model); /* AZ_OK once firmware accepts it */
    void (*clear)(void);
};

__attribute__((visibility("hidden"))) int azsp_bind_native(const struct azsp_native *native);
__attribute__((visibility("hidden"))) int azsp_request_source(void *source, const struct azsp_request *request, struct azsp_result *result);
__attribute__((visibility("hidden"))) enum az_source_storage_mode azsp_source_storage_mode(const void *source);
__attribute__((visibility("hidden"))) int azsp_prepare_source(void *source, const az_source_track_v1 *track, const char *parent,
                                                              enum az_source_storage_mode mode, struct azsp_stage **out);

#endif
