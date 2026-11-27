/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "source_import.h"
#include "source_import_format.h"
#include "source_provider_carrier.h"
#include "source_provider_catalog.h"
#include "source_provider_lifetime.h"
#include "source_provider_stream.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char audio[] = "/Media/song.m4a";
static const char dat[] = "/PIONEER/USBANLZ/P051/00011239/ANLZ0000.DAT";
static const char ext[] = "/PIONEER/USBANLZ/P051/00011239/ANLZ0000.EXT";
static const char ex2[] = "/PIONEER/USBANLZ/P051/00011239/ANLZ0000.2EX";
static const az_source_track_v1 track = {.id = "source/1", .title = "Track 'α'; --", .artist = "An 'artist'", .album = "Album"};
static const struct azspi_audio format = {.file_type = 4, .sample_rate = 44100, .bitrate_kbps = 192, .duration_ms = 47710};

static int prepare(void *db, const char *query, int length, void **stmt, const char **tail) {
    sqlite3_stmt *prepared = NULL;
    int status = sqlite3_prepare_v2(db, query, length, &prepared, tail);
    *stmt = prepared;
    return status;
}
static int bind_text(void *stmt, int index, const char *text, int length, void (*dispose)(void *)) {
    return sqlite3_bind_text(stmt, index, text, length, dispose);
}
static int bind_number(void *stmt, int index, long long number) { return sqlite3_bind_int64(stmt, index, number); }
static int step(void *stmt) { return sqlite3_step(stmt); }
static long long read_number(void *stmt, int index) { return sqlite3_column_int64(stmt, index); }
static const unsigned char *read_text(void *stmt, int index) { return sqlite3_column_text(stmt, index); }
static int finalize(void *stmt) { return sqlite3_finalize(stmt); }
static int autocommit(void *db) { return sqlite3_get_autocommit(db); }
static int open_database(const char *path, void **out, int flags, const char *vfs) {
    sqlite3 *db = NULL;
    int status = sqlite3_open_v2(path, &db, flags, vfs);
    *out = db;
    return status;
}
static int close_database(void *connection) { return sqlite3_close(connection); }

static const struct az_library_sql library = {.prepare = prepare, .bind_int64 = bind_number, .step = step, .col_int64 = read_number, .col_text = read_text, .finalize = finalize, .open_v2 = open_database, .close = close_database};
static const struct azspi_sql sql = {.api = &library, .bind_text = bind_text, .autocommit = autocommit};

/* track_storage.c resolves assets in the fixture mount. This test does not use
 * its USB-discovery or read-only database interface. */
const struct az_library_sql *az_storage_sql(void) { return &library; }
const char *az_storage_mountinfo(void) { return "/dev/null"; }
int az_storage_stick_mount(int stick, char *path, size_t capacity) {
    (void) stick;
    (void) path;
    (void) capacity;
    return 0;
}

static void absolute(char path[1024], const struct st_source *source, const char *relative) {
    assert((unsigned) snprintf(path, 1024, "%s%s", source->mount, relative) < 1024);
}

static void write_bytes(const struct st_source *source, const char *relative, const void *data, size_t size) {
    char path[1024];
    absolute(path, source, relative);
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(data, 1, size, file) == size && fclose(file) == 0);
}

static void put32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t) (value >> 24);
    dst[1] = (uint8_t) (value >> 16);
    dst[2] = (uint8_t) (value >> 8);
    dst[3] = (uint8_t) value;
}

static void tag(uint8_t *dst, const char *name, uint32_t header, uint32_t size) {
    memcpy(dst, name, 4);
    put32(dst + 4, header);
    put32(dst + 8, size);
}

static void fixture_anlz(const struct st_source *source, const char *relative, const char *associated,
                         const char *wave, int grid) {
    uint8_t bytes[256] = {0};
    uint32_t path_bytes = ((uint32_t) strlen(associated) + 1) * 2;
    tag(bytes + 28, "PPTH", 16, 16 + path_bytes);
    put32(bytes + 40, path_bytes);
    for (size_t i = 0; associated[i]; ++i) bytes[45 + i * 2] = (uint8_t) associated[i];
    uint32_t end = 44 + path_bytes;
    if (grid) {
        tag(bytes + end, "PQTZ", 24, 32);
        put32(bytes + end + 20, 1);
        bytes[end + 26] = 0x2e;
        bytes[end + 27] = 0xe0;
        end += 32;
    }
    tag(bytes + end, wave, 12, 13);
    end += 13;
    tag(bytes, "PMAI", 28, end);
    write_bytes(source, relative, bytes, end);
}

static void empty_mount(const struct st_source *source, int remove) {
    static const char *const dirs[] = {"/Media", "/PIONEER", "/PIONEER/USBANLZ", "/PIONEER/USBANLZ/P051", "/PIONEER/USBANLZ/P051/00011239"};
    char path[1024];
    if (!remove) {
        for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
            absolute(path, source, dirs[i]);
            assert(mkdir(path, 0700) == 0);
        }
        return;
    }
    const char *const files[] = {audio, dat, ext, ex2};
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        absolute(path, source, files[i]);
        assert(unlink(path) == 0);
    }
    for (unsigned i = sizeof(dirs) / sizeof(dirs[0]); i > 0; --i) {
        absolute(path, source, dirs[i - 1]);
        assert(rmdir(path) == 0);
    }
}

static void validation(const struct st_source *source, struct azspi_record *out) {
    write_bytes(source, audio, "complete", 8);
    fixture_anlz(source, dat, audio, "PWAV", 1);
    fixture_anlz(source, ext, audio, "PWV3", 0);
    assert(azspi_inspect(source, &track, &format, audio, dat, out) == AZ_STALE);
    fixture_anlz(source, ex2, "/Media/other.m4a", "PWV6", 0);
    assert(azspi_inspect(source, &track, &format, audio, dat, out) == AZ_FAILED);
    fixture_anlz(source, ex2, audio, "WRNG", 0);
    assert(azspi_inspect(source, &track, &format, audio, dat, out) == AZ_FAILED);
    fixture_anlz(source, ex2, audio, "PWV6", 0);
    assert(azspi_inspect(source, &track, &format, audio, ext, out) == AZ_INVALID);
    az_source_track_v1 bad = track;
    memset(bad.title, 'x', sizeof(bad.title));
    assert(azspi_inspect(source, &bad, &format, audio, dat, out) == AZ_INVALID);
    assert(azspi_inspect(source, &track, &format, audio, dat, out) == AZ_OK);
    assert(out->bpm_x100 == 12000 && out->file_size == 8 && azspi_current(out) == AZ_OK);
}

static void exec(sqlite3 *db, const char *query) {
    assert(sqlite3_exec(db, query, NULL, NULL, NULL) == SQLITE_OK);
}

static long long scalar(sqlite3 *db, const char *query) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK && stmt);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    long long result = sqlite3_column_int64(stmt, 0);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    return result;
}

static sqlite3 *schema(void) {
    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    exec(db, "CREATE TABLE artist(artist_id INTEGER PRIMARY KEY,name TEXT);"
             "CREATE TABLE album(album_id INTEGER PRIMARY KEY,name,artist_id,isComplation);"
             "CREATE TABLE content(content_id INTEGER PRIMARY KEY,title,titleForSearch,artist_id_artist,album_id,"
             "path,fileName,analysisDataFilePath,bpmx100,length,fileSize,fileType,bitrate,samplingRate,analysedBits,isHotCueAutoLoadOn);"
             "INSERT INTO content(content_id,title,path) VALUES(9,'Stock','/Contents/stock.m4a');");
    return db;
}

static void transactions(const struct azspi_record *record) {
    sqlite3 *db = schema();
    uint32_t id = 0;
    exec(db, "BEGIN;");
    assert(azspi_commit(&sql, db, record, &id) == AZ_BUSY && id == 0);
    exec(db, "ROLLBACK;");
    exec(db, "CREATE TRIGGER refuse BEFORE INSERT ON content BEGIN SELECT RAISE(ABORT,'blocked'); END;");
    assert(azspi_commit(&sql, db, record, &id) == AZ_FAILED && id == 0);
    assert(scalar(db, "SELECT (SELECT COUNT(*) FROM artist)+(SELECT COUNT(*) FROM album)") == 0);
    exec(db, "DROP TRIGGER refuse;");
    assert(azspi_commit(&sql, db, record, &id) == AZ_OK && id == 10);
    assert(azspi_commit(&sql, db, record, &id) == AZ_OK && id == 10);
    assert(scalar(db, "SELECT COUNT(*) FROM content") == 2);
    assert(scalar(db, "SELECT COUNT(*) FROM content JOIN artist ON artist.artist_id=artist_id_artist "
                      "JOIN album USING(album_id) WHERE content.title='Track ''α''; --' AND artist.name='An ''artist''' "
                      "AND album.name='Album' AND bpmx100=12000 AND length=48 AND analysedBits=57") == 1);
    exec(db, "UPDATE content SET fileSize=99 WHERE content_id=10;");
    id = 0;
    assert(azspi_commit(&sql, db, record, &id) == AZ_STALE && id == 0);
    exec(db, "INSERT INTO content(content_id,path) VALUES(11,'/Media/song.m4a');");
    assert(azspi_commit(&sql, db, record, &id) == AZ_STALE);
    assert(sqlite3_close(db) == SQLITE_OK);
}

static int reject_audio;
static const char publication_db[] = "/PIONEER/rekordbox/exportLibrary.db";

static void publication_mount(struct st_source *source) {
    char path[1024];
    absolute(path, source, "/PIONEER/rekordbox");
    assert(mkdir(path, 0700) == 0);
    write_bytes(source, publication_db, "library", 7);
    absolute(path, source, publication_db);
    struct stat info;
    assert(!stat(path, &info));
    source->inode = (uint64_t) info.st_ino;
#ifdef __linux__
    int fd = open(source->mount, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    FILE *stream = fopen(path, "r");
    assert(stream);
    unsigned long long id = 0;
    while (fgets(path, sizeof path, stream) && sscanf(path, "mnt_id: %llu", &id) != 1);
    assert(id && fclose(stream) == 0 && close(fd) == 0);
    source->mount_id = id;
#endif
}

static void refuse_replaced_mount(struct azspi_request request) {
    const struct st_source *original = request.source;
    struct st_source replaced = *original;
    ++replaced.inode;
    request.source = &replaced;
    char relative[AZ_SOURCE_PATH_BYTES];
    struct azspi_audio measured;
    assert(azspi_publish(&request, relative, &measured) == AZ_STALE);
#ifdef __linux__
    replaced = *original;
    ++replaced.mount_id;
    assert(azspi_publish(&request, relative, &measured) == AZ_STALE);
#endif
    char path[1024];
    absolute(path, original, "/AZ-MODS-IMPORTS");
    assert(access(path, F_OK) == -1 && errno == ENOENT);
}

static void publication(const struct st_source *source) {
    struct azsp_stage *stage;
    assert(azsp_stage_create(source->mount, AZ_SOURCE_IMPORT, &stage) == AZ_OK);
    struct azsp_result result = {0};
    assert(snprintf(result.value.path, sizeof(result.value.path), "%s/track.M4A", azsp_stage_directory(stage)) > 0);
    FILE *stream = fopen(result.value.path, "wb");
    assert(stream && fputs("audio", stream) >= 0 && fclose(stream) == 0);
    azsp_stage_set_path(stage, &result);
    struct azspi_request request = {.source = source, .firmware = 2, .provider = "provider", .track = &track, .stage = stage};
    refuse_replaced_mount(request);
    struct azspi_audio measured = {0};
    char path[AZ_SOURCE_PATH_BYTES], again[AZ_SOURCE_PATH_BYTES];
    strcpy(path, "unchanged");
    reject_audio = 1;
    assert(azspi_publish(&request, path, &measured) == AZ_UNSUPPORTED);
    assert(!strcmp(path, "unchanged") && measured.file_type == 0);
    char full[1024];
    absolute(full, source, "/AZ-MODS-IMPORTS");
    assert(rmdir(full) == 0); /* decoder rejection left no published or private copy */
    reject_audio = 0;
    assert(azspi_publish(&request, path, &measured) == AZ_OK);
    assert(measured.file_type == format.file_type && measured.sample_rate == format.sample_rate);
    assert(azspi_publish(&request, again, &measured) == AZ_OK && !strcmp(path, again));
    char lowercase[AZ_SOURCE_PATH_BYTES];
    assert(snprintf(lowercase, sizeof lowercase, "%s/track.m4a", azsp_stage_directory(stage)) > 0);
    assert(rename(result.value.path, lowercase) == 0);
    strcpy(result.value.path, lowercase);
    azsp_stage_set_path(stage, &result);
    assert(azspi_publish(&request, again, &measured) == AZ_OK && !strcmp(path, again));
    reject_audio = 1;
    assert(azspi_publish(&request, again, &measured) == AZ_UNSUPPORTED);
    reject_audio = 0;
    write_bytes(source, path, "changed", 7);
    assert(azspi_publish(&request, again, &measured) == AZ_FAILED);
    azsp_stage_release(stage);
    absolute(full, source, path);
    struct stat info;
    assert(!stat(full, &info) && info.st_size == 7);
    assert(unlink(full) == 0);
    *strrchr(full, '/') = 0;
    assert(rmdir(full) == 0);
    *strrchr(full, '/') = 0;
    assert(rmdir(full) == 0);
    assert(azsp_stage_create(source->mount, AZ_SOURCE_STREAMING, &stage) == AZ_OK);
    azsp_stage_set_path(stage, &result);
    request.stage = stage;
    assert(azspi_publish(&request, again, &measured) == AZ_INVALID);
    azsp_stage_release(stage);
}

static int test_key(void *connection) {
    assert(connection);
    return 0;
}
static int key_failure(void *connection) {
    assert(connection);
    return -1;
}

static void catalog_projection(sqlite3 *db) {
    struct azsp_request request = {.operation = AZSP_PLAYLISTS};
    struct azsp_result page = {.page.count = 1, .value.playlists[0] = {.id = "mix", .name = "Source mix"}};
    uint32_t first[AZ_SOURCE_PAGE_MAX], second[AZ_SOURCE_PAGE_MAX];
    assert(azsp_catalog_publish(&sql, db, "source-a", &request, &page, first) == AZ_OK);
    assert(azsp_catalog_publish(&sql, db, "source-b", &request, &page, second) == AZ_OK);
    assert(first[0] && second[0] && first[0] != second[0]);
    assert(scalar(db, "SELECT COUNT(*) FROM playlist WHERE playlist_id_parent=0") == 1);

    request = (struct azsp_request) {.operation = AZSP_PLAYLIST_TRACKS, .value.playlist_id = "mix"};
    page = (struct azsp_result) {.page.count = 2, .value.tracks = {track, track}};
    assert(azsp_catalog_publish(&sql, db, "missing", &request, &page, NULL) == AZ_NOT_FOUND);
    assert(azsp_catalog_publish(&sql, db, "source-b", &request, &page, first) == AZ_OK);
    assert(first[0] && first[0] == first[1]);
    assert(scalar(db, "SELECT COUNT(*) FROM playlist_content") == 2);
    request.offset = 1;
    page.page.count = 1;
    assert(azsp_catalog_publish(&sql, db, "source-b", &request, &page, second) == AZ_OK);
    assert(second[0] == first[0] && scalar(db, "SELECT COUNT(*) FROM playlist_content") == 2);

    struct az_source_track_v1 found;
    char provider[33];
    assert(azsp_catalog_lookup(&sql, db, UINT32_MAX, provider, &found) == AZ_NOT_FOUND);
    assert(azsp_catalog_lookup(&sql, db, first[0], provider, &found) == AZ_OK);
    assert(!strcmp(provider, "source-b") && !strcmp(found.id, track.id));
    assert(!strcmp(found.title, track.title) && !strcmp(found.album, track.album) && !strcmp(found.artist, track.artist));

    request = (struct azsp_request) {.operation = AZSP_SEARCH, .value.query = "keyword"};
    assert(azsp_catalog_publish(&sql, db, "source-a", &request, &page, second) == AZ_OK && first[0] != second[0]);
    assert(scalar(db, "SELECT content_id FROM azProviderSearch WHERE sequenceNo=1") == second[0]);
    assert(scalar(db, "SELECT COUNT(*) FROM content WHERE titleForSearch='keyword'") == 0);
    request = (struct azsp_request) {.operation = AZSP_SEARCH, .value.query = "next"};
    assert(azsp_catalog_publish(&sql, db, "source-b", &request, &page, second) == AZ_OK && first[0] == second[0]);
    assert(scalar(db, "SELECT content_id FROM azProviderSearch WHERE sequenceNo=1") == first[0]);
    assert(scalar(db, "SELECT COUNT(*) FROM azProviderSearch") == 1);
}

static struct {
    sqlite3 *database;
    const char *root;
    int delay, fail_bind, fail_clear, empty_terminal;
    unsigned binds, clears;
    uint64_t analysis_ticket;
    int terminal;
    struct st_source source;
    char audio[AZ_SOURCE_PATH_BYTES], dat[AZ_SOURCE_PATH_BYTES];
} stream_peer;

uint64_t azsp_carrier_media(void) { return UINT64_C(0x14200000001); }
const char *azsp_carrier_root(void) { return stream_peer.root; }

int azspd_analysis_status(uint64_t ticket) {
    assert(ticket && ticket == stream_peer.analysis_ticket);
    return stream_peer.terminal ? AZ_OK : AZ_PENDING;
}

void azspd_analysis_forget(uint64_t ticket) {
    assert(azspd_analysis_status(ticket) == AZ_OK);
    stream_peer.analysis_ticket = 0;
}

int azspi_probe_format(int firmware, const struct st_source *source, const char *relative, struct azspi_audio *out) {
    assert(firmware == 2);
    char path[AZ_SOURCE_PATH_BYTES];
    absolute(path, source, relative);
    assert(!access(path, R_OK));
    if (reject_audio) {
        assert(!strncmp(relative, "/AZ-MODS-IMPORTS/.partial-", strlen("/AZ-MODS-IMPORTS/.partial-")));
        return AZ_UNSUPPORTED;
    }
    *out = format;
    return AZ_OK;
}

static void stream_fixture_parent(char path[AZ_SOURCE_PATH_BYTES], size_t root_length) {
    for (char *separator = path + root_length + 1; (separator = strchr(separator, '/')); ++separator) {
        *separator = 0;
        assert(!mkdir(path, 0700) || errno == EEXIST);
        *separator = '/';
    }
}

static void stream_fixture_generate(void) {
    char full[AZ_SOURCE_PATH_BYTES], file[AZ_SOURCE_PATH_BYTES];
    absolute(full, &stream_peer.source, stream_peer.dat);
    stream_fixture_parent(full, strlen(stream_peer.source.mount));
    strcpy(file, stream_peer.dat);
    size_t extension = strlen(file) - 3;
    fixture_anlz(&stream_peer.source, file, stream_peer.audio, "PWAV", 1);
    strcpy(file + extension, "EXT");
    fixture_anlz(&stream_peer.source, file, stream_peer.audio, "PWV3", 0);
    strcpy(file + extension, "2EX");
    fixture_anlz(&stream_peer.source, file, stream_peer.audio, "PWV6", 0);
}

int azsp_catalog_stream(enum azsp_catalog_stream_operation operation, const struct azsp_catalog_stream_request *request) {
    assert(request->media == azsp_carrier_media() && request->id);
    sqlite3 *db = stream_peer.database;
    switch (operation) {
    case AZSP_CATALOG_STREAM_ANALYZE: {
        assert(azsp_catalog_stream_idle(&sql, db, request->id) == AZ_OK);
        assert(request->analysis_ticket && !stream_peer.analysis_ticket);
        *request->analysis_ticket = stream_peer.analysis_ticket = 46;
        stream_peer.terminal = !stream_peer.delay || stream_peer.empty_terminal;
        stream_peer.source = *request->source;
        strcpy(stream_peer.audio, request->audio);
        strcpy(stream_peer.dat, request->dat);
        if (!stream_peer.delay)
            stream_fixture_generate();
        return AZ_OK;
    }
    case AZSP_CATALOG_STREAM_BIND:
        ++stream_peer.binds;
        if (stream_peer.fail_bind) {
            stream_peer.fail_bind = 0;
            return AZ_STALE;
        }
        return azsp_catalog_stream_bind(&sql, db, request->id, request->record);
    case AZSP_CATALOG_STREAM_CLEAR:
        ++stream_peer.clears;
        if (stream_peer.fail_clear) {
            stream_peer.fail_clear = 0;
            return AZ_BUSY;
        }
        return azsp_catalog_stream_clear(&sql, db, request->id, request->audio, request->dat);
    }
    assert(0);
    return AZ_INVALID;
}

static struct azsp_stage *stream_fixture_stage(const char *root, char path[AZ_SOURCE_PATH_BYTES]) {
    struct azsp_stage *stage = NULL;
    assert(azsp_stage_create(root, AZ_SOURCE_STREAMING, &stage) == AZ_OK);
    const char *dir = azsp_stage_directory(stage);
    assert(!strncmp(strrchr(dir, '/') + 1, "source-", 7));
    struct azsp_result output = {0};
    assert((unsigned) snprintf(output.value.path, sizeof output.value.path, "%s/audio.m4a", dir) < sizeof output.value.path);
    FILE *file = fopen(output.value.path, "wb");
    assert(file && fputs("audio", file) >= 0 && !fclose(file));
    azsp_stage_set_path(stage, &output);
    strcpy(path, output.value.path);
    return stage;
}

static void stream_fixture_prune(void) {
    char path[AZ_SOURCE_PATH_BYTES];
    absolute(path, &stream_peer.source, stream_peer.dat);
    assert(access(path, F_OK) && errno == ENOENT);
    *strrchr(path, '/') = 0;
    assert(access(path, F_OK) && errno == ENOENT);
    *strrchr(path, '/') = 0;
    assert(rmdir(path) == 0);
    *strrchr(path, '/') = 0;
    assert(rmdir(path) == 0);
}

static int last_stream_reader(void *context) {
    assert(scalar(stream_peer.database, "SELECT COUNT(*) FROM content WHERE analysisDataFilePath IS NOT NULL") == 0);
    return *(int *) context;
}

static void stream_binding(sqlite3 *db, uint32_t id) {
    char path[AZ_SOURCE_PATH_BYTES], second_path[AZ_SOURCE_PATH_BYTES];
    struct azsp_stage *stage = stream_fixture_stage(stream_peer.root, path);
    struct azsp_stream *stream = NULL;
    stream_peer.fail_bind = 1;
    assert(azsp_stream_prepare(2, azsp_carrier_media(), id, &track, stage, NULL, NULL, &stream) == AZ_OK);
    assert(stream && !strcmp(azsp_stream_path(stream), path) && stream_peer.binds == 2);
    azsp_stage_release(stage);
    assert(!access(path, R_OK));
    assert(scalar(db, "SELECT COUNT(*) FROM content WHERE path LIKE '/source-%/audio.m4a' AND fileType=4 "
                      "AND analysisDataFilePath LIKE '/PIONEER/USBANLZ/%' AND bitrate=192 AND samplingRate=44100 "
                      "AND bpmx100=12000 AND length=48 AND analysedBits=57") == 1);
    assert(azsp_catalog_stream_clear(&sql, db, id, "/wrong.m4a", stream_peer.dat) == AZ_STALE);
    stage = stream_fixture_stage(stream_peer.root, second_path);
    struct azsp_stream *duplicate = NULL;
    assert(azsp_stream_prepare(2, azsp_carrier_media(), id, &track, stage, NULL, NULL, &duplicate) == AZ_BUSY);
    assert(!duplicate);
    azsp_stage_release(stage);
    stream_peer.fail_clear = 1;
    int reader_done = 0;
    assert(azsp_stream_release_when(stream, last_stream_reader, &reader_done) == AZ_BUSY && !access(path, R_OK));
    assert(azsp_stream_release_when(stream, last_stream_reader, &reader_done) == AZ_BUSY && !access(path, R_OK));
    reader_done = 1;
    assert(azsp_stream_release_when(stream, last_stream_reader, &reader_done) == AZ_OK && stream_peer.clears == 2);
    assert(access(path, F_OK) && errno == ENOENT);
    assert(scalar(db, "SELECT COUNT(*) FROM content WHERE analysisDataFilePath IS NOT NULL") == 0);
    assert(azsp_catalog_stream_idle(&sql, db, id) == AZ_OK);
    stream_fixture_prune();
}

static int cancel_after_poll(void *attempt) { return ++*(unsigned *) attempt >= 3; }

static void stream_cancelled_analysis(uint32_t id) {
    char path[AZ_SOURCE_PATH_BYTES];
    struct azsp_stage *stage = stream_fixture_stage(stream_peer.root, path);
    struct azsp_stream *pending = NULL;
    unsigned attempt = 0;
    stream_peer.delay = 1;
    assert(azsp_stream_prepare(2, azsp_carrier_media(), id, &track, stage, cancel_after_poll, &attempt, &pending) == AZ_CANCELLED);
    azsp_stage_release(stage);
    assert(pending && !access(path, R_OK));
    assert(azsp_stream_release(pending) == AZ_BUSY);
    stream_fixture_generate();
    assert(azsp_stream_release(pending) == AZ_BUSY && !access(path, R_OK));
    stream_peer.terminal = 1;
    assert(azsp_stream_release(pending) == AZ_OK);
    assert(access(path, F_OK) && errno == ENOENT);
    stream_fixture_prune();
}

static struct azsp_stream *cancelled_stream(uint32_t id, char path[AZ_SOURCE_PATH_BYTES]) {
    struct azsp_stage *stage = stream_fixture_stage(stream_peer.root, path);
    struct azsp_stream *pending = NULL;
    unsigned attempt = 0;
    stream_peer.delay = 1;
    assert(azsp_stream_prepare(2, azsp_carrier_media(), id, &track, stage, cancel_after_poll, &attempt, &pending) == AZ_CANCELLED);
    azsp_stage_release(stage);
    assert(pending && !access(path, R_OK));
    return pending;
}

static int reader_releases_with_partial_output(void *context) {
    const char *relative = context;
    write_bytes(&stream_peer.source, relative, "partial", 7);
    return 1;
}

static void stream_unqualified_output(struct azsp_stream *pending, const char *audio_path) {
    char relative[AZ_SOURCE_PATH_BYTES], full[AZ_SOURCE_PATH_BYTES], parent[AZ_SOURCE_PATH_BYTES];
    snprintf(relative, sizeof relative, "%s", stream_peer.dat);
    memcpy(relative + strlen(relative) - 3, "EXT", 4);
    absolute(full, &stream_peer.source, relative);
    stream_fixture_parent(full, strlen(stream_peer.source.mount));
    absolute(parent, &stream_peer.source, relative);
    *strrchr(parent, '/') = '\0';

    write_bytes(&stream_peer.source, relative, "partial", 7);
    assert(azsp_stream_release(pending) == AZ_STALE && !access(audio_path, R_OK));
    assert(unlink(full) == 0);
    assert(rmdir(parent) == 0 && symlink("missing-parent", parent) == 0);
    assert(azsp_stream_release(pending) == AZ_STALE && !access(audio_path, R_OK));
    assert(unlink(parent) == 0 && mkdir(parent, 0700) == 0);

    assert(azsp_stream_release_when(pending, reader_releases_with_partial_output, relative) == AZ_STALE);
    assert(!access(audio_path, R_OK) && unlink(full) == 0);
    assert(rmdir(parent) == 0);
    stream_fixture_prune();
}

static void stream_cancelled_without_output(uint32_t id) {
    char path[AZ_SOURCE_PATH_BYTES];
    struct azsp_stream *pending = cancelled_stream(id, path);

    int reader_done = 0;
    assert(azsp_stream_release_when(pending, last_stream_reader, &reader_done) == AZ_BUSY);
    stream_peer.terminal = 1;
    assert(azsp_stream_release_when(pending, last_stream_reader, &reader_done) == AZ_BUSY);
    assert(stream_peer.analysis_ticket && !access(path, R_OK));
    stream_unqualified_output(pending, path);
    reader_done = 1;
    assert(azsp_stream_release_when(pending, last_stream_reader, &reader_done) == AZ_OK);
    assert(!stream_peer.analysis_ticket && access(path, F_OK) && errno == ENOENT);

    stream_peer.empty_terminal = 1;
    pending = cancelled_stream(id, path);
    assert(stream_peer.terminal && stream_peer.analysis_ticket);
    assert(azsp_stream_release_when(pending, last_stream_reader, &reader_done) == AZ_OK);
    assert(!stream_peer.analysis_ticket && access(path, F_OK) && errno == ENOENT);
    stream_peer.empty_terminal = 0;
}

static void streaming(sqlite3 *db, const char *root) {
    char *canonical = realpath(root, NULL);
    assert(canonical);
    stream_peer.database = db;
    stream_peer.root = canonical;
    struct azsp_request request = {.operation = AZSP_SEARCH, .value.query = "stream"};
    struct azsp_result page = {.page.count = 1};
    page.value.tracks[0] = track;
    uint32_t ids[AZ_SOURCE_PAGE_MAX];
    assert(azsp_catalog_publish(&sql, db, "stream", &request, &page, ids) == AZ_OK);
    stream_binding(db, ids[0]);
    stream_cancelled_analysis(ids[0]);
    stream_cancelled_without_output(ids[0]);
    stream_peer.root = NULL;
    free(canonical);
}

static void catalog_creation(const struct st_source *source) {
    char root[1024], path[1024];
    absolute(root, source, "/Virtual");
    assert(mkdir(root, 0700) == 0);
    assert(azsp_catalog_create(root, &library, key_failure) == AZ_FAILED);
    assert(azsp_catalog_create(root, &library, test_key) == AZ_OK);
    assert(azsp_catalog_create(root, &library, test_key) == AZ_BUSY);
    absolute(path, source, "/Virtual/PIONEER/rekordbox/exportLibrary.db");
    sqlite3 *db = NULL;
    assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    assert(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'") == 25);
    assert(scalar(db, "SELECT COUNT(*) FROM content") == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM category JOIN menuItem USING (menuItem_id) WHERE kind IN (131,132,145)") == 3);
    catalog_projection(db);
    streaming(db, root);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(unlink(path) == 0);
    *strrchr(path, '/') = 0;
    assert(rmdir(path) == 0);
    *strrchr(path, '/') = 0;
    assert(rmdir(path) == 0 && rmdir(root) == 0);
}

int main(void) {
    struct st_source source = {.mount = "/tmp/az-source-import.XXXXXX"};
    assert(mkdtemp(source.mount));
    struct stat mount;
    assert(!stat(source.mount, &mount));
    source.device = (uint64_t) mount.st_dev;
    empty_mount(&source, 0);
    publication_mount(&source);
    struct azspi_record record;
    validation(&source, &record);
    transactions(&record);
    publication(&source);
    catalog_creation(&source);
    write_bytes(&source, audio, "replaced bytes", 14);
    assert(azspi_current(&record) == AZ_STALE);
    sqlite3 *db = schema();
    uint32_t id = 0;
    assert(azspi_commit(&sql, db, &record, &id) == AZ_STALE && id == 0);
    assert(sqlite3_close(db) == SQLITE_OK);
    char path[1024];
    absolute(path, &source, publication_db);
    assert(unlink(path) == 0);
    *strrchr(path, '/') = 0;
    assert(rmdir(path) == 0);
    empty_mount(&source, 1);
    assert(rmdir(source.mount) == 0);
    puts("Source import fileset identity, native library rows and rollback passed");
    return 0;
}
