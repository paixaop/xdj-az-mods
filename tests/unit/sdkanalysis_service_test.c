/* SPDX-License-Identifier: GPL-3.0-only */
#define main sdklib_fixture_main
#include "sdklib_test.c"
#undef main
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/wait.h>
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
    *out = (struct azn_analysis_source) {.size = sizeof *out, .version = 1, .token = 7, .epoch = epoch, .load = load, .source_kind = AZ_STATUS_SOURCE_STREAMING};
    out->track_id[0] = 1;
    out->track_id[4] = 6;
    out->track_id[8] = 23;
    memcpy(out->audio_path, fixture_source_path, sizeof out->audio_path);
    return AZ_OK;
}
static int fixture_source_current(uint64_t token) {
    assert(token == 7);
    return fixture_source_ready ? AZ_OK : AZ_STALE;
}
static void fixture_source_close(uint64_t token) {
    assert(token == 7);
    fixture_source_closed++;
}
static const struct azn_analysis_source_api *fixture_source(void) {
    static const struct azn_analysis_source_api api = {.size = sizeof api, .version = 1, .open = fixture_source_open, .current = fixture_source_current, .close = fixture_source_close};
    return &api;
}
static int fixture_current = 1, fixture_published, fixture_released, fixture_native_phrases;
static int sa_loaded_ref(unsigned deck, uint64_t load, az_track_ref *out) {
    if (deck || load != 44) return AZ_STALE;
    *out = fixture_loaded_ref;
    return AZ_OK;
}
static int fixture_capture(unsigned deck, uint64_t load, struct aznm_packet *out) {
    if (deck || load != 44) return AZ_STALE;
    memset(out, 0, sizeof *out);
    out->owner = 1;
    out->grid.content = 1;
    out->phrases.content = 2;
    out->grid.sample_rate = 44100;
    return AZ_OK;
}
static int fixture_valid(unsigned deck, uint64_t load, const struct aznm_packet *map) {
    return !deck && load == 44 && map->owner == 1 && fixture_current ? AZ_OK : AZ_STALE;
}
static int fixture_read(const struct aznm_component *c, unsigned kind, void *rows, uint32_t cap, uint32_t *count) {
    (void) c;
    *count = kind == AZNM_GRID ? 2 : (uint32_t) fixture_native_phrases;
    if (rows && kind == AZNM_GRID) {
        assert(cap >= 2);
        az_beat beats[2] = {{0, 120, 1, 0}, {0.5, 120, 2, 0}};
        memcpy(rows, beats, sizeof beats);
    }
    return AZ_OK;
}
static int fixture_samples(const struct aznm_component *c, int64_t *ticks, uint32_t cap, uint32_t *count, uint32_t *rate) {
    (void) c;
    assert(cap >= 2);
    *count = 2;
    *rate = 44100;
    ticks[0] = 0;
    ticks[1] = 22050;
    return AZ_OK;
}
static void fixture_release(const struct aznm_packet *map) {
    assert(map->owner == 1);
    fixture_released++;
}
static int fixture_publish(unsigned deck, uint64_t load, const struct aznm_packet *map, const void *bytes, uint32_t n) {
    if (fixture_valid(deck, load, map)) return AZ_STALE;
    assert(n == 56 && !memcmp(bytes, "PSSI", 4));
    fixture_published++;
    return AZ_OK;
}
static const struct azn_maps_api *fixture_maps(void) {
    static const struct azn_maps_api api = {.size = sizeof api, .version = 1, .read = fixture_read, .release = fixture_release, .read_samples = fixture_samples, .capture = fixture_capture, .current = fixture_valid, .publish_phrases = fixture_publish};
    return &api;
}
static int fixture_seed(unsigned deck, struct azstatus_native_receipt *out) {
    if (deck || out->version != 1 || out->size < sizeof *out) return 0;
    out->epoch = 9;
    out->load_generation = 44;
    out->source_kind = fixture_source_kind;
    return 1;
}
const struct azstatus_native_v1 *azstem_status_service_v1(void) {
    static const struct azstatus_native_v1 api = {.size = sizeof api, .version = 1, .load_seed = fixture_seed, .maps = fixture_maps, .analysis_source = fixture_source};
    return &api;
}
#ifndef CM_STATE
#define CM_STATE st_test_state
#endif
#include "sdk_analysis_service.c"
static void analysis_write(const char *path, const void *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(bytes, 1, n, f) == n && !fclose(f));
}
static void typed_grid_result_validation(void) {
    az_beat beats[2] = {{.time_s = 0, .bpm = 120, .beat_in_bar = 1},
                        {.time_s = 0.5, .bpm = 120, .beat_in_bar = 2}};
    az_analysis_phrase_v1 phrase = {.start_beat = 1, .kind = 1};
    az_analysis_result_v1 result = {.size = sizeof result, .version = AZ_DATA_VERSION, .source_rate = 44100, .source_frames = 44100, .beats = beats, .beat_count = 2, .phrases = {.entries = &phrase, .count = 1, .mood = 1, .end_beat = 2}};
    uint32_t size = 0;
    uint8_t *bytes = sa_encode_result(&result, &size);
    assert(bytes && size == 124 && !memcmp(bytes, "TA03", 4));
    assert(sa_u32(bytes + 4) == 40 && sa_u32(bytes + 8) == 56 && !sa_u32(bytes + 12));
    assert(!memcmp(bytes + 28, "PQTZ", 4) && sa_u32(bytes + 48) == 2);
    assert(sa_u16(bytes + 52) == 1 && sa_u16(bytes + 54) == 12000);
    assert(compare_timestamps(sa_u32(bytes + 56), 0, 1000, result.source_rate, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(sa_u32(bytes + 64), 500, 1000, result.source_rate, 1) == AZ_TIME_EQUAL);
    free(bytes);

    /* Invalid source coordinates and millisecond serialization collisions must
     * be rejected before any partially encoded record can be published. */
    const double invalid_times[] = {NAN, INFINITY, -1, 1e10, 0, 0.0001};
    for (unsigned i = 0; i < sizeof invalid_times / sizeof *invalid_times; i++) {
        beats[1].time_s = invalid_times[i];
        assert(!sa_encode_result(&result, &size) && !size);
    }
    beats[1].time_s = 0.5;
    const double invalid_tempos[] = {NAN, INFINITY, 0, -1, 1000};
    for (unsigned i = 0; i < sizeof invalid_tempos / sizeof *invalid_tempos; i++) {
        beats[1].bpm = invalid_tempos[i];
        assert(!sa_encode_result(&result, &size) && !size);
    }
    beats[1].bpm = 120;
    beats[1].beat_in_bar = 0;
    assert(!sa_encode_result(&result, &size) && !size);
    beats[1].beat_in_bar = 5;
    assert(!sa_encode_result(&result, &size) && !size);
    beats[1].beat_in_bar = 2;
    beats[1].reserved = 1;
    assert(!sa_encode_result(&result, &size) && !size);
}
static unsigned active_jobs(int owner) {
    unsigned count = 0;
    pthread_mutex_lock(&sa_mutex);
    for (unsigned i = 0; i < 8; ++i) count += sa_jobs[i].id && sa_jobs[i].owner == owner;
    pthread_mutex_unlock(&sa_mutex);
    return count;
}
static void await_retirement(int owner) {
    for (unsigned i = 0; i < 1000 && active_jobs(owner); ++i) usleep(1000);
    assert(!active_jobs(owner));
}
static void owner_lifecycle(const az_track_ref *track) {
    az_analysis_owner before = analysis_owner_get(0);
    az_analysis_job_v1 jobs[8];
    memset(jobs, 0, sizeof jobs);
    analysis_owner_begin(1);
    for (unsigned i = 0; i < 8; ++i) {
        jobs[i].size = sizeof jobs[i];
        assert(sa_open((int) (i / 4), track, &jobs[i], 0, 1) == AZ_OK);
    }
    az_analysis_job_v1 overflow = {.size = sizeof overflow};
    assert(sa_open(0, track, &overflow, 0, 1) == AZ_LIMIT);
    int reader = open(jobs[0].audio_path, O_RDONLY);
    assert(reader >= 0);
    int ready[2], resume[2];
    assert(!pipe(ready) && !pipe(resume));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[0]); close(resume[1]);
        int own = open(jobs[0].audio_path, O_RDONLY);
        char mark = own >= 0, bytes[17];
        assert(write(ready[1], &mark, 1) == 1 && mark);
        assert(read(resume[0], &mark, 1) == 1);
        assert(read(own, bytes, sizeof bytes) == sizeof bytes);
        assert(!memcmp(bytes, "audio-placeholder", sizeof bytes));
        close(own); _exit(0);
    }
    close(ready[1]); close(resume[0]);
    char mark;
    assert(read(ready[0], &mark, 1) == 1 && mark);
    /* Failed init/live disable uses this same nonblocking retirement edge.
     * Holding the lease mutex proves retirement does not wait for public work. */
    pthread_mutex_lock(&sa_mutex);
    analysis_source_retire(analysis_owner_retire(0));
    assert(!analysis_owner_current(before));
    pthread_mutex_unlock(&sa_mutex);
    await_retirement(0);
    assert(active_jobs(1) == 4);
    assert(sa_open(0, track, &overflow, 0, 1) == AZ_STALE);
    assert(open(jobs[0].audio_path, O_RDONLY) < 0);
    char bytes[17];
    assert(read(reader, bytes, sizeof bytes) == sizeof bytes);
    assert(!memcmp(bytes, "audio-placeholder", sizeof bytes));
    close(reader);
    assert(write(resume[1], "x", 1) == 1);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(ready[0]); close(resume[1]);
    az_analysis_owner after = analysis_owner_begin(0);
    assert(after.generation != before.generation && !analysis_owner_current(before));
    assert(sdk_analysis_close("mod-a", jobs[0].job) == AZ_STALE);
    for (unsigned i = 0; i < 4; ++i) {
        assert(sa_open(0, track, &jobs[i], 0, 1) == AZ_OK);
        assert(jobs[i].job > jobs[7].job);
    }
    analysis_source_retire(analysis_owner_retire(0));
    analysis_source_retire(analysis_owner_retire(1));
    await_retirement(0); await_retirement(1);
    analysis_owner_begin(0);
}
int main(void) {
    analysis_owner_begin(0);
    typed_grid_result_validation();
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
    owner_lifecycle(&ref);
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
    /* Raw packet validation stays internal; public owner checks use the typed entry below. */
    assert(sa_commit(-1, job.job, bundle, sizeof bundle) == AZ_INVALID);
    /* A mutated audio file invalidates the lease without writing either container. */
    analysis_write(path, "changed-audio-placeholder", 25);
    assert(sa_commit(0, job.job, bundle, sizeof bundle) == AZ_STALE);
    assert(sdk_analysis_close("mod-a", job.job) == AZ_OK);
    assert(sdk_analysis_open("mod-a", &ref, &job) == AZ_OK);
    assert(sa_commit(0, job.job, bundle, sizeof bundle) == AZ_OK);
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
    assert(sa_commit(0, 1, bundle, 4) == AZ_INVALID);
    const char *actual = getenv("TRACK_ANALYSIS_RESULT");
    if (actual) {
        FILE *f = fopen(actual, "rb");
        assert(f);
        assert(!fseek(f, 0, SEEK_END));
        long size = ftell(f);
        assert(size > 16 && size < 600000);
        rewind(f);
        uint8_t *generated = malloc((size_t) size);
        assert(generated);
        assert(fread(generated, 1, (size_t) size, f) == (size_t) size && !fclose(f));
        snprintf(path, sizeof path, "%s/sdb1/analysis.DAT", g_media);
        assert(!unlink(path));
        snprintf(path, sizeof path, "%s/sdb1/analysis.EXT", g_media);
        assert(!unlink(path));
        int opened = !memcmp(generated, "TA02", 4) ? sdk_analysis_open_v2("mod-a", &ref, &job) : sdk_analysis_open("mod-a", &ref, &job);
        assert(opened == AZ_OK);
        assert(sa_commit(0, job.job, generated, (uint32_t) size) == AZ_OK);
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
    assert(sa_commit(0, job.job, bundle, sizeof bundle) == AZ_OK);
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
    az_analysis_grid_v1 typed_grid = {.size = sizeof typed_grid};
    az_beat typed_beats[2] = {{.reserved = 99}, {.reserved = 99}};
    assert(sdk_analysis_grid("mod-b", job.job, &typed_grid, typed_beats, 2) == AZ_INVALID);
    assert(sdk_analysis_grid("mod-a", job.job, &typed_grid, NULL, 0) == AZ_LIMIT);
    assert(typed_grid.beat_count == 2 && typed_grid.version == AZ_DATA_VERSION);
    assert(sdk_analysis_grid("mod-a", job.job, &typed_grid, typed_beats, 1) == AZ_LIMIT);
    assert(typed_beats[0].reserved == 99 && typed_beats[1].reserved == 99);
    fixture_current = 0;
    assert(sdk_analysis_grid("mod-a", job.job, &typed_grid, typed_beats, 2) == AZ_STALE);
    assert(typed_beats[0].reserved == 99);
    fixture_current = 1;
    assert(sdk_analysis_grid("mod-a", job.job, &typed_grid, typed_beats, 2) == AZ_OK);
    assert(typed_beats[0].beat_in_bar == 1 && typed_beats[1].beat_in_bar == 2);
    assert(compare_timestamps(typed_beats[1].time_s, 0.5, 1, 44100, 1) == AZ_TIME_EQUAL);
    char snapshot[1024];
    memcpy(snapshot, job.analysis_path, sizeof snapshot);
    uint8_t loaded_bundle[72] = {0};
    memcpy(loaded_bundle, "TA01", 4);
    sa_put32(loaded_bundle + 8, 56);
    sa_put32(loaded_bundle + 12, 44100);
    memcpy(loaded_bundle + 16, phrase, 56);
    fixture_current = 0;
    assert(sa_commit(0, job.job, loaded_bundle, sizeof loaded_bundle) == AZ_STALE);
    assert(!fixture_published && sdk_analysis_close("mod-a", job.job) == AZ_OK);
    assert(access(snapshot, F_OK) && fixture_released == 1);
    fixture_current = 1;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert(sa_commit(0, job.job, loaded_bundle, sizeof loaded_bundle) == AZ_OK);
    assert(fixture_published == 1 && fixture_released == 2);
    /* Cache hit refreshes native state immediately instead of running analysis. */
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_NOT_FOUND && !job.job);
    assert(fixture_published == 2 && fixture_released == 3);
    /* Same row count is insufficient: a shifted disk grid must not receive
     * phrases inferred against the pinned current native grid. */
    snprintf(path, sizeof path, "%s/sdb1/DJ.EXT", g_media);
    analysis_write(path, "", 0);
    snprintf(path, sizeof path, "%s/sdb1/DJ.DAT", g_media);
    failed = 0;
    file = sa_read(path, &n, &failed);
    assert(file && !failed);
    sa_put32(file + 28 + 28, 100);
    analysis_write(path, file, n);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert(sa_commit(0, job.job, loaded_bundle, sizeof loaded_bundle) == AZ_STALE);
    assert(sdk_analysis_close("mod-a", job.job) == AZ_OK);
    sa_put32(file + 28 + 28, 0);
    size_t with_phrase_n = 0;
    uint8_t *with_phrase = sa_merge(file, n, phrase, 56, &with_phrase_n);
    assert(with_phrase);
    analysis_write(path, with_phrase, with_phrase_n);
    free(file);
    free(with_phrase);
    uint8_t placeholder[60] = {0};
    memcpy(placeholder, "PMAI", 4);
    sa_put32(placeholder + 4, 28);
    sa_put32(placeholder + 8, 60);
    memcpy(placeholder + 28, phrase, 32);
    sa_put32(placeholder + 36, 32);
    sa_put16(placeholder + 44, 0);
    snprintf(path, sizeof path, "%s/sdb1/DJ.EXT", g_media);
    analysis_write(path, placeholder, sizeof placeholder);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_NOT_FOUND && !job.job);
    assert(fixture_published == 3); /* populated DAT wins over empty EXT */
    fixture_published = 2; /* independent streaming assertions below */
    snprintf(st_test_state, sizeof st_test_state, "%s/settings", root);
    assert(!mkdir(st_test_state, 0700));
    snprintf(fixture_source_path, sizeof fixture_source_path, "%s/12345.m4a", root);
    analysis_write(fixture_source_path, "native-completed-audio", 22);
    fixture_source_kind = AZ_STATUS_SOURCE_STREAMING;
    fixture_source_ready = 0;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_BUSY);
    fixture_source_ready = 1;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert((job.flags & AZ_ANALYSIS_STREAMING) && !job.track.track_id && !job.track.source_id);
    uint64_t streaming_job = job.job;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_BUSY);
    fixture_source_ready = 0;
    assert(sa_commit(0, streaming_job, loaded_bundle, sizeof loaded_bundle) == AZ_STALE);
    assert(sdk_analysis_close("mod-a", streaming_job) == AZ_OK);
    fixture_source_ready = 1;
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_OK);
    assert(sa_commit(0, job.job, loaded_bundle, sizeof loaded_bundle) == AZ_OK);
    assert(fixture_published == 3 && fixture_source_closed == 3);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_NOT_FOUND);
    assert(fixture_published == 4 && fixture_source_closed == 4);
    uint8_t malformed[40] = {0};
    memcpy(malformed, "PMAI", 4);
    sa_put32(malformed + 4, 28);
    sa_put32(malformed + 8, 40);
    memcpy(malformed + 28, "PSSI", 4);
    sa_put32(malformed + 32, 12);
    sa_put32(malformed + 36, 12);
    snprintf(path, sizeof path, "%s/analysis/beatport-0/analysis.EXT", st_test_state);
    analysis_write(path, malformed, sizeof malformed);
    assert(sdk_analysis_open_loaded("mod-a", 0, 44, &job) == AZ_FAILED);
    puts("Shifted grids, empty EXT placeholders and short framed PSSI regression checks passed");
    puts("Beatport leases: wait for completion, no USB identity, withdrawal refusal and bounded cache reuse passed");
    puts("Loaded analysis: native grid snapshot, stale-load refusal, refresh and cache reuse passed");
    puts("Missing ANLZ location allocated and linked; conditional spool preserves later DJ edits");
    puts("Analysis leases, source mutation refusal, saved grid/phrases and existing-map refusal passed");
    /* New clients independently request missing vocals even with native phrases. */
    fixture_source_kind = AZ_STATUS_SOURCE_USB1;
    fixture_native_phrases = 1;
    az_vocal_snapshot_v1 vocals = {.size = sizeof vocals};
    az_vocal_interval intervals[4];
    assert(sdk_vocal_intervals("mod-a", &ref, &vocals, intervals, 4) == AZ_NOT_FOUND);
    assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_OK);
    assert(job.needed == AZ_ANALYSIS_VOCALS && !job.analysis_path[0]);
    uint8_t vocal_bundle[70] = {0};
    memcpy(vocal_bundle, "TA02", 4);
    sa_put32(vocal_bundle + 12, 46);
    sa_put32(vocal_bundle + 16, 44100);
    sa_put32(vocal_bundle + 20, 44100);
    uint8_t *pvdi = vocal_bundle + 24;
    memcpy(pvdi, "PVDI", 4);
    sa_put32(pvdi + 4, 24);
    sa_put32(pvdi + 8, 46);
    sa_put32(pvdi + 12, 1024);
    sa_put16(pvdi + 16, 22050);
    sa_put16(pvdi + 18, 1);
    sa_put32(pvdi + 20, 22);
    memset(pvdi + 24 + 3, 4, 8);
    memset(pvdi + 24 + 20, 4, 2);
    char vocals_path[1024], saved_dat[1024], saved_ext[1024];
    snprintf(vocals_path, sizeof vocals_path, "%s/sdb1/DJ.2EX", g_media);
    snprintf(saved_dat, sizeof saved_dat, "%s/sdb1/DJ.DAT", g_media);
    snprintf(saved_ext, sizeof saved_ext, "%s/sdb1/DJ.EXT", g_media);
    uint64_t before_dat = st_disk_sig(saved_dat), before_ext = st_disk_sig(saved_ext);
    int published_before = fixture_published;
    assert(sa_commit(0, job.job, vocal_bundle, sizeof vocal_bundle) == AZ_OK);
    assert(st_disk_sig(saved_dat) == before_dat && st_disk_sig(saved_ext) == before_ext && fixture_published == published_before);
    assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_NOT_FOUND && !job.job && !job.needed);
    assert(sdk_vocal_intervals("mod-b", &ref, &vocals, intervals, 4) == AZ_INVALID);
    g_cm_on_ui = 1;
    assert(sdk_vocal_intervals("mod-a", &ref, &vocals, intervals, 4) == AZ_INVALID);
    g_cm_on_ui = 0;
    assert(sdk_vocal_intervals("mod-a", &ref, &vocals, NULL, 0) == AZ_LIMIT && vocals.interval_count == 2);
    assert(sdk_vocal_intervals_loaded("mod-a", 0, 43, &vocals, intervals, 4) == AZ_STALE);
    assert(sdk_vocal_intervals_loaded("mod-a", 0, 44, &vocals, intervals, 4) == AZ_OK);
    assert(vocals.method == AZ_VOCAL_SPECTRAL && vocals.interval_count == 2);
    assert(compare_timestamps(intervals[0].start_s, 3072.0 / 22050, 1, 22050, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(intervals[1].stop_s, 1.0, 1, 44100, 1) == AZ_TIME_EQUAL);
    /* TA03 identifies LSTM output and floors the resampled length before hop
     * rounding. Both lengths below have nine frames, not the old ten. */
    assert(sdk_analysis_formats() & AZ_ANALYSIS_FORMAT_TA03);
    for (int source = 0; source < 2; source++) {
        assert(!unlink(vocals_path));
        assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_OK);
        uint8_t neural[61] = {0};
        memcpy(neural, "TA03", 4);
        sa_put32(neural + 12, 33);
        sa_put32(neural + 16, source ? 48000 : 44100);
        sa_put32(neural + 20, source ? 20063 : 18433);
        sa_put32(neural + 24, AZ_VOCAL_LSTM);
        memcpy(neural + 28, pvdi, 24);
        sa_put32(neural + 28 + 8, 33);
        sa_put32(neural + 28 + 20, 9);
        memset(neural + 52, 4, 9);
        sa_put32(neural + 24, 99);
        assert(sa_commit(0, job.job, neural, sizeof neural) == AZ_INVALID);
        sa_put32(neural + 24, AZ_VOCAL_LSTM);
        assert(sa_commit(0, job.job, neural, sizeof neural - 1) == AZ_INVALID);
        az_vocal_frame_v1 frames[9];
        for (unsigned i = 0; i < 9; i++) frames[i].activity = 4;
        az_analysis_result_v1 result = {.size = sizeof result, .version = AZ_DATA_VERSION, .source_rate = source ? 48000 : 44100, .source_frames = source ? 20063 : 18433, .vocals = {.sample_rate = 22050, .hop_samples = 1024, .method = AZ_VOCAL_LSTM, .count = 9, .frames = frames}};
        uint32_t encoded_size;
        uint8_t *encoded = sa_encode_result(&result, &encoded_size);
        assert(encoded && encoded_size == sizeof neural && !memcmp(encoded, neural, sizeof neural));
        free(encoded);
        assert(sdk_analysis_commit_result("mod-b", job.job, &result) == AZ_INVALID);
        frames[0].activity = 5;
        assert(sdk_analysis_commit_result("mod-a", job.job, &result) == AZ_INVALID);
        frames[0].activity = 4;
        result.vocals.count = UINT32_MAX;
        assert(sdk_analysis_commit_result("mod-a", job.job, &result) == AZ_INVALID);
        result.vocals.count = 9;
        result.vocals.frames = NULL;
        assert(sdk_analysis_commit_result("mod-a", job.job, &result) == AZ_INVALID);
        result.vocals.frames = frames;
        fixture_current = 0;
        assert(sdk_analysis_commit_result("mod-a", job.job, &result) == AZ_STALE);
        fixture_current = 1;
        assert(sdk_analysis_commit_result("mod-a", job.job, &result) == AZ_OK);
        assert(sdk_vocal_intervals_loaded("mod-a", 0, 44, &vocals, intervals, 4) == AZ_OK);
        assert(vocals.method == AZ_VOCAL_LSTM && vocals.frame_count == 9 && vocals.interval_count == 1);
        assert(compare_timestamps(vocals.analyzed_s, (double) (source ? 20063 : 18433) / (source ? 48000 : 44100),
                                  1, source ? 48000 : 44100, 1) == AZ_TIME_EQUAL);
    }
    assert(!unlink(vocals_path));
    assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_OK);
    assert(sa_commit(0, job.job, vocal_bundle, sizeof vocal_bundle) == AZ_OK);
    /* Existing PVDI is preserved when phrases and the DAT are absent. */
    uint64_t before_vocals = st_disk_sig(vocals_path);
    assert(!unlink(saved_dat) && !unlink(saved_ext));
    fixture_native_phrases = 0;
    assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_OK && job.needed == AZ_DATA_PHRASES);
    uint8_t phrase_v2[80] = {0};
    memcpy(phrase_v2, "TA02", 4);
    sa_put32(phrase_v2 + 8, 56);
    sa_put32(phrase_v2 + 16, 44100);
    sa_put32(phrase_v2 + 20, 44100);
    memcpy(phrase_v2 + 24, phrase, 56);
    az_analysis_phrase_v1 typed_phrase = {.start_beat = 1, .kind = 1};
    az_analysis_result_v1 phrase_result = {.size = sizeof phrase_result, .version = AZ_DATA_VERSION, .source_rate = 44100, .source_frames = 44100, .phrases = {.mood = 1, .end_beat = 2, .count = 1, .entries = &typed_phrase}};
    uint32_t typed_size;
    uint8_t *typed_bytes = sa_encode_result(&phrase_result, &typed_size);
    assert(typed_bytes && typed_size == 84 && !memcmp(typed_bytes + 28, phrase_v2 + 24, 56));
    free(typed_bytes);
    typed_phrase.variant1 = 256;
    assert(sdk_analysis_commit_result("mod-a", job.job, &phrase_result) == AZ_INVALID);
    typed_phrase.variant1 = 0;
    assert(sdk_analysis_commit_result("mod-a", job.job, &phrase_result) == AZ_OK);
    assert(st_disk_sig(vocals_path) == before_vocals);
    /* No ANLZ at all: one loaded job publishes grid, phrases and vocals. */
    assert(!unlink(saved_dat) && !unlink(saved_ext) && !unlink(vocals_path));
    assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_OK && job.needed == (AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS));
    uint8_t combined[126] = {0};
    memcpy(combined, vocal_bundle, 24);
    sa_put32(combined + 8, 56);
    memcpy(combined + 24, phrase, 56);
    memcpy(combined + 80, pvdi, 46);
    /* Malformed and stale publications leave all target files untouched. */
    combined[80 + 24] = 5;
    assert(sa_commit(0, job.job, combined, sizeof combined) == AZ_INVALID);
    combined[80 + 24] = 0;
    fixture_current = 0;
    assert(sa_commit(0, job.job, combined, sizeof combined) == AZ_STALE);
    fixture_current = 1;
    assert(!st_disk_sig(saved_dat) && !st_disk_sig(saved_ext) && !st_disk_sig(vocals_path));
    assert(sa_commit(0, job.job, combined, sizeof combined) == AZ_OK);
    /* Saved phrases can refresh, then a vocal-only job retains the new map. */
    assert(!unlink(vocals_path));
    assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_OK && job.needed == AZ_ANALYSIS_VOCALS);
    memset(vocal_bundle + 24 + 24, 0, 22);
    assert(sa_commit(0, job.job, vocal_bundle, sizeof vocal_bundle) == AZ_OK);
    assert(sdk_vocal_intervals("mod-a", &ref, &vocals, NULL, 0) == AZ_OK && !vocals.interval_count && vocals.frame_count == 22);
    assert(sdk_analysis_open_v2("mod-a", &ref, &job) == AZ_NOT_FOUND);
    /* A PVDI created while a job is running invalidates the lease. */
    assert(!unlink(vocals_path));
    fixture_native_phrases = 1;
    assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_OK);
    size_t direct_n = 0;
    uint8_t *direct = sa_merge(NULL, 0, pvdi, 46, &direct_n);
    assert(direct);
    analysis_write(vocals_path, direct, direct_n);
    free(direct);
    assert(sa_commit(0, job.job, vocal_bundle, sizeof vocal_bundle) == AZ_STALE);
    assert(sdk_analysis_close("mod-a", job.job) == AZ_OK);
    assert(sdk_vocal_intervals("mod-a", &ref, &vocals, intervals, 4) == AZ_OK && vocals.method == AZ_VOCAL_PVDI);
    /* Streaming vocal-only output is readable through the loaded API. */
    fixture_source_kind = AZ_STATUS_SOURCE_STREAMING;
    snprintf(path, sizeof path, "%s/analysis/beatport-0/analysis.EXT", st_test_state);
    assert(!unlink(path));
    assert(sdk_analysis_open_loaded_v2("mod-a", 0, 44, &job) == AZ_OK && job.needed == AZ_ANALYSIS_VOCALS);
    assert(sa_commit(0, job.job, vocal_bundle, sizeof vocal_bundle) == AZ_OK);
    assert(sdk_vocal_intervals_loaded("mod-a", 0, 44, &vocals, NULL, 0) == AZ_OK && !vocals.interval_count);
    puts("Vocal SDK: PVDI preference, vocal-only/combined publication, instrumentals, stale races and Beatport reads passed");
    return 0;
}
