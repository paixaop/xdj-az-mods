/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "source_provider.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fixture {
    unsigned calls;
    int status;
    az_source_page_v1 page;
    az_source_playlist_v1 playlist;
    az_source_track_v1 track;
    char directory[AZ_SOURCE_PATH_BYTES];
    char path[AZ_SOURCE_PATH_BYTES];
};

static int playlists(void *user, uint32_t offset, az_source_playlist_v1 *rows,
                     uint32_t capacity, az_source_page_v1 *page) {
    struct fixture *f = user;
    assert(offset == 8 && capacity == 1);
    ++f->calls;
    *page = f->page;
    rows[0] = f->playlist;
    return f->status;
}

static int track_page(struct fixture *f, az_source_track_v1 *rows, az_source_page_v1 *page) {
    ++f->calls;
    *page = f->page;
    rows[0] = f->track;
    return f->status;
}

static int playlist_tracks(void *user, const char *id, uint32_t offset, az_source_track_v1 *rows,
                           uint32_t capacity, az_source_page_v1 *page) {
    assert(!strcmp(id, "foreign/id") && offset == 15 && capacity == 3);
    return track_page(user, rows, page);
}

static int search(void *user, const char *query, uint32_t offset, az_source_track_v1 *rows,
                  uint32_t capacity, az_source_page_v1 *page) {
    assert(!strcmp(query, "électronique") && !offset && capacity == 4);
    return track_page(user, rows, page);
}

static int prepare(void *user, const az_source_track_v1 *track, const char *directory,
                   char *path, uint32_t capacity) {
    struct fixture *f = user;
    assert(!strcmp(directory, f->directory) && !strcmp(track->id, f->track.id));
    ++f->calls;
    assert(capacity == sizeof(f->path));
    memcpy(path, f->path, capacity);
    return f->status;
}

static void descriptors(az_source_provider_v1 provider) {
    assert(!azsp_provider_valid(NULL));
    assert(azsp_provider_valid(&provider));
    provider.storage_mode = AZ_SOURCE_IMPORT;
    assert(azsp_provider_valid(&provider));
    provider.storage_mode = 0;
    assert(!azsp_provider_valid(&provider));
    provider.storage_mode = AZ_SOURCE_STREAMING;
    provider.icon_bytes = 5;
    assert(!azsp_provider_valid(&provider));
    provider.icon_png = (const unsigned char *) "sample";
    provider.icon_bytes = 16385;
    assert(!azsp_provider_valid(&provider));
    provider.icon_bytes = 5;
    assert(azsp_provider_valid(&provider));
    provider.name = "";
    assert(!azsp_provider_valid(&provider));
    provider.name = "Fixture";
    provider.prepare_track = NULL;
    assert(!azsp_provider_valid(&provider));
    provider.size--;
    assert(!azsp_provider_valid(&provider));
}

static void pages(const az_source_provider_v1 *provider, struct fixture *f) {
    struct azsp_result out;
    struct azsp_request request = {.operation = AZSP_PLAYLISTS, .offset = 8, .capacity = 1};
    assert(azsp_run(provider, &request, &out) == AZ_OK);
    assert(out.page.count == 1 && out.page.has_more == 1);
    assert(!strcmp(out.value.playlists[0].id, "foreign/id"));
    f->page.count = 2;
    assert(azsp_run(provider, &request, &out) == AZ_INVALID && !out.page.count);
    f->page.count = 0;
    assert(azsp_run(provider, &request, &out) == AZ_INVALID && !out.page.has_more);
    f->page.has_more = 0;
    assert(azsp_run(provider, &request, &out) == AZ_OK && !out.page.count);
    f->page.count = 1;
    f->playlist.name[0] = 0;
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    request.capacity = AZ_SOURCE_PAGE_MAX + 1;
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    request.capacity = 2;
    request.offset = UINT32_MAX - 1;
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    assert(f->calls == 5);
}

static void track_pages(const az_source_provider_v1 *provider, struct fixture *f) {
    struct azsp_result out;
    struct azsp_request request = {.operation = AZSP_PLAYLIST_TRACKS, .offset = 15, .capacity = 3};
    strcpy(request.value.playlist_id, "foreign/id");
    assert(azsp_run(provider, &request, &out) == AZ_OK);
    assert(!strcmp(out.value.tracks[0].id, "m/42") && out.value.tracks[0].duration_ms == 1234);
    request.value.playlist_id[0] = 0;
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    request = (struct azsp_request) {.operation = AZSP_SEARCH, .capacity = 4};
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    strcpy(request.value.query, "électronique");
    assert(azsp_run(provider, &request, &out) == AZ_OK && !strcmp(out.value.tracks[0].title, "Suite"));
    f->status = AZ_CANCELLED;
    assert(azsp_run(provider, &request, &out) == AZ_CANCELLED && !out.page.count);
    f->status = AZ_PENDING;
    assert(azsp_run(provider, &request, &out) == AZ_FAILED && !out.page.count);
    f->status = AZ_OK;
    memset(f->track.artist, 'a', sizeof(f->track.artist));
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    memset(f->track.artist, 0, sizeof(f->track.artist));
    request.operation = 100;
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
}

static void file_paths(const az_source_provider_v1 *provider, struct fixture *f) {
    struct azsp_result out;
    char scratch[] = "/tmp/az-source-provider.XXXXXX";
    assert(mkdtemp(scratch));
    strcpy(f->directory, scratch);
    struct azsp_request request = {.operation = AZSP_PREPARE_TRACK, .value.prepare.track = f->track};
    strcpy(request.value.prepare.directory, scratch);
    strcpy(f->path, "relative.wav");
    assert(azsp_run(provider, &request, &out) == AZ_INVALID && !out.value.path[0]);
    strcpy(f->path, "/outside.wav");
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    assert(snprintf(f->path, sizeof(f->path), "%s/../outside.wav", scratch) > 0);
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    assert(snprintf(f->path, sizeof(f->path), "%s/track.wav", scratch) > 0);
    assert(azsp_run(provider, &request, &out) == AZ_NOT_FOUND);
    assert(mkfifo(f->path, 0600) == 0);
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    assert(unlink(f->path) == 0);
    assert(symlink(scratch, f->path) == 0);
    assert(azsp_run(provider, &request, &out) == AZ_FAILED);
    assert(unlink(f->path) == 0);
    FILE *audio = fopen(f->path, "w");
    assert(audio && fclose(audio) == 0);
    assert(azsp_run(provider, &request, &out) == AZ_INVALID);
    audio = fopen(f->path, "w");
    assert(audio && fputs("audio bytes", audio) >= 0 && fclose(audio) == 0);
    assert(azsp_run(provider, &request, &out) == AZ_OK && !strcmp(out.value.path, f->path));
    assert(unlink(f->path) == 0 && rmdir(scratch) == 0);
    assert(azsp_run(provider, &request, &out) == AZ_FAILED);
}

int main(void) {
    struct fixture fixture = {
        .page = {.count = 1, .has_more = 1},
        .playlist = {.id = "foreign/id", .name = "My playlist"},
        .track = {.id = "m/42", .title = "Suite", .duration_ms = 1234}};
    az_source_provider_v1 provider = {.size = sizeof(provider), .name = "Fixture", .storage_mode = AZ_SOURCE_STREAMING, .user = &fixture, .list_playlists = playlists, .playlist_tracks = playlist_tracks, .search = search, .prepare_track = prepare};
    descriptors(provider);
    pages(&provider, &fixture);
    track_pages(&provider, &fixture);
    file_paths(&provider, &fixture);
    puts("Source provider requests, pages and prepared file ownership passed");
    return 0;
}
