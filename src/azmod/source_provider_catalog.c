/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "source_provider_catalog.h"
#include "azmod-mod.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* The native 2.00 OneLibrary interrogates these tables when binding a medium.
 * The catalog starts empty. Menu kinds are firmware category identifiers. */
static const char catalog_cue_table[] =
    "CREATE TABLE cue(cue_id INTEGER PRIMARY KEY,content_id INTEGER,kind INTEGER,colorTableIndex INTEGER,cueComment TEXT,isActiveLoop INTEGER,"
    "beatLoopNumerator INTEGER,beatLoopDenominator INTEGER,inUsec INTEGER,outUsec INTEGER,in150FramePerSec INTEGER,out150FramePerSec INTEGER,"
    "inMpegFrameNumber INTEGER,outMpegFrameNumber INTEGER,inMpegAbs INTEGER,outMpegAbs INTEGER,inDecodingStartFramePosition INTEGER,"
    "outDecodingStartFramePosition INTEGER,inFileOffsetInBlock INTEGER,OutFileOffsetInBlock INTEGER,inNumberOfSampleInBlock INTEGER,outNumberOfSampleInBlock INTEGER)";

static const char *const catalog_schema[] = {
    "CREATE TABLE content(content_id INTEGER PRIMARY KEY,title TEXT,titleForSearch TEXT,subtitle TEXT,bpmx100 INTEGER,"
    "length INTEGER,trackNo INTEGER,discNo INTEGER,artist_id_artist INTEGER,artist_id_remixer INTEGER,"
    "artist_id_originalArtist INTEGER,artist_id_composer INTEGER,artist_id_lyricist INTEGER,album_id INTEGER,"
    "genre_id INTEGER,label_id INTEGER,key_id INTEGER,color_id INTEGER,image_id INTEGER,djComment TEXT,rating INTEGER,"
    "releaseYear INTEGER,releaseDate TEXT,dateCreated TEXT,dateAdded TEXT,path TEXT,fileName TEXT,fileSize INTEGER,"
    "fileType INTEGER,bitrate INTEGER,bitDepth INTEGER,samplingRate INTEGER,isrc TEXT,djPlayCount INTEGER,"
    "isHotCueAutoLoadOn INTEGER,isKuvoDeliverStatusOn INTEGER,kuvoDeliveryComment TEXT,masterDbId INTEGER,"
    "masterContentId INTEGER,analysisDataFilePath TEXT,analysedBits INTEGER,contentLink INTEGER,hasModified INTEGER,"
    "cueUpdateCount INTEGER,analysisDataUpdateCount INTEGER,informationUpdateCount INTEGER)",
    "CREATE TABLE genre(genre_id INTEGER PRIMARY KEY,name TEXT)",
    "CREATE TABLE artist(artist_id INTEGER PRIMARY KEY,name TEXT,nameForSearch TEXT)",
    "CREATE TABLE album(album_id INTEGER PRIMARY KEY,name TEXT,artist_id INTEGER,image_id INTEGER,isComplation INTEGER,nameForSearch TEXT)",
    "CREATE TABLE label(label_id INTEGER PRIMARY KEY,name TEXT)",
    "CREATE TABLE key(key_id INTEGER PRIMARY KEY,name TEXT)",
    "CREATE TABLE color(color_id INTEGER PRIMARY KEY,name TEXT)",
    "CREATE TABLE playlist(playlist_id INTEGER PRIMARY KEY,sequenceNo INTEGER,name TEXT,image_id INTEGER,attribute INTEGER,playlist_id_parent INTEGER)",
    "CREATE TABLE playlist_content(playlist_id INTEGER,content_id INTEGER,sequenceNo INTEGER)",
    "CREATE TABLE hotCueBankList(hotCueBankList_id INTEGER PRIMARY KEY,sequenceNo INTEGER,name TEXT,image_id INTEGER,attribute INTEGER,hotCueBankList_id_parent INTEGER)",
    "CREATE TABLE hotCueBankList_cue(hotCueBankList_id INTEGER,cue_id INTEGER,sequenceNo INTEGER)",
    "CREATE TABLE history(history_id INTEGER PRIMARY KEY,sequenceNo INTEGER,name TEXT,attribute INTEGER,history_id_parent INTEGER)",
    "CREATE TABLE history_content(history_id INTEGER,content_id INTEGER,sequenceNo INTEGER)",
    "CREATE TABLE image(image_id INTEGER PRIMARY KEY,path TEXT)",
    catalog_cue_table,
    "CREATE TABLE menuItem(menuItem_id INTEGER PRIMARY KEY,kind INTEGER,name TEXT)",
    "CREATE TABLE category(category_id INTEGER PRIMARY KEY,menuItem_id INTEGER,sequenceNo INTEGER,isVisible INTEGER)",
    "CREATE TABLE sort(sort_id INTEGER PRIMARY KEY,menuItem_id INTEGER,sequenceNo INTEGER,isVisible INTEGER,isSelectedAsSubColumn INTEGER)",
    "CREATE TABLE property(deviceName TEXT,dbVersion TEXT,numberOfContents INTEGER,createdDate TEXT,backGroundColorType INTEGER,myTagMasterDBID INTEGER)",
    "CREATE TABLE recommendedLike(content_id_1 INTEGER,content_id_2 INTEGER,rating INTEGER,createdDate INTEGER)",
    "CREATE TABLE myTag(myTag_id INTEGER PRIMARY KEY,sequenceNo INTEGER,name TEXT,attribute INTEGER,myTag_id_parent INTEGER)",
    "CREATE TABLE myTag_content(myTag_id INTEGER,content_id INTEGER)",
    "CREATE INDEX index_playlist_content_playlist_id ON playlist_content(playlist_id)",
    "CREATE INDEX index_myTag_content_myTag_id ON myTag_content(myTag_id)",
    "CREATE INDEX index_myTag_content_content_id ON myTag_content(content_id)",
    "CREATE INDEX index_hotCueBankList_cue_hotCueBankList_id ON hotCueBankList_cue(hotCueBankList_id)",
    ("CREATE TABLE azProviderTrack(id INTEGER PRIMARY KEY,provider TEXT NOT NULL,remote TEXT NOT NULL,"
     "duration INTEGER,UNIQUE(provider,remote))"),
    "CREATE TABLE azProviderPlaylist(id INTEGER PRIMARY KEY,provider TEXT NOT NULL,remote TEXT NOT NULL,UNIQUE(provider,remote))",
    "CREATE TABLE azProviderSearch(sequenceNo INTEGER PRIMARY KEY,content_id INTEGER NOT NULL)",
    ("INSERT INTO menuItem VALUES (2,129,'￺ARTIST￻'),(3,130,'￺ALBUM￻'),(4,131,'￺TRACK￻'),"
     "(5,133,'￺BPM￻'),(17,132,'￺PLAYLIST￻'),(19,149,'￺HISTORY￻'),(20,145,'￺SEARCH￻'),"
     "(25,161,'￺DEFAULT￻'),(26,162,'￺ALPHABET￻')"),
    "INSERT INTO category VALUES (4,4,1,1),(5,17,2,1),(18,20,3,1),(22,19,4,1)",
    "INSERT INTO sort VALUES (0,25,1,1,0),(1,26,2,1,0),(2,2,3,1,1),(3,3,4,1,0),(4,5,5,1,0)",
    "INSERT INTO property VALUES ('Community Sources','1000',0,'',0,0)",
};

static int catalog_statement(const struct az_library_sql *sql, void *db, const char *query) {
    void *stmt = NULL;
    int ok = sql->prepare(db, query, -1, &stmt, NULL) == 0 && stmt;
    if (ok)
        ok = sql->step(stmt) == 101;
    if (stmt && sql->finalize(stmt))
        ok = 0;
    return ok;
}

static int catalog_tables(const struct az_library_sql *sql, void *db) {
    for (size_t i = 0; i < sizeof(catalog_schema) / sizeof(catalog_schema[0]); ++i) {
        if (!catalog_statement(sql, db, catalog_schema[i]))
            return 0;
    }
    return 1;
}

static int catalog_schema_write(const struct az_library_sql *sql, void *db) {
    if (!catalog_statement(sql, db, "BEGIN IMMEDIATE"))
        return 0;
    if (catalog_tables(sql, db) && catalog_statement(sql, db, "COMMIT"))
        return 1;
    (void) catalog_statement(sql, db, "ROLLBACK");
    return 0;
}

static int catalog_sql_write(const struct az_library_sql *sql, const char *path, int (*key)(void *)) {
    void *db = NULL;
    int ok = sql->open_v2(path, &db, 2, NULL) == 0 && db;
    if (ok)
        ok = key(db) == 0 && catalog_schema_write(sql, db);
    if (db && sql->close(db))
        ok = 0;
    return ok;
}

static int catalog_directory(const char *root, const char *relative, char path[AZ_SOURCE_PATH_BYTES]) {
    if ((unsigned) snprintf(path, AZ_SOURCE_PATH_BYTES, "%s/%s", root, relative) >= AZ_SOURCE_PATH_BYTES)
        return 0;
    if (!mkdir(path, 0700))
        return 1;
    struct stat st;
    return errno == EEXIST && !lstat(path, &st) && S_ISDIR(st.st_mode);
}

static int catalog_path(const char *root, char path[AZ_SOURCE_PATH_BYTES]) {
    if (!root || root[0] != '/')
        return 0;
    if (!catalog_directory(root, "PIONEER", path) || !catalog_directory(root, "PIONEER/rekordbox", path))
        return 0;
    return (unsigned) snprintf(path, AZ_SOURCE_PATH_BYTES, "%s/PIONEER/rekordbox/exportLibrary.db", root) < AZ_SOURCE_PATH_BYTES;
}

int azsp_catalog_create(const char *root, const struct az_library_sql *sql, int (*key)(void *)) {
    char path[AZ_SOURCE_PATH_BYTES];
    if (!catalog_path(root, path))
        return AZ_INVALID;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return errno == EEXIST ? AZ_BUSY : AZ_FAILED;
    close(fd);
    if (catalog_sql_write(sql, path, key))
        return AZ_OK;
    (void) unlink(path);
    return AZ_FAILED;
}
