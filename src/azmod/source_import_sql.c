/* SPDX-License-Identifier: GPL-3.0-only */
#include "source_import.h"
#include "source_provider_catalog.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
    IMPORT_SQL_ROW = 100,
    IMPORT_SQL_DONE = 101,
    IMPORT_ANALYZED_BITS = 57
};

static int import_prepare(const struct azspi_sql *sql, void *db, const char *query, void **stmt) {
    return sql->api->prepare(db, query, -1, stmt, NULL) == 0 && *stmt;
}

static int import_finish(const struct azspi_sql *sql, void *stmt, int status) {
    if (stmt && sql->api->finalize(stmt) != 0)
        return AZ_FAILED;
    return status;
}

static int import_exec(const struct azspi_sql *sql, void *db, const char *query) {
    void *stmt = NULL;
    int status = AZ_FAILED;
    if (import_prepare(sql, db, query, &stmt) && sql->api->step(stmt) == IMPORT_SQL_DONE)
        status = AZ_OK;
    return import_finish(sql, stmt, status);
}

static int import_bind_texts(const struct azspi_sql *sql, void *stmt, const char *const *values,
                             unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (sql->bind_text(stmt, (int) i + 1, values[i], -1, NULL))
            return 0;
    }
    return 1;
}

static int import_bind_numbers(const struct azspi_sql *sql, void *stmt, int first,
                               const long long *values, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (sql->api->bind_int64(stmt, first + (int) i, values[i]))
            return 0;
    }
    return 1;
}

static int import_write_text(const struct azspi_sql *sql, void *db, const char *query,
                             const char *const *arguments, unsigned count) {
    void *stmt = NULL;
    int status = AZ_FAILED;
    if (import_prepare(sql, db, query, &stmt) && import_bind_texts(sql, stmt, arguments, count) &&
        sql->api->step(stmt) == IMPORT_SQL_DONE)
        status = AZ_OK;
    return import_finish(sql, stmt, status);
}

static int import_artist(const struct azspi_sql *sql, void *db, const char *artist) {
    if (!artist[0])
        return AZ_OK;
    static const char query[] =
        "INSERT INTO artist(artist_id,name) SELECT (SELECT COALESCE(MAX(artist_id),0)+1 FROM artist),?1 "
        "WHERE NOT EXISTS(SELECT 1 FROM artist WHERE name=?1 COLLATE BINARY)";
    return import_write_text(sql, db, query, &artist, 1);
}

#define IMPORT_ARTIST_ID "(SELECT artist_id FROM artist WHERE name=NULLIF(?2,'') COLLATE BINARY ORDER BY artist_id LIMIT 1)"
static int import_album(const struct azspi_sql *sql, void *db, const az_source_track_v1 *track) {
    if (!track->album[0])
        return AZ_OK;
    static const char query[] =
        "INSERT INTO album(album_id,name,artist_id,isComplation) SELECT "
        "(SELECT COALESCE(MAX(album_id),0)+1 FROM album),?1," IMPORT_ARTIST_ID ",0 "
        "WHERE NOT EXISTS(SELECT 1 FROM album WHERE name=?1 COLLATE BINARY AND artist_id IS " IMPORT_ARTIST_ID ")";
    const char *const args[] = {track->album, track->artist};
    return import_write_text(sql, db, query, args, 2);
}

static int import_bind_content(const struct azspi_sql *sql, void *stmt, const struct azspi_record *record) {
    const char *name = strrchr(record->audio.relative, '/') + 1;
    const char *const texts[] = {record->track.title, record->track.artist, record->track.album,
                                 record->audio.relative, name, record->analysis[0].relative};
    long long numbers[] = {record->bpm_x100, (record->format.duration_ms + 500LL) / 1000,
                           (long long) record->file_size, record->format.file_type, record->format.bitrate_kbps,
                           record->format.sample_rate, IMPORT_ANALYZED_BITS};
    return import_bind_texts(sql, stmt, texts, sizeof(texts) / sizeof(texts[0])) &&
           import_bind_numbers(sql, stmt, 7, numbers, sizeof(numbers) / sizeof(numbers[0]));
}

static int import_content(const struct azspi_sql *sql, void *db, const struct azspi_record *record) {
    static const char query[] =
        "INSERT INTO content(content_id,title,titleForSearch,artist_id_artist,album_id,path,fileName,analysisDataFilePath,"
        "bpmx100,length,fileSize,fileType,bitrate,samplingRate,analysedBits,isHotCueAutoLoadOn) "
        "SELECT next.id,?1,?1," IMPORT_ARTIST_ID ","
        "(SELECT album_id FROM album WHERE name=NULLIF(?3,'') COLLATE BINARY AND artist_id IS " IMPORT_ARTIST_ID
        " ORDER BY album_id LIMIT 1),?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,1 "
        "FROM (SELECT COALESCE(MAX(content_id),0)+1 AS id FROM content) next WHERE next.id BETWEEN 1 AND 4294967295";
    void *stmt = NULL;
    int status = AZ_FAILED;
    if (import_prepare(sql, db, query, &stmt) && import_bind_content(sql, stmt, record) &&
        sql->api->step(stmt) == IMPORT_SQL_DONE)
        status = AZ_OK;
    return import_finish(sql, stmt, status);
}
#undef IMPORT_ARTIST_ID

static int import_read_id(const struct azspi_sql *sql, void *stmt, uint32_t *track_id) {
    int step = sql->api->step(stmt);
    if (step != IMPORT_SQL_ROW)
        return step == IMPORT_SQL_DONE ? AZ_NOT_FOUND : AZ_FAILED;
    long long id = sql->api->col_int64(stmt, 0);
    if (id <= 0 || id > UINT32_MAX || sql->api->step(stmt) != IMPORT_SQL_DONE)
        return AZ_STALE;
    *track_id = (uint32_t) id;
    return AZ_OK;
}

static int import_lookup(const struct azspi_sql *sql, void *db, const char *query, const char *path,
                         uint32_t *track_id) {
    void *stmt = NULL;
    int status = AZ_FAILED;
    if (import_prepare(sql, db, query, &stmt) && import_bind_texts(sql, stmt, &path, 1))
        status = import_read_id(sql, stmt, track_id);
    return import_finish(sql, stmt, status);
}

static int import_match(const struct azspi_sql *sql, void *db, const struct azspi_record *record, uint32_t *id) {
    static const char query[] =
        "SELECT content_id FROM content WHERE path=?1 COLLATE BINARY AND analysisDataFilePath=?2 COLLATE BINARY AND fileSize=?3";
    const char *const texts[] = {record->audio.relative, record->analysis[0].relative};
    long long size = (long long) record->file_size;
    void *stmt = NULL;
    int status = AZ_FAILED;
    if (import_prepare(sql, db, query, &stmt) && import_bind_texts(sql, stmt, texts, 2) &&
        import_bind_numbers(sql, stmt, 3, &size, 1))
        status = import_read_id(sql, stmt, id);
    return import_finish(sql, stmt, status);
}

static int import_insert(const struct azspi_sql *sql, void *db, const struct azspi_record *record) {
    int status = import_artist(sql, db, record->track.artist);
    if (status == AZ_OK)
        status = import_album(sql, db, &record->track);
    if (status == AZ_OK)
        status = import_content(sql, db, record);
    return status;
}

static int import_transaction(const struct azspi_sql *sql, void *db, const struct azspi_record *record,
                              uint32_t *id) {
    static const char query[] = "SELECT content_id FROM content WHERE path=?1 COLLATE BINARY";
    uint32_t existing;
    int status = import_lookup(sql, db, query, record->audio.relative, &existing);
    if (status == AZ_OK) {
        status = import_match(sql, db, record, id);
        return status == AZ_NOT_FOUND ? AZ_STALE : status;
    }
    if (status != AZ_NOT_FOUND)
        return status;
    status = import_insert(sql, db, record);
    return status == AZ_OK ? import_match(sql, db, record, id) : status;
}

int azspi_commit(const struct azspi_sql *sql, void *db, const struct azspi_record *record,
                 uint32_t *track_id) {
    if (record->file_size > LLONG_MAX)
        return AZ_LIMIT;
    if (sql->autocommit(db) != 1)
        return AZ_BUSY;
    int status = azspi_current(record);
    if (status != AZ_OK)
        return status;
    status = import_exec(sql, db, "BEGIN IMMEDIATE");
    if (status != AZ_OK)
        return status;
    uint32_t id;
    status = import_transaction(sql, db, record, &id);
    if (status == AZ_OK)
        status = azspi_current(record);
    if (status == AZ_OK)
        status = import_exec(sql, db, "COMMIT");
    if (status != AZ_OK)
        (void) import_exec(sql, db, "ROLLBACK");
    else
        *track_id = id;
    return status;
}

static int catalog_step(const struct azspi_sql *sql, void *db, const char *query,
                        const char *const texts[], unsigned nt, const long long *numbers, unsigned nn) {
    void *stmt = NULL;
    int status = AZ_FAILED;
    if (import_prepare(sql, db, query, &stmt) && import_bind_texts(sql, stmt, texts, nt) &&
        import_bind_numbers(sql, stmt, (int) nt + 1, numbers, nn) && sql->api->step(stmt) == IMPORT_SQL_DONE)
        status = AZ_OK;
    return import_finish(sql, stmt, status);
}

static int catalog_find_id(const struct azspi_sql *sql, void *db, const char *query,
                           const char *const args[2], uint32_t *id) {
    void *stmt = NULL;
    int status = AZ_FAILED;
    if (import_prepare(sql, db, query, &stmt) && import_bind_texts(sql, stmt, args, 2))
        status = import_read_id(sql, stmt, id);
    return import_finish(sql, stmt, status);
}

static int catalog_track_id(const struct azspi_sql *sql, void *db, const char *provider,
                            const az_source_track_v1 *track, uint32_t *id) {
    static const char put[] = "INSERT OR IGNORE INTO azProviderTrack(provider,remote) VALUES(?1,?2)";
    static const char update[] = "UPDATE azProviderTrack SET duration=?3 WHERE provider=?1 AND remote=?2";
    static const char get[] = "SELECT id FROM azProviderTrack WHERE provider=?1 AND remote=?2";
    const char *const texts[] = {provider, track->id};
    long long duration = track->duration_ms;
    int status = import_write_text(sql, db, put, texts, 2);
    if (status == AZ_OK)
        status = catalog_step(sql, db, update, texts, 2, &duration, 1);
    return status == AZ_OK ? catalog_find_id(sql, db, get, texts, id) : status;
}

static void catalog_stream_placeholder(uint32_t id, char path[64]) {
    snprintf(path, 64, "/cache/%u", id);
}

static int catalog_content(const struct azspi_sql *sql, void *db, const az_source_track_v1 *track, uint32_t id) {
    static const char insert[] = "INSERT OR IGNORE INTO content(content_id,path,fileName,isHotCueAutoLoadOn) VALUES(?2,?1,?1,1)";
    static const char statement[] =
        "UPDATE content SET title=?1,titleForSearch=?1,"
        "artist_id_artist=(SELECT artist_id FROM artist WHERE name=NULLIF(?2,'') COLLATE BINARY ORDER BY artist_id LIMIT 1),"
        "album_id=(SELECT album_id FROM album WHERE name=NULLIF(?3,'') COLLATE BINARY "
        "AND artist_id IS (SELECT artist_id FROM artist WHERE name=NULLIF(?2,'') COLLATE BINARY ORDER BY artist_id LIMIT 1) "
        "ORDER BY album_id LIMIT 1),bpmx100=CASE WHEN analysisDataFilePath IS NULL THEN ?5 ELSE bpmx100 END,"
        "length=CASE WHEN analysisDataFilePath IS NULL THEN ?6 ELSE length END WHERE content_id=?4";
    char cache[64];
    catalog_stream_placeholder(id, cache);
    const char *const path[] = {cache};
    long long row = id;
    int status = catalog_step(sql, db, insert, path, 1, &row, 1);
    if (status != AZ_OK)
        return status;
    const char *const texts[] = {track->title, track->artist, track->album};
    const long long values[] = {id, track->bpm_x100, (track->duration_ms + 500LL) / 1000};
    return catalog_step(sql, db, statement, texts, 3, values, 3);
}

static int catalog_track(const struct azspi_sql *sql, void *db, const char *provider, const az_source_track_v1 *track,
                         uint32_t *id) {
    int status = catalog_track_id(sql, db, provider, track, id);
    if (status == AZ_OK)
        status = import_artist(sql, db, track->artist);
    if (status == AZ_OK)
        status = import_album(sql, db, track);
    if (status == AZ_OK)
        status = catalog_content(sql, db, track, *id);
    return status;
}

static int catalog_playlist(const struct azspi_sql *sql, void *db, const char *provider,
                            const az_source_playlist_v1 *playlist, uint32_t sequence, uint32_t *id) {
    static const char put[] = "INSERT OR IGNORE INTO azProviderPlaylist(provider,remote) VALUES(?1,?2)";
    static const char get[] = "SELECT id FROM azProviderPlaylist WHERE provider=?1 AND remote=?2";
    const char *const key[] = {provider, playlist->id};
    int status = import_write_text(sql, db, put, key, 2);
    if (status != AZ_OK)
        return status;
    status = catalog_find_id(sql, db, get, key, id);
    if (status != AZ_OK)
        return status;
    static const char upsert[] =
        "INSERT OR REPLACE INTO playlist(playlist_id,sequenceNo,name,attribute,playlist_id_parent) VALUES(?2,?3,?1,0,0)";
    const char *const texts[] = {playlist->name};
    long long values[] = {*id, sequence};
    return catalog_step(sql, db, upsert, texts, 1, values, 2);
}

static int catalog_rewind(const struct azspi_sql *sql, void *db, uint32_t playlist) {
    static const char remove[] = "DELETE FROM playlist_content WHERE playlist_id=?1";
    long long id = playlist;
    return catalog_step(sql, db, remove, NULL, 0, &id, 1);
}

static int catalog_list_id(const struct azspi_sql *sql, void *db, const char *provider,
                           const struct azsp_request *request, uint32_t *playlist) {
    static const char get[] = "SELECT id FROM azProviderPlaylist WHERE provider=?1 AND remote=?2";
    const char *const texts[] = {provider, request->value.playlist_id};
    int status = catalog_find_id(sql, db, get, texts, playlist);
    if (status != AZ_OK || request->offset)
        return status;
    return catalog_rewind(sql, db, *playlist);
}

static int catalog_link(const struct azspi_sql *sql, void *db, uint32_t playlist, uint32_t track, uint32_t sequence) {
    static const char remove[] = "DELETE FROM playlist_content WHERE playlist_id=?1 AND sequenceNo=?2";
    static const char put[] = "INSERT INTO playlist_content(playlist_id,sequenceNo,content_id) VALUES(?1,?2,?3)";
    const long long values[] = {playlist, sequence, track};
    int status = catalog_step(sql, db, remove, NULL, 0, values, 2);
    return status == AZ_OK ? catalog_step(sql, db, put, NULL, 0, values, 3) : status;
}

static int catalog_begin_page(const struct azspi_sql *sql, void *db, const char *provider,
                              const struct azsp_request *request, uint32_t *playlist) {
    if (request->operation == AZSP_PLAYLIST_TRACKS)
        return catalog_list_id(sql, db, provider, request, playlist);
    if (request->offset)
        return AZ_OK;
    if (request->operation == AZSP_PLAYLISTS)
        return import_exec(sql, db, "UPDATE playlist SET playlist_id_parent=4294967295");
    return import_exec(sql, db, "DELETE FROM azProviderSearch");
}

static int catalog_search_rank(const struct azspi_sql *sql, void *db, uint32_t sequence, uint32_t track) {
    static const char query[] = "INSERT OR REPLACE INTO azProviderSearch(sequenceNo,content_id) VALUES(?1,?2)";
    const long long values[] = {sequence, track};
    return catalog_step(sql, db, query, NULL, 0, values, 2);
}

static int catalog_project_row(const struct azspi_sql *sql, void *db, const char *provider,
                               const struct azsp_request *request, const struct azsp_result *result,
                               uint32_t playlist, uint32_t row, uint32_t *id) {
    uint32_t sequence = request->offset + row + 1;
    if (request->operation == AZSP_PLAYLISTS)
        return catalog_playlist(sql, db, provider, &result->value.playlists[row], sequence, id);
    int status = catalog_track(sql, db, provider, &result->value.tracks[row], id);
    if (status != AZ_OK)
        return status;
    if (playlist)
        return catalog_link(sql, db, playlist, *id, sequence);
    if (request->operation == AZSP_SEARCH)
        return catalog_search_rank(sql, db, sequence, *id);
    return status;
}

static int catalog_page(const struct azspi_sql *sql, void *db, const char *provider,
                        const struct azsp_request *request, const struct azsp_result *result,
                        uint32_t ids[AZ_SOURCE_PAGE_MAX]) {
    uint32_t playlist = 0;
    int status = catalog_begin_page(sql, db, provider, request, &playlist);
    for (uint32_t i = 0; i < result->page.count && status == AZ_OK; ++i)
        status = catalog_project_row(sql, db, provider, request, result, playlist, i, &ids[i]);
    return status;
}

int azsp_catalog_publish(const struct azspi_sql *sql, void *db, const char *provider,
                         const struct azsp_request *request, const struct azsp_result *result,
                         uint32_t ids[AZ_SOURCE_PAGE_MAX]) {
    if (result->page.count > AZ_SOURCE_PAGE_MAX || (unsigned) request->operation > AZSP_SEARCH ||
        request->offset > UINT32_MAX - result->page.count)
        return AZ_INVALID;
    int status = import_exec(sql, db, "BEGIN IMMEDIATE");
    if (status != AZ_OK)
        return status;
    uint32_t written[AZ_SOURCE_PAGE_MAX];
    status = catalog_page(sql, db, provider, request, result, written);
    if (status == AZ_OK)
        status = import_exec(sql, db, "COMMIT");
    if (status != AZ_OK)
        (void) import_exec(sql, db, "ROLLBACK");
    else if (ids)
        memcpy(ids, written, result->page.count * sizeof(written[0]));
    return status;
}

static void catalog_text(const struct azspi_sql *sql, void *stmt, int col, char *out, size_t capacity) {
    const unsigned char *text = sql->api->col_text(stmt, col);
    snprintf(out, capacity, "%s", text ? (const char *) text : "");
}

static int catalog_read_track(const struct azspi_sql *sql, void *stmt, char provider[33], az_source_track_v1 *out) {
    int step = sql->api->step(stmt);
    if (step != IMPORT_SQL_ROW)
        return step == IMPORT_SQL_DONE ? AZ_NOT_FOUND : AZ_FAILED;
    az_source_track_v1 track = {0};
    char source[33];
    catalog_text(sql, stmt, 0, source, sizeof(source));
    catalog_text(sql, stmt, 1, track.id, sizeof(track.id));
    catalog_text(sql, stmt, 2, track.title, sizeof(track.title));
    catalog_text(sql, stmt, 3, track.artist, sizeof(track.artist));
    catalog_text(sql, stmt, 4, track.album, sizeof(track.album));
    track.duration_ms = (uint32_t) sql->api->col_int64(stmt, 5);
    track.bpm_x100 = (uint32_t) sql->api->col_int64(stmt, 6);
    if (sql->api->step(stmt) != IMPORT_SQL_DONE)
        return AZ_FAILED;
    memcpy(provider, source, sizeof(source));
    *out = track;
    return AZ_OK;
}

int azsp_catalog_lookup(const struct azspi_sql *sql, void *db, uint32_t id, char provider[33],
                        az_source_track_v1 *track) {
    static const char query[] =
        "SELECT t.provider,t.remote,c.title,a.name,l.name,t.duration,c.bpmx100 FROM azProviderTrack t "
        "JOIN content c ON c.content_id=t.id LEFT JOIN artist a ON a.artist_id=c.artist_id_artist "
        "LEFT JOIN album l ON l.album_id=c.album_id WHERE t.id=?1";
    void *stmt = NULL;
    int status = AZ_FAILED;
    if (import_prepare(sql, db, query, &stmt) && sql->api->bind_int64(stmt, 1, id) == 0)
        status = catalog_read_track(sql, stmt, provider, track);
    return import_finish(sql, stmt, status);
}

static int catalog_stream_state(const struct azspi_sql *sql, void *db, uint32_t id,
                                const char *audio, const char *dat) {
    static const char query[] =
        "SELECT content_id FROM content WHERE content_id=?3 AND path=?1 COLLATE BINARY "
        "AND analysisDataFilePath IS (?2 COLLATE BINARY) AND EXISTS(SELECT 1 FROM azProviderTrack WHERE id=?3)";
    void *stmt = NULL;
    int status = AZ_FAILED;
    const char *const paths[] = {audio, dat};
    long long number = id;
    uint32_t verified;
    if (import_prepare(sql, db, query, &stmt) && import_bind_texts(sql, stmt, paths, 2) &&
        import_bind_numbers(sql, stmt, 3, &number, 1))
        status = import_read_id(sql, stmt, &verified);
    return import_finish(sql, stmt, status);
}

int azsp_catalog_stream_idle(const struct azspi_sql *sql, void *db, uint32_t id) {
    char path[64];
    catalog_stream_placeholder(id, path);
    int status = catalog_stream_state(sql, db, id, path, NULL);
    return status == AZ_NOT_FOUND ? AZ_BUSY : status;
}

static int catalog_stream_write(const struct azspi_sql *sql, void *db, uint32_t id,
                                const struct azspi_record *record) {
    static const char query[] =
        "UPDATE content SET path=?1,fileName=?2,analysisDataFilePath=?3,bpmx100=?4,length=?5,"
        "fileSize=?6,fileType=?7,bitrate=?8,samplingRate=?9,analysedBits=?10,isHotCueAutoLoadOn=1 "
        "WHERE content_id=?11";
    const char *audio = record->audio.relative;
    const char *basename = strrchr(audio, '/');
    if (!basename || record->file_size > LLONG_MAX)
        return AZ_INVALID;
    const char *const texts[] = {audio, basename + 1, record->analysis[0].relative};
    const long long numbers[] = {record->bpm_x100, (record->format.duration_ms + 500LL) / 1000,
                                 (long long) record->file_size, record->format.file_type, record->format.bitrate_kbps,
                                 record->format.sample_rate, IMPORT_ANALYZED_BITS, id};
    return catalog_step(sql, db, query, texts, sizeof(texts) / sizeof(texts[0]),
                        numbers, sizeof(numbers) / sizeof(numbers[0]));
}

static int catalog_stream_finish(const struct azspi_sql *sql, void *db, int status) {
    if (status == AZ_OK)
        status = import_exec(sql, db, "COMMIT");
    if (status != AZ_OK)
        (void) import_exec(sql, db, "ROLLBACK");
    return status;
}

static int catalog_stream_attach(const struct azspi_sql *sql, void *db, uint32_t id,
                                 const struct azspi_record *record) {
    int status = azsp_catalog_stream_idle(sql, db, id);
    if (status == AZ_OK)
        status = catalog_stream_write(sql, db, id, record);
    if (status == AZ_OK)
        status = catalog_stream_state(sql, db, id, record->audio.relative, record->analysis[0].relative);
    if (status == AZ_OK)
        status = azspi_current(record);
    return status;
}

int azsp_catalog_stream_bind(const struct azspi_sql *sql, void *db, uint32_t id, const struct azspi_record *record) {
    int status = azspi_current(record);
    if (status != AZ_OK)
        return status;
    status = import_exec(sql, db, "BEGIN IMMEDIATE");
    if (status != AZ_OK)
        return status;
    return catalog_stream_finish(sql, db, catalog_stream_attach(sql, db, id, record));
}

static int catalog_stream_reset(const struct azspi_sql *sql, void *db, uint32_t id) {
    static const char erase_cues[] = "DELETE FROM cue WHERE content_id=?1";
    static const char reset[] =
        "UPDATE content SET path=?1,fileName=?1,analysisDataFilePath=NULL,analysedBits=0,"
        "fileSize=NULL,fileType=NULL,bitrate=NULL,samplingRate=NULL,bitDepth=NULL,"
        "cueUpdateCount=NULL,analysisDataUpdateCount=NULL WHERE content_id=?2";
    char path[64];
    catalog_stream_placeholder(id, path);
    long long row = id;
    int status = catalog_step(sql, db, erase_cues, NULL, 0, &row, 1);
    if (status == AZ_OK) {
        const char *const paths[] = {path};
        status = catalog_step(sql, db, reset, paths, 1, &row, 1);
    }
    return status == AZ_OK ? catalog_stream_state(sql, db, id, path, NULL) : status;
}

static int catalog_stream_detach(const struct azspi_sql *sql, void *db, uint32_t id,
                                 const char *audio, const char *dat) {
    int status = catalog_stream_state(sql, db, id, audio, dat);
    if (status == AZ_NOT_FOUND)
        return azsp_catalog_stream_idle(sql, db, id) == AZ_OK ? AZ_OK : AZ_STALE;
    return status == AZ_OK ? catalog_stream_reset(sql, db, id) : status;
}

int azsp_catalog_stream_clear(const struct azspi_sql *sql, void *db, uint32_t id, const char *audio, const char *dat) {
    int status = import_exec(sql, db, "BEGIN IMMEDIATE");
    if (status != AZ_OK)
        return status;
    return catalog_stream_finish(sql, db, catalog_stream_detach(sql, db, id, audio, dat));
}
