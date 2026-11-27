/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include "source_provider.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct azsp_stage {
    _Atomic unsigned refs;
    enum az_source_storage_mode mode;
    char directory[AZ_SOURCE_PATH_BYTES];
    char path[AZ_SOURCE_PATH_BYTES];
};

static int bounded_text(const char *text, size_t capacity) {
    return strnlen(text, capacity) < capacity;
}

static int has_text(const char *text, size_t capacity) {
    return text && *text && bounded_text(text, capacity);
}

static int provider_icon_valid(const az_source_provider_v1 *provider) {
    if (!provider->icon_bytes)
        return 1;
    return provider->icon_png && provider->icon_bytes <= 16384;
}

static int provider_callbacks_valid(const az_source_provider_v1 *provider) {
    return provider->list_playlists && provider->playlist_tracks &&
           provider->search && provider->prepare_track;
}

unsigned azsp_storage_flag(enum az_source_storage_mode mode) {
    switch (mode) {
    case AZ_SOURCE_STREAMING: return 1u;
    case AZ_SOURCE_IMPORT: return 2u;
    }
    return 0;
}

int azsp_provider_valid(const az_source_provider_v1 *provider) {
    if (!provider || provider->size < sizeof(*provider))
        return 0;
    return azsp_storage_flag(provider->storage_mode) && has_text(provider->name, AZ_SOURCE_NAME_BYTES) &&
           provider_icon_valid(provider) && provider_callbacks_valid(provider);
}

static int track_text_valid(const az_source_track_v1 *track) {
    return has_text(track->id, sizeof(track->id)) && has_text(track->title, sizeof(track->title)) &&
           bounded_text(track->artist, sizeof(track->artist)) &&
           bounded_text(track->album, sizeof(track->album));
}

static int playlist_text_valid(const az_source_playlist_v1 *playlist) {
    return has_text(playlist->id, sizeof(playlist->id)) && has_text(playlist->name, sizeof(playlist->name));
}

static int request_range_valid(const struct azsp_request *request) {
    return request->capacity && request->capacity <= AZ_SOURCE_PAGE_MAX &&
           request->offset <= UINT32_MAX - request->capacity;
}

static int absolute_path(const char path[AZ_SOURCE_PATH_BYTES]) {
    return path[0] == '/' && bounded_text(path, AZ_SOURCE_PATH_BYTES);
}

static int stage_directory(struct azsp_stage *stage, const char *parent, enum az_source_storage_mode mode) {
    /* Streaming audio is indexed through the native Folder scanner. */
    const char *name = mode == AZ_SOURCE_STREAMING ? "source-XXXXXX" : ".source.XXXXXX";
    int length = snprintf(stage->directory, sizeof(stage->directory), "%s/%s", parent, name);
    if ((unsigned) length >= sizeof(stage->directory))
        return AZ_LIMIT;
    return mkdtemp(stage->directory) ? AZ_OK : AZ_FAILED;
}

int azsp_stage_create(const char *parent, enum az_source_storage_mode mode, struct azsp_stage **out) {
    *out = NULL;
    if (!absolute_path(parent) || !azsp_storage_flag(mode))
        return AZ_INVALID;
    struct azsp_stage *stage = calloc(1, sizeof(*stage));
    if (!stage)
        return AZ_FAILED;
    int status = stage_directory(stage, parent, mode);
    if (status != AZ_OK) {
        free(stage);
        return status;
    }
    atomic_init(&stage->refs, 1u);
    stage->mode = mode;
    *out = stage;
    return AZ_OK;
}

static int remove_stage_entry(const char *path, const struct stat *file, int kind, struct FTW *context) {
    (void) file;
    (void) kind;
    (void) context;
    (void) remove(path);
    return 0;
}

void azsp_stage_retain(struct azsp_stage *stage) {
    atomic_fetch_add_explicit(&stage->refs, 1, memory_order_relaxed);
}

void azsp_stage_release(struct azsp_stage *stage) {
    if (atomic_fetch_sub_explicit(&stage->refs, 1, memory_order_acq_rel) != 1)
        return;
    (void) nftw(stage->directory, remove_stage_entry, 8, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
    free(stage);
}

const char *azsp_stage_directory(const struct azsp_stage *stage) {
    return stage->directory;
}

const char *azsp_stage_path(const struct azsp_stage *stage) {
    return stage->path;
}

enum az_source_storage_mode azsp_stage_mode(const struct azsp_stage *stage) {
    return stage->mode;
}

void azsp_stage_set_path(struct azsp_stage *stage, const struct azsp_result *result) {
    memcpy(stage->path, result->value.path, sizeof(stage->path));
}

static int request_valid(const struct azsp_request *request) {
    switch (request->operation) {
    case AZSP_PLAYLISTS:
        return request_range_valid(request);
    case AZSP_PLAYLIST_TRACKS:
        return request_range_valid(request) &&
               has_text(request->value.playlist_id, sizeof(request->value.playlist_id));
    case AZSP_SEARCH:
        return request_range_valid(request) &&
               has_text(request->value.query, sizeof(request->value.query));
    case AZSP_PREPARE_TRACK:
        return absolute_path(request->value.prepare.directory) &&
               track_text_valid(&request->value.prepare.track);
    }
    return 0;
}

static int call(const az_source_provider_v1 *provider, const struct azsp_request *request,
                struct azsp_result *result) {
    void *user = provider->user;
    uint32_t offset = request->offset, capacity = request->capacity;
    switch (request->operation) {
    case AZSP_PLAYLISTS:
        return provider->list_playlists(user, offset, result->value.playlists, capacity, &result->page);
    case AZSP_PLAYLIST_TRACKS:
        return provider->playlist_tracks(user, request->value.playlist_id, offset,
                                         result->value.tracks, capacity, &result->page);
    case AZSP_SEARCH:
        return provider->search(user, request->value.query, offset,
                                result->value.tracks, capacity, &result->page);
    case AZSP_PREPARE_TRACK:
        return provider->prepare_track(user, &request->value.prepare.track,
                                       request->value.prepare.directory, result->value.path,
                                       sizeof(result->value.path));
    }
    return AZ_INVALID;
}

static int page_valid(const struct azsp_request *request, const az_source_page_v1 *page) {
    if (page->count > request->capacity || page->has_more > 1)
        return 0;
    if (!page->has_more)
        return 1;
    return page->count && request->offset < UINT32_MAX - page->count;
}

static int rows_valid(enum azsp_operation operation, const struct azsp_result *result) {
    for (uint32_t i = 0; i < result->page.count; ++i) {
        int valid = operation == AZSP_PLAYLISTS ? playlist_text_valid(&result->value.playlists[i]) : track_text_valid(&result->value.tracks[i]);
        if (!valid)
            return 0;
    }
    return 1;
}

static const char *staged_name(const char *directory, const char *path) {
    size_t length = strlen(directory);
    if (!absolute_path(path) || strncmp(directory, path, length) || path[length] != '/')
        return NULL;
    const char *name = path + length + 1;
    return *name && !strchr(name, '/') ? name : NULL;
}

static int staged_file_status(int directory, const char *name) {
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? AZ_NOT_FOUND : AZ_FAILED;
    struct stat file;
    int ready = fstat(fd, &file) == 0 && S_ISREG(file.st_mode) && file.st_size > 0;
    close(fd);
    return ready ? AZ_OK : AZ_INVALID;
}

static int audio_status(const char *directory, const char *path) {
    const char *name = staged_name(directory, path);
    if (!name)
        return AZ_INVALID;
    int fd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        return AZ_FAILED;
    int status = staged_file_status(fd, name);
    close(fd);
    return status;
}

static int result_status(const struct azsp_request *request, const struct azsp_result *result) {
    if (request->operation == AZSP_PREPARE_TRACK)
        return audio_status(request->value.prepare.directory, result->value.path);
    return page_valid(request, &result->page) && rows_valid(request->operation, result) ? AZ_OK : AZ_INVALID;
}

int azsp_run(const az_source_provider_v1 *provider, const struct azsp_request *request,
             struct azsp_result *result) {
    memset(result, 0, sizeof(*result));
    if (!request_valid(request))
        return AZ_INVALID;
    int status = call(provider, request, result);
    if (status == AZ_OK)
        status = result_status(request, result);
    if (status != AZ_OK)
        memset(result, 0, sizeof(*result));
    return status > AZ_OK ? AZ_FAILED : status;
}
