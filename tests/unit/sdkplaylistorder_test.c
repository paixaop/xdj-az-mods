/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>

#include "../../src/azmod/sdk_playlist_order.c"

static const char *database_path;
static uint64_t generation = 4, signature = 8;
static bool quiet = true, source_present = true, fail_open;
static const char *native_mount = "/usb";
static bool current_owner = true;
static long long now_ms;
static unsigned moves, syncs;
static int writer_result, sync_result = AZ_PDB_CURRENT;

static int prepare(void *db, const char *sql, int n, void **out, const char **tail)
{
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, n, &statement, tail);
    *out = statement;
    return rc;
}
static int bind(void *s, int i, long long value) { return sqlite3_bind_int64(s, i, value); }
static int step(void *s) { return sqlite3_step(s); }
static long long integer(void *s, int i) { return sqlite3_column_int64(s, i); }
static const unsigned char *text(void *s, int i) { return sqlite3_column_text(s, i); }
static int finalize(void *s) { return sqlite3_finalize(s); }
static int close_db(void *db) { return sqlite3_close(db); }

const struct az_library_sql *az_storage_sql(void)
{
    static const struct az_library_sql api = {
        .prepare = prepare, .bind_int64 = bind, .step = step, .col_int64 = integer,
        .col_text = text, .finalize = finalize, .close = close_db
    };
    return &api;
}
int az_storage_sql_ready(void) { return 1; }
void *sdk_db_open(const char *path)
{
    sqlite3 *db = NULL;
    if (fail_open)
        return NULL;
    assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    return db;
}
uint64_t st_disk_sig(const char *path) { assert(!strcmp(path, database_path)); return signature; }
int st_database_quiet(const char *path) { assert(!strcmp(path, database_path)); return quiet; }
int st_source_path(const char *mount, const char *relative, char out[1024])
{
    assert(!strcmp(mount, "/usb") && strstr(relative, "exportLibrary.db"));
    snprintf(out, 1024, "%s", database_path);
    return 1;
}
int st_source_read(int stick, struct st_source *out)
{
    if (stick != 0 || !source_present)
        return AZ_STALE;
    struct stat db;
    assert(stat(database_path, &db) == 0);
    *out = (struct st_source){ .ref = { .source_generation = generation, .library_id = 23 },
        .device = (uint64_t)db.st_dev, .inode = (uint64_t)db.st_ino, .mount = "/usb" };
    return AZ_OK;
}
int st_resolution_source_equal(const struct st_source *a, const struct st_source *b)
{
    return a->ref.source_generation == b->ref.source_generation &&
           a->ref.library_id == b->ref.library_id && a->device == b->device && a->inode == b->inode &&
           !strcmp(a->mount, b->mount);
}

enum { MV_IDLE, MV_ARMED, MV_TAKEN, MV_DONE, MV_SDK_ARMED, MV_SDK_TAKEN, MV_OP_MOVE };
#define MV_DEADLINE_MS 2500
#define SQLDB_CONN 16
#define SQLITE_MAGIC_Q 24
#define SQLITE_MAGIC_OPEN 0xa5
static _Atomic int g_mv_state, g_mv_kick, g_reorder_enabled = 1, g_drag_on;
static struct {
    int op;
    unsigned pl, cid, from, to, media;
} g_mv;
static int t_pst_own;
static sqlite3 *active;
static int autocommit(void *db) { return sqlite3_get_autocommit(db); }
static const struct { int (*get_autocommit)(void *); } g_sq = { .get_autocommit = autocommit };
static int sq_resolve(void) { return 1; }
static long long drag_now_ms(void) { return now_ms; }
static int mlserver_chain(void *server, uintptr_t *service, uintptr_t *library,
                         uintptr_t *db, unsigned *media)
{
    assert(server == active);
    *service = *library = *db = (uintptr_t)active;
    *media = 258;
    return 0;
}
static bool own_q(uintptr_t address, uintptr_t *out)
{
    uintptr_t offset = address - (uintptr_t)active;
    *out = offset == SQLDB_CONN ? (uintptr_t)active : SQLITE_MAGIC_OPEN;
    return true;
}
static int cp_mount_of(void *conn, char *out, size_t cap)
{
    assert(conn == active);
    snprintf(out, cap, "%s", native_mount);
    return 1;
}
static void sql(const char *statement)
{
    assert(sqlite3_exec(active, statement, NULL, NULL, NULL) == SQLITE_OK);
}
static int mv_execute(void *server, FILE *log)
{
    assert(server == active && !log && t_pst_own == 1);
    assert(g_mv.op == MV_OP_MOVE && g_mv.pl == 5 && g_mv.media == 258);
    assert(g_mv.from == 2 && g_mv.to == 3 && g_mv.cid == 11);
    ++moves;
    if (writer_result)
        return writer_result;
    sql("UPDATE playlist_content SET sequenceNo=CASE rowid WHEN 200 THEN 3 WHEN 300 THEN 2 "
        "ELSE sequenceNo END WHERE playlist_id=5");
    return 0;
}
static FILE *azlog_open(const char *path, const char *mode)
{
    assert(strstr(path, "azmod-libspool") && !strcmp(mode, "a"));
    return NULL;
}
static int ls_pdb_sync_root(void *conn, const char *root, const char *src, const char *why, FILE *log)
{
    assert(conn == active && !t_pst_own && !src && !log);
    assert(!strcmp(root, "/usb") && strstr(why, "edit op "));
    ++syncs;
    return sync_result == AZ_PDB_FAILED ? -1 : 0;
}
#include "../../src/azmod/playlist_device_sync.c"
#include "../../src/azmod/sdk_playlist_move.c"

#define CM_RA() 1
static int g_cm_on_ui;
static int st_mod_owner(const char *id, uintptr_t caller, const char *name)
{
    assert(caller == 1 && name[0]);
    return current_owner && (!strcmp(id, "a") || !strcmp(id, "b")) ? 0 : -1;
}
static int sdk_sq_resolve(void) { return 1; }
#include "../../src/azmod/sdk_playlist_bindings.c"

static az_playlist_order_v1 order(unsigned playlist, unsigned offset, az_playlist_entry_v1 *rows, unsigned capacity)
{
    az_playlist_order_v1 out = { .size = sizeof out, .version = AZ_DATA_VERSION };
    assert(sdk_playlist_order("a", 0, playlist, offset, rows, capacity, &out) == AZ_OK);
    return out;
}
static az_playlist_move_v1 prepare_move(void)
{
    az_playlist_entry_v1 entry;
    az_playlist_order_v1 before = order(5, 1, &entry, 1);
    assert(before.total == 3 && before.returned == 1 && entry.position == 2 && entry.track_id == 11);
    return (az_playlist_move_v1){ .size = sizeof(az_playlist_move_v1), .version = AZ_DATA_VERSION,
       .playlist_id = 5, .from = 2, .to = 3, .entry_id = entry.entry_id,
       .source_generation = before.source_generation, .library_id = before.library_id,
       .revision = before.revision };
}
static uint64_t submit(const az_playlist_move_v1 *request)
{
    uint64_t id;
    assert(sdk_playlist_move("a", request, &id) == AZ_PENDING);
    return id;
}
static az_playlist_move_result_v1 result(uint64_t id, int status)
{
    az_playlist_move_result_v1 out = { .size = sizeof out, .version = AZ_DATA_VERSION };
    assert(sdk_playlist_move_status("a", id, &out) == status);
    return out;
}
static void read_guards(void)
{
    az_playlist_order_v1 out = { .size = sizeof out, .version = AZ_DATA_VERSION };
    assert(order(6, 0, NULL, 0).total == 0);
    assert(sdk_playlist_order("a", 0, 7, 0, NULL, 0, &out) == AZ_NOT_FOUND);
    assert(sdk_playlist_order("a", 0, 0, 0, NULL, 0, &out) == AZ_INVALID);
    assert(sdk_playlist_order("a", 0, 5, 0, NULL, 1, &out) == AZ_INVALID);
    assert(sdk_playlist_order("a", 0, 5, 0, NULL, 0, NULL) == AZ_INVALID);
    assert(sdk_playlist_order("a", 0, 5, 0, NULL, AZ_PLAYLIST_PAGE_LIMIT + 1, &out) == AZ_INVALID);
    quiet = false;
    assert(sdk_playlist_order("a", 0, 5, 0, NULL, 0, &out) == AZ_BUSY);
    quiet = true;
    fail_open = true;
    assert(sdk_playlist_order("a", 0, 5, 0, NULL, 0, &out) == AZ_FAILED);
    fail_open = false;
    source_present = false;
    assert(sdk_playlist_order("a", 0, 5, 0, NULL, 0, &out) == AZ_STALE);
    source_present = true;
    g_cm_on_ui = 1;
    assert(sdk_playlist_order("a", 0, 5, 0, NULL, 0, &out) == AZ_INVALID);
    g_cm_on_ui = 0;
}

static void guarded_move(void)
{
    az_playlist_move_v1 move = prepare_move();
    uint64_t id = submit(&move), unused = 0;
    assert(sdk_playlist_move("a", &move, &unused) == AZ_BUSY);
    result(id, AZ_PENDING);
    az_playlist_move_result_v1 foreign = { .size = sizeof foreign, .version = AZ_DATA_VERSION };
    assert(sdk_playlist_move_status("b", id, &foreign) == AZ_STALE);
    assert(sdk_playlist_move_status("a", 0, &foreign) == AZ_INVALID);
    atomic_store(&g_drag_on, 1);
    playlist_sdk_tick();
    assert(atomic_load(&g_mv_state) == MV_IDLE);
    atomic_store(&g_drag_on, 0);
    playlist_sdk_tick();
    assert(atomic_load(&g_mv_state) == MV_SDK_ARMED && atomic_load(&g_mv_kick));
    native_mount = "/other";
    playlist_sdk_native(active);
    result(id, AZ_PENDING);
    native_mount = "/usb";
    sync_result = AZ_PDB_FAILED;
    playlist_sdk_native(active);
    az_playlist_move_result_v1 done = result(id, AZ_OK);
    assert(done.status == AZ_OK && done.outcome == AZ_PLAYLIST_MOVED && done.device_library == AZ_PDB_FAILED);
    az_playlist_entry_v1 rows[3];
    az_playlist_order_v1 after = order(5, 0, rows, 3);
    assert(after.revision != move.revision && rows[2].entry_id == move.entry_id);
    assert(rows[0].entry_id == 100 && rows[0].track_id == rows[2].track_id && rows[1].entry_id == 300);
    assert(moves == 1 && syncs == 1 && atomic_load(&g_mv_state) == MV_IDLE);

    id = submit(&move);
    playlist_sdk_tick();
    playlist_sdk_native(active);
    done = result(id, AZ_OK);
    assert(done.status == AZ_STALE && done.outcome == AZ_PLAYLIST_NOT_MOVED && moves == 1);
    sql("UPDATE playlist_content SET sequenceNo=CASE rowid WHEN 200 THEN 2 WHEN 300 THEN 3 ELSE sequenceNo END");
}

static void refused_moves(void)
{
    az_playlist_move_v1 move = prepare_move();
    uint64_t receipt = 0;
    assert(sdk_playlist_move("a", NULL, &receipt) == AZ_INVALID);
    atomic_store(&g_reorder_enabled, 0);
    assert(sdk_playlist_move("a", &move, &receipt) == AZ_UNSUPPORTED);
    atomic_store(&g_reorder_enabled, 1);
    ++generation;
    assert(sdk_playlist_move("a", &move, &receipt) == AZ_STALE);
    move = prepare_move();
    receipt = submit(&move);
    playlist_sdk_tick();
    now_ms += MV_DEADLINE_MS;
    playlist_sdk_native(active);
    playlist_sdk_tick();
    assert(result(receipt, AZ_OK).status == AZ_CANCELLED && moves == 1);

    receipt = submit(&move);
    playlist_sdk_tick();
    sqlite3 *other = NULL;
    assert(sqlite3_open(":memory:", &other) == SQLITE_OK);
    sqlite3 *usb = active;
    active = other;
    playlist_sdk_native(other);
    active = usb;
    assert(result(receipt, AZ_OK).status == AZ_STALE && moves == 1);
    assert(sqlite3_close(other) == SQLITE_OK);

    move.to = 4;
    receipt = submit(&move);
    playlist_sdk_tick();
    playlist_sdk_native(active);
    assert(result(receipt, AZ_OK).status == AZ_INVALID && moves == 1);
    move.to = 3;
    writer_result = -13;
    receipt = submit(&move);
    playlist_sdk_tick();
    playlist_sdk_native(active);
    assert(result(receipt, AZ_OK).outcome == AZ_PLAYLIST_UNCERTAIN && moves == 2);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    database_path = argv[1];
    assert(sqlite3_open(database_path, &active) == SQLITE_OK);
    sql("CREATE TABLE playlist(playlist_id INTEGER PRIMARY KEY, attribute INT, name TEXT);"
        "CREATE TABLE playlist_content(playlist_id INT, content_id INT, sequenceNo INT);"
        "INSERT INTO playlist VALUES(5,0,'Set'),(6,0,'Empty'),(7,1,'Folder');"
        "INSERT INTO playlist_content(rowid,playlist_id,content_id,sequenceNo)"
        "VALUES(100,5,11,1),(200,5,11,2),(300,5,22,3);");
    read_guards();
    guarded_move();
    refused_moves();
    assert(sqlite3_close(active) == SQLITE_OK);
    puts("USB playlist SDK snapshots, duplicate occurrences, move receipts and stale guards passed");
}
