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
#include "../../src/common/azstatus_native.h"
#include "../../src/common/azanalysis_native.h"
static az_track_ref fixture_loaded_ref;
static unsigned fixture_source_kind = AZ_STATUS_SOURCE_USB1;
static int fixture_source_ready = 1, fixture_source_closed;
static char fixture_source_path[1024], st_test_state[PATH_MAX];
static int fixture_source_open(unsigned deck, uint64_t epoch, uint64_t load, struct azn_analysis_source *out) {
    if (!fixture_source_ready) return AZ_NOT_FOUND;
    assert(!deck && epoch == 9 && load == 44);
    *out = (struct azn_analysis_source){.size=sizeof *out,.version=1,.token=7,.epoch=epoch,.load=load,.source_kind=AZ_STATUS_SOURCE_STREAMING};
    out->track_id[0]=1; out->track_id[4]=6; out->track_id[8]=23;
    memcpy(out->audio_path, fixture_source_path, sizeof out->audio_path); return AZ_OK;
}
static int fixture_source_current(uint64_t token) { assert(token == 7); return fixture_source_ready ? AZ_OK : AZ_STALE; }
static void fixture_source_close(uint64_t token) { assert(token == 7); fixture_source_closed++; }
static const struct azn_analysis_source_api *fixture_source(void) {
    static const struct azn_analysis_source_api api = {.size=sizeof api,.version=1,.open=fixture_source_open,.current=fixture_source_current,.close=fixture_source_close}; return &api;
}
static int fixture_current = 1, fixture_published, fixture_released;
static int sa_loaded_ref(unsigned deck, uint64_t load, az_track_ref *out) {
    if (deck || load != 44) return AZ_STALE;
    *out = fixture_loaded_ref; return AZ_OK;
}
static int fixture_capture(unsigned deck, uint64_t load, struct aznm_packet *out) {
    if (deck || load != 44) return AZ_STALE;
    memset(out, 0, sizeof *out); out->owner = 1; out->grid.content = 1; out->phrases.content = 2;
    out->grid.sample_rate = 44100; return AZ_OK;
}
static int fixture_valid(unsigned deck, uint64_t load, const struct aznm_packet *map) {
    return !deck && load == 44 && map->owner == 1 && fixture_current ? AZ_OK : AZ_STALE;
}
static int fixture_read(const struct aznm_component *c, unsigned kind, void *rows, uint32_t cap, uint32_t *count) {
    (void)c; *count = kind == AZNM_GRID ? 2 : 0;
    if (rows && kind == AZNM_GRID) { assert(cap >= 2); az_beat beats[2] = {{0,120,1,0},{0.5,120,2,0}}; memcpy(rows, beats, sizeof beats); }
    return AZ_OK;
}
static int fixture_samples(const struct aznm_component *c, int64_t *ticks, uint32_t cap, uint32_t *count, uint32_t *rate) {
    (void)c; assert(cap >= 2); *count=2; *rate=44100; ticks[0]=0; ticks[1]=22050; return AZ_OK;
}
static void fixture_release(const struct aznm_packet *map) { assert(map->owner == 1); fixture_released++; }
static int fixture_publish(unsigned deck, uint64_t load, const struct aznm_packet *map, const void *bytes, uint32_t n) {
    if (fixture_valid(deck, load, map)) return AZ_STALE;
    assert(n == 56 && !memcmp(bytes, "PSSI", 4)); fixture_published++; return AZ_OK;
}
static const struct azn_maps_api *fixture_maps(void) {
    static const struct azn_maps_api api = {.size=sizeof api,.version=1,.read=fixture_read,.release=fixture_release,
        .read_samples=fixture_samples,.capture=fixture_capture,.current=fixture_valid,.publish_phrases=fixture_publish}; return &api;
}
static int fixture_seed(unsigned deck, struct azstatus_native_receipt *out) {
    if (deck || out->version != 1 || out->size < sizeof *out) return 0;
    out->epoch=9; out->load_generation=44; out->source_kind=fixture_source_kind; return 1;
}
const struct azstatus_native_v1 *azstem_status_service_v1(void) {
    static const struct azstatus_native_v1 api = {.size=sizeof api,.version=1,.load_seed=fixture_seed,.maps=fixture_maps,.analysis_source=fixture_source}; return &api;
}
#ifndef CM_STATE
#define CM_STATE st_test_state
#endif
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
    /* Native-only lease pins its grid, rejects a replaced load, then publishes
     * both a missing persistent grid and the native phrase notification. */
    fixture_loaded_ref = ref;
    assert(sdk_analysis_open_loaded("mod-b", 0, 44, &job) == AZ_INVALID);
    assert(sdk_analysis_open_loaded("mod-a", 0, 43, &job) == AZ_STALE);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert(job.flags & AZ_ANALYSIS_LOADED);
    assert(job.needed == AZ_DATA_PHRASES && strstr(job.analysis_path, "/tmp/az-analysis-grid-"));
    char snapshot[1024]; memcpy(snapshot, job.analysis_path, sizeof snapshot);
    uint8_t loaded_bundle[72] = {0}; memcpy(loaded_bundle, "TA01", 4);
    sa_put32(loaded_bundle + 8, 56); sa_put32(loaded_bundle + 12, 44100); memcpy(loaded_bundle + 16, phrase, 56);
    fixture_current = 0;
    assert(sdk_analysis_commit("mod-a", job.job, loaded_bundle, sizeof loaded_bundle) == AZ_STALE);
    assert(!fixture_published && sdk_analysis_close("mod-a", job.job) == AZ_OK);
    assert(access(snapshot, F_OK) && fixture_released == 1);
    fixture_current = 1;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert(sdk_analysis_commit("mod-a", job.job, loaded_bundle, sizeof loaded_bundle) == AZ_OK);
    assert(fixture_published == 1 && fixture_released == 2);
    /* Cache hit refreshes native state immediately instead of running analysis. */
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_NOT_FOUND && !job.job);
    assert(fixture_published == 2 && fixture_released == 3);
    /* Same row count is insufficient: a shifted disk grid must not receive
     * phrases inferred against the pinned current native grid. */
    snprintf(path, sizeof path, "%s/sdb1/DJ.EXT", g_media); analysis_write(path, "", 0);
    snprintf(path, sizeof path, "%s/sdb1/DJ.DAT", g_media);
    failed=0; file=sa_read(path, &n, &failed); assert(file && !failed);
    sa_put32(file + 28 + 28, 100); analysis_write(path, file, n);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert(sdk_analysis_commit("mod-a", job.job, loaded_bundle, sizeof loaded_bundle) == AZ_STALE);
    assert(sdk_analysis_close("mod-a", job.job) == AZ_OK);
    sa_put32(file + 28 + 28, 0); size_t with_phrase_n=0;
    uint8_t *with_phrase=sa_merge(file, n, phrase, 56, &with_phrase_n); assert(with_phrase);
    analysis_write(path, with_phrase, with_phrase_n); free(file); free(with_phrase);
    uint8_t placeholder[60]={0}; memcpy(placeholder,"PMAI",4); sa_put32(placeholder+4,28); sa_put32(placeholder+8,60);
    memcpy(placeholder+28,phrase,32); sa_put32(placeholder+36,32); sa_put16(placeholder+44,0);
    snprintf(path, sizeof path, "%s/sdb1/DJ.EXT", g_media); analysis_write(path, placeholder, sizeof placeholder);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_NOT_FOUND && !job.job);
    assert(fixture_published == 3); /* populated DAT wins over empty EXT */
    fixture_published=2; /* independent streaming assertions below */
    snprintf(st_test_state, sizeof st_test_state, "%s/settings", root); assert(!mkdir(st_test_state, 0700));
    snprintf(fixture_source_path, sizeof fixture_source_path, "%s/12345.m4a", root);
    analysis_write(fixture_source_path, "native-completed-audio", 22);
    fixture_source_kind = AZ_STATUS_SOURCE_STREAMING; fixture_source_ready = 0;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_BUSY);
    fixture_source_ready = 1;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert((job.flags & AZ_ANALYSIS_STREAMING) && !job.track.track_id && !job.track.source_id);
    uint64_t streaming_job = job.job;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_BUSY);
    fixture_source_ready = 0;
    assert(sdk_analysis_commit("mod-a", streaming_job, loaded_bundle, sizeof loaded_bundle) == AZ_STALE);
    assert(sdk_analysis_close("mod-a", streaming_job) == AZ_OK);
    fixture_source_ready = 1;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert(sdk_analysis_commit("mod-a", job.job, loaded_bundle, sizeof loaded_bundle) == AZ_OK);
    assert(fixture_published == 3 && fixture_source_closed == 3);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_NOT_FOUND);
    assert(fixture_published == 4 && fixture_source_closed == 4);
    uint8_t malformed[40]={0}; memcpy(malformed,"PMAI",4); sa_put32(malformed+4,28); sa_put32(malformed+8,40);
    memcpy(malformed+28,"PSSI",4); sa_put32(malformed+32,12); sa_put32(malformed+36,12);
    snprintf(path, sizeof path, "%s/analysis/beatport-0/analysis.EXT", st_test_state);
    analysis_write(path, malformed, sizeof malformed);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_FAILED);
    puts("Shifted grids, empty EXT placeholders and short framed PSSI regression checks passed");
    puts("Beatport leases: wait for completion, no USB identity, withdrawal refusal and bounded cache reuse passed");
    puts("Loaded analysis: native grid snapshot, stale-load refusal, refresh and cache reuse passed");
    puts("Missing ANLZ location allocated and linked; conditional spool preserves later DJ edits");
    puts("Analysis leases, source mutation refusal, saved grid/phrases and existing-map refusal passed");
    return 0;
}
