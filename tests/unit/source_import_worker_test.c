/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "source_import.h"
#include "source_import_analyzer.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char audio[] = "/audio.m4a";
static const char dat[] = "/ANLZ0000.DAT";
static const char second_audio[] = "/second.m4a";
static const char second_dat[] = "/ANLZ0001.DAT";
static const uint64_t analysis_ticket = 12, write_ticket = 13;
static uint64_t mount_generation = 1;
static uint64_t watched_ticket;
static unsigned analyses, commits, inspections, waiting, forgotten;
static int result_ready, delay_result, cancel, cancel_on_analysis, cancel_on_write;
static int second_ready, analyzing_second, hold_analysis, missing_files, stale_on_analysis;
static int audio_failure, publish_failure;
static int failed_writes;
static unsigned available_ports = 3;
static unsigned resolve_port;
static unsigned acquisitions, released;
static int acquisition_failure;

static int is_second_audio(const char *path) {
    assert(!strcmp(path, audio) || !strcmp(path, second_audio));
    return !strcmp(path, second_audio);
}

int st_source_read(int port, struct st_source *out) {
    assert(port == 0 || port == 1);
    if (!(available_ports & (1u << port)))
        return AZ_NOT_FOUND;
    *out = (struct st_source) {.ref.source_id = (unsigned) port + 1, .ref.source_generation = mount_generation};
    return AZ_OK;
}
int st_resolution_source_equal(const struct st_source *left, const struct st_source *right) {
    return left->ref.source_generation == right->ref.source_generation;
}
int st_source_file(const struct st_source *source, const char *part, char path[1024]) {
    return (unsigned) snprintf(path, 1024, "%s%s", source->mount, part) < 1024;
}
int azspi_publish(const struct azspi_request *request, char relative[AZ_SOURCE_PATH_BYTES], struct azspi_audio *format) {
    assert(request->source && !strcmp(request->provider, "example") && request->track && request->stage);
    assert(request->firmware == 2);
    if (publish_failure)
        return AZ_FAILED;
    if (audio_failure)
        return AZ_UNSUPPORTED;
    strcpy(relative, !strcmp(request->track->id, "0001") ? second_audio : audio);
    *format = (struct azspi_audio) {.file_type = 4, .sample_rate = 44100};
    return AZ_OK;
}
int azspi_analyzer_locator(const char *path, char out[AZ_SOURCE_PATH_BYTES]) {
    strcpy(out, is_second_audio(path) ? second_dat : dat);
    return AZ_OK;
}
int azspi_inspect(const struct st_source *source, const az_source_track_v1 *track,
                  const struct azspi_audio *format, const char *path, const char *locator, struct azspi_record *out) {
    assert(source && track && format->file_type == 4);
    bool second = is_second_audio(path);
    assert(!strcmp(locator, second ? second_dat : dat));
    ++inspections;
    if (!(second ? second_ready : result_ready))
        return AZ_STALE;
    struct azspi_record record = {.source = *source, .track = *track, .format = *format, .bpm_x100 = 12000};
    strcpy(record.audio.relative, path);
    strcpy(record.analysis[0].relative, locator);
    *out = record;
    return AZ_OK;
}
int azspi_submit_analysis(const struct st_source *source, unsigned media, const char *path, uint64_t *ticket) {
    assert(source && media == 0x102 && !watched_ticket);
    ++analyses;
    *ticket = analysis_ticket;
    analyzing_second = is_second_audio(path);
    return AZ_PENDING;
}
int azspi_poll_analysis(uint64_t ticket, uint64_t *watch) {
    assert(ticket == analysis_ticket && watch);
    if (cancel_on_analysis)
        cancel = 1;
    if (stale_on_analysis)
        ++mount_generation;
    if (!hold_analysis && !missing_files) {
        if (analyzing_second)
            second_ready = 1;
        else
            result_ready = 1;
    }
    *watch = watched_ticket = 100 + analyses;
    return AZ_OK;
}
int azspd_analysis_status(uint64_t ticket) {
    assert(ticket && ticket == watched_ticket);
    return hold_analysis ? AZ_PENDING : AZ_OK;
}
void azspd_analysis_forget(uint64_t ticket) {
    assert(ticket && ticket == watched_ticket && !hold_analysis);
    watched_ticket = 0;
    ++forgotten;
}
int azspi_submit(const struct azspi_record *record, unsigned media, uint64_t *ticket) {
    assert(record->bpm_x100 == 12000 && media == 0x102);
    ++commits;
    *ticket = write_ticket;
    if (cancel_on_write)
        cancel = 1;
    return AZ_PENDING;
}
int azspi_poll(uint64_t ticket, uint32_t *track_id) {
    assert(ticket == write_ticket && track_id);
    if (failed_writes) {
        --failed_writes;
        return AZ_STALE;
    }
    *track_id = 89;
    return AZ_OK;
}
int azspi_cancel(uint64_t ticket) {
    assert(ticket == write_ticket || ticket == analysis_ticket || ticket == 14);
    return AZ_BUSY;
}
int azspi_submit_destination(const struct st_source *source, uint64_t *ticket) {
    resolve_port = (unsigned) source->ref.source_id;
    *ticket = 14;
    return AZ_PENDING;
}
int azspi_poll_destination(uint64_t ticket, unsigned *media) {
    assert(ticket == 14 && media);
    if (delay_result) {
        delay_result = 0;
        return AZ_PENDING;
    }
    *media = resolve_port == 1 ? 0x102 : 0x122;
    return AZ_OK;
}
int azsp_prepare_source(void *source, const az_source_track_v1 *track, const char *root,
                        enum az_source_storage_mode mode, struct azsp_stage **out) {
    assert(source == &acquisitions && track && !strcmp(root, "/tmp") && mode == AZ_SOURCE_IMPORT);
    if (acquisition_failure)
        return AZ_FAILED;
    ++acquisitions;
    *out = (void *) &acquisitions;
    return AZ_OK;
}
void azsp_stage_release(struct azsp_stage *stage) {
    assert((void *) stage == &acquisitions);
    ++released;
}
static int user_cancel(void *context) {
    assert(context == &cancel);
    return cancel;
}
static int test_sleep(const struct timespec *requested, struct timespec *left) {
    assert(requested && !left);
    ++waiting;
    return 0;
}

#define nanosleep test_sleep
#include "source_import_worker.c"

static void paths_and_dispatch(const struct azspi_request *request) {
    uint32_t track_id = 0;
    assert(azspi_import_stage(request, &track_id) == AZ_OK && track_id == 89);
    assert(analyses == 1 && commits == 1 && inspections == 2);
    assert(azspi_import_stage(request, &track_id) == AZ_OK && analyses == 1 && commits == 2);
    failed_writes = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_OK && analyses == 1 && commits == 4);
    ++mount_generation;
    assert(azspi_import_stage(request, &track_id) == AZ_STALE);
    --mount_generation;
    audio_failure = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_UNSUPPORTED);
    audio_failure = 0;
    publish_failure = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_FAILED);
    publish_failure = 0;
}

static void destinations(void) {
    struct azspi_destination dest = {0};
    delay_result = 1;
    assert(azspi_import_destination(&dest, user_cancel, &cancel) == AZ_OK && dest.media == 0x102);
    available_ports = 2;
    assert(azspi_import_destination(&dest, NULL, NULL) == AZ_OK && dest.media == 0x122);
    available_ports = 0;
    assert(azspi_import_destination(&dest, NULL, NULL) == AZ_NOT_FOUND && dest.media == 0x122);
    available_ports = 3;
    cancel = 1;
    assert(azspi_import_destination(&dest, user_cancel, &cancel) == AZ_CANCELLED && dest.media == 0x122);
    cancel = 0;
}

static void generation_and_conflict(const struct azspi_request *request) {
    uint32_t track_id = 0;
    result_ready = 0;
    hold_analysis = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_FAILED && waiting > 0 && analyses == 2);
    assert(azspi_import_stage(request, &track_id) == AZ_BUSY && analyses == 2 && !track_id);
    result_ready = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_BUSY && analyses == 2 && watched_ticket);
    hold_analysis = 0;
    assert(azspi_import_stage(request, &track_id) == AZ_OK && track_id == 89);
    assert(!watched_ticket && forgotten == 2);
    result_ready = 0;
    char path[1024];
    assert(st_source_file(request->source, dat, path));
    FILE *somebody = fopen(path, "w");
    assert(somebody && fclose(somebody) == 0);
    assert(azspi_import_stage(request, &track_id) == AZ_BUSY && analyses == 2);
    assert(unlink(path) == 0);
}

static void terminal_without_fileset(const struct azspi_request *request) {
    uint32_t track_id = 0;
    unsigned before = analyses;
    missing_files = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_FAILED);
    assert(analyses == before + 1 && !track_id && !watched_ticket);
    char path[1024];
    assert(st_source_file(request->source, dat, path));
    FILE *partial = fopen(path, "w");
    assert(partial && fputs("partial", partial) >= 0 && fclose(partial) == 0);
    assert(azspi_import_stage(request, &track_id) == AZ_BUSY && analyses == before + 1);

    const az_source_track_v1 track = {.id = "0001", .title = "Another track"};
    struct azspi_request next = *request;
    next.track = &track;
    missing_files = 0;
    assert(azspi_import_stage(&next, &track_id) == AZ_OK && track_id == 89);
    assert(analyses == before + 2 && second_ready && !watched_ticket);
    assert(access(path, F_OK) == 0 && unlink(path) == 0);
}

static void cancellation(const struct azspi_request *request) {
    uint32_t track_id = 0;
    assert(!pthread_mutex_lock(&import_stick[0]));
    cancel = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_CANCELLED);
    assert(!pthread_mutex_unlock(&import_stick[0]));
    cancel = 0;
    cancel_on_analysis = 1;
    hold_analysis = 1;
    unsigned before = commits;
    unsigned before_analyses = analyses;
    assert(azspi_import_stage(request, &track_id) == AZ_CANCELLED && commits == before);
    assert(watched_ticket && analyses == before_analyses + 1);
    cancel_on_analysis = 0;
    hold_analysis = 0;
    result_ready = 1;
    cancel_on_write = 1;
    cancel = 0;
    assert(azspi_import_stage(request, &track_id) == AZ_CANCELLED && commits == before + 1 && !track_id);
}

static void replacement_during_analysis(const struct azspi_request *request) {
    uint32_t track_id = 0;
    result_ready = 0;
    hold_analysis = 1;
    stale_on_analysis = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_STALE && watched_ticket);
    stale_on_analysis = 0;
    --mount_generation;
    hold_analysis = 0;
    result_ready = 1;
    assert(azspi_import_stage(request, &track_id) == AZ_OK && !watched_ticket);
}

static void provider_import(const az_source_track_v1 *track) {
    const struct azspi_provider_load request = {.source = &acquisitions, .provider = "example", .staging_root = "/tmp", .track = track, .firmware = 2, .cancelled = user_cancel, .context = &cancel};
    struct azspi_imported_track imported = {0};
    cancel = 0;
    cancel_on_write = 0;
    assert(azspi_import_provider(&request, &imported) == AZ_OK);
    assert(imported.media == 0x102 && imported.id == 89 && acquisitions == 1 && released == 1);
    acquisition_failure = 1;
    assert(azspi_import_provider(&request, &imported) == AZ_FAILED && acquisitions == released);
    acquisition_failure = 0;
    publish_failure = 1;
    assert(azspi_import_provider(&request, &imported) == AZ_FAILED && acquisitions == 2 && released == 2);
    publish_failure = 0;
}

int main(void) {
    struct st_source source = {.ref.source_id = 1, .ref.source_generation = 1, .mount = "/tmp/az-source-import-worker.XXXXXX"};
    assert(mkdtemp(source.mount));
    az_source_track_v1 track = {.id = "0000", .title = "Import"};
    const struct azspi_request request = {.source = &source, .media = 0x102, .firmware = 2, .provider = "example", .track = &track, .stage = (void *) &source, .cancelled = user_cancel, .context = &cancel};
    uint32_t unused;
    assert(azspi_import_stage(NULL, &unused) == AZ_INVALID);
    assert(azspi_import_stage(&request, NULL) == AZ_INVALID);
    struct azspi_request no_source = request;
    no_source.source = NULL;
    assert(azspi_import_stage(&no_source, &unused) == AZ_INVALID);
    destinations();
    paths_and_dispatch(&request);
    generation_and_conflict(&request);
    terminal_without_fileset(&request);
    cancellation(&request);
    provider_import(&track);
    replacement_during_analysis(&request);
    assert(rmdir(source.mount) == 0);
    puts("Source import worker publication, analysis settlement and commit passed");
    return 0;
}
