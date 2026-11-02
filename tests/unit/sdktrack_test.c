/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the actual disk provider with the existing library suite's fake USB/sysfs and
 * read-only SQLite fixtures. No firmware key, native objects or physical deck required. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#define main sdklib_fixture_main
#include "sdklib_test.c"
#undef main
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
/* Fail an existing file open without depending on test-user/root permissions. */
static const char *st_test_open_path;
static int st_test_open_error;
static int st_test_count_reads;
static size_t st_test_read_bytes, st_test_largest_read;
static ssize_t st_test_pread(int fd, void *out, size_t n, off_t offset) {
    ssize_t rc = pread(fd, out, n, offset);
    if (st_test_count_reads) {
        if (n > st_test_largest_read) st_test_largest_read = n;
        if (rc > 0) st_test_read_bytes += (size_t) rc;
    }
    return rc;
}
static int st_test_open(const char *path, int flags) {
    if (st_test_open_path && !strcmp(path, st_test_open_path)) {
        errno = st_test_open_error;
        return -1;
    }
    return open(path, flags);
}
static char st_test_mountinfo[PATH_MAX];
#ifdef __APPLE__
static int st_test_thread_name(pthread_t thread, const char *name) {
    (void) thread;
    return pthread_setname_np(name);
}
#define pthread_setname_np(thread, name) st_test_thread_name(thread, name)
#define st_mtim st_mtimespec
#endif
#define ST_MOUNTINFO st_test_mountinfo
#define g_cm (&g_mod)
#define az_thread_create pthread_create
#define open st_test_open
#define pread st_test_pread
#include "sdk_track.c"
#undef pread
#undef open
#ifdef __APPLE__
#undef st_mtim
#undef pthread_setname_np
#endif
unsigned azsdk_sources_sweep(void) { return 0; } /* Registry ownership is tested separately. */
/* The library fixture exposes SQLite's opaque typed handle. The provider's
 * dynamic API uses void*, so retain its exact function-pointer signature. */
static int st_test_database_key(void *db, const void *key, int bytes) {
    return sqlite3_key((sqlite3 *) db, key, bytes);
}
static uint32_t sd_live_fields(void) { return 0; }
static int sd_deck_snapshot_v1(int deck, az_deck_snapshot *out) {
    (void) deck;
    (void) out;
    return AZ_UNSUPPORTED;
}
static void mountinfo(const char *root, int mid) {
    FILE *f = fopen(st_test_mountinfo, "w");
    assert(f);
    fprintf(f, "%d 1 8:1 / %s/media/sdb1 rw - vfat /dev/sdb1 rw\n", mid, root);
    fprintf(f, "200 1 8:2 / %s/media/sda1 rw - vfat /dev/sda1 rw\n", root);
    fclose(f);
}
static void source_generation_exhaustion(const char *root) {
    /* A new connection must never receive zero or a previously issued token.
     * Existing connections remain readable when the process counter is full. */
    mountinfo(root, 901);
    g_st_generation = UINT64_MAX - 1;
    az_track_ref last = {.size = sizeof last}, second = {.size = sizeof second};
    assert(sdk_track_resolve("mod-a", 0, 10, &last) == AZ_OK && last.source_generation == UINT64_MAX);
    assert(sdk_track_resolve("mod-a", 1, 10, &second) == AZ_OK);
    FILE *f = fopen(st_test_mountinfo, "w");
    assert(f);
    fprintf(f, "901 1 8:1 / %s/media/sdb1 rw - vfat /dev/sdb1 rw\n", root);
    fprintf(f, "902 1 8:2 / %s/media/sda1 rw - vfat /dev/sda1 rw\n", root);
    assert(!fclose(f));
    az_track_ref unchanged;
    memset(&unchanged, 0x5a, sizeof unchanged);
    unchanged.size = sizeof unchanged;
    az_track_ref out = unchanged;
    CHECK("source generation exhaustion refuses a reconnect without publishing zero", sdk_track_resolve("mod-a", 1, 10, &out) == AZ_LIMIT);
    CHECK("source generation exhaustion preserves the caller buffer", !memcmp(&out, &unchanged, sizeof out));
    out.size = sizeof out;
    CHECK("counter exhaustion never reuses a prior token on retry", sdk_track_resolve("mod-a", 1, 10, &out) == AZ_LIMIT);
    CHECK("full generation counter leaves an unchanged current source usable", sdk_track_resolve("mod-a", 0, 10, &out) == AZ_OK && out.source_generation == UINT64_MAX);
    mountinfo(root, 903);
    out = unchanged;
    CHECK("both USB source slots fail closed on later reconnect", sdk_track_resolve("mod-a", 0, 10, &out) == AZ_LIMIT && !memcmp(&out, &unchanged, sizeof out));
    CHECK("source identity counter never wraps", g_st_generation == UINT64_MAX);
}
static void short_records(void) {
    uint32_t *tiny = malloc(sizeof *tiny);
    assert(tiny);
    *tiny = sizeof *tiny;
    az_track_ref safe_ref = {sizeof safe_ref, 1, 1, 1, 1, 1, 0};
    az_wave_query safe_query = {sizeof safe_query, 1, AZ_WAVE_PWV3_MONO, 0, 1, 0};
    uint64_t safe_ticket = UINT64_MAX;
    az_command_request safe_request = {.size = sizeof safe_request, .version = 1, .operation = AZ_CMD_LOOP_EXIT, .request_id = 1, .expected_load_generation = 1, .deadline_ns = 1};
    az_command_result safe_result = {.size = sizeof safe_result};
    assert(sdk_data_capabilities((az_data_capabilities *) tiny) == AZ_INVALID);
    assert(*tiny == sizeof *tiny);
    *tiny = sizeof *tiny;
    assert(sdk_track_resolve("mod-a", 0, 1, (az_track_ref *) tiny) == AZ_INVALID);
    assert(*tiny == sizeof *tiny);
    *tiny = sizeof *tiny;
    assert(sdk_track_request("mod-a", (const az_track_ref *) tiny, 1, &safe_ticket) == AZ_INVALID);
    assert(sdk_waveform_request("mod-a", (const az_track_ref *) tiny, &safe_query, &safe_ticket) == AZ_INVALID);
    assert(sdk_waveform_request("mod-a", &safe_ref, (const az_wave_query *) tiny, &safe_ticket) == AZ_INVALID);
    /* GCC 7 fortify warns about an unreachable full event write when it inlines
     * this tiny allocation. Exercise the SDK's runtime function-pointer boundary. */
    int (*volatile read_event)(const char *, uint64_t, az_data_change *) = sdk_data_event;
    assert(read_event("mod-a", 0, (az_data_change *) tiny) == AZ_INVALID);
    assert(*tiny == sizeof *tiny);
    *tiny = sizeof *tiny;
    assert(st_interval(NULL, 0, 1, 1, (az_time_interval *) tiny) == AZ_INVALID);
    assert(*tiny == sizeof *tiny);
    *tiny = sizeof *tiny;
    assert(sdk_command_submit("mod-a", (const az_command_request *) tiny, &safe_result) == AZ_INVALID);
    assert(sdk_command_submit("mod-a", &safe_request, (az_command_result *) tiny) == AZ_INVALID);
    assert(*tiny == sizeof *tiny);
    *tiny = sizeof *tiny;
    assert(*tiny == sizeof *tiny && safe_ticket == UINT64_MAX);
    free(tiny);
}
static void provider_bindings(const az_track_ref *ref) {
    char source[PATH_MAX], dbname[PATH_MAX], first[PATH_MAX], second[PATH_MAX], alias[PATH_MAX];
    snprintf(source, sizeof source, "%s/sdb1", g_media);
    snprintf(dbname, sizeof dbname, "%s/PIONEER/rekordbox/exportLibrary.db", source);
    sqlite3 *db = NULL;
    assert(!sqlite3_open(dbname, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/linked.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
    uint8_t grid[68] = {
        [0] = 'P', [1] = 'M', [2] = 'A', [3] = 'I', [7] = 28, [11] = 68, [28] = 'P', [29] = 'Q', [30] = 'T', [31] = 'Z', [35] = 24, [39] = 40, [51] = 2, [53] = 1, [54] = 0x2e, [55] = 0xe0, [61] = 2, [62] = 0x2e, [63] = 0xe0, [66] = 1, [67] = 0xf4};
    uint8_t detail[38] = {
        [0] = 'P', [1] = 'M', [2] = 'A', [3] = 'I', [7] = 12, [11] = 38, [12] = 'P', [13] = 'W', [14] = 'V', [15] = '3', [19] = 24, [23] = 26, [27] = 1, [31] = 2, [33] = 0x96, [36] = 0x11, [37] = 0x22};
    for (int wave = 0; wave < 2; wave++) {
        snprintf(first, sizeof first, "%s/first.%s", source, wave ? "EXT" : "DAT");
        snprintf(second, sizeof second, "%s/second.%s", source, wave ? "EXT" : "DAT");
        snprintf(alias, sizeof alias, "%s/linked.%s", source, wave ? "EXT" : "DAT");
        FILE *f = fopen(first, "wb");
        const uint8_t *bytes = wave ? detail : grid;
        size_t count = wave ? sizeof detail : sizeof grid;
        assert(f && fwrite(bytes, 1, count, f) == count && !fclose(f));
        if (wave) detail[36] = 0x77;
        else {
            grid[66] = 3;
            grid[67] = 0xe8; /* The replacement grid's second beat is at one second. */
        }
        f = fopen(second, "wb");
        assert(f && fwrite(bytes, 1, count, f) == count && !fclose(f));
        assert(!symlink(first, alias));
        az_wave_query query = {sizeof query, 1, AZ_WAVE_PWV3_MONO, 0, 1, 0};
        uint64_t ticket;
        assert((wave ? sdk_waveform_request("mod-a", ref, &query, &ticket)
                     : sdk_track_request("mod-a", ref, AZ_DATA_GRID, &ticket)) == AZ_OK);
        st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
        const az_track_snapshot *track = NULL;
        const az_wave_snapshot *window = NULL;
        assert((wave ? sdk_waveform_acquire("mod-a", ticket, &window)
                     : sdk_track_acquire("mod-a", ticket, &track)) == AZ_OK);
        assert(wave ? window->columns[0] == 0x11 : track->beats[1].time_s == .5);
        assert(!unlink(alias) && !symlink(second, alias));
        st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
        const az_track_snapshot *new_track = NULL;
        const az_wave_snapshot *new_window = NULL;
        CHECK("retargeting an analysis symlink invalidates its snapshot",
              (wave ? sdk_waveform_acquire("mod-a", ticket, &new_window)
                    : sdk_track_acquire("mod-a", ticket, &new_track)) == AZ_STALE);
        CHECK("retargeting keeps the acquired old allocation readable",
              wave ? window->columns[0] == 0x11 : track->beats[1].time_s == .5);
        assert(sdk_track_release("mod-a", ticket) == AZ_OK);
        assert((wave ? sdk_waveform_request("mod-a", ref, &query, &ticket)
                     : sdk_track_request("mod-a", ref, AZ_DATA_GRID, &ticket)) == AZ_OK);
        st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
        assert((wave ? sdk_waveform_acquire("mod-a", ticket, &new_window)
                     : sdk_track_acquire("mod-a", ticket, &new_track)) == AZ_OK);
        CHECK("a new request reads the replacement analysis target",
              wave ? new_window->columns[0] == 0x77 : new_track->beats[1].time_s == 1);
        assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    }
    assert(!sqlite3_open(dbname, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/analysis.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
}
static void blocked_read(int signal_number) {
    (void) signal_number;
    static const char message[] = "FAIL  analysis reader blocked on a FIFO\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof message - 1);
    (void) written;
    _Exit(1);
}
static void provider_special_files(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/sdb1/analysis.fifo", g_media);
    assert(!mkfifo(path, 0600));
    struct st_data *data = malloc(sizeof *data);
    assert(data);
    st_data_init(data);
    data->wave_query = (az_wave_query) {sizeof(az_wave_query), 1, AZ_WAVE_PWV3_MONO, 0, 1, 0};
    void (*previous)(int) = signal(SIGALRM, blocked_read);
    assert(previous != SIG_ERR);
    size_t size = 0;
    alarm(2);
    CHECK("full analysis reader rejects a FIFO without waiting for a writer", !st_read_file(path, &size));
    alarm(0);
    alarm(2);
    CHECK("detail reader rejects a FIFO without waiting for a writer", st_wave_read(path, data) == AZ_FAILED);
    alarm(0);
    assert(signal(SIGALRM, previous) != SIG_ERR);
    assert(!unlink(path));
    free(data);
}
static void provider_paths(const az_track_ref *ref) {
    char dbname[PATH_MAX], backup[PATH_MAX], wal[PATH_MAX], analysis[PATH_MAX];
    snprintf(dbname, sizeof dbname, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    snprintf(backup, sizeof backup, "%s/sdb1/PIONEER/rekordbox/actual%%23#?.db", g_media);
    struct st_data *data = malloc(sizeof *data);
    assert(data);
    /* An in-source database symlink is allowed, but its real sidecars still count. */
    assert(!rename(dbname, backup) && !symlink("actual%23#?.db", dbname));
    st_data_init(data);
    CHECK("in-source database symlink resolves", st_disk_load(ref, AZ_DATA_METADATA, data, NULL) == AZ_OK);
    FILE *f;
    const char *suffixes[] = {"-wal", "-journal"};
    for (unsigned i = 0; i < 2; i++) {
        snprintf(wal, sizeof wal, "%s%s", backup, suffixes[i]);
        f = fopen(wal, "wb");
        assert(f && fputs("committed sidecar", f) >= 0 && !fclose(f));
        st_data_init(data);
        CHECK("database symlink cannot hide a nonempty sidecar", st_disk_load(ref, AZ_DATA_METADATA, data, NULL) == AZ_STALE);
        assert(!unlink(wal));
        st_data_init(data);
        CHECK("quiet symlink target is readable again", st_disk_load(ref, AZ_DATA_METADATA, data, NULL) == AZ_OK);
    }
    assert(!unlink(dbname) && !rename(backup, dbname));

    /* Read errors on an existing detail file are failures, not absent support. */
    snprintf(analysis, sizeof analysis, "%s/sdb1/analysis.EXT", g_media);
    data->wave_query = (az_wave_query) {sizeof(az_wave_query), 1, AZ_WAVE_PWV3_MONO, 0, 1, 0};
    st_test_open_path = analysis;
    st_test_open_error = EIO;
    CHECK("waveform I/O errors are reported as failures", st_wave_read(analysis, data) == AZ_FAILED);
    st_test_open_error = EACCES;
    CHECK("waveform access errors are reported as failures", st_wave_read(analysis, data) == AZ_FAILED);
    st_test_open_error = ENOENT;
    CHECK("missing waveform remains unsupported", st_wave_read(analysis, data) == AZ_UNSUPPORTED);
    st_test_open_path = NULL;

    sqlite3 *db = NULL;
    assert(!sqlite3_open(dbname, &db));
    /* A truncated row must never select a different, valid analysis file. */
    char long_path[1100];
    memset(long_path, '/', 1023);
    static const char basename[] = "truncated.DAT";
    memcpy(long_path + 1023 - (sizeof basename - 1), basename, sizeof basename - 1);
    snprintf(long_path + 1023, sizeof long_path - 1023, "/replacement.DAT");
    char *update = sqlite3_mprintf("UPDATE content SET analysisDataFilePath=%Q WHERE content_id=10", long_path);
    assert(update);
    sql(db, update);
    sqlite3_free(update);
    assert(!sqlite3_close(db));
    snprintf(analysis, sizeof analysis, "%s/sdb1/truncated.DAT", g_media);
    static const uint8_t empty[28] = {'P', 'M', 'A', 'I', 0, 0, 0, 28, 0, 0, 0, 28};
    f = fopen(analysis, "wb");
    assert(f && fwrite(empty, 1, sizeof empty, f) == sizeof empty && !fclose(f));
    st_data_init(data);
    CHECK("oversized analysis paths cannot select their truncated prefix", st_disk_load(ref, AZ_DATA_GRID, data, NULL) == AZ_LIMIT);
    st_data_init(data);
    data->wave_query = (az_wave_query) {sizeof(az_wave_query), 1, AZ_WAVE_PWV3_MONO, 0, 1, 0};
    CHECK("detail requests reject oversized analysis paths", st_disk_load(ref, ST_DETAIL, data, NULL) == AZ_LIMIT);
    st_data_init(data);
    CHECK("metadata-only requests do not depend on analysis path limits", st_disk_load(ref, AZ_DATA_METADATA, data, NULL) == AZ_OK);
    assert(!unlink(analysis));
    assert(!sqlite3_open(dbname, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/new/nested/analysis.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
    char source[PATH_MAX], missing[1024];
    snprintf(source, sizeof source, "%s/sdb1", g_media);
    snprintf(analysis, sizeof analysis, "%s/new/nested/analysis.DAT", source);
    CHECK("missing analysis descendants retain their exact source path",
          st_source_path(source, "/new/nested/analysis.DAT", missing) && !strcmp(missing, analysis));
    uint64_t ticket;
    assert(sdk_track_request("mod-a", ref, AZ_DATA_GRID, &ticket) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    const az_track_snapshot *view;
    assert(sdk_track_acquire("mod-a", ticket, &view) == AZ_OK && view->grid_info.availability == AZ_AVAIL_UNSUPPORTED);
    snprintf(analysis, sizeof analysis, "%s/sdb1/new", g_media);
    assert(!mkdir(analysis, 0700));
    snprintf(analysis, sizeof analysis, "%s/sdb1/new/nested", g_media);
    assert(!mkdir(analysis, 0700));
    snprintf(analysis, sizeof analysis, "%s/sdb1/new/nested/analysis.DAT", g_media);
    static const uint8_t grid[68] = {
        [0] = 'P', [1] = 'M', [2] = 'A', [3] = 'I', [7] = 28, [11] = 68, [28] = 'P', [29] = 'Q', [30] = 'T', [31] = 'Z', [35] = 24, [39] = 40, [51] = 2, [53] = 1, [54] = 0x2e, [55] = 0xe0, /* Beat one at zero, 120 BPM. */
        [61] = 2,
        [62] = 0x2e,
        [63] = 0xe0,
        [66] = 1,
        [67] = 0xf4 /* Beat two at 500 ms. */
    };
    f = fopen(analysis, "wb");
    assert(f && fwrite(grid, 1, sizeof grid, f) == sizeof grid && !fclose(f));
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    CHECK("creating missing analysis directories invalidates cached absence", sdk_track_acquire("mod-a", ticket, &view) == AZ_STALE);
    assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    assert(sdk_track_request("mod-a", ref, AZ_DATA_GRID, &ticket) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    assert(sdk_track_acquire("mod-a", ticket, &view) == AZ_OK);
    CHECK("fresh request sees the newly created beat grid", view->grid_info.availability == AZ_AVAIL_READY && view->beat_count == 2 && view->beats[1].time_s == .5);
    assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    assert(!sqlite3_open(dbname, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/analysis.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
    free(data);
}
/* Exercise the actual public request -> source binding -> disk-load path for both
 * preview encodings; direct st_wave_read tests cannot catch a wrong sibling suffix. */
static void provider_previews(const az_track_ref *ref) {
    char source[PATH_MAX], database[PATH_MAX], path[PATH_MAX];
    snprintf(source, sizeof source, "%s/sdb1", g_media);
    snprintf(database, sizeof database, "%s/PIONEER/rekordbox/exportLibrary.db", source);
    sqlite3 *db = NULL;
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/preview.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
    for (unsigned encoding = AZ_WAVE_PWV4_PREVIEW; encoding <= AZ_WAVE_PWV6_PREVIEW; encoding++) {
        unsigned header = encoding == AZ_WAVE_PWV4_PREVIEW ? 24 : 20;
        unsigned stride = encoding == AZ_WAVE_PWV4_PREVIEW ? 6 : 3;
        uint8_t file[12 + 24 + 4 * 6] = {0};
        size_t bytes = 12 + header + 4 * stride;
        memcpy(file, "PMAI", 4);
        file[7] = 12;
        file[11] = (uint8_t) bytes;
        memcpy(file + 12, encoding == AZ_WAVE_PWV4_PREVIEW ? "PWV4" : "PWV6", 4);
        file[19] = (uint8_t) header;
        file[23] = (uint8_t) (bytes - 12);
        file[27] = (uint8_t) stride;
        file[31] = 4;
        for (unsigned i = 0; i < 4 * stride; i++) file[12 + header + i] = (uint8_t) (encoding * 32 + i);
        snprintf(path, sizeof path, "%s/preview.%s", source, encoding == AZ_WAVE_PWV4_PREVIEW ? "EXT" : "2EX");
        FILE *f = fopen(path, "wb");
        assert(f && fwrite(file, 1, bytes, f) == bytes && !fclose(f));
    }
    for (unsigned encoding = AZ_WAVE_PWV4_PREVIEW; encoding <= AZ_WAVE_PWV6_PREVIEW; encoding++) {
        unsigned stride = encoding == AZ_WAVE_PWV4_PREVIEW ? 6 : 3;
        az_wave_query query = {sizeof query, AZ_DATA_VERSION, encoding, 1, AZ_WAVE_WINDOW_MAX, 0};
        uint64_t ticket;
        assert(sdk_waveform_request("mod-a", ref, &query, &ticket) == AZ_OK);
        st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
        const az_wave_snapshot *view = NULL;
        int rc = sdk_waveform_acquire("mod-a", ticket, &view);
        CHECK("PWV4/PWV6 previews load from the correct EXT/2EX sibling", rc == AZ_OK && view);
        if (rc == AZ_OK && view) {
            CHECK("preview public snapshot preserves source columns and stride", view->encoding == encoding &&
                                                                                     view->column_count == 3 && view->total_columns == 4 && view->bytes_per_column == stride &&
                                                                                     view->data_bytes == 3 * stride && view->columns[0] == encoding * 32 + stride);
            CHECK("preview public snapshot uses DB duration, never 150 Hz", view->start_s == 75.25 &&
                                                                                view->seconds_per_column == 75.25 && view->info.provenance == AZ_PROV_FILE);
        }
        assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    }
    const uint32_t outside[] = {4, UINT32_MAX};
    for (unsigned i = 0; i < sizeof outside / sizeof *outside; i++) {
        az_wave_query query = {sizeof query, AZ_DATA_VERSION, AZ_WAVE_PWV6_PREVIEW, outside[i], 1, 0};
        uint64_t ticket;
        assert(sdk_waveform_request("mod-a", ref, &query, &ticket) == AZ_OK);
        st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
        const az_wave_snapshot *view = NULL;
        CHECK("preview out-of-range first column fails without a snapshot", sdk_waveform_acquire("mod-a", ticket, &view) == AZ_INVALID && !view);
        assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    }
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET length=0 WHERE content_id=10");
    assert(!sqlite3_close(db));
    az_wave_query query = {sizeof query, AZ_DATA_VERSION, AZ_WAVE_PWV6_PREVIEW, 0, 4, 0};
    uint64_t ticket;
    assert(sdk_waveform_request("mod-a", ref, &query, &ticket) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    const az_wave_snapshot *view = NULL;
    CHECK("preview with missing DB duration remains unsupported", sdk_waveform_acquire("mod-a", ticket, &view) == AZ_UNSUPPORTED && !view);
    assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET length=301 WHERE content_id=10");
    assert(!sqlite3_close(db));
    snprintf(path, sizeof path, "%s/preview.2EX", source);
    assert(!unlink(path));
    query.column_count = 1;
    assert(sdk_waveform_request("mod-a", ref, &query, &ticket) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    view = NULL;
    CHECK("missing 2EX does not substitute EXT for a PWV6 preview", sdk_waveform_acquire("mod-a", ticket, &view) == AZ_UNSUPPORTED && !view);
    assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    snprintf(path, sizeof path, "%s/preview.EXT", source);
    assert(!unlink(path));
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/analysis.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
}
/* The older byte previews live in DAT. An EXT decoy must never win merely
 * because all range requests formerly used EXT/2EX. */
static void provider_mono_previews(const az_track_ref *ref) {
    char database[PATH_MAX], path[PATH_MAX], decoy[PATH_MAX];
    snprintf(database, sizeof database, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    sqlite3 *db = NULL;
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/mono.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
    uint8_t bytes[12 + 20 + 5] = {0};
    memcpy(bytes, "PMAI", 4);
    bytes[7] = 12;
    bytes[11] = sizeof bytes;
    bytes[19] = 20;
    bytes[23] = 25;
    bytes[27] = 5;
    bytes[29] = 1;
    snprintf(path, sizeof path, "%s/sdb1/mono.DAT", g_media);
    snprintf(decoy, sizeof decoy, "%s/sdb1/mono.EXT", g_media);
    for (unsigned encoding = AZ_WAVE_PWV2_TINY_PREVIEW; encoding <= AZ_WAVE_PWAV_PREVIEW; ++encoding) {
        memcpy(bytes + 12, encoding == AZ_WAVE_PWV2_TINY_PREVIEW ? "PWV2" : "PWAV", 4);
        for (unsigned i = 0; i < 5; ++i) bytes[32 + i] = (uint8_t) (encoding * 16 + i);
        FILE *f = fopen(path, "wb");
        assert(f && fwrite(bytes, 1, sizeof bytes, f) == sizeof bytes && !fclose(f));
        bytes[33] = 0xff;
        f = fopen(decoy, "wb");
        assert(f && fwrite(bytes, 1, sizeof bytes, f) == sizeof bytes && !fclose(f));
        for (unsigned deck_owner = 0; deck_owner < 4; ++deck_owner) {
            uint64_t ticket;
            az_wave_query query = {sizeof query, 1, encoding, 1, AZ_WAVE_WINDOW_MAX, 0};
            /* Exercise every owner through the same production queue/provider,
             * including immutable cache sharing, independently of deck state. */
            assert(st_request_with_wave(&g_st, deck_owner, ref, ST_DETAIL, &query, &ticket) == AZ_OK);
            st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
            const az_wave_snapshot *view = NULL;
            int rc = st_wave_acquire(&g_st, deck_owner, ticket, &view);
            CHECK("PWV2/PWAV public range selects DAT rather than EXT", rc == AZ_OK && view);
            if (rc == AZ_OK && view) {
                CHECK("mono preview preserves exact byte columns and file provenance", view->encoding == encoding && view->column_count == 4 && view->total_columns == 5 && view->bytes_per_column == 1 && view->columns[0] == encoding * 16 + 1 && view->info.provenance == AZ_PROV_FILE);
                CHECK("mono preview uses metadata duration rather than detail clock", view->start_s == 60.2 && view->seconds_per_column == 60.2);
            }
            assert(st_release(&g_st, deck_owner, ticket, 0) == AZ_OK);
        }
    }
    assert(!unlink(path) && !unlink(decoy));
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/analysis.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
}
/* Numeric8 compiles against the old public header and reaches a real missing
 * request/provider behavior before the append-only name is introduced. Native
 * layer IDs/magnitudes are symbolic raw data, not player colors or pixels. */
static void layered_put32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t) (n >> 24);
    p[1] = (uint8_t) (n >> 16);
    p[2] = (uint8_t) (n >> 8);
    p[3] = (uint8_t) n;
}
static unsigned resolution_opens, resolution_queries, resolution_keys, resolution_closes;
static unsigned resolution_prepare_failure;
static void (*resolution_after_close)(void);
static char resolution_mutation_database[PATH_MAX];
static int (*resolution_open)(const char *, void **, int, const char *);
static int (*resolution_prepare)(void *, const char *, int, void **, const char **);
static int (*resolution_key)(void *, const void *, int);
static int (*resolution_close)(void *);
static int counted_resolution_open(const char *path, void **db, int flags, const char *vfs) {
    resolution_opens++;
    return resolution_open(path, db, flags, vfs);
}
static int counted_resolution_prepare(void *db, const char *query, int bytes, void **stmt, const char **tail) {
    resolution_queries++;
    if (resolution_prepare_failure) {
        resolution_prepare_failure--;
        *stmt = NULL;
        return SQLITE_ERROR;
    }
    return resolution_prepare(db, query, bytes, stmt, tail);
}
static int counted_resolution_key(void *db, const void *key, int bytes) {
    resolution_keys++;
    return resolution_key(db, key, bytes);
}
static int counted_resolution_close(void *db) {
    resolution_closes++;
    int rc = resolution_close(db);
    if (resolution_after_close) {
        void (*fn)(void) = resolution_after_close;
        resolution_after_close = NULL;
        fn();
    }
    return rc;
}
static void resolution_update(const char *database, const char *query) {
    sqlite3 *db = NULL;
    assert(!sqlite3_open(database, &db));
    sql(db, query);
    assert(!sqlite3_close(db));
}
static void resolution_mutate(void) {
    resolution_update(resolution_mutation_database, "UPDATE content SET title='Changed during resolution' WHERE content_id=10");
}
static int resolution_metadata(const az_track_ref *ref, struct st_data *data) {
    st_data_init(data);
    return st_disk_load(ref, AZ_DATA_METADATA, data, NULL);
}
/* Distinct actual loads bypass the waveform snapshot cache. Only source-bound
 * metadata/analysis resolution may be reused; every window still reads its file. */
static void provider_resolution_reuse(az_track_ref *ref, const az_track_ref *other, const char *root) {
    char database[PATH_MAX], path[PATH_MAX];
    snprintf(database, sizeof database, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    snprintf(path, sizeof path, "%s/sdb1/resolution.2EX", g_media);
    sqlite3 *db = NULL;
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/resolution.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
    uint8_t file[36 + 6 * 3] = {0};
    memcpy(file, "PMAI", 4); layered_put32(file + 4, 12); layered_put32(file + 8, sizeof file);
    memcpy(file + 12, "PWV7", 4); layered_put32(file + 16, 24); layered_put32(file + 20, sizeof file - 12);
    layered_put32(file + 24, 3); layered_put32(file + 28, 6); layered_put32(file + 32, 0x00960000);
    for (unsigned i = 0; i < 6; ++i) {
        file[36 + i * 3] = (uint8_t) (200 + i);
        file[37 + i * 3] = (uint8_t) (100 + i);
        file[38 + i * 3] = (uint8_t) (1 + i);
    }
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(file, 1, sizeof file, f) == sizeof file && !fclose(f));
    resolution_open = g_sdk_sq.open_v2; resolution_prepare = g_sdk_sq.prepare;
    resolution_key = g_sdk_sq.key; resolution_close = g_sdk_sq.close;
    g_sdk_sq.open_v2 = counted_resolution_open; g_sdk_sq.prepare = counted_resolution_prepare;
    g_sdk_sq.key = counted_resolution_key; g_sdk_sq.close = counted_resolution_close;
    resolution_opens = resolution_queries = resolution_keys = resolution_closes = 0;
    struct st_data *data = malloc(sizeof *data);
    assert(data);
    for (unsigned i = 0; i < 4; ++i) {
        st_data_init(data);
        uint32_t encoding = i < 2 ? AZ_WAVE_PWV7_BANDS : AZ_WAVE_PWV7_LAYERED_RAW;
        data->wave_query = (az_wave_query) {sizeof(az_wave_query), AZ_DATA_VERSION, encoding, i, 2, 0};
        int rc = st_disk_load(ref, ST_DETAIL, data, NULL);
        CHECK("resolution reuse still reads each actual raw/derived window", rc == AZ_OK &&
              data->wave_view.first_column == i && data->wave_view.column_count == 2 &&
              data->wave_view.encoding == encoding && data->detail[encoding == AZ_WAVE_PWV7_BANDS ? 0 : 2] == 200 + i);
        if (!i) {
            CHECK("first real disk resolution opens, keys, queries and closes its fixture database", resolution_opens == 1 && resolution_keys == 1 && resolution_queries == 1 && resolution_closes == 1);
            puts("disk resolution baseline: actual SQL lookup and file window passed");
        }
    }
    printf("disk resolution: opens=%u queries=%u keys=%u closes=%u\n",
           resolution_opens, resolution_queries, resolution_keys, resolution_closes);
    CHECK("same exact stable source/ref reuses metadata and analysis locator across windows/encodings",
          resolution_opens == 1 && resolution_queries == 1 && resolution_keys == 1 && resolution_closes == 1);

    unsigned before = resolution_opens;
    resolution_update(database, "UPDATE content SET title='Changed resolution',length=123 WHERE content_id=10");
    CHECK("database edits replace cached metadata", resolution_metadata(ref, data) == AZ_OK &&
          !strcmp(data->view.metadata.title, "Changed resolution") && data->view.metadata.duration_s == 123 && resolution_opens == before + 1);
    CHECK("fresh edited metadata is reusable", resolution_metadata(ref, data) == AZ_OK && resolution_opens == before + 1);
    before = resolution_opens;
    CHECK("equal track IDs on a different source require their own resolution", resolution_metadata(other, data) == AZ_OK &&
          !strcmp(data->view.metadata.title, "Track Ten") && resolution_opens == before + 1);
    CHECK("the other source cannot relabel cached metadata", resolution_metadata(ref, data) == AZ_OK &&
          !strcmp(data->view.metadata.title, "Changed resolution") && resolution_opens == before + 1);

    char sidecar[PATH_MAX];
    snprintf(sidecar, sizeof sidecar, "%s-wal", database);
    f = fopen(sidecar, "wb");
    assert(f && fputs("pending fixture transaction", f) >= 0 && !fclose(f));
    before = resolution_opens;
    CHECK("stored freshness observes a nonquiet database", !st_disk_fresh(ref, data, NULL));
    assert(!unlink(sidecar));
    CHECK("freshness-observed nonquiet state retires its matching locator", resolution_metadata(ref, data) == AZ_OK && resolution_opens == before + 1);
    f = fopen(sidecar, "wb");
    assert(f && fputs("pending fixture transaction", f) >= 0 && !fclose(f));
    before = resolution_opens;
    CHECK("a warm locator still refuses a nonempty canonical DB sidecar", resolution_metadata(ref, data) == AZ_STALE && resolution_opens == before);
    assert(!unlink(sidecar));
    CHECK("observed nonquiet DB retirement requires a new lookup", resolution_metadata(ref, data) == AZ_OK && resolution_opens == before + 1);

    /* Audio paths are derived from copied raw row values on every hit, so an
     * unchanged DB cannot retain the old target of an in-source symlink. */
    char audio[PATH_MAX], target[2][PATH_MAX];
    snprintf(audio, sizeof audio, "%s/sdb1/resolution-audio", g_media);
    for (unsigned i = 0; i < 2; ++i) {
        snprintf(target[i], sizeof target[i], "%s/sdb1/resolution-audio-%u", g_media, i);
        f = fopen(target[i], "wb");
        assert(f && fputc((int) i, f) == (int) i && !fclose(f));
    }
    assert(!symlink(target[0], audio));
    resolution_update(database, "UPDATE content SET path='/resolution-audio' WHERE content_id=10");
    assert(resolution_metadata(ref, data) == AZ_OK && !strcmp(data->view.metadata.path, target[0]));
    before = resolution_opens;
    assert(!unlink(audio) && !symlink(target[1], audio));
    CHECK("reused row re-resolves the current contained audio target", resolution_metadata(ref, data) == AZ_OK &&
          !strcmp(data->view.metadata.path, target[1]) && resolution_opens == before);
    assert(!unlink(audio) && !unlink(target[0]) && !unlink(target[1]));

    /* Same physical DB/inode and fingerprint, different canonical filename. */
    struct st_data *held = malloc(sizeof *held);
    assert(held && resolution_metadata(ref, held) == AZ_OK);
    char backup[PATH_MAX];
    snprintf(backup, sizeof backup, "%s/sdb1/PIONEER/rekordbox/resolution-target.db", g_media);
    uint64_t signature = st_disk_sig(database);
    assert(!rename(database, backup) && !symlink("resolution-target.db", database));
    assert(st_disk_sig(database) == signature);
    CHECK("canonical DB retarget invalidates a held stored snapshot", !st_disk_fresh(ref, held, NULL));
    before = resolution_opens;
    CHECK("same inode through a different canonical DB path is re-resolved", resolution_metadata(ref, data) == AZ_OK &&
          !strcmp(data->files[0], backup) && resolution_opens == before + 1);
    assert(!unlink(database) && !rename(backup, database));
    before = resolution_opens;
    CHECK("restoring the DB pathname also validates a new locator", resolution_metadata(ref, data) == AZ_OK && resolution_opens == before + 1);
    free(held);

    resolution_update(database, "INSERT INTO content SELECT * FROM content WHERE content_id=10");
    before = resolution_opens;
    CHECK("ambiguous lookup cannot return previously cached metadata", resolution_metadata(ref, data) == AZ_FAILED && resolution_opens == before + 1);
    CHECK("ambiguous failure is not retained as a positive resolution", resolution_metadata(ref, data) == AZ_FAILED && resolution_opens == before + 2);
    resolution_update(database, "DELETE FROM content WHERE rowid NOT IN (SELECT MIN(rowid) FROM content GROUP BY content_id)");
    assert(resolution_metadata(ref, data) == AZ_OK);
    az_track_ref different = *ref;
    different.track_id = 11;
    resolution_prepare_failure = 1;
    before = resolution_opens;
    CHECK("failed query refuses a different exact track reference", resolution_metadata(&different, data) == AZ_FAILED && resolution_opens == before + 1);
    CHECK("failed query is retried through actual SQL", resolution_metadata(&different, data) == AZ_OK &&
          !strcmp(data->view.metadata.title, "Track Eleven") && resolution_opens == before + 2);
    different.track_id = 999999;
    before = resolution_opens;
    CHECK("missing lookup does not reuse a preceding track", resolution_metadata(&different, data) == AZ_NOT_FOUND && resolution_opens == before + 1);
    CHECK("missing lookup never becomes a positive cache hit", resolution_metadata(&different, data) == AZ_NOT_FOUND && resolution_opens == before + 2);

    resolution_update(database, "UPDATE content SET title='Before lookup mutation' WHERE content_id=10");
    snprintf(resolution_mutation_database, sizeof resolution_mutation_database, "%s", database);
    resolution_after_close = resolution_mutate;
    before = resolution_opens;
    CHECK("DB edit during resolution refuses publication", resolution_metadata(ref, data) == AZ_STALE && resolution_opens == before + 1);
    CHECK("retry after the edit queries and publishes current metadata", resolution_metadata(ref, data) == AZ_OK &&
          !strcmp(data->view.metadata.title, "Changed during resolution") && resolution_opens == before + 2);

    size_t db_bytes = 0;
    uint8_t *copy = st_read_file(database, &db_bytes);
    assert(copy);
    f = fopen(backup, "wb");
    assert(f && fwrite(copy, 1, db_bytes, f) == db_bytes && !fclose(f));
    free(copy);
    az_track_ref old = *ref;
    assert(!rename(backup, database));
    before = resolution_opens;
    CHECK("replacing the DB file retires the old public source reference", resolution_metadata(&old, data) == AZ_STALE && resolution_opens == before);
    assert(sdk_track_resolve("mod-a", 0, 10, ref) == AZ_OK && ref->source_generation != old.source_generation);
    CHECK("replacement source reads a new resolution despite equal content bytes", resolution_metadata(ref, data) == AZ_OK && resolution_opens == before + 1);
    old = *ref;
    mountinfo(root, 501);
    before = resolution_opens;
    CHECK("remounting an unchanged DB retires its old locator", resolution_metadata(&old, data) == AZ_STALE && resolution_opens == before);
    assert(sdk_track_resolve("mod-a", 0, 10, ref) == AZ_OK && ref->source_generation != old.source_generation);
    CHECK("a remounted source must query before reusing metadata", resolution_metadata(ref, data) == AZ_OK && resolution_opens == before + 1);
    CHECK("another source keeps its independently valid locator", resolution_metadata(other, data) == AZ_OK && resolution_opens == before + 1);
    CHECK("every actual resolution SQL handle was keyed and closed", resolution_opens == resolution_keys && resolution_opens == resolution_queries && resolution_opens == resolution_closes);
    g_sdk_sq.open_v2 = resolution_open; g_sdk_sq.prepare = resolution_prepare;
    g_sdk_sq.key = resolution_key; g_sdk_sq.close = resolution_close;
    free(data);
    assert(!unlink(path));
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET title='Track Ten',length=301,path='/Contents/a/ten.mp3',analysisDataFilePath='/analysis.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
}
static void provider_layered_detail(const az_track_ref *ref) {
    enum { LAYERED = 8 };
    _Static_assert((unsigned) AZ_WAVE_PWV7_LAYERED_RAW == (unsigned) LAYERED,
                   "append-only derived layered waveform ID");
    char database[PATH_MAX], path[PATH_MAX], ext[PATH_MAX], dat[PATH_MAX];
    snprintf(database, sizeof database, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    snprintf(path, sizeof path, "%s/sdb1/layers.2EX", g_media);
    snprintf(ext, sizeof ext, "%s/sdb1/layers.EXT", g_media);
    snprintf(dat, sizeof dat, "%s/sdb1/layers.DAT", g_media);
    sqlite3 *db = NULL;
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/layers.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
    uint8_t bytes[12 + 24 + 5 * 3] = {0};
    memcpy(bytes, "PMAI", 4);
    bytes[7] = 12;
    bytes[11] = sizeof bytes;
    memcpy(bytes + 12, "PWV7", 4);
    bytes[19] = 24;
    bytes[23] = sizeof bytes - 12;
    bytes[27] = 3;
    bytes[31] = 5;
    bytes[33] = 0x96;
    const uint8_t raw[] = {255,128,1, 0,0,255, 128,128,128, 0,0,0, 255,255,0};
    memcpy(bytes + 36, raw, sizeof raw);
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(bytes, 1, sizeof bytes, f) == sizeof bytes && !fclose(f));
    /* Both wrong siblings contain valid but different PWV7 bytes. */
    memset(bytes + 36, 7, sizeof raw);
    const char *decoys[] = {ext, dat};
    for (unsigned i = 0; i < 2; ++i) {
        f = fopen(decoys[i], "wb");
        assert(f && fwrite(bytes, 1, sizeof bytes, f) == sizeof bytes && !fclose(f));
    }
    assert(!ut_fail);
    puts("LAYERED_PROVIDER_BASELINE_OK existing_file_windows=1 old_encodings=1");
    fflush(stdout);
    const uint8_t expected[] = {1,16,255,0,0,0, 8,16,128,0,0,0,
                                8,16,0,0,0,0, 6,16,255,0,0,0};
    az_wave_query query = {sizeof query, AZ_DATA_VERSION, LAYERED, 1, AZ_WAVE_WINDOW_MAX, 0};
    uint64_t tickets[4] = {0};
    const az_wave_snapshot *views[4] = {NULL};
    for (unsigned owner = 0; owner < 4; ++owner) {
        int rc = owner ? st_request_with_wave(&g_st, owner, ref, ST_DETAIL, &query, &tickets[owner])
                       : sdk_waveform_request("mod-a", ref, &query, &tickets[owner]);
        if (rc != AZ_OK) {
            fprintf(stderr, "FAIL layered-provider: missing derived layered waveform request (owner=%u rc=%d)\n", owner, rc);
            abort();
        }
        st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
        rc = st_wave_acquire(&g_st, owner, tickets[owner], &views[owner]);
        CHECK("layered request selects actual 2EX PWV7 rather than valid DAT/EXT decoys", rc == AZ_OK && views[owner]);
        if (rc != AZ_OK || !views[owner]) continue;
        const az_wave_snapshot *v = views[owner];
        CHECK("layered snapshot has deterministic SDK reserved bytes and absolute raw magnitudes", v->encoding == LAYERED && v->first_column == 1 && v->column_count == 4 && v->total_columns == 5 && v->bytes_per_column == 6 && v->data_bytes == sizeof expected && !memcmp(v->columns, expected, sizeof expected));
        CHECK("layered detail uses150Hz rather than metadata duration", v->start_s == 1.0 / 150.0 && v->seconds_per_column == 1.0 / 150.0);
        CHECK("layered conversion is derived with retained revision/clock", v->info.provenance == AZ_PROV_DERIVED && v->revision && v->observed_ns && v->info.availability == AZ_AVAIL_READY);
    }
    /* The raw query must remain distinct from its derived layered query. */
    uint64_t raw_ticket;
    query.encoding = AZ_WAVE_PWV7_BANDS;
    assert(sdk_waveform_request("mod-a", ref, &query, &raw_ticket) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    const az_wave_snapshot *raw_view = NULL;
    CHECK("existing raw PWV7 remains byte-for-byte file data", sdk_waveform_acquire("mod-a", raw_ticket, &raw_view) == AZ_OK && raw_view && raw_view->encoding == AZ_WAVE_PWV7_BANDS && raw_view->data_bytes == 12 && raw_view->bytes_per_column == 3 && raw_view->info.provenance == AZ_PROV_FILE && !memcmp(raw_view->columns, raw + 3, 12));
    f = fopen(ext, "ab");
    assert(f && fputc(0, f) == 0 && !fclose(f));
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    for (unsigned owner = 0; owner < 4; ++owner) {
        const az_wave_snapshot *v = NULL;
        CHECK("changing EXT decoy cannot invalidate a2EX layered snapshot", st_wave_acquire(&g_st, owner, tickets[owner], &v) == AZ_OK && v == views[owner]);
    }
    f = fopen(path, "ab");
    assert(f && fputc(0, f) == 0 && !fclose(f));
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    for (unsigned owner = 0; owner < 4; ++owner) {
        const az_wave_snapshot *v = NULL;
        CHECK("changing actual2EX invalidates every caller without overwriting held layered bytes", st_wave_acquire(&g_st, owner, tickets[owner], &v) == AZ_STALE && !v && views[owner] && !memcmp(views[owner]->columns, expected, sizeof expected));
        assert(st_release(&g_st, owner, tickets[owner], 0) == AZ_OK);
    }
    assert(sdk_track_release("mod-a", raw_ticket) == AZ_OK);
    /* Check the bounded streaming transform on a file much larger than the
     * requested window, with deliberately nonzero private destination bytes. */
    const uint32_t total = 70000;
    size_t large_bytes = 36u + total * 3u;
    uint8_t *large = malloc(large_bytes);
    assert(large);
    memcpy(large, bytes, 36);
    layered_put32(large + 8, (uint32_t) large_bytes);
    layered_put32(large + 20, (uint32_t) large_bytes - 12);
    layered_put32(large + 28, total);
    for (uint32_t i = 0; i < total; ++i) memcpy(large + 36 + i * 3, raw, 3);
    f = fopen(path, "wb");
    assert(f && fwrite(large, 1, large_bytes, f) == large_bytes && !fclose(f));
    free(large);
    struct st_data *scratch = malloc(sizeof *scratch);
    assert(scratch);
    st_data_init(scratch);
    memset(scratch->detail, 0xa5, sizeof scratch->detail);
    scratch->wave_query = (az_wave_query) {sizeof query, AZ_DATA_VERSION, LAYERED, 150, AZ_WAVE_WINDOW_MAX, 0};
    st_test_read_bytes = st_test_largest_read = 0;
    st_test_count_reads = 1;
    int rc = st_wave_read(path, scratch);
    st_test_count_reads = 0;
    CHECK("layered provider retains at most4096 converted columns", rc == AZ_OK && scratch->wave_view.column_count == AZ_WAVE_WINDOW_MAX && scratch->wave_view.total_columns == total && scratch->wave_view.data_bytes == sizeof scratch->detail && scratch->wave_view.start_s == 1.0);
    CHECK("layered provider reads only bounded packed input rather than entire analysis", st_test_read_bytes <= AZ_WAVE_WINDOW_MAX * 3u + 60u && st_test_largest_read <= AZ_WAVE_WINDOW_MAX * 3u);
    const uint8_t first_record[] = {0x63,0x38,255,128,1,0};
    CHECK("layered SDK reserved byte is zero even when destination was nonzero", !memcmp(scratch->detail, first_record, sizeof first_record) && !memcmp(scratch->detail + sizeof scratch->detail - 6, first_record, sizeof first_record));
    scratch->wave_query.first_column = total;
    az_wave_snapshot before = scratch->wave_view;
    CHECK("out-of-range layered request refuses unchanged view", st_wave_read(path, scratch) == AZ_INVALID && !memcmp(&scratch->wave_view, &before, sizeof before));
    scratch->wave_query.first_column = total - 1;
    CHECK("last-column layered request clips and preserves detail clock", st_wave_read(path, scratch) == AZ_OK && scratch->wave_view.column_count == 1 && scratch->wave_view.data_bytes == 6 && scratch->wave_view.seconds_per_column == 1.0 / 150.0);
    free(scratch);
    assert(!unlink(path) && !unlink(ext) && !unlink(dat));
    assert(!sqlite3_open(database, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/analysis.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
}
int main(void) {
    ut_suite = "sdk disk provider";
    char root_template[] = "/tmp/az-sdk-provider.XXXXXX", root[PATH_MAX];
    assert(mkdtemp(root_template) && realpath(root_template, root));
    snprintf(g_media, sizeof g_media, "%s/media", root);
    snprintf(g_sysblock, sizeof g_sysblock, "%s/sys", root);
    snprintf(g_mounts, sizeof g_mounts, "%s/mounts", root);
    snprintf(st_test_mountinfo, sizeof st_test_mountinfo, "%s/mountinfo", root);
    mkdir(g_media, 0700);
    mkdir(g_sysblock, 0700);
    make_stick("sdb1", "A");
    plug("sdb1", "1-1", "sdb1");
    make_stick("sda1", "B");
    plug("sda1", "4-1", "sda1");
    mountinfo(root, 100);
    g_st_enabled = 1;
    g_st_started = 1;
    assert(sdk_sq_resolve());
    g_sdk_sq.key = st_test_database_key;
    short_records();
    for (int i = 0; i < 2; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s/PIONEER/rekordbox/exportLibrary.db", g_media, i ? "sda1" : "sdb1");
        sqlite3 *db = NULL;
        assert(!sqlite3_open(path, &db));
        sql(db, "ALTER TABLE content ADD COLUMN analysisDataFilePath TEXT");
        sql(db, "UPDATE content SET analysisDataFilePath='/analysis.DAT'");
        sqlite3_close(db);
        snprintf(path, sizeof path, "%s/%s/analysis.DAT", g_media, i ? "sda1" : "sdb1");
        uint8_t file[28] = {'P', 'M', 'A', 'I', 0, 0, 0, 28, 0, 0, 0, 28};
        FILE *f = fopen(path, "wb");
        assert(f && fwrite(file, 1, sizeof file, f) == sizeof file);
        fclose(f);
    }
    az_track_ref a = {.size = sizeof a}, b = {.size = sizeof b};
    g_cm_on_ui = 1;
    assert(sdk_track_resolve("mod-a", 0, 10, &a) == AZ_INVALID);
    g_cm_on_ui = 0;
    assert(sdk_track_resolve("mod-b", 0, 10, &a) == AZ_INVALID);
    assert(sdk_track_resolve("mod-a", 0, 10, &a) == AZ_OK && sdk_track_resolve("mod-a", 1, 10, &b) == AZ_OK);
    assert(a.source_id != b.source_id && !st_ref_equal(&a, &b));
    uint64_t ticket;
    assert(sdk_track_request("mod-a", &a, 31, &ticket) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    const az_track_snapshot *view;
    assert(sdk_track_acquire("mod-a", ticket, &view) == AZ_OK);
    assert(!strcmp(view->metadata.title, "Track Ten") && view->metadata.original_bpm == 128 && view->metadata.duration_s == 301);
    assert(view->metadata_info.provenance == AZ_PROV_FILE && view->grid_info.availability == AZ_AVAIL_UNSUPPORTED);
    assert(sdk_track_acquire("mod-b", ticket, &view) == AZ_INVALID);
    /* A committed WAL must not be silently ignored by an immutable DB reader. */
    char wal[PATH_MAX];
    snprintf(wal, sizeof wal, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db-wal", g_media);
    FILE *wf = fopen(wal, "wb");
    assert(wf);
    fputs("pending", wf);
    fclose(wf);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    assert(sdk_track_acquire("mod-a", ticket, &view) == AZ_STALE);
    assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    assert(sdk_track_request("mod-a", &a, 31, &ticket) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    assert(sdk_track_acquire("mod-a", ticket, &view) == AZ_STALE);
    assert(sdk_track_release("mod-a", ticket) == AZ_OK && !unlink(wal));
    assert(sdk_track_request("mod-a", &a, 31, &ticket) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    assert(sdk_track_acquire("mod-a", ticket, &view) == AZ_OK);
    /* Missing EXT creation must invalidate a previously cached absence. */
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/sdb1/analysis.EXT", g_media);
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs("bad", f);
    fclose(f);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    assert(sdk_track_acquire("mod-a", ticket, &view) == AZ_STALE);
    assert(sdk_track_release("mod-a", ticket) == AZ_OK);
    /* Real provider window: streaming headers, clipped tail, shared ticket lifetime and edits. */
    uint8_t wavefile[12 + 24 + 200] = {'P', 'M', 'A', 'I', 0, 0, 0, 12};
#define WPUT(p, n)          \
    do {                    \
        uint32_t wv_ = (n); \
        (p)[0] = wv_ >> 24; \
        (p)[1] = wv_ >> 16; \
        (p)[2] = wv_ >> 8;  \
        (p)[3] = wv_;       \
    } while (0)
    WPUT(wavefile + 8, sizeof wavefile);
    memcpy(wavefile + 12, "PWV3", 4);
    WPUT(wavefile + 16, 24);
    WPUT(wavefile + 20, 224);
    WPUT(wavefile + 24, 1);
    WPUT(wavefile + 28, 200);
    WPUT(wavefile + 32, 0x00960000u);
    for (unsigned i = 0; i < 200; i++) wavefile[36 + i] = (uint8_t) i;
    f = fopen(path, "wb");
    assert(f && fwrite(wavefile, 1, sizeof wavefile, f) == sizeof wavefile);
    fclose(f);
    az_wave_query query = {sizeof query, 1, AZ_WAVE_PWV3_MONO, 150, 100, 0};
    uint64_t wt, ws;
    assert(sdk_waveform_request("mod-a", &a, &query, &wt) == AZ_OK);
    assert(sdk_waveform_request("mod-a", &a, &query, &ws) == AZ_OK);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    const az_wave_snapshot *wv, *shared_wave;
    assert(sdk_waveform_acquire("mod-a", wt, &wv) == AZ_OK);
    assert(wv->column_count == 50 && wv->total_columns == 200 && wv->columns[0] == 150 && wv->columns[49] == 199);
    assert(wv->start_s == 1.0 && wv->seconds_per_column == 1.0 / 150.0 && wv->info.provenance == AZ_PROV_FILE);
    assert(sdk_waveform_acquire("mod-a", ws, &shared_wave) == AZ_OK && shared_wave == wv);
    assert(sdk_waveform_acquire("mod-b", wt, &shared_wave) == AZ_INVALID);
    assert(sdk_track_acquire("mod-a", wt, &view) == AZ_INVALID);
    f = fopen(path, "ab");
    assert(f);
    fputc(0, f);
    fclose(f);
    st_work(&g_st, st_disk_load, st_disk_fresh, NULL);
    assert(sdk_waveform_acquire("mod-a", wt, &shared_wave) == AZ_STALE);
    assert(wv->columns[0] == 150); /* Pinned old data survives invalidation. */
    assert(sdk_track_release("mod-a", wt) == AZ_OK && sdk_track_release("mod-a", ws) == AZ_OK);
#undef WPUT
    provider_previews(&a);
    provider_mono_previews(&a);
    provider_layered_detail(&a);
    provider_resolution_reuse(&a, &b, root);
    provider_bindings(&a);
    provider_special_files();
    provider_paths(&a);
    mountinfo(root, 101);
    az_track_ref newer = {.size = sizeof newer};
    assert(sdk_track_resolve("mod-a", 0, 10, &newer) == AZ_OK);
    assert(newer.source_generation != a.source_generation);
    struct st_data *data = malloc(sizeof *data);
    assert(data);
    st_data_init(data);
    assert(st_disk_load(&a, 1, data, NULL) == AZ_STALE);
    /* A malicious analysis path must not escape the selected source. */
    char source[PATH_MAX], out[1024];
    snprintf(source, sizeof source, "%s/sdb1", g_media);
    assert(!st_source_path(source, "/../sda1/analysis.DAT", out));
    assert(!st_source_path(source, "/missing/../../sda1/analysis.DAT", out));
    snprintf(path, sizeof path, "%s/link", source);
    assert(!symlink("/etc/passwd", path));
    assert(!st_source_path(source, "/link", out));
    /* The database itself must stay inside its port's source, not only ANLZ paths. */
    char dbname[PATH_MAX], backup[PATH_MAX], foreign[PATH_MAX];
    snprintf(dbname, sizeof dbname, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    snprintf(backup, sizeof backup, "%s/sdb1/PIONEER/rekordbox/saved.db", g_media);
    snprintf(foreign, sizeof foreign, "%s/sda1/PIONEER/rekordbox/exportLibrary.db", g_media);
    assert(!rename(dbname, backup) && !symlink(foreign, dbname));
    struct st_source rejected;
    assert(st_source_read(0, &rejected) == AZ_STALE);
    assert(!unlink(dbname) && !rename(backup, dbname));
    az_data_capabilities caps = {.size = sizeof caps};
    assert(sdk_data_capabilities(&caps) == AZ_OK && !caps.commands);
    az_command_request request = {.size = sizeof request, .version = 1, .operation = AZ_CMD_LOOP_EXIT, .request_id = 7, .expected_load_generation = 3, .deadline_ns = 1};
    az_command_result result = {.size = sizeof result};
    assert(sdk_command_submit("mod-a", &request, &result) == AZ_UNSUPPORTED && result.outcome == AZ_COMMAND_REJECTED && result.request_id == 7);
    for (unsigned field = 0; field < 9; field++) {
        az_command_request invalid = request;
        switch (field) {
        case 0: invalid.size--; break;
        case 1: invalid.version++; break;
        case 2: invalid.deck = 4; break;
        case 3: invalid.operation = 0; break;
        case 4: invalid.operation = AZ_CMD_CUE_COLOR + 1; break;
        case 5: invalid.request_id = 0; break;
        case 6: invalid.expected_load_generation = 0; break;
        case 7: invalid.deadline_ns = 0; break;
        case 8: invalid.quantization = AZ_QUANTIZE_ON + 1; break;
        }
        memset(&result, 0x5a, sizeof result);
        result.size = sizeof result;
        az_command_result before = result;
        assert(sdk_command_submit("mod-a", &invalid, &result) == AZ_INVALID);
        assert(!memcmp(&result, &before, sizeof result));
    }
    g_sdk_sq.ok = -1;
    assert(sdk_data_capabilities(&caps) == AZ_OK && !caps.stored_components);
    assert(sdk_track_resolve("mod-a", 0, 10, &newer) == AZ_UNSUPPORTED);
    assert(sdk_track_request("mod-a", &a, 1, &ticket) == AZ_UNSUPPORTED);
    g_sdk_sq.ok = 1;
    source_generation_exhaustion(root);
    g_st_enabled = 0;
    assert(sdk_data_capabilities(&caps) == AZ_OK && !caps.stored_components);
    assert(sdk_track_resolve("mod-a", 0, 10, &newer) == AZ_UNSUPPORTED);
    free(data);
    st_destroy(&g_st);
    char cmd[PATH_MAX + 20];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    assert(!system(cmd));
    if (ut_summary()) return 1;
    puts("sdk disk provider: exact sources, metadata, invalidation, reconnect, path containment and command refusal passed");
    return 0;
}
