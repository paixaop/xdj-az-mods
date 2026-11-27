/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "source_provider_stream.h"
#include "source_import_analyzer.h"
#include "source_import_format.h"
#include "source_provider_carrier.h"
#include "source_provider_catalog.h"
#include "source_provider_lifetime.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct azsp_stream {
    struct azsp_stream *next;
    struct azsp_stage *stage;
    uint64_t media;
    uint64_t analysis_ticket;
    uint32_t id;
    bool analysis_started, attached;
    unsigned removed;
    char audio[AZ_SOURCE_PATH_BYTES], dat[AZ_SOURCE_PATH_BYTES];
    struct azspi_record record;
};

static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;
static struct azsp_stream *streams;

static int stream_conflict(const struct azsp_stream *candidate, const struct azsp_stream *active) {
    return candidate->id == active->id || !strcmp(candidate->dat, active->dat);
}

static int stream_reserve_locked(struct azsp_stream *stream) {
    for (const struct azsp_stream *active = streams; active; active = active->next) {
        if (stream_conflict(stream, active))
            return AZ_BUSY;
    }
    stream->next = streams;
    streams = stream;
    azsp_stage_retain(stream->stage);
    return AZ_OK;
}

static int stream_reserve(struct azsp_stream *stream) {
    pthread_mutex_lock(&streams_lock);
    int status = stream_reserve_locked(stream);
    pthread_mutex_unlock(&streams_lock);
    return status;
}

static void stream_forget(struct azsp_stream *stream) {
    pthread_mutex_lock(&streams_lock);
    struct azsp_stream **link = &streams;
    while (*link != stream)
        link = &(*link)->next;
    *link = stream->next;
    pthread_mutex_unlock(&streams_lock);
    if (stream->analysis_ticket)
        (void) azspd_analysis_forget(stream->analysis_ticket);
    azsp_stage_release(stream->stage);
    free(stream);
}

static int stream_database_stat(const char *root, struct stat *status) {
    char path[AZ_SOURCE_PATH_BYTES];
    return st_source_path(root, "/PIONEER/rekordbox/exportLibrary.db", path) &&
           !stat(path, status) && S_ISREG(status->st_mode);
}

static int stream_root_current(const struct azsp_stream *stream) {
    const char *root = azsp_carrier_root();
    return root && azsp_carrier_media() == stream->media && !strcmp(root, stream->record.source.mount);
}

static int stream_current(const struct azsp_stream *stream) {
    const struct st_source *source = &stream->record.source;
    struct stat status;
    return stream_root_current(stream) && stream_database_stat(source->mount, &status) &&
           (uint64_t) status.st_dev == source->device && (uint64_t) status.st_ino == source->inode;
}

static int stream_inside(const char *root, const char *path) {
    size_t prefix = strlen(root);
    return !strncmp(root, path, prefix) && path[prefix] == '/';
}

static int stream_stage_child(const struct azsp_stage *stage, const char *root) {
    const char *directory = azsp_stage_directory(stage);
    const char *path = azsp_stage_path(stage);
    return stream_inside(root, directory) && stream_inside(directory, path) &&
           !strchr(path + strlen(directory) + 1, '/');
}

static int stream_paths(struct azsp_stream *stream, const char *root) {
    if (!stream_stage_child(stream->stage, root))
        return AZ_INVALID;
    const char *relative = azsp_stage_path(stream->stage) + strlen(root);
    int length = snprintf(stream->audio, sizeof(stream->audio), "%s", relative);
    if ((unsigned) length >= sizeof(stream->audio))
        return AZ_LIMIT;
    return azspi_analyzer_locator(stream->audio, stream->dat);
}

static int stream_source(struct azsp_stream *stream) {
    const char *root = azsp_carrier_root();
    if (!root || azsp_carrier_media() != stream->media)
        return AZ_STALE;
    struct st_source *source = &stream->record.source;
    int length = snprintf(source->mount, sizeof(source->mount), "%s", root);
    if ((unsigned) length >= sizeof(source->mount))
        return AZ_LIMIT;
    struct stat status;
    if (!stream_database_stat(root, &status))
        return AZ_STALE;
    source->device = (uint64_t) status.st_dev;
    source->inode = (uint64_t) status.st_ino;
    return stream_paths(stream, root);
}

static int stream_file_path(const struct st_source *source, const char *relative, char path[AZ_SOURCE_PATH_BYTES]) {
    char resolved[AZ_SOURCE_PATH_BYTES];
    int length = snprintf(path, AZ_SOURCE_PATH_BYTES, "%s%s", source->mount, relative);
    return (unsigned) length < AZ_SOURCE_PATH_BYTES && st_source_path(source->mount, relative, resolved) &&
           !strcmp(path, resolved);
}

static int stream_entry_absent(int directory, const char *name) {
    struct stat status;
    return fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
}

static int stream_open_directory(int parent, const char *name, uint64_t device) {
    int directory = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return -1;
    struct stat status;
    if (!fstat(directory, &status) && (uint64_t) status.st_dev == device)
        return directory;
    close(directory);
    return -1;
}

static int stream_open_analysis_dir(const struct st_source *source, char *relative, int *directory) {
    int parent = stream_open_directory(AT_FDCWD, source->mount, source->device);
    if (parent < 0)
        return AZ_STALE;
    for (char *name = relative, *slash; (slash = strchr(name, '/')); name = slash + 1) {
        *slash = '\0';
        int next = stream_open_directory(parent, name, source->device);
        if (next < 0) {
            int status = stream_entry_absent(parent, name) ? AZ_NOT_FOUND : AZ_STALE;
            close(parent);
            return status;
        }
        close(parent);
        parent = next;
    }
    *directory = parent;
    return AZ_OK;
}

static int stream_anlz_absent(int directory, const char *name, const char *extension) {
    char leaf[AZ_SOURCE_PATH_BYTES];
    int length = snprintf(leaf, sizeof leaf, "%s", name);
    if (length < 3 || (unsigned) length >= sizeof leaf)
        return 0;
    memcpy(leaf + length - 3, extension, 4);
    return stream_entry_absent(directory, leaf);
}

static int stream_destination_clear(const struct azsp_stream *stream) {
    const struct st_source *source = &stream->record.source;
    char relative[AZ_SOURCE_PATH_BYTES], path[AZ_SOURCE_PATH_BYTES];
    if (!stream_file_path(source, stream->dat, path))
        return 0;
    memcpy(relative, stream->dat, sizeof relative);
    int directory;
    int status = stream_open_analysis_dir(source, relative + 1, &directory);
    if (status != AZ_OK)
        return status == AZ_NOT_FOUND;
    const char *leaf = strrchr(stream->dat, '/') + 1;
    int absent = stream_anlz_absent(directory, leaf, "DAT") && stream_anlz_absent(directory, leaf, "EXT") &&
                 stream_anlz_absent(directory, leaf, "2EX");
    close(directory);
    return absent;
}

static int stream_command(struct azsp_stream *stream, enum azsp_catalog_stream_operation command) {
    const struct azsp_catalog_stream_request request = {
        .media = stream->media,
        .id = stream->id,
        .source = &stream->record.source,
        .audio = stream->audio,
        .dat = stream->dat,
        .record = &stream->record,
        .analysis_ticket = &stream->analysis_ticket,
    };
    return azsp_catalog_stream(command, &request);
}

static int stream_inspect(struct azsp_stream *stream) {
    struct azspi_record *record = &stream->record;
    if (!stream_current(stream))
        return AZ_STALE;
    return azspi_inspect(&record->source, &record->track, &record->format,
                         stream->audio, stream->dat, record);
}

static void stream_wait(void) {
    const struct timespec delay = {.tv_nsec = 50000000};
    (void) nanosleep(&delay, NULL);
}

static int stream_cancelled(int (*cancelled)(void *), void *context) {
    return cancelled && cancelled(context);
}

static int stream_poll_analysis(struct azsp_stream *stream) {
    int status = azspd_analysis_status(stream->analysis_ticket);
    return status == AZ_OK ? stream_inspect(stream) : status;
}

static int stream_wait_analysis(struct azsp_stream *stream, int (*cancelled)(void *), void *context) {
    enum {
        ANALYSIS_ATTEMPTS = 2400
    };
    for (unsigned attempt = 0; attempt < ANALYSIS_ATTEMPTS; ++attempt) {
        if (stream_cancelled(cancelled, context))
            return AZ_CANCELLED;
        int status = stream_poll_analysis(stream);
        if (status == AZ_OK || status == AZ_INVALID || status == AZ_LIMIT)
            return status;
        if (!stream_root_current(stream))
            return AZ_STALE;
        stream_wait();
    }
    return AZ_FAILED;
}

static int stream_bind_attempt(struct azsp_stream *stream) {
    int status = stream_inspect(stream);
    if (status == AZ_FAILED)
        return AZ_STALE; /* A native atomic replacement can straddle the fileset read. */
    return status == AZ_OK ? stream_command(stream, AZSP_CATALOG_STREAM_BIND) : status;
}

static int stream_bind(struct azsp_stream *stream, int (*cancelled)(void *), void *context) {
    enum {
        BIND_ATTEMPTS = 16
    };
    for (unsigned attempt = 0; attempt < BIND_ATTEMPTS; ++attempt) {
        if (stream_cancelled(cancelled, context))
            return AZ_CANCELLED;
        int status = stream_bind_attempt(stream);
        if (status != AZ_STALE && status != AZ_BUSY)
            return status;
        stream_wait();
    }
    return AZ_STALE;
}

static int stream_analyze(struct azsp_stream *stream, int (*cancelled)(void *), void *context) {
    if (stream_cancelled(cancelled, context))
        return AZ_CANCELLED;
    if (!stream_destination_clear(stream))
        return AZ_BUSY;
    int status = stream_command(stream, AZSP_CATALOG_STREAM_ANALYZE);
    if (status != AZ_OK)
        return status;
    stream->analysis_started = true;
    return stream_wait_analysis(stream, cancelled, context);
}

static int stream_work(struct azsp_stream *stream, int firmware, int (*cancelled)(void *), void *context) {
    struct azspi_record *record = &stream->record;
    int status = azspi_probe_format(firmware, &record->source, stream->audio, &record->format);
    if (status == AZ_OK)
        status = stream_analyze(stream, cancelled, context);
    if (status == AZ_OK)
        status = stream_bind(stream, cancelled, context);
    stream->attached = status == AZ_OK;
    return status;
}

static int stream_remove_file(const struct st_source *source, const struct azspi_asset *asset) {
    char path[AZ_SOURCE_PATH_BYTES];
    if (!stream_file_path(source, asset->relative, path))
        return AZ_STALE;
    struct stat status;
    if (lstat(path, &status))
        return errno == ENOENT ? AZ_OK : AZ_STALE;
    if (!S_ISREG(status.st_mode) || asset->signature != st_stat_sig(&status))
        return AZ_STALE;
    return unlink(path) ? AZ_FAILED : AZ_OK;
}

static void stream_remove_directory(const struct azsp_stream *stream) {
    char path[AZ_SOURCE_PATH_BYTES];
    const char *slash = strrchr(stream->dat, '/');
    int length = snprintf(path, sizeof(path), "%s%.*s", stream->record.source.mount,
                          (int) (slash - stream->dat), stream->dat);
    if ((unsigned) length < sizeof(path))
        (void) rmdir(path);
}

static int stream_remove_analysis(struct azsp_stream *stream) {
    for (unsigned file = 0; file < 3; ++file) {
        if (stream->removed & (1u << file))
            continue;
        int status = stream_remove_file(&stream->record.source, &stream->record.analysis[file]);
        if (status != AZ_OK)
            return status;
        stream->removed |= 1u << file;
    }
    stream_remove_directory(stream);
    return AZ_OK;
}

static int stream_prepare_disposal(struct azsp_stream *stream, bool *absent) {
    int terminal = azspd_analysis_status(stream->analysis_ticket);
    if (terminal != AZ_OK)
        return terminal == AZ_PENDING ? AZ_BUSY : terminal;
    if (stream->removed)
        return AZ_OK;
    if (!stream->attached && stream_destination_clear(stream)) {
        *absent = true;
        return AZ_OK;
    }
    int status = stream_inspect(stream);
    if (status != AZ_OK)
        return status;
    stream_wait();
    if (azspi_current(&stream->record) != AZ_OK)
        return AZ_BUSY;
    return AZ_OK;
}

static int stream_detach(struct azsp_stream *stream) {
    if (!stream->attached)
        return AZ_OK;
    int status = stream_command(stream, AZSP_CATALOG_STREAM_CLEAR);
    if (status == AZ_OK)
        stream->attached = false;
    return status;
}

static int stream_dispose(struct azsp_stream *stream, bool absent) {
    /* An empty result has no files to unlink. Check again after the caller
     * establishes its final native-reader fence. */
    int status = absent ? (stream_destination_clear(stream) ? AZ_OK : AZ_STALE) : stream_remove_analysis(stream);
    if (status == AZ_OK)
        stream_forget(stream);
    return status;
}

int azsp_stream_release_when(struct azsp_stream *stream, int (*last_reader)(void *), void *context) {
    if (!stream)
        return AZ_INVALID;
    if (!stream_current(stream))
        return AZ_STALE;
    bool absent = false;
    int status = stream_prepare_disposal(stream, &absent);
    if (status == AZ_OK)
        status = stream_detach(stream);
    if (status != AZ_OK)
        return status;
    if (last_reader && !last_reader(context))
        return AZ_BUSY;
    return stream_dispose(stream, absent);
}

int azsp_stream_release(struct azsp_stream *stream) {
    return azsp_stream_release_when(stream, NULL, NULL);
}

static int stream_new(struct azsp_stream *stream, uint64_t media, uint32_t id,
                      const az_source_track_v1 *track, struct azsp_stage *stage) {
    stream->media = media;
    stream->id = id;
    stream->stage = stage;
    stream->record.track = *track;
    int status = stream_source(stream);
    return status == AZ_OK ? stream_reserve(stream) : status;
}

static int stream_request_valid(uint64_t media, uint32_t id, const az_source_track_v1 *track,
                                const struct azsp_stage *stage) {
    return media && id && track && stage && azsp_stage_mode(stage) == AZ_SOURCE_STREAMING;
}

int azsp_stream_prepare(int firmware, uint64_t carrier_media, uint32_t content_id,
                        const az_source_track_v1 *track, struct azsp_stage *stage,
                        int (*cancelled)(void *), void *context, struct azsp_stream **out) {
    if (!out)
        return AZ_INVALID;
    *out = NULL;
    if (!stream_request_valid(carrier_media, content_id, track, stage))
        return AZ_INVALID;
    struct azsp_stream *stream = calloc(1, sizeof(*stream));
    if (!stream)
        return AZ_FAILED;
    int status = stream_new(stream, carrier_media, content_id, track, stage);
    if (status != AZ_OK) {
        free(stream);
        return status;
    }
    status = stream_work(stream, firmware, cancelled, context);
    /* The caller owns the path watch. A submitted native request needs that
     * reader fence even if analysis already settled during preparation. */
    if (status != AZ_OK && !stream->analysis_started)
        stream_forget(stream);
    else
        *out = stream;
    return status;
}

const char *azsp_stream_path(const struct azsp_stream *stream) {
    return azsp_stage_path(stream->stage);
}
