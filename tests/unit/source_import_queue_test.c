/* SPDX-License-Identifier: GPL-3.0-only */
#include "source_import.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum {
    EPB_V200 = 2,
    SQLDB_CONN = 8,
    SQLITE_MAGIC_Q = 0,
    INFOUPD_CACHE = 0,
    SQLITE_MAGIC_OPEN = 0xa629a3b7u,
    MV_DEADLINE_MS = 2500
};
static int g_ep147_build = EPB_V200;
static _Atomic int g_mv_kick;
static long long time_ms;
static struct st_source mounts[2];
static int db_current = 1, backup_fails, sql_ready = 1, transaction, writes, analyses, invalidated;
static int watch_failure;
static uint64_t running_ticket;
static uint64_t file_signature = 201;

struct server {
    unsigned media;
    uintptr_t magic;
    uintptr_t model[2];
    uintptr_t cache;
};

static void stock(void *server, void *message, unsigned arg) {
    (void) server;
    (void) message;
    (void) arg;
}
static void (*g_orig_mlserver_onmsg)(void *, void *, unsigned) = stock;
static long long drag_now_ms(void) { return time_ms; }
static int own_q(uintptr_t at, uintptr_t *out) {
    memcpy(out, (const void *) at, sizeof(*out));
    return 1;
}
static int mlserver_chain(void *server, uintptr_t *service, uintptr_t *library, uintptr_t *model, unsigned *media) {
    struct server *s = server;
    if (!s->media) return -1;
    *service = (uintptr_t) &s->cache;
    *library = *service;
    *model = (uintptr_t) s->model;
    *media = s->media;
    return 0;
}
static int autocommit(void *connection) {
    assert(connection);
    return !transaction;
}
static const struct {
    int (*get_autocommit)(void *);
} g_sq = {.get_autocommit = autocommit};
static int sq_resolve(void) { return 1; }
static int playlist_database_current(void *db, const struct st_source *source) {
    assert(db && (source->mount_id || source->device));
    return db_current;
}
static int lib_backup(void *db, FILE *log) {
    assert(db && !log);
    return backup_fails;
}
static void lib_invalidate(uintptr_t cache, void *db, FILE *log) {
    assert(cache && db && !log);
    ++invalidated;
}
static int text(void *stmt, int col, const char *str, int length, void (*destroy)(void *)) {
    (void) stmt;
    (void) col;
    (void) str;
    (void) length;
    (void) destroy;
    return 0;
}
static void *native_symbol(void *scope, const char *name) {
    assert(!scope && !strcmp(name, "sqlite3_bind_text"));
    return (void *) text;
}
#define RTLD_DEFAULT NULL
#define dlsym native_symbol

const struct az_library_sql *az_storage_sql(void) {
    static const struct az_library_sql sql = {0};
    return &sql;
}
int az_storage_sql_ready(void) { return sql_ready; }
int st_source_read(int port, struct st_source *out) {
    *out = mounts[port];
    return AZ_OK;
}
int st_resolution_source_equal(const struct st_source *a, const struct st_source *b) {
    return a->mount_id == b->mount_id && a->ref.source_generation == b->ref.source_generation;
}
int st_source_path(const char *mount, const char *relative, char out[1024]) {
    if (relative[0] != '/') return 0;
    snprintf(out, 1024, "%s%s", mount, relative);
    return 1;
}
uint64_t st_disk_sig(const char *path) {
    assert(path[0] == '/');
    return file_signature;
}

int azspi_commit(const struct azspi_sql *sql, void *db, const struct azspi_record *record, uint32_t *id) {
    assert(sql->api && sql->bind_text && db && record->source.mount_id);
    if (running_ticket) assert(azspi_cancel(running_ticket) == AZ_BUSY);
    *id = 40u + (unsigned) ++writes;
    return AZ_OK;
}
int azspi_analyzer_submit(int firmware, void *database, uint16_t media, const struct st_source *source,
                          const char *path, uint8_t raw[12]) {
    assert(firmware == EPB_V200 && database);
    assert(((media == 0x102 || media == 0x122) && source->mount_id) ||
           (media == 0x142 && source->device));
    assert(!strcmp(path, "/Media/audio.m4a"));
    raw[0] = 1;
    raw[6] = 2;
    raw[8] = 7;
    ++analyses;
    return AZ_PENDING;
}

#include "source_import_native.c"

int azspi_analyzer_submit_watched(int firmware, void *database, uint16_t media,
                                  const struct st_source *source, const char *path,
                                  uint8_t raw[12], azspi_analyzer_arm_fn arm, void *context) {
    const uint8_t requester[12] = {226, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 64};
    assert((media == 0x102 || media == 0x122 || media == 0x142) && arm && context);
    memset(raw, 0, 12);
    raw[0] = 1;
    raw[4] = (uint8_t) media;
    raw[5] = (uint8_t) (media >> 8);
    raw[6] = 2;
    raw[8] = 7;
    int status = arm(raw, requester, context);
    if (status != AZ_OK)
        return status;
    return azspi_analyzer_submit(firmware, database, media, source, path, raw);
}

int azspd_analysis_watch(const uint8_t raw[12], const uint8_t requester[12], uint64_t *ticket) {
    assert(raw[0] == 1 && raw[5] == 1 && raw[6] == 2 && raw[8] == 7);
    assert(raw[4] == 0x02 || raw[4] == 0x22 || raw[4] == 0x42);
    assert(requester[0] == 226 && requester[4] == 2 && requester[11] == 64);
    if (watch_failure)
        return AZ_BUSY;
    *ticket = 46 + (0x42 - raw[4]) / 0x20;
    return AZ_OK;
}

static uint64_t carrier_media;
static struct server *carrier_server;
static unsigned catalog_writes;
static unsigned stream_idle, stream_binds, stream_clears;
uint64_t azsp_carrier_media(void) { return carrier_media; }
uint16_t azsp_carrier_library_media(const void *server) {
    return server == carrier_server ? (uint16_t) carrier_server->media : 0;
}
int azsp_catalog_publish(const struct azspi_sql *sql, void *db, const char *provider,
                         const struct azsp_request *request, const struct azsp_result *result,
                         uint32_t ids[AZ_SOURCE_PAGE_MAX]) {
    assert(sql->api && db && !strcmp(provider, "provider") && request->operation == AZSP_SEARCH);
    assert(result->page.count == 1);
    ++catalog_writes;
    ids[0] = 51;
    return AZ_OK;
}
int azsp_catalog_lookup(const struct azspi_sql *sql, void *db, uint32_t id, char provider[33],
                        az_source_track_v1 *track) {
    assert(sql->api && db && id == 51);
    strcpy(provider, "provider");
    strcpy(track->id, "example");
    return AZ_OK;
}

int azsp_catalog_stream_idle(const struct azspi_sql *sql, void *db, uint32_t id) {
    assert(sql->api && db && id == 51);
    ++stream_idle;
    return AZ_OK;
}
int azsp_catalog_stream_bind(const struct azspi_sql *sql, void *db, uint32_t id, const struct azspi_record *record) {
    assert(sql->api && db && id == 51 && record->source.device);
    ++stream_binds;
    return AZ_OK;
}
int azsp_catalog_stream_clear(const struct azspi_sql *sql, void *db, uint32_t id, const char *audio, const char *dat) {
    assert(sql->api && db && id == 51);
    assert(!strcmp(audio, "/Media/audio.m4a") && !strcmp(dat, "/PIONEER/USBANLZ/ANLZ0000.DAT"));
    ++stream_clears;
    return AZ_OK;
}

#include "source_provider_catalog_native.c"

static void *catalog_page_worker(void *unused) {
    (void) unused;
    const struct azsp_request request = {.operation = AZSP_SEARCH, .value.query = "example"};
    const struct azsp_result result = {.page.count = 1};
    uint32_t ids[AZ_SOURCE_PAGE_MAX];
    assert(azsp_catalog_apply("provider", &request, &result, ids) == AZ_OK && ids[0] == 51);
    return NULL;
}

static void *catalog_lookup_worker(void *unused) {
    (void) unused;
    char provider[33];
    az_source_track_v1 track;
    assert(azsp_catalog_track(51, provider, &track) == AZ_OK);
    assert(!strcmp(provider, "provider") && !strcmp(track.id, "example"));
    return NULL;
}

static void run_catalog_worker(struct server *stock, void *(*worker)(void *), void *argument) {
    pthread_t thread;
    assert(pthread_create(&thread, NULL, worker, argument) == 0);
    while (!azsp_catalog_take_wake()) sched_yield();
    assert(!azsp_catalog_take_wake());
    source_catalog_native(stock);
    struct catalog_job *job = atomic_load(&catalog_job_pending);
    assert(job && job->phase == CATALOG_QUEUED);
    source_catalog_native(carrier_server);
    assert(pthread_join(thread, NULL) == 0);
}

struct stream_command_fixture {
    enum azsp_catalog_stream_operation operation;
    int status;
    uint64_t media;
};

static void *catalog_stream_worker(void *opaque) {
    const struct stream_command_fixture *fixture = opaque;
    const struct azspi_record record = {.source = {.mount = "/carrier", .device = 7}};
    uint64_t analysis_ticket = 0;
    const struct azsp_catalog_stream_request request = {
        .media = fixture->media,
        .id = 51,
        .source = &record.source,
        .record = &record,
        .audio = "/Media/audio.m4a",
        .dat = "/PIONEER/USBANLZ/ANLZ0000.DAT",
        .analysis_ticket = &analysis_ticket,
    };
    assert(azsp_catalog_stream(fixture->operation, &request) == fixture->status);
    if (fixture->operation == AZSP_CATALOG_STREAM_ANALYZE && fixture->status == AZ_OK)
        assert(analysis_ticket == 46);
    else
        assert(!analysis_ticket);
    return NULL;
}

static void drive_stream_jobs(struct server *stock) {
    struct stream_command_fixture commands[] = {
        {AZSP_CATALOG_STREAM_ANALYZE, AZ_STALE, 0x10200000001ULL},
        {AZSP_CATALOG_STREAM_ANALYZE, AZ_OK, 0x14200000001ULL},
        {AZSP_CATALOG_STREAM_BIND, AZ_OK, 0x14200000001ULL},
        {AZSP_CATALOG_STREAM_CLEAR, AZ_OK, 0x14200000001ULL},
    };
    int previous = invalidated;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        run_catalog_worker(stock, catalog_stream_worker, &commands[i]);
    assert(stream_idle == 1 && stream_binds == 1 && stream_clears == 1 && invalidated == previous + 2);
}

static void drive_catalog(struct server *stock) {
    char provider[33];
    az_source_track_v1 track;
    assert(azsp_catalog_track(0, provider, &track) == AZ_INVALID);
    assert(azsp_catalog_track(51, provider, &track) == AZ_UNSUPPORTED);
    struct server carrier = {.media = 0x142, .magic = SQLITE_MAGIC_OPEN, .cache = 231};
    carrier.model[1] = (uintptr_t) &carrier.magic;
    carrier_server = &carrier;
    carrier_media = 0x14200000001ULL;
    int previous = invalidated;
    run_catalog_worker(stock, catalog_page_worker, NULL);
    run_catalog_worker(stock, catalog_lookup_worker, NULL);
    assert(catalog_writes == 1 && invalidated == previous + 1);
    drive_stream_jobs(stock);
}

static void drive_writes(struct server *usb1, struct server *usb2) {
    const struct azspi_record first = {.source = mounts[0]}, second = {.source = mounts[1]};
    uint64_t a, b, c;
    uint32_t track = 0;
    source_import_native(usb1);
    assert(azspi_poll(0, &track) == AZ_STALE);
    assert(azspi_submit(&first, 0x122, &a) == AZ_INVALID);
    g_orig_mlserver_onmsg = NULL;
    assert(azspi_submit(&first, 0x102, &a) == AZ_UNSUPPORTED);
    g_orig_mlserver_onmsg = stock;
    assert(azspi_submit(&first, 0x102, &a) == AZ_PENDING && atomic_load(&g_mv_kick));
    assert(azspi_submit(&second, 0x122, &b) == AZ_PENDING);
    assert(azspi_submit(&first, 0x102, &c) == AZ_PENDING);
    assert(azspi_poll(a, &track) == AZ_PENDING);
    assert(azspi_cancel(a) == AZ_OK && azspi_poll(a, &track) == AZ_CANCELLED);
    source_import_native(usb2);
    assert(azspi_poll(c, &track) == AZ_PENDING);
    assert(azspi_poll(b, &track) == AZ_OK && track == 41);
    running_ticket = c;
    source_import_native(usb1);
    running_ticket = 0;
    assert(azspi_poll(c, &track) == AZ_OK && track == 42 && invalidated == 2);
    assert(azspi_submit(&first, 0x102, &a) == AZ_PENDING);
    time_ms += MV_DEADLINE_MS;
    assert(azspi_poll(a, &track) == AZ_CANCELLED);
    source_import_native(usb1);
    assert(writes == 2);
}

static void check_writer_failures(struct server *usb1) {
    const struct azspi_record first = {.source = mounts[0]};
    uint64_t ticket;
    uint32_t track = 0;
    assert(azspi_submit(&first, 0x102, &ticket) == AZ_PENDING);
    backup_fails = 1;
    source_import_native(usb1);
    assert(azspi_poll(ticket, &track) == AZ_FAILED);
    backup_fails = 0;
    assert(azspi_submit(&first, 0x102, &ticket) == AZ_PENDING);
    sql_ready = 0;
    source_import_native(usb1);
    assert(azspi_poll(ticket, &track) == AZ_UNSUPPORTED);
    sql_ready = 1;
    transaction = 1;
    assert(azspi_submit(&first, 0x102, &ticket) == AZ_PENDING);
    source_import_native(usb1);
    assert(azspi_poll(ticket, &track) == AZ_BUSY);
    transaction = 0;
    assert(azspi_submit(&first, 0x102, &ticket) == AZ_PENDING);
    ++mounts[0].ref.source_generation;
    source_import_native(usb1);
    assert(azspi_poll(ticket, &track) == AZ_STALE && writes == 2);
}

static void drive_analysis(struct server *usb1, struct server *usb2) {
    uint64_t ticket, other, watched = 0;
    uint32_t result;
    assert(azspi_submit_analysis(&mounts[0], 0x102, "relative", &ticket) == AZ_INVALID);
    assert(azspi_submit_analysis(&mounts[0], 0x102, "/Media/audio.m4a", &ticket) == AZ_PENDING);
    assert(azspi_poll(ticket, &result) == AZ_INVALID);
    source_import_native(usb2);
    assert(azspi_poll_analysis(ticket, &watched) == AZ_PENDING && !watched);
    source_import_native(usb1);
    assert(azspi_poll_analysis(ticket, &watched) == AZ_OK && watched == 48);
    assert(azspi_submit_analysis(&mounts[0], 0x102, "/Media/audio.m4a", &other) == AZ_PENDING);
    ++file_signature;
    source_import_native(usb1);
    assert(azspi_poll_analysis(other, &watched) == AZ_STALE && watched == 48 && analyses == 1);
    assert(azspi_submit_analysis(&mounts[1], 0x122, "/Media/audio.m4a", &ticket) == AZ_PENDING);
    watch_failure = 1;
    source_import_native(usb2);
    assert(azspi_poll_analysis(ticket, &watched) == AZ_BUSY && watched == 48 && analyses == 1);
    watch_failure = 0;
    assert(azspi_submit_analysis(&mounts[1], 0x122, "/Media/audio.m4a", &ticket) == AZ_PENDING);
    source_import_native(usb2);
    assert(azspi_poll_analysis(ticket, &watched) == AZ_OK && watched == 47 && analyses == 2);
}

static void drive_destination(struct server *usb1, struct server *usb2) {
    uint64_t ticket;
    unsigned media = 0;
    assert(azspi_submit_destination(&mounts[0], &ticket) == AZ_PENDING);
    source_import_native(usb2);
    assert(azspi_poll_destination(ticket, &media) == AZ_PENDING);
    usb1->media = 0x502;
    source_import_native(usb1);
    assert(azspi_poll_destination(ticket, &media) == AZ_OK && media == 0x502);
    usb1->media = 0x102;
    assert(azspi_submit_destination(&mounts[1], &ticket) == AZ_PENDING);
    db_current = 0;
    source_import_native(usb2);
    assert(azspi_poll_destination(ticket, &media) == AZ_STALE && media == 0x502);
    db_current = 1;
}

int main(void) {
    struct server usb1 = {.media = 0x102, .magic = SQLITE_MAGIC_OPEN, .cache = 123};
    struct server usb2 = {.media = 0x122, .magic = SQLITE_MAGIC_OPEN, .cache = 321};
    usb1.model[1] = (uintptr_t) &usb1.magic;
    usb2.model[1] = (uintptr_t) &usb2.magic;
    mounts[0] = (struct st_source) {.ref.source_id = 1, .mount_id = 11, .mount = "/USB1"};
    mounts[1] = (struct st_source) {.ref.source_id = 2, .mount_id = 22, .mount = "/USB2"};
    drive_writes(&usb1, &usb2);
    check_writer_failures(&usb1);
    drive_analysis(&usb1, &usb2);
    drive_destination(&usb1, &usb2);
    drive_catalog(&usb1);
    puts("Source import native queue ownership, cancellation and dispatch passed");
    return 0;
}
