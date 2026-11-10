/* SPDX-License-Identifier: GPL-3.0-only */
#define main sdklib_fixture_main
#include "sdklib_test.c"
#undef main
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
static __thread int g_sdk_on_tap_thread;
static char st_test_mountinfo[PATH_MAX];
#define ST_MOUNTINFO st_test_mountinfo
#define g_cm (&g_mod)
#define az_thread_create pthread_create
#include "sdk_track.c"
unsigned azsdk_sources_sweep(void) { return 0; }
static int st_test_database_key(void *db, const void *key, int bytes) { return sqlite3_key(db, key, bytes); }
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
    fclose(f);
}
#include "sdk_analysis_service.c"
static void analysis_write(const char *path, const void *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(bytes, 1, n, f) == n && !fclose(f));
}
int main(void) {
    char root_template[] = "/tmp/az-analysis.XXXXXX", root[PATH_MAX], path[PATH_MAX];
    assert(mkdtemp(root_template) && realpath(root_template, root));
    snprintf(g_media, sizeof g_media, "%s/media", root);
    snprintf(g_sysblock, sizeof g_sysblock, "%s/sys", root);
    snprintf(g_mounts, sizeof g_mounts, "%s/mounts", root);
    snprintf(st_test_mountinfo, sizeof st_test_mountinfo, "%s/mountinfo", root);
    mkdir(g_media, 0700);
    mkdir(g_sysblock, 0700);
    make_stick("sdb1", "A");
    plug("sdb1", "1-1", "sdb1");
    mountinfo(root, 100);
    assert(sdk_sq_resolve());
    g_sdk_sq.key = st_test_database_key;
    snprintf(path, sizeof path, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    sqlite3 *db = NULL;
    assert(!sqlite3_open(path, &db));
    sql(db, "ALTER TABLE content ADD COLUMN analysisDataFilePath TEXT");
    sql(db, "UPDATE content SET path='/audio.wav',analysisDataFilePath='/analysis.DAT' WHERE content_id=10");
    assert(!sqlite3_close(db));
    snprintf(path, sizeof path, "%s/sdb1/audio.wav", g_media);
    analysis_write(path, "audio-placeholder", 17);
    char empty[PATH_MAX];
    snprintf(empty, sizeof empty, "%s/sdb1/analysis.DAT", g_media);
    analysis_write(empty, "", 0);
    snprintf(empty, sizeof empty, "%s/sdb1/analysis.EXT", g_media);
    analysis_write(empty, "", 0);
    az_track_ref ref = {.size = sizeof ref};
    struct st_source source;
    assert(!st_source_read(0, &source));
    ref = source.ref;
    ref.track_id = 10;
    az_analysis_job_v1 job = {.size = sizeof job};
    assert(sdk_analysis_open("mod-b", &ref, &job) == AZ_INVALID);
    g_cm_on_ui = 1;
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_INVALID);
    g_cm_on_ui = 0;
    g_sdk_on_tap_thread = 1;
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_INVALID);
    g_sdk_on_tap_thread = 0;
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_OK);
    assert(job.needed == (AZ_DATA_GRID | AZ_DATA_PHRASES) && job.job);
    uint8_t bundle[112] = {0};
    memcpy(bundle, "TA01", 4);
    sa_put32(bundle + 4, 40);
    sa_put32(bundle + 8, 56);
    sa_put32(bundle + 12, 44100);
    uint8_t *grid = bundle + 16, *phrase = grid + 40;
    memcpy(grid, "PQTZ", 4);
    sa_put32(grid + 4, 24);
    sa_put32(grid + 8, 40);
    sa_put32(grid + 20, 2);
    grid[25] = 1;
    grid[26] = 0x2e;
    grid[27] = 0xe0;
    grid[33] = 2;
    grid[34] = 0x2e;
    grid[35] = 0xe0;
    sa_put32(grid + 36, 500);
    memcpy(phrase, "PSSI", 4);
    sa_put32(phrase + 4, 32);
    sa_put32(phrase + 8, 56);
    phrase[15] = 24;
    phrase[17] = 1;
    phrase[19] = 1;
    phrase[27] = 2;
    phrase[33] = 1;
    phrase[35] = 1;
    phrase[37] = 1;
    assert(sdk_analysis_commit("mod-b", job.job, bundle, sizeof bundle) == AZ_INVALID);
    /* A mutated audio file invalidates the lease without writing either container. */
    analysis_write(path, "changed-audio-placeholder", 25);
    assert(sdk_analysis_commit("mod-a", job.job, bundle, sizeof bundle) == AZ_STALE);
    assert(sdk_analysis_close("mod-a", job.job) == AZ_OK);
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_OK);
    assert(sdk_analysis_commit("mod-a", job.job, bundle, sizeof bundle) == AZ_OK);
    assert(sdk_analysis_close("mod-a", job.job) == AZ_STALE);
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_NOT_FOUND);
    size_t n = 0;
    int failed = 0;
    snprintf(path, sizeof path, "%s/sdb1/analysis.DAT", g_media);
    uint8_t *file = sa_read(path, &n, &failed);
    assert(file && !failed && sa_tag_count(file, n, "PQTZ") == 2);
    free(file);
    snprintf(path, sizeof path, "%s/sdb1/analysis.EXT", g_media);
    file = sa_read(path, &n, &failed);
    assert(file && !failed && sa_tag_count(file, n, "PSSI") == 1);
    free(file);
    /* Malformed size inputs refuse before any read beyond their short prefix. */
    assert(sdk_analysis_commit("mod-a", 1, bundle, 4) == AZ_INVALID);
    const char *actual = getenv("TRACK_ANALYSIS_RESULT");
    if (actual) {
        FILE *f = fopen(actual, "rb");
        assert(f);
        assert(!fseek(f, 0, SEEK_END));
        long size = ftell(f);
        assert(size > 16 && size < 300000);
        rewind(f);
        uint8_t *generated = malloc((size_t) size);
        assert(generated);
        assert(fread(generated, 1, (size_t) size, f) == (size_t) size && !fclose(f));
        snprintf(path, sizeof path, "%s/sdb1/analysis.DAT", g_media);
        assert(!unlink(path));
        snprintf(path, sizeof path, "%s/sdb1/analysis.EXT", g_media);
        assert(!unlink(path));
        assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_OK);
        assert(sdk_analysis_commit("mod-a", job.job, generated, (uint32_t) size) == AZ_OK);
        free(generated);
        puts("Actual whole-audio worker output persisted through the production core service");
    }
    /* A row without an analysis location gets complete files and a conditional
     * native-spool update. Apostrophes in audio paths remain literal SQL data. */
    snprintf(path, sizeof path, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    assert(!sqlite3_open(path, &db));
    sql(db, "UPDATE content SET path='/quote''track.wav',analysisDataFilePath=NULL WHERE content_id=10");
    assert(!sqlite3_close(db));
    snprintf(path, sizeof path, "%s/sdb1/quote'track.wav", g_media);
    analysis_write(path, "audio-placeholder", 17);
    job = (az_analysis_job_v1) {.size = sizeof job};
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_OK && (job.flags & AZ_ANALYSIS_NEEDS_LINK));
    assert(strstr(job.analysis_path, "/PIONEER/USBANLZ/AZMODS/"));
    assert(sdk_analysis_commit("mod-a", job.job, bundle, sizeof bundle) == AZ_OK);
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_NOT_FOUND && job.job == 0 && (job.flags & AZ_ANALYSIS_NEEDS_LINK));
    snprintf(path, sizeof path, "%s/sdb1/PIONEER/azspool/track-analysis-0000000a.sql", g_media);
    FILE *spool = fopen(path, "r");
    char statement[6500];
    assert(spool && fgets(statement, sizeof statement, spool) && !fclose(spool));
    assert(strstr(statement, "path=CAST(X'2f71756f746527747261636b2e776176' AS TEXT)") &&
           strstr(statement, "analysisDataFilePath IS NULL"));
    assert(strchr(statement, ';') == strrchr(statement, ';') && !strchr(statement, 0x01));
    snprintf(path, sizeof path, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    assert(!sqlite3_open(path, &db));
    sql(db, statement);
    assert(!sqlite3_close(db));
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_NOT_FOUND && !job.flags && !job.job);
    assert(!sqlite3_open(path, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/DJ.DAT' WHERE content_id=10");
    sql(db, statement);
    struct st_resolution manual = {0};
    assert(st_read_row(db, 10, &manual) == AZ_OK && !strcmp(manual.analysis, "/DJ.DAT"));
    assert(!sqlite3_close(db));
    puts("Missing ANLZ location allocated and linked; conditional spool preserves later DJ edits");
    puts("Analysis leases, source mutation refusal, saved grid/phrases and existing-map refusal passed");
    return 0;
}
