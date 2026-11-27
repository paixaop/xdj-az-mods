/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "source_import.h"
#include "source_import_analyzer.h"
#include "source_provider_lifetime.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Serialize durable publication and native fileset ownership per USB. The
 * native analyzer compresses a path to a small hash space. Independent deck
 * loads must never analyze overlapping locators on the same medium. */
static pthread_mutex_t import_stick[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
struct import_analysis_reservation {
    uint64_t ticket;
    char dat[AZ_SOURCE_PATH_BYTES];
};
static struct import_analysis_reservation import_unsettled[2];

static int import_cancelled(const struct azspi_request *request) {
    return request->cancelled && request->cancelled(request->context);
}

static void import_wait(void) {
    const struct timespec delay = {.tv_nsec = 50000000};
    (void) nanosleep(&delay, NULL);
}

static int import_lock_stick(const struct azspi_request *request, unsigned port) {
    while (pthread_mutex_trylock(&import_stick[port])) {
        if (import_cancelled(request))
            return 0;
        import_wait();
    }
    return 1;
}

static int import_mount_current(const struct st_source *mounted) {
    struct st_source source;
    int port = (int) mounted->ref.source_id - 1;
    return st_source_read(port, &source) == AZ_OK && st_resolution_source_equal(&source, mounted);
}

static int import_preferred_source(struct st_source *source) {
    for (int port = 0; port < 2; ++port) {
        if (st_source_read(port, source) == AZ_OK)
            return AZ_OK;
    }
    return AZ_NOT_FOUND;
}

static int import_wait_destination(uint64_t ticket, int (*cancelled)(void *), void *context,
                                   unsigned *media) {
    for (;;) {
        int stop = cancelled && cancelled(context);
        if (stop)
            (void) azspi_cancel(ticket);
        int status = azspi_poll_destination(ticket, media);
        if (status != AZ_PENDING)
            return stop ? AZ_CANCELLED : status;
        import_wait();
    }
}

int azspi_import_destination(struct azspi_destination *out, int (*cancelled)(void *), void *context) {
    struct azspi_destination destination = {0};
    int status = import_preferred_source(&destination.source);
    if (status != AZ_OK)
        return status;
    uint64_t ticket;
    status = azspi_submit_destination(&destination.source, &ticket);
    if (status != AZ_PENDING)
        return status;
    status = import_wait_destination(ticket, cancelled, context, &destination.media);
    if (status == AZ_OK)
        *out = destination;
    return status;
}

static int import_assets_clear(const struct st_source *source, const char *dat) {
    static const char *const extensions[] = {"DAT", "EXT", "2EX"};
    char relative[AZ_SOURCE_PATH_BYTES];
    size_t stem = strlen(dat) - 3;
    snprintf(relative, sizeof relative, "%s", dat);
    for (unsigned file = 0; file < 3; ++file) {
        memcpy(relative + stem, extensions[file], 4);
        char path[AZ_SOURCE_PATH_BYTES];
        if (!st_source_file(source, relative, path))
            return 0;
        struct stat info;
        if (lstat(path, &info) == 0 || errno != ENOENT)
            return 0;
    }
    return 1;
}

static int import_wait_request(const struct azspi_request *request, uint64_t ticket,
                               uint32_t *track_id, uint64_t *analysis_ticket) {
    for (;;) {
        int cancelled = import_cancelled(request);
        if (cancelled)
            (void) azspi_cancel(ticket);
        int status = track_id ? azspi_poll(ticket, track_id) : azspi_poll_analysis(ticket, analysis_ticket);
        if (status != AZ_PENDING)
            return cancelled && track_id ? AZ_CANCELLED : status;
        import_wait();
    }
}

static int import_read_result(const struct azspi_request *request, const struct azspi_audio *format, const char *audio,
                              const char *dat, struct azspi_record *record) {
    enum {
        FILESET_ATTEMPTS = 16
    };
    for (unsigned i = 0; i < FILESET_ATTEMPTS; ++i) {
        if (import_cancelled(request))
            return AZ_CANCELLED;
        int status = azspi_inspect(request->source, request->track, format, audio, dat, record);
        if (status == AZ_OK || status == AZ_INVALID || status == AZ_LIMIT)
            return status;
        if (!import_mount_current(request->source))
            return AZ_STALE;
        import_wait();
    }
    return AZ_FAILED;
}

static struct import_analysis_reservation *import_analysis_slot(const struct azspi_request *request) {
    return &import_unsettled[request->source->ref.source_id - 1];
}

static int import_analysis_settled(struct import_analysis_reservation *held) {
    if (!held->ticket)
        return 1;
    if (azspd_analysis_status(held->ticket) != AZ_OK)
        return 0;
    azspd_analysis_forget(held->ticket);
    held->ticket = 0;
    return 1;
}

static int import_wait_analysis(const struct azspi_request *request,
                                struct import_analysis_reservation *held) {
    enum {
        ANALYSIS_ATTEMPTS = 2400
    }; /* 120 seconds of wait time */
    for (unsigned i = 0; i < ANALYSIS_ATTEMPTS; ++i) {
        if (import_analysis_settled(held))
            return AZ_OK;
        if (import_cancelled(request))
            return AZ_CANCELLED;
        if (!import_mount_current(request->source))
            return AZ_STALE;
        import_wait();
    }
    return AZ_FAILED;
}

static int import_run_analysis(const struct azspi_request *request, const struct azspi_audio *format, const char *audio,
                               const char *dat, struct azspi_record *record) {
    if (!import_assets_clear(request->source, dat))
        return AZ_BUSY;
    uint64_t job_ticket;
    int status = azspi_submit_analysis(request->source, request->media, audio, &job_ticket);
    if (status != AZ_PENDING)
        return status;
    struct import_analysis_reservation *held = import_analysis_slot(request);
    status = import_wait_request(request, job_ticket, NULL, &held->ticket);
    if (status != AZ_OK)
        return status;
    snprintf(held->dat, sizeof(held->dat), "%s", dat);
    status = import_wait_analysis(request, held);
    return status == AZ_OK ? import_read_result(request, format, audio, dat, record) : status;
}

static int import_analyzed_record(const struct azspi_request *request, const struct azspi_audio *format, const char *audio,
                                  struct azspi_record *record) {
    char dat[AZ_SOURCE_PATH_BYTES];
    int status = azspi_analyzer_locator(audio, dat);
    if (status != AZ_OK)
        return status;
    struct import_analysis_reservation *held = import_analysis_slot(request);
    int settled = import_analysis_settled(held);
    if (!settled && !strcmp(held->dat, dat))
        return AZ_BUSY;
    status = azspi_inspect(request->source, request->track, format, audio, dat, record);
    if (status == AZ_OK || status == AZ_INVALID)
        return status;
    return settled ? import_run_analysis(request, format, audio, dat, record) : AZ_BUSY;
}

static int import_write_record(const struct azspi_request *request, const struct azspi_record *record,
                               uint32_t *track_id) {
    if (import_cancelled(request))
        return AZ_CANCELLED;
    uint64_t ticket;
    int status = azspi_submit(record, request->media, &ticket);
    return status == AZ_PENDING ? import_wait_request(request, ticket, track_id, NULL) : status;
}

static int import_retryable(int status) {
    return status == AZ_STALE || status == AZ_BUSY;
}

static int import_commit_settled(const struct azspi_request *request, struct azspi_record *record,
                                 uint32_t *track_id) {
    enum {
        COMMIT_ATTEMPTS = 16
    };
    int status = AZ_OK;
    for (unsigned i = 0; i < COMMIT_ATTEMPTS; ++i) {
        if (status == AZ_OK) {
            status = import_write_record(request, record, track_id);
            if (!import_retryable(status))
                return status;
        }
        if (!import_mount_current(request->source) || import_cancelled(request))
            return import_cancelled(request) ? AZ_CANCELLED : AZ_STALE;
        /* Native analysis can republish DAT/EXT after a complete read. Keep
         * the native writer's stat guard and re-qualify every file on retry. */
        import_wait();
        status = azspi_inspect(request->source, request->track, &record->format,
                               record->audio.relative, record->analysis[0].relative, record);
    }
    return AZ_STALE;
}

static int import_work(const struct azspi_request *request, uint32_t *track_id) {
    if (import_cancelled(request))
        return AZ_CANCELLED;
    if (!import_mount_current(request->source))
        return AZ_STALE;
    char audio[AZ_SOURCE_PATH_BYTES];
    struct azspi_audio format;
    int status = azspi_publish(request, audio, &format);
    if (status != AZ_OK)
        return status;
    struct azspi_record record;
    status = import_analyzed_record(request, &format, audio, &record);
    return status == AZ_OK ? import_commit_settled(request, &record, track_id) : status;
}

int azspi_import_stage(const struct azspi_request *request, uint32_t *track_id) {
    if (!request || !request->source || !track_id)
        return AZ_INVALID;
    uint64_t source = request->source->ref.source_id;
    if (!source || source > 2)
        return AZ_INVALID;
    unsigned port = (unsigned) (source - 1);
    if (!import_lock_stick(request, port))
        return AZ_CANCELLED;
    uint32_t id = 0;
    int status = import_work(request, &id);
    pthread_mutex_unlock(&import_stick[port]);
    if (status != AZ_OK)
        return status;
    if (!id)
        return AZ_FAILED;
    *track_id = id;
    return AZ_OK;
}

static int import_provider_file(const struct azspi_provider_load *load, const struct azspi_destination *dest,
                                uint32_t *id) {
    struct azsp_stage *stage;
    int status = azsp_prepare_source(load->source, load->track, load->staging_root, AZ_SOURCE_IMPORT, &stage);
    if (status != AZ_OK)
        return status;
    const struct azspi_request request = {.source = &dest->source, .media = dest->media, .firmware = load->firmware, .provider = load->provider, .track = load->track, .stage = stage, .cancelled = load->cancelled, .context = load->context};
    status = azspi_import_stage(&request, id);
    azsp_stage_release(stage);
    return status;
}

int azspi_import_provider(const struct azspi_provider_load *load, struct azspi_imported_track *out) {
    struct azspi_destination dest = {0};
    int status = azspi_import_destination(&dest, load->cancelled, load->context);
    if (status != AZ_OK)
        return status;
    struct azspi_imported_track track = {.media = dest.media};
    status = import_provider_file(load, &dest, &track.id);
    if (status == AZ_OK)
        *out = track;
    return status;
}
