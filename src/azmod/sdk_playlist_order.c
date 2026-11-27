/* SPDX-License-Identifier: GPL-3.0-only */
#include "library_sql.h"
#include "track_storage.h"
#include <stdbool.h>
#include <string.h>

#define PLAYLIST_SCAN_LIMIT 20000u

static uint64_t playlist_hash_number(uint64_t hash, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        hash = (hash ^ (value & 255)) * UINT64_C(1099511628211);
        value >>= 8;
    }
    return hash;
}

static uint64_t playlist_hash_name(const unsigned char *name, uint32_t playlist) {
    uint64_t hash = playlist_hash_number(UINT64_C(14695981039346656037), playlist);
    for (const unsigned char *p = name; p && *p; ++p)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}

static bool playlist_empty_row(long long row, long long track, long long sequence) {
    return !row && !track && !sequence;
}

static int playlist_order_row(const struct az_library_sql *sql, void *statement,
                              uint32_t offset, az_playlist_entry_v1 *page, uint32_t capacity,
                              az_playlist_order_v1 *order) {
    long long row = sql->col_int64(statement, 1);
    long long track = sql->col_int64(statement, 2);
    long long sequence = sql->col_int64(statement, 3);
    if (!order->total && playlist_empty_row(row, track, sequence))
        return AZ_OK; /* LEFT JOIN placeholder for an empty playlist */
    if (row <= 0 || track <= 0 || track > UINT32_MAX)
        return AZ_FAILED;
    if (order->total == PLAYLIST_SCAN_LIMIT)
        return AZ_LIMIT;
    uint32_t position = ++order->total;
    order->revision = playlist_hash_number(order->revision, (uint64_t) row);
    order->revision = playlist_hash_number(order->revision, (uint64_t) track);
    order->revision = playlist_hash_number(order->revision, (uint64_t) sequence);
    if (position > offset && order->returned < capacity)
        page[order->returned++] = (az_playlist_entry_v1) {(uint64_t) row, (uint32_t) track, position};
    return AZ_OK;
}

static int playlist_scan(const struct az_library_sql *sql, void *statement, uint32_t playlist,
                         uint32_t offset, az_playlist_entry_v1 *page, uint32_t capacity,
                         az_playlist_order_v1 *order) {
    int step = sql->step(statement);
    if (step != 100)
        return step == 101 ? AZ_NOT_FOUND : AZ_FAILED;
    order->revision = playlist_hash_name(sql->col_text(statement, 0), playlist);
    do {
        int status = playlist_order_row(sql, statement, offset, page, capacity, order);
        if (status != AZ_OK)
            return status;
        step = sql->step(statement);
    } while (step == 100);
    if (!order->revision)
        order->revision = 1;
    return step == 101 ? AZ_OK : AZ_FAILED;
}

static int playlist_order_query(void *database, uint32_t playlist, uint32_t offset,
                                az_playlist_entry_v1 *page, uint32_t capacity,
                                az_playlist_order_v1 *order) {
    static const char query[] =
        "SELECT p.name, pc.rowid, pc.content_id, pc.sequenceNo FROM playlist p "
        "LEFT JOIN playlist_content pc USING (playlist_id) "
        "WHERE p.playlist_id=?1 AND p.attribute=0 ORDER BY pc.sequenceNo, pc.rowid";
    const struct az_library_sql *sql = az_storage_sql();
    void *statement = NULL;
    if (sql->prepare(database, query, -1, &statement, NULL) != 0)
        return AZ_FAILED;
    int rc = sql->bind_int64(statement, 1, playlist) == 0
                 ? playlist_scan(sql, statement, playlist, offset, page, capacity, order)
                 : AZ_FAILED;
    sql->finalize(statement);
    return rc;
}

static int playlist_snapshot_open(const char *path, uint32_t playlist, uint32_t offset,
                                  az_playlist_entry_v1 *page, uint32_t capacity,
                                  az_playlist_order_v1 *order) {
    uint64_t before = st_disk_sig(path);
    if (!before || !st_database_quiet(path))
        return AZ_BUSY;
    void *database = sdk_db_open(path);
    if (!database)
        return AZ_FAILED;
    int rc = playlist_order_query(database, playlist, offset, page, capacity, order);
    az_storage_sql()->close(database);
    if (before != st_disk_sig(path) || !st_database_quiet(path))
        return AZ_STALE;
    return rc;
}

static int playlist_snapshot(int stick, uint32_t playlist, uint32_t offset,
                             az_playlist_entry_v1 *page, uint32_t capacity,
                             az_playlist_order_v1 *out) {
    struct st_source source, after;
    char database[1024];
    if (st_source_read(stick, &source) != AZ_OK ||
        !st_source_database(&source, database))
        return AZ_STALE;
    az_playlist_order_v1 order = {.size = sizeof order, .version = AZ_DATA_VERSION, .source_generation = source.ref.source_generation, .library_id = source.ref.library_id};
    int rc = playlist_snapshot_open(database, playlist, offset, page, capacity, &order);
    if (rc != AZ_OK)
        return rc;
    if (st_source_read(stick, &after) != AZ_OK || !st_resolution_source_equal(&source, &after))
        return AZ_STALE;
    *out = order;
    return AZ_OK;
}
