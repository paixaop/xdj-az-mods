/* SPDX-License-Identifier: GPL-3.0-only */
#include "source_provider_catalog.h"
#include "source_provider_query.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define TRACKS                                                                                                   \
    "SELECT content_id, title, subtitle, bpmx100, length, trackNo, artist_id_artist, artist_id_remixer, "        \
    "artist_id_originalArtist, artist_id_composer, artist_id_lyricist, album_id, genre_id, label_id, key_id, "   \
    "color_id, image_id, djComment, rating, releaseYear, dateAdded, path, fileName, fileType, bitrate, isrc, "   \
    "djPlayCount, isHotCueAutoLoadOn, isKuvoDeliverStatusOn, kuvoDeliveryComment, masterDbId, masterContentId, " \
    "analysisDataFilePath FROM content"
#define PLAYLISTS "SELECT playlist_id, sequenceNo, name, image_id, attribute, playlist_id_parent FROM playlist"
#define HISTORY "SELECT history_id, sequenceNo, name, attribute, history_id_parent FROM history"

static uint16_t library_owner;
static const uint16_t browse = 0x1004, search = 0x1500;

uint16_t azsp_carrier_library_media(const void *server) {
    return server == &library_owner ? 0x142 : 0;
}

static int open_database(const char *path, void **out, int flags, const char *vfs) {
    sqlite3 *db = NULL;
    int status = sqlite3_open_v2(path, &db, flags, vfs);
    *out = db;
    return status;
}

static int prepare(void *db, const char *sql, int len, void **out, const char **tail) {
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(db, sql, len, &statement, tail);
    if (out) *out = statement;
    return result;
}

static int step(void *stmt) { return sqlite3_step(stmt); }
static int finalize(void *stmt) { return sqlite3_finalize(stmt); }
static int close_database(void *db) { return sqlite3_close(db); }
static int plaintext_test_key(void *db) { return db ? 0 : 1; }

static sqlite3 *fixture(const char *root) {
    const struct az_library_sql sql = {
        .open_v2 = open_database,
        .prepare = prepare,
        .step = step,
        .finalize = finalize,
        .close = close_database,
    };
    assert(azsp_catalog_create(root, &sql, plaintext_test_key) == AZ_OK);
    char path[AZ_SOURCE_PATH_BYTES];
    int size = snprintf(path, sizeof path, "%s/PIONEER/rekordbox/exportLibrary.db", root);
    assert(size > 0 && (size_t) size < sizeof path);
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    static const char rows[] =
        "INSERT INTO content(content_id,title,titleForSearch,artist_id_artist,album_id) VALUES"
        "(1,'Blue','Unrelated',10,20),(2,'Red','Unrelated',11,21),(3,'Green','Unrelated',10,20),"
        "(4,'Test literal','',11,21);"
        "INSERT INTO azProviderTrack(id,provider,remote) VALUES"
        "(1,'alpha','1'),(2,'beta','2'),(3,'alpha','3'),(4,'x''); DELETE FROM content; --','4');"
        "INSERT INTO azProviderSearch VALUES (1,3),(2,2),(3,1);"
        "INSERT INTO playlist(playlist_id,name,playlist_id_parent) VALUES (10,'A',0),(20,'B',0);"
        "INSERT INTO azProviderPlaylist VALUES (10,'alpha','1'),(20,'beta','1');"
        "INSERT INTO playlist_content VALUES (10,1,1),(20,2,1);"
        "INSERT INTO history(history_id,name) VALUES (100,'First'),(200,'Second');"
        "INSERT INTO history_content VALUES (100,1,1),(200,2,1);"
        "INSERT INTO artist(artist_id,name) VALUES (10,'Artist A'),(11,'Artist B');"
        "INSERT INTO album(album_id,name) VALUES (20,'Album A'),(21,'Album B');";
    assert(sqlite3_exec(db, rows, NULL, NULL, NULL) == SQLITE_OK);
    return db;
}

static void expect_ids(sqlite3 *db, const char *query, const int *ids, size_t count) {
    void *stmt = NULL;
    const char *tail = NULL;
    assert(azsp_query_prepare(db, query, -1, &stmt, &tail, prepare) == SQLITE_OK);
    assert(tail == query + strlen(query));
    for (size_t i = 0; i < count; ++i) {
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(sqlite3_column_int(stmt, 0) == ids[i]);
    }
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
}

static void browse_scope(sqlite3 *db) {
    struct azsp_query_scope owner, nested;
    azsp_query_begin(&library_owner, &browse, db, "alpha", &owner);
    expect_ids(db, TRACKS " ORDER BY content_id", (int[]) {1, 3}, 2);
    expect_ids(db, TRACKS " WHERE content_id = 2", (int[]) {2}, 1);
    expect_ids(db, "SELECT artist.artist_id, artist.name FROM artist INNER JOIN ( SELECT DISTINCT artist_id_artist FROM content ) AS sub ON sub.artist_id_artist = artist.artist_id",
               (int[]) {10}, 1);
    expect_ids(db, "SELECT album.album_id, album.name, album.image_id FROM album INNER JOIN ( SELECT DISTINCT album_id FROM content WHERE artist_id_artist = 10 ) AS sub ON sub.album_id = album.album_id",
               (int[]) {20}, 1);
    expect_ids(db, "SELECT album_id FROM content WHERE artist_id_artist = 11", NULL, 0);
    expect_ids(db, PLAYLISTS " WHERE playlist_id_parent = 0", (int[]) {10}, 1);
    expect_ids(db, "SELECT playlist_id, content_id, sequenceNo FROM playlist_content WHERE playlist_id = 20", NULL, 0);
    expect_ids(db, HISTORY, (int[]) {100}, 1);
    expect_ids(db, "SELECT history_id, content_id, sequenceNo FROM history_content WHERE history_id = 200", NULL, 0);
    azsp_query_begin(NULL, &browse, db, "alpha", &nested);
    expect_ids(db, TRACKS " ORDER BY content_id", (int[]) {1, 2, 3, 4}, 4);
    azsp_query_end(&nested);
    expect_ids(db, TRACKS, (int[]) {1, 3}, 2);
    azsp_query_end(&owner);
}

static void search_scope(sqlite3 *db) {
    struct azsp_query_scope scope;
    azsp_query_begin(&library_owner, &search, db, "alpha", &scope);
    expect_ids(db, TRACKS, (int[]) {3, 1}, 2);
    void *stmt = NULL;
    assert(azsp_query_prepare(db, TRACKS, -1, &stmt, NULL, prepare) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(!strcmp((const char *) sqlite3_column_text(stmt, 1), scope.search_token));
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    assert(azsp_query_prepare(db, TRACKS " WHERE content_id = 2", -1, &stmt, NULL, prepare) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(!strcmp((const char *) sqlite3_column_text(stmt, 1), "Red"));
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    char previous[8];
    memcpy(previous, scope.search_token, sizeof previous);
    azsp_query_end(&scope);
    azsp_query_begin(&library_owner, &search, db, "beta", &scope);
    assert(strcmp(previous, scope.search_token));
    expect_ids(db, TRACKS, (int[]) {2}, 1);
    azsp_query_end(&scope);
}

static void guard_scope(sqlite3 *db) {
    struct azsp_query_scope scope;
    azsp_query_begin(&library_owner, NULL, db, "x'); DELETE FROM content; --", &scope);
    expect_ids(db, TRACKS, (int[]) {4}, 1);
    char exact[sizeof TRACKS - 1];
    memcpy(exact, TRACKS, sizeof exact);
    const char *tail = NULL;
    void *stmt = NULL;
    assert(azsp_query_prepare(db, exact, sizeof exact, &stmt, &tail, prepare) == SQLITE_OK);
    assert(tail == exact + sizeof exact && sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 4 && sqlite3_finalize(stmt) == SQLITE_OK);
    assert(azsp_query_prepare(db, TRACKS " ; DELETE FROM content", -1, &stmt, &tail, prepare) == SQLITE_ERROR);
    assert(!stmt);
    /* Unknown SQL and writes keep the original SQLite contract. */
    assert(azsp_query_prepare(db, "UPDATE content SET rating=1 WHERE content_id=4", -1, &stmt, NULL, prepare) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_DONE && sqlite3_finalize(stmt) == SQLITE_OK);
    azsp_query_end(&scope);
    azsp_query_begin(&library_owner, &browse, db, NULL, &scope);
    expect_ids(db, TRACKS, NULL, 0);
    expect_ids(db, TRACKS " WHERE content_id = 2", (int[]) {2}, 1);
    azsp_query_end(&scope);
}

static int unexpected_hook(uintptr_t address, void *entry, void **original) {
    (void) address;
    (void) entry;
    (void) original;
    assert(!"an unrecognized firmware build must leave native code untouched");
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(azsp_query_install(0, unexpected_hook) == AZ_UNSUPPORTED);
    sqlite3 *db = fixture(argv[1]);
    browse_scope(db);
    search_scope(db);
    guard_scope(db);
    assert(sqlite3_close(db) == SQLITE_OK);
    puts("native provider query tests passed");
    return 0;
}
