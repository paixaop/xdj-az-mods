#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
/* SPDX-License-Identifier: GPL-3.0-only */
#define main sdklib_fixture_main
#include "sdklib_test.c"
#undef main
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <dirent.h>
#include <stdatomic.h>
#include <sys/wait.h>
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
static int fixture_second_stream_deck, fixture_grid_shift;
static unsigned char fixture_stream_id = 23;
static char fixture_source_path[1024], st_test_state[PATH_MAX];
static int fixture_loaded_deck(unsigned deck) {
    return !deck || (deck == 1 && fixture_second_stream_deck);
}
static int fixture_source_open(unsigned deck, uint64_t epoch, uint64_t load, struct azn_analysis_source *out) {
    if (!fixture_source_ready) return AZ_NOT_FOUND;
    assert(fixture_loaded_deck(deck) && epoch == 9 && load == 44);
    *out = (struct azn_analysis_source) {.size = sizeof *out, .version = 1, .token = 7, .epoch = epoch, .load = load, .source_kind = AZ_STATUS_SOURCE_STREAMING};
    out->track_id[0] = 1;
    out->track_id[4] = 6;
    out->track_id[8] = fixture_stream_id;
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
static int fixture_current = 1, fixture_published, fixture_native_phrases;
static int fixture_publish_busy, fixture_publish_calls;
static _Atomic int fixture_released;
static uint32_t fixture_grid_count = 2;
static _Atomic int fixture_pause_current, fixture_current_entered, fixture_resume_current;
static _Atomic int fixture_skip_current;
static _Atomic int fixture_pause_admission, fixture_admission_entered, fixture_resume_admission;
static _Atomic int fixture_fail_input_beats;
static _Thread_local int fixture_publication_busy;
int __real_pthread_mutex_trylock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (fixture_publication_busy) {
        fixture_publication_busy = 0;
        return EBUSY;
    }
    return __real_pthread_mutex_trylock(mutex);
}
void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size) {
    if (count == 2 && size == sizeof(az_beat) && atomic_exchange(&fixture_fail_input_beats, 0)) return NULL;
    return __real_calloc(count, size);
}
int __real_az_storage_sql_ready(void);
int __wrap_az_storage_sql_ready(void) {
    if (atomic_exchange(&fixture_pause_admission, 0)) {
        atomic_store(&fixture_admission_entered, 1);
        while (!atomic_load(&fixture_resume_admission)) usleep(1000);
    }
    return __real_az_storage_sql_ready();
}
int az_status_loaded_reference(unsigned deck, uint64_t load, az_track_ref *out) {
    if (deck || load != 44) return AZ_STALE;
    *out = fixture_loaded_ref;
    return AZ_OK;
}
static int fixture_capture(unsigned deck, uint64_t load, struct aznm_packet *out) {
    if (!fixture_loaded_deck(deck) || load != 44) return AZ_STALE;
    memset(out, 0, sizeof *out);
    out->owner = 1;
    out->grid.content = 1;
    out->phrases.content = 2;
    out->grid.sample_rate = 44100;
    return AZ_OK;
}
static int fixture_valid(unsigned deck, uint64_t load, const struct aznm_packet *map) {
    int skip = atomic_load(&fixture_skip_current);
    if (skip) atomic_fetch_sub(&fixture_skip_current, 1);
    if (!skip && atomic_exchange(&fixture_pause_current, 0)) {
        atomic_store(&fixture_current_entered, 1);
        while (!atomic_load(&fixture_resume_current)) usleep(1000);
    }
    return fixture_loaded_deck(deck) && load == 44 && map->owner == 1 && fixture_current ? AZ_OK : AZ_STALE;
}
static int fixture_read(const struct aznm_component *c, unsigned kind, void *rows, uint32_t cap, uint32_t *count) {
    (void) c;
    *count = kind == AZNM_GRID ? fixture_grid_count : (uint32_t) fixture_native_phrases;
    if (rows && kind == AZNM_GRID) {
        assert(cap >= 2);
        az_beat beats[2] = {{0, 120, 1, 0}, {0.5, 120, 2, 0}};
        beats[0].time_s += fixture_grid_shift / 44100.0;
        beats[1].time_s += fixture_grid_shift / 44100.0;
        memcpy(rows, beats, sizeof beats);
    }
    return AZ_OK;
}
static int fixture_samples(const struct aznm_component *c, int64_t *ticks, uint32_t cap, uint32_t *count, uint32_t *rate) {
    (void) c;
    assert(cap >= 2);
    *count = 2;
    *rate = 44100;
    ticks[0] = fixture_grid_shift;
    ticks[1] = 22050 + fixture_grid_shift;
    return AZ_OK;
}
static void fixture_release(const struct aznm_packet *map) {
    assert(map->owner == 1);
    fixture_released++;
}
static uint32_t fixture_spool_track;
static uint64_t fixture_spool_after, fixture_spool_signature;
static char fixture_spool_path[PATH_MAX];
static void fixture_saved_link(void) {
    if (!fixture_spool_track) return;
    char directory[PATH_MAX], prefix[64];
    snprintf(directory, sizeof directory, "%s/sdb1/PIONEER/azspool", g_media);
    snprintf(prefix, sizeof prefix, "track-analysis-%08x-", fixture_spool_track);
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    unsigned found = 0;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, prefix, strlen(prefix))) continue;
        if (strtoull(entry->d_name + strlen(prefix), NULL, 10) <= fixture_spool_after) continue;
        snprintf(fixture_spool_path, sizeof fixture_spool_path, "%s/%s", directory, entry->d_name);
        fixture_spool_signature = st_disk_sig(fixture_spool_path);
        assert(fixture_spool_signature);
        found++;
    }
    closedir(dir);
    assert(found == 1);
}
static int fixture_publish(unsigned deck, uint64_t load, const struct aznm_packet *map, const void *bytes, uint32_t n) {
    if (fixture_valid(deck, load, map)) return AZ_STALE;
    assert(n == 56 && !memcmp(bytes, "PSSI", 4));
    fixture_publish_calls++;
    if (fixture_publish_busy) return AZ_BUSY;
    fixture_saved_link();
    fixture_published++;
    return AZ_OK;
}
static const struct azn_maps_api *fixture_maps(void) {
    static const struct azn_maps_api api = {.size = sizeof api, .version = 1, .read = fixture_read, .release = fixture_release, .read_samples = fixture_samples, .capture = fixture_capture, .current = fixture_valid, .publish_phrases = fixture_publish};
    return &api;
}
static int fixture_seed(unsigned deck, struct azstatus_native_receipt *out) {
    if (!fixture_loaded_deck(deck) || out->version != 1 || out->size < sizeof *out) return 0;
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
#include "analysis_source.h"
#include "analysis_encoding.h"
#include "analysis_vocals.h"
#include "analysis_native_adapter.h"
#include "sdk_analysis_write.h"
const char *az_analysis_state_dir(void) { return st_test_state; }
int az_analysis_thread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *x) {
    return pthread_create(t, a, f, x);
}
static void sa_put16(uint8_t *p, unsigned n) {
    p[0] = n >> 8;
    p[1] = n;
}
static uint8_t *read_analysis(const char *path, size_t *size, int *failed) {
    uint8_t *bytes = st_read_file(path, size);
    if (!bytes || !sa_framed(bytes, *size)) *failed = 1;
    return bytes;
}
static az_analysis_owner owner_at(int slot) {
    return slot < 0 ? (az_analysis_owner) {0} : analysis_owner_get((unsigned) slot);
}
static int acquire_track(int slot, uint32_t components, const az_track_ref *track, analysis_source_input *input) {
    analysis_source_request request = {.components = components, .track = *track};
    int rc = analysis_source_acquire(owner_at(slot), &request, input);
    if (rc == ANALYSIS_SOURCE_COMPLETE) memset(input, 0, sizeof *input);
    return rc;
}
static int acquire_loaded(int slot, uint32_t components, int deck, uint64_t load, analysis_source_input *input) {
    analysis_source_request request = {.loaded = 1, .components = components, .deck = deck, .load = load};
    int rc = analysis_source_acquire(owner_at(slot), &request, input);
    if (rc == ANALYSIS_SOURCE_COMPLETE) memset(input, 0, sizeof *input);
    return rc;
}
static int publish_result(int slot, analysis_source_input *input, const az_analysis_result_v1 *result) {
    az_analysis_owner owner = owner_at(slot);
    int rc = AZ_BUSY;
    /* General fixture callers want the admitted outcome; BUSY is a documented
     * try-lock refusal. Continuation tests call the service directly. */
    for (unsigned i = 0; i < 1000 && rc == AZ_BUSY; i++) {
        rc = analysis_source_publish(owner, input, result);
        if (rc == AZ_BUSY) usleep(1000);
    }
    if (rc == AZ_OK) analysis_source_release(input);
    return rc;
}
#include "analysis_spool_fixture.h"
static void analysis_write(const char *path, const void *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(bytes, 1, n, f) == n && !fclose(f));
}
static void display_read(int expected, unsigned frames) {
    struct vd_track track = {0};
    unsigned length = 99;
    assert(!analysis_owner_current(analysis_owner_core()));
    assert(analysis_vocal_track_loaded(0, 44, &track, &length) == expected);
    if (expected == AZ_OK) {
        assert(track.n == frames && track.rate == 22050 && track.hop == 1024 && length);
        free(track.v);
    } else assert(!track.v && length == 99);
}
static void display_masked(const char *path) {
    int failed = 0;
    size_t size = 0;
    uint8_t *bytes = read_analysis(path, &size, &failed);
    assert(bytes && !failed);
    uint8_t *masked = malloc(size);
    assert(masked);
    memcpy(masked, bytes, size);
    size_t at = sa_u32(masked + 4);
    assert(!memcmp(masked + at, "PVDI", 4));
    masked[at + 12] |= 0x80;
    for (unsigned i = 0; i < sa_u32(masked + at + 20); ++i) masked[at + 24 + i] ^= VD_KEY[i % 19];
    analysis_write(path, masked, size);
    display_read(AZ_OK, 22);
    masked[at + 24] = (uint8_t) (5 ^ VD_KEY[0]);
    analysis_write(path, masked, size);
    display_read(AZ_FAILED, 0);
    analysis_write(path, bytes, size);
    free(masked);
    free(bytes);
}
static void typed_grid_result_validation(void) {
    az_analysis_result_v1 empty = {0};
    struct sa_tags empty_tags = {0};
    assert(!sa_result_valid(&empty) && !sa_encode_native(&empty, &empty_tags));
    az_beat beats[2] = {{.time_s = 0, .bpm = 120, .beat_in_bar = 1},
                        {.time_s = 0.5, .bpm = 120, .beat_in_bar = 2}};
    az_analysis_phrase_v1 phrase = {.start_beat = 1, .kind = 1};
    az_analysis_result_v1 result = {.size = sizeof result, .version = AZ_DATA_VERSION, .source_rate = 44100, .source_frames = 44100, .beats = beats, .beat_count = 2, .phrases = {.entries = &phrase, .count = 1, .mood = 1, .end_beat = 2}};
    assert(sa_result_valid(&result));
    struct sa_tags tags = {0};
    uint8_t *bytes = sa_encode_native(&result, &tags);
    assert(bytes && tags.gn == 40 && tags.pn == 56 && !tags.vn);
    assert(!memcmp(tags.grid, "PQTZ", 4) && sa_u32(tags.grid + 20) == 2);
    assert(sa_u16(tags.grid + 24) == 1 && sa_u16(tags.grid + 26) == 12000);
    assert(compare_timestamps(sa_u32(tags.grid + 28), 0, 1000, result.source_rate, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(sa_u32(tags.grid + 36), 500, 1000, result.source_rate, 1) == AZ_TIME_EQUAL);
    free(bytes);

    /* Invalid source coordinates and millisecond serialization collisions must
     * be rejected before any partially encoded record can be published. */
    const double invalid_times[] = {NAN, INFINITY, -1, 1e10, 0, 0.0001};
    for (unsigned i = 0; i < sizeof invalid_times / sizeof *invalid_times; i++) {
        beats[1].time_s = invalid_times[i];
        assert(!sa_result_valid(&result));
    }
    beats[1].time_s = 0.5;
    const double invalid_tempos[] = {NAN, INFINITY, 0, -1, 1000};
    for (unsigned i = 0; i < sizeof invalid_tempos / sizeof *invalid_tempos; i++) {
        beats[1].bpm = invalid_tempos[i];
        assert(!sa_result_valid(&result));
    }
    beats[1].bpm = 120;
    beats[1].beat_in_bar = 0;
    assert(!sa_result_valid(&result));
    beats[1].beat_in_bar = 5;
    assert(!sa_result_valid(&result));
    beats[1].beat_in_bar = 2;
    beats[1].reserved = 1;
    assert(!sa_result_valid(&result));
}
static void input_reader_close(analysis_source_input *input) {
    char locator[1024];
    memcpy(locator, input->audio_path, sizeof locator);
    analysis_source_release(input);
    assert(!input->lease && !input->audio_path[0] && !input->beats);
    assert(access(locator, F_OK));
    analysis_source_release(input); /* runtime shutdown may repeat cleanup */
}
static void owner_lifecycle(const az_track_ref *track) {
    az_analysis_owner before = analysis_owner_get(0);
    analysis_source_input jobs[8] = {0}, replacement[4] = {0};
    analysis_owner_begin(1);
    for (unsigned i = 0; i < 8; ++i)
        assert(acquire_track((int) (i / 4), AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, track, &jobs[i]) == AZ_OK);
    analysis_source_input overflow = {0};
    assert(acquire_track(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, track, &overflow) == AZ_LIMIT);
    int reader = open(jobs[0].audio_path, O_RDONLY);
    assert(reader >= 0);
    int ready[2], resume[2];
    assert(!pipe(ready) && !pipe(resume));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[0]);
        close(resume[1]);
        char mark = 1, bytes[17];
        assert(write(ready[1], &mark, 1) == 1);
        assert(read(resume[0], &mark, 1) == 1);
        /* A decoder child is allowed to open its borrowed locator after the
         * owner retires, provided the runtime has not released the input. */
        int own = open(jobs[0].audio_path, O_RDONLY);
        assert(own >= 0 && read(own, bytes, sizeof bytes) == sizeof bytes);
        assert(!memcmp(bytes, "audio-placeholder", sizeof bytes));
        close(own);
        _exit(0);
    }
    close(ready[1]);
    close(resume[0]);
    char mark;
    assert(read(ready[0], &mark, 1) == 1 && mark);
    analysis_source_retire(analysis_owner_retire(0));
    assert(!analysis_owner_current(before));
    assert(acquire_track(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, track, &overflow) == AZ_STALE);
    char audio[PATH_MAX], changed[PATH_MAX];
    snprintf(audio, sizeof audio, "%s/sdb1/audio.wav", g_media);
    snprintf(changed, sizeof changed, "%s/sdb1/replacement.wav", g_media);
    analysis_write(changed, "replacement-audio", 17);
    assert(!rename(changed, audio));
    az_analysis_owner after = analysis_owner_begin(0);
    assert(!analysis_owner_current(analysis_owner_for_caller(0)));
    analysis_owner_thread_bind(after);
    assert(after.generation != before.generation && !analysis_owner_current(before));
    for (unsigned i = 0; i < 4; ++i) {
        int rc = AZ_LIMIT;
        for (unsigned attempt = 0; attempt < 1000 && (rc == AZ_LIMIT || rc == AZ_BUSY); ++attempt) {
            rc = acquire_track(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, track, &replacement[i]);
            if (rc != AZ_OK) usleep(1000);
        }
        assert(rc == AZ_OK && replacement[i].lease > jobs[7].lease);
    }
    /* Reused capacity proves native reclamation ran. The input owns its
     * descriptor independently, so no descriptor-number reuse can redirect it. */
    assert(!access(jobs[0].audio_path, F_OK) && !access(jobs[4].audio_path, F_OK));
    char bytes[17];
    assert(read(reader, bytes, sizeof bytes) == sizeof bytes);
    assert(!memcmp(bytes, "audio-placeholder", sizeof bytes));
    close(reader);
    assert(write(resume[1], "x", 1) == 1);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(ready[0]);
    close(resume[1]);
    for (unsigned i = 0; i < 4; ++i) input_reader_close(&jobs[i]);
    analysis_source_retire(analysis_owner_retire(0));
    analysis_source_retire(analysis_owner_retire(1));
    for (unsigned i = 4; i < 8; ++i) input_reader_close(&jobs[i]);
    for (unsigned i = 0; i < 4; ++i) input_reader_close(&replacement[i]);
    analysis_owner_thread_bind(analysis_owner_begin(0));
    analysis_write(audio, "audio-placeholder", 17);
    puts("Analysis input readers: paused subprocess opens retained audio after retirement, source replacement and capacity reuse");
}
static unsigned reader_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    unsigned count = 0;
    while (readdir(directory)) ++count;
    assert(!closedir(directory));
    return count;
}
static void acquisition_failure_cleanup(const az_track_ref *track) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/sdb1/analysis.EXT", g_media);
    size_t bytes = 0;
    uint8_t *saved = st_read_file(path, &bytes);
    assert(saved && bytes);
    analysis_write(path, "", 0);
    unsigned readers = reader_count();
    atomic_store(&fixture_fail_input_beats, 1);
    analysis_source_input input = {0};
    assert(acquire_track(0, AZ_DATA_PHRASES, track, &input) == AZ_FAILED);
    assert(!atomic_load(&fixture_fail_input_beats) && !input.lease && !input.audio_path[0]);
    assert(reader_count() == readers);
    analysis_source_release(&input);
    assert(acquire_track(0, AZ_DATA_PHRASES, track, &input) == AZ_OK);
    input_reader_close(&input);
    analysis_write(path, saved, bytes);
    free(saved);
    puts("Analysis input failure: allocation refusal closes opened audio and releases admission capacity");
}
struct pending_request {
    az_analysis_owner owner;
    az_track_ref track;
    analysis_source_input input;
    int result;
};
static void *acquire_pending_after_replacement(void *opaque) {
    struct pending_request *request = opaque;
    analysis_source_request source = {.components = AZ_DATA_PHRASES, .track = request->track};
    request->result = analysis_source_acquire(request->owner, &source, &request->input);
    return NULL;
}
static void pending_admission_lifetime(const az_track_ref *track, int core) {
    struct pending_request request = {.owner = core ? analysis_owner_core_begin() : analysis_owner_begin(1), .track = *track};
    atomic_store(&fixture_admission_entered, 0);
    atomic_store(&fixture_resume_admission, 0);
    atomic_store(&fixture_pause_admission, 1);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, acquire_pending_after_replacement, &request));
    while (!atomic_load(&fixture_admission_entered)) usleep(1000);
    if (core) analysis_owner_core_retire();
    else analysis_owner_retire(1);
    az_analysis_owner replacement = core ? analysis_owner_core_begin() : analysis_owner_begin(1);
    analysis_source_input no_link = {.lease = UINT64_MAX};
    int available = AZ_BUSY;
    for (unsigned i = 0; i < 1000 && available == AZ_BUSY; ++i) {
        available = analysis_source_outcome(request.owner, &no_link);
        if (available == AZ_BUSY) usleep(1000);
    }
    assert(available == AZ_STALE);
    atomic_store(&fixture_resume_admission, 1);
    assert(!pthread_join(worker, NULL));
    if (request.result != AZ_STALE) fprintf(stderr, "pending admission core=%d rc=%d lease=%llu\n", core, request.result, (unsigned long long) request.input.lease);
    assert(request.result == AZ_STALE && !request.input.lease);
    analysis_source_retire(request.owner);
    analysis_source_release(&request.input);
    analysis_source_request source = {.components = AZ_DATA_PHRASES, .track = *track};
    int rc = AZ_BUSY;
    for (unsigned i = 0; i < 1000 && rc == AZ_BUSY; ++i) {
        rc = analysis_source_acquire(replacement, &source, &request.input);
        if (rc == AZ_BUSY) usleep(1000);
    }
    assert(rc == AZ_PENDING && request.input.lease);
    analysis_source_release(&request.input);
    if (core) analysis_owner_core_retire();
    else analysis_owner_retire(1);
    analysis_source_retire(replacement);
    puts(core ? "Core pending-link admission: old generation refused, replacement usable" : "Mod pending-link admission: old generation refused, replacement usable");
}
struct retirement_request {
    az_analysis_owner owner;
    int result;
};
static void *acquire_during_retirement(void *opaque) {
    struct retirement_request *request = opaque;
    analysis_source_request source = {.loaded = 1, .deck = 0, .load = 44, .components = AZ_DATA_PHRASES};
    analysis_source_input input = {0};
    request->result = analysis_source_acquire(request->owner, &source, &input);
    analysis_source_release(&input);
    return NULL;
}
static void nonblocking_retirement(int core) {
    struct retirement_request request = {.owner = core ? analysis_owner_core_begin() : owner_at(0)};
    atomic_store(&fixture_pause_current, 1);
    atomic_store(&fixture_current_entered, 0);
    atomic_store(&fixture_resume_current, 0);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, acquire_during_retirement, &request));
    while (!atomic_load(&fixture_current_entered)) usleep(1000);
    /* Native current() is paused while the source service holds its own lock.
     * Revocation must return before that worker is allowed to proceed. */
    if (core) analysis_owner_core_retire();
    else analysis_source_retire(analysis_owner_retire(0));
    assert(!analysis_owner_current(request.owner));
    atomic_store(&fixture_resume_current, 1);
    assert(!pthread_join(worker, NULL));
    assert(request.result == AZ_STALE && !fixture_published);
    if (!core) analysis_owner_thread_bind(analysis_owner_begin(0));
    fixture_released = 0;
}
static void core_source_lifetime(const az_analysis_result_v1 *result) {
    az_analysis_owner old = analysis_owner_core_begin();
    analysis_source_request request = {.loaded = 1, .deck = 0, .load = 44, .components = AZ_DATA_PHRASES};
    analysis_source_input input = {0};
    assert(analysis_source_acquire(old, &request, &input) == AZ_OK);
    assert(input.beat_count == 2 && input.beats[1].beat_in_bar == 2);
    int releases_before = atomic_load(&fixture_released);
    analysis_owner_core_retire();
    analysis_source_retire(old);
    az_analysis_owner replacement = analysis_owner_core_begin();
    assert(replacement.generation != old.generation);
    assert(analysis_source_publish(old, &input, result) == AZ_STALE && !fixture_published);
    assert(input.beats[1].beat_in_bar == 2); /* copied input remains caller-owned */
    for (unsigned i = 0; i < 1000 && atomic_load(&fixture_released) == releases_before; ++i) usleep(1000);
    assert(atomic_load(&fixture_released) > releases_before);
    int reader = open(input.audio_path, O_RDONLY);
    char bytes[17];
    assert(reader >= 0 && read(reader, bytes, sizeof bytes) == sizeof bytes);
    assert(!memcmp(bytes, "audio-placeholder", sizeof bytes));
    close(reader);
    input_reader_close(&input);
    int rc = AZ_BUSY;
    for (unsigned i = 0; i < 1000 && rc == AZ_BUSY; ++i) {
        rc = analysis_source_acquire(replacement, &request, &input);
        if (rc == AZ_BUSY) usleep(1000);
    }
    assert(rc == AZ_OK);
    analysis_source_release(&input);
    analysis_owner_core_retire();
    fixture_released = 0;
}
struct publication_request {
    az_analysis_owner owner;
    analysis_source_input input;
    const az_analysis_result_v1 *result;
    int outcome;
};
static void *publish_during_retirement(void *opaque) {
    struct publication_request *request = opaque;
    request->outcome = analysis_source_publish(request->owner, &request->input, request->result);
    return NULL;
}
static void nonblocking_publication_retirement(const az_analysis_result_v1 *result, int core) {
    struct publication_request request = {.owner = core ? analysis_owner_core_begin() : owner_at(0), .result = result};
    analysis_source_request source = {.loaded = 1, .deck = 0, .load = 44, .components = AZ_DATA_PHRASES};
    assert(analysis_source_acquire(request.owner, &source, &request.input) == AZ_OK);
    char dat[PATH_MAX], ext[PATH_MAX];
    snprintf(dat, sizeof dat, "%s/sdb1/DJ.DAT", g_media);
    snprintf(ext, sizeof ext, "%s/sdb1/DJ.EXT", g_media);
    uint64_t before_dat = st_disk_sig(dat), before_ext = st_disk_sig(ext);
    /* Pause the final validation after staging, not the initial validation. */
    atomic_store(&fixture_skip_current, 1);
    atomic_store(&fixture_current_entered, 0);
    atomic_store(&fixture_resume_current, 0);
    atomic_store(&fixture_pause_current, 1);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, publish_during_retirement, &request));
    while (!atomic_load(&fixture_current_entered)) usleep(1000);
    if (core) analysis_owner_core_retire();
    else analysis_owner_retire(0);
    analysis_source_retire(request.owner);
    atomic_store(&fixture_resume_current, 1);
    assert(!pthread_join(worker, NULL));
    assert(request.outcome == AZ_STALE && !fixture_published);
    assert(st_disk_sig(dat) == before_dat && st_disk_sig(ext) == before_ext);
    analysis_source_release(&request.input);
    for (unsigned i = 0; i < 1000 && !atomic_load(&fixture_released); i++) usleep(1000);
    assert(atomic_load(&fixture_released) == 1);
    if (!core) analysis_owner_thread_bind(analysis_owner_begin(0));
    fixture_released = 0;
    puts(core ? "Core publication: retirement during native validation prevents file/native writes" : "Mod publication: retirement during native validation prevents file/native writes");
}
static void external_analysis_edit(const char *path) {
    size_t size = 0;
    int failed = 0;
    uint8_t *file = read_analysis(path, &size, &failed);
    uint8_t tag[12] = {'E', 'D', 'I', 'T', 0, 0, 0, 12, 0, 0, 0, 12};
    size_t updated_size = size + sizeof tag;
    uint8_t *updated = malloc(updated_size);
    assert(file && !failed && updated);
    memcpy(updated, file, size);
    memcpy(updated + size, tag, sizeof tag);
    sa_put32(updated + 8, (uint32_t) updated_size);
    analysis_write(path, updated, updated_size);
    free(updated);
    free(file);
}
static void edit_during_native_validation(az_analysis_owner owner, const analysis_source_input *input,
                                          const az_analysis_result_v1 *result, const char *path, unsigned skip, int calls) {
    struct publication_request publication = {.owner = owner, .input = *input, .result = result};
    atomic_store(&fixture_skip_current, (int) skip);
    atomic_store(&fixture_current_entered, 0);
    atomic_store(&fixture_resume_current, 0);
    atomic_store(&fixture_pause_current, 1);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, publish_during_retirement, &publication));
    while (!atomic_load(&fixture_current_entered)) usleep(1000);
    /* The vocal signature was checked before this native validation callback.
     * Publishing a different component must not adopt this later edit. */
    external_analysis_edit(path);
    atomic_store(&fixture_resume_current, 1);
    assert(!pthread_join(worker, NULL));
    assert(publication.outcome == AZ_BUSY && fixture_publish_calls == calls);
}
static void busy_publication(const az_analysis_result_v1 *phrases, int scenario) {
    az_analysis_owner owner = scenario == 3 ? analysis_owner_core_begin() : owner_at(0);
    az_vocal_frame_v1 frames[22] = {0};
    az_analysis_result_v1 result = *phrases;
    result.vocals = (az_analysis_vocal_map_v1) {.sample_rate = 22050, .hop_samples = 1024, .method = AZ_VOCAL_LSTM, .count = 22, .frames = frames};
    analysis_source_request request = {.loaded = 1, .deck = 0, .load = 44, .components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS};
    char paths[3][PATH_MAX];
    const char *suffix[3] = {"DAT", "EXT", "2EX"};
    for (unsigned i = 0; i < 3; i++) snprintf(paths[i], sizeof paths[i], "%s/sdb1/DJ.%s", g_media, suffix[i]);
    if (scenario == 6) {
        /* Native instrumental data already exists; this request needs phrases
         * only and must preserve the original vocal file's identity. */
        struct sa_tags tags = {0};
        uint8_t *encoded = sa_encode_native(&result, &tags);
        size_t size = 0;
        uint8_t *file = sa_merge(NULL, 0, tags.vocal, tags.vn, &size);
        assert(encoded && file);
        analysis_write(paths[2], file, size);
        free(file);
        free(encoded);
        memset(&result.vocals, 0, sizeof result.vocals);
    }
    analysis_source_input input = {0};
    assert(analysis_source_acquire(owner, &request, &input) == AZ_OK);
    uint64_t lease = input.lease;
    fixture_publish_busy = 1;
    fixture_publish_calls = 0;
    if (scenario == 6) {
        assert(input.needed == AZ_DATA_PHRASES);
        edit_during_native_validation(owner, &input, &result, paths[2], 1, 1);
    } else assert(analysis_source_publish(owner, &input, &result) == AZ_BUSY);
    assert(input.lease == lease && fixture_publish_calls == 1 && !fixture_published);
    uint64_t saved[3];
    for (unsigned i = 0; i < 3; i++) assert((saved[i] = st_disk_sig(paths[i])) != 0);
    if (scenario == 0) {
        /* Encoding identity includes all components and source provenance. */
        az_analysis_result_v1 changed = result;
        changed.source_frames++;
        assert(analysis_source_publish(owner, &input, &changed) == AZ_STALE);
        az_analysis_phrase_v1 phrase = *result.phrases.entries;
        phrase.kind = 2;
        changed = result;
        changed.phrases.entries = &phrase;
        assert(analysis_source_publish(owner, &input, &changed) == AZ_STALE);
        frames[0].activity = 4;
        int retry = analysis_source_publish(owner, &input, &result);
        if (retry != AZ_STALE) fprintf(stderr, "Native publication scenario %d: expected STALE, got %d\n", scenario, retry);
        assert(retry == AZ_STALE);
        frames[0].activity = 0;
        assert(fixture_publish_calls == 1);
        assert(analysis_source_publish(owner, &input, &result) == AZ_BUSY);
        assert(input.lease == lease && fixture_publish_calls == 2 && !fixture_published);
        fixture_publish_busy = 0;
        assert(analysis_source_publish(owner, &input, &result) == AZ_OK);
        assert(fixture_publish_calls == 3 && fixture_published == 1);
    } else {
        if (scenario == 1) {
            external_analysis_edit(paths[1]);
            saved[1] = st_disk_sig(paths[1]);
        } else if (scenario == 2) {
            fixture_current = 0;
        } else if (scenario == 5) {
            edit_during_native_validation(owner, &input, &result, paths[2], 0, 2);
            saved[2] = st_disk_sig(paths[2]);
        } else if (scenario == 3 || scenario == 4) {
            if (scenario == 3) analysis_owner_core_retire();
            else analysis_owner_retire(0);
            analysis_source_retire(owner);
        }
        fixture_publish_busy = 0;
        int retry = analysis_source_publish(owner, &input, &result);
        if (retry != AZ_STALE) fprintf(stderr, "Native publication scenario %d: expected STALE, got %d\n", scenario, retry);
        assert(retry == AZ_STALE);
        assert(fixture_publish_calls == (scenario == 5 ? 2 : 1) && !fixture_published);
    }
    for (unsigned i = 0; i < 3; i++) assert(st_disk_sig(paths[i]) == saved[i]);
    int fd = open(input.audio_path, O_RDONLY);
    char audio[17];
    assert(fd >= 0 && read(fd, audio, sizeof audio) == sizeof audio);
    assert(!memcmp(audio, "audio-placeholder", sizeof audio));
    close(fd);
    analysis_source_release(&input);
    assert(!input.lease);
    /* Retirement may detach the job before the cleaner releases native owners
     * outside its lock. Observe that release before resetting fixture counts. */
    for (unsigned i = 0; i < 1000 && !atomic_load(&fixture_released); i++) usleep(1000);
    assert(atomic_load(&fixture_released) == 1);
    if (scenario == 4) analysis_owner_thread_bind(analysis_owner_begin(0));
    fixture_current = 1;
    for (unsigned i = 0; i < 3; i++) assert(!unlink(paths[i]));
    fixture_released = fixture_published = 0;
}
static int refuse_commit(void *unused) {
    (void) unused;
    return 1;
}
static void link_failure_case(az_track_ref ref, const az_analysis_result_v1 *result, int later_edit) {
    char database[PATH_MAX], audio[PATH_MAX], statement[1024], spool[PATH_MAX];
    snprintf(database, sizeof database, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    snprintf(audio, sizeof audio, "%s/sdb1/link-%d.wav", g_media, later_edit);
    analysis_write(audio, "audio-placeholder", 17);
    ref.track_id = 20 + (uint64_t) later_edit;
    sqlite3 *db = NULL;
    assert(!sqlite3_open(database, &db));
    snprintf(statement, sizeof statement, "INSERT INTO content(content_id,path) VALUES(%llu,'/link-%d.wav')", (unsigned long long) ref.track_id, later_edit);
    sql(db, statement);
    struct st_resolution before = {0};
    assert(st_read_row(db, ref.track_id, &before) == AZ_OK && before.analysis_generated);
    assert(!sqlite3_close(db));
    analysis_source_input input = {0};
    assert(acquire_track(0, AZ_DATA_PHRASES, &ref, &input) == AZ_OK);
    assert(publish_result(0, &input, result) == AZ_PENDING);
    snprintf(spool, sizeof spool, "%s/sdb1/PIONEER/azspool/track-analysis-%08x-%llu.sql", g_media,
             (unsigned) ref.track_id, (unsigned long long) input.lease);
    char saved[PATH_MAX];
    snprintf(saved, sizeof saved, "%s/sdb1%s", g_media, before.analysis);
    uint64_t durable_grid = st_disk_sig(saved);
    assert(durable_grid);
    assert(!sqlite3_open(database, &db));
    if (later_edit) {
        snprintf(statement, sizeof statement, "UPDATE content SET path='/later.wav' WHERE content_id=%llu", (unsigned long long) ref.track_id);
        sql(db, statement);
    } else sqlite3_commit_hook(db, refuse_commit, NULL);
    assert(apply_analysis_spool(db, spool) == -4);
    sqlite3_commit_hook(db, NULL, NULL);
    struct st_resolution after = {0};
    assert(st_read_row(db, ref.track_id, &after) == AZ_OK && after.analysis_generated);
    if (later_edit) assert(!strcmp(after.content, "/later.wav"));
    assert(!sqlite3_close(db));
    await_link(&input, later_edit ? AZ_STALE : AZ_FAILED);
    assert(st_disk_sig(saved) == durable_grid);
    analysis_source_release(&input);
}
struct loaded_reuse {
    az_track_ref ref;
    uint64_t seed_lease;
    char database[PATH_MAX], paths[3][PATH_MAX];
};
static int reuse_request(az_analysis_owner owner, const analysis_source_request *request, analysis_source_input *input) {
    int rc = AZ_BUSY;
    for (unsigned i = 0; i < 1000 && rc == AZ_BUSY; i++) {
        rc = analysis_source_acquire(owner, request, input);
        if (rc == AZ_BUSY) usleep(1000);
    }
    return rc;
}
static int reuse_outcome(const analysis_source_input *input) {
    int rc = AZ_BUSY;
    for (unsigned i = 0; i < 1000 && rc == AZ_BUSY; i++) {
        rc = analysis_source_outcome(input->owner, input);
        if (rc == AZ_BUSY) usleep(1000);
    }
    return rc;
}
static _Atomic unsigned reuse_wakes;
static void reuse_wake(void) { atomic_fetch_add(&reuse_wakes, 1); }
static struct loaded_reuse loaded_reuse_seed(az_track_ref ref, const az_analysis_result_v1 *complete, unsigned track) {
    struct loaded_reuse fixture = {.ref = ref};
    fixture.ref.track_id = track;
    char audio[PATH_MAX], statement[1024];
    snprintf(fixture.database, sizeof fixture.database, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    snprintf(audio, sizeof audio, "%s/sdb1/reuse-%u.wav", g_media, track);
    analysis_write(audio, "audio-placeholder", 17);
    sqlite3 *db = NULL;
    assert(!sqlite3_open(fixture.database, &db));
    snprintf(statement, sizeof statement, "INSERT INTO content(content_id,path) VALUES(%u,'/reuse-%u.wav')", track, track);
    sql(db, statement);
    struct st_resolution row = {0};
    assert(st_read_row(db, track, &row) == AZ_OK && row.analysis_generated);
    assert(!sqlite3_close(db));
    const char *suffix[3] = {"DAT", "EXT", "2EX"};
    for (unsigned i = 0; i < 3; i++) {
        snprintf(fixture.paths[i], sizeof fixture.paths[i], "%s/sdb1%s", g_media, row.analysis);
        memcpy(fixture.paths[i] + strlen(fixture.paths[i]) - 3, suffix[i], 4);
    }
    analysis_source_input initial = {0};
    analysis_source_request seed = {.track = fixture.ref, .components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS};
    assert(reuse_request(owner_at(0), &seed, &initial) == AZ_OK);
    assert(publish_result(0, &initial, complete) == AZ_PENDING);
    fixture.seed_lease = initial.lease;
    analysis_source_release(&initial);
    fixture_source_kind = AZ_STATUS_SOURCE_USB1;
    fixture_loaded_ref = fixture.ref;
    fixture_native_phrases = 1;
    return fixture;
}
static void reuse_spool_path(const struct loaded_reuse *fixture, uint64_t lease, char path[PATH_MAX]) {
    snprintf(path, PATH_MAX, "%s/sdb1/PIONEER/azspool/track-analysis-%08x-%llu.sql", g_media,
             fixture->ref.track_id, (unsigned long long) lease);
}
static void reuse_commit_without_polling(const struct loaded_reuse *fixture, analysis_source_input *input) {
    char spool[PATH_MAX];
    reuse_spool_path(fixture, input->lease, spool);
    assert(!access(spool, F_OK));
    atomic_store(&reuse_wakes, 0);
    analysis_source_set_wake(reuse_wake);
    sqlite3 *db = NULL;
    assert(!sqlite3_open(fixture->database, &db));
    assert(apply_analysis_spool(db, spool) == 0);
    assert(!sqlite3_close(db));
    /* No outcome/status call drives progress: wait only for the service's wake. */
    for (unsigned i = 0; i < 3000 && !atomic_load(&reuse_wakes); i++) usleep(1000);
    assert(atomic_load(&reuse_wakes));
    await_link(input, AZ_OK);
    analysis_source_set_wake(NULL);
    analysis_source_release(input);
}
static void loaded_link_reuse(az_track_ref ref, const az_analysis_result_v1 *complete, int scenario) {
    struct loaded_reuse fixture = loaded_reuse_seed(ref, complete, (unsigned) (40 + scenario));
    if (scenario == 1 || scenario == 2 || scenario == 4) fixture_native_phrases = 0;
    if (scenario == 2 || scenario == 4) assert(!unlink(fixture.paths[2]));
    if (scenario == 3) assert(!unlink(fixture.paths[0]) && !unlink(fixture.paths[1]));
    if (scenario == 1) {
        fixture_spool_track = fixture.ref.track_id;
        fixture_spool_after = fixture.seed_lease;
        fixture_spool_signature = 0;
    }
    int published = fixture_published;
    __atomic_store_n(&g_ls_leftover, 0, __ATOMIC_RELEASE);
    analysis_source_input input = {0};
    analysis_source_request request = {.loaded = 1, .deck = 0, .load = 44, .components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS};
    int rc = reuse_request(owner_at(0), &request, &input);
    fprintf(stderr, "Loaded link reuse: scenario=%d acquire=%d lease=%llu\n", scenario, rc, (unsigned long long) input.lease);
    if (scenario == 1) {
        fixture_spool_track = 0;
        assert(fixture_spool_signature && st_disk_sig(fixture_spool_path) == fixture_spool_signature);
    }
    if (scenario == 3) {
        assert(rc == ANALYSIS_SOURCE_COMPLETE && !input.lease);
        assert(!st_disk_sig(fixture.paths[0]) && !st_disk_sig(fixture.paths[1]));
        return; /* Native PSSI plus a vocal file is not complete durable ANLZ. */
    }
    assert(rc == (scenario == 2 || scenario == 4 ? AZ_OK : AZ_PENDING) && input.lease);
    if (scenario == 2 || scenario == 4) {
        assert(input.needed == AZ_ANALYSIS_VOCALS);
        char spool[PATH_MAX];
        reuse_spool_path(&fixture, input.lease, spool);
        assert(access(spool, F_OK) && errno == ENOENT);
        assert(!__atomic_load_n(&g_ls_leftover, __ATOMIC_ACQUIRE));
        /* A commit cannot race the remaining computation: no native transaction
         * is submitted until its vocal file has been durably published. */
        az_analysis_result_v1 vocal = *complete;
        vocal.beats = NULL;
        vocal.beat_count = 0;
        memset(&vocal.phrases, 0, sizeof vocal.phrases);
        if (scenario == 4) {
            sqlite3 *external = NULL;
            assert(!sqlite3_open(fixture.database, &external));
            sql(external, "UPDATE content SET title='external edit' WHERE content_id=44");
            assert(!sqlite3_close(external));
            assert(publish_result(0, &input, &vocal) == AZ_STALE);
            assert(!st_disk_sig(fixture.paths[2]) && !st_disk_sig(spool));
            analysis_source_release(&input);
            return;
        }
        assert(publish_result(0, &input, &vocal) == AZ_PENDING);
        assert(st_disk_sig(spool));
    }
    assert(fixture_published == published + (scenario == 1 || scenario == 2));
    int outcome = reuse_outcome(&input);
    fprintf(stderr, "Loaded link reuse: initial outcome=%d\n", outcome);
    assert(outcome == AZ_PENDING);
    reuse_commit_without_polling(&fixture, &input);
}
static void native_only_phrase_reuse(az_track_ref ref) {
    fixture_loaded_ref = ref;
    fixture_loaded_ref.track_id = 99999; /* No USB row or persistent files. */
    fixture_native_phrases = 1;
    for (unsigned streaming = 0; streaming < 2; streaming++) {
        fixture_source_kind = streaming ? AZ_STATUS_SOURCE_STREAMING : AZ_STATUS_SOURCE_USB1;
        analysis_source_input input = {0};
        assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &input) == ANALYSIS_SOURCE_COMPLETE && !input.lease);
    }
}
struct reuse_admission {
    az_analysis_owner owner;
    analysis_source_input input;
    int rc;
};
static void *reuse_acquire(void *opaque) {
    struct reuse_admission *admission = opaque;
    analysis_source_request request = {.loaded = 1, .deck = 0, .load = 44, .components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS};
    admission->rc = reuse_request(admission->owner, &request, &admission->input);
    return NULL;
}
static void loaded_reuse_freshness(az_track_ref ref, const az_analysis_result_v1 *complete, int scenario) {
    struct loaded_reuse fixture = loaded_reuse_seed(ref, complete, (unsigned) (50 + scenario));
    struct reuse_admission admission = {.owner = scenario == 1 ? analysis_owner_core_begin() : owner_at(0)};
    int published = fixture_published;
    if (scenario == 3) fixture_native_phrases = 0;
    atomic_store(&fixture_skip_current, scenario == 3 ? 0 : 1); /* Before refresh, or final validation before first enqueue. */
    atomic_store(&fixture_current_entered, 0);
    atomic_store(&fixture_resume_current, 0);
    atomic_store(&fixture_pause_current, 1);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, reuse_acquire, &admission));
    while (!atomic_load(&fixture_current_entered)) usleep(1000);
    if (scenario == 0) analysis_source_retire(analysis_owner_retire(0));
    else if (scenario == 1) {
        analysis_owner_core_retire();
        analysis_source_retire(admission.owner);
    } else if (scenario == 3) analysis_write(fixture.paths[2], "malformed", 9);
    else external_analysis_edit(fixture.paths[0]);
    atomic_store(&fixture_resume_current, 1);
    assert(!pthread_join(worker, NULL));
    assert(admission.rc == (scenario == 3 ? AZ_FAILED : AZ_STALE) && !admission.input.lease);
    assert(fixture_published == published);
    analysis_source_release(&admission.input);
    if (scenario == 0) analysis_owner_thread_bind(analysis_owner_begin(0));
}

static void failed_refresh_releases_link(az_track_ref ref, const az_analysis_result_v1 *complete) {
    (void) loaded_reuse_seed(ref, complete, 60);
    fixture_native_phrases = 0;
    fixture_publish_busy = 1;
    az_analysis_owner owner = owner_at(0);
    analysis_source_request request = {.loaded = 1, .deck = 0, .load = 44, .components = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS};
    for (unsigned attempt = 0; attempt < 12; attempt++) {
        analysis_source_input input = {0};
        int calls = fixture_publish_calls, rc = AZ_BUSY;
        for (unsigned i = 0; i < 1000 && rc == AZ_BUSY && fixture_publish_calls == calls; i++) {
            rc = analysis_source_acquire(owner, &request, &input);
            if (rc == AZ_BUSY && fixture_publish_calls == calls) usleep(1000);
        }
        fprintf(stderr, "Native refresh refusal: attempt=%u rc=%d published_calls=%d\n", attempt, rc, fixture_publish_calls - calls);
        assert(rc == AZ_BUSY && fixture_publish_calls == calls + 1 && !input.lease);
        analysis_source_release(&input);
    }
    fixture_publish_busy = 0;
    analysis_source_input input = {0};
    assert(reuse_request(owner, &request, &input) == AZ_PENDING);
    analysis_source_release(&input);
}

static void beatport_reload_cache(const char *root, const az_analysis_result_v1 *phrases,
                                 const az_analysis_result_v1 *combined) {
    const uint32_t both = AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS;
    analysis_source_input job = {0}, other = {0};
    char original[1024], reload[1024], different[1024], collision[1024];
    memcpy(original, fixture_source_path, sizeof original);
    snprintf(reload, sizeof reload, "%s/reloaded.m4a", root);
    snprintf(different, sizeof different, "%s/different.m4a", root);
    snprintf(collision, sizeof collision, "%s/collision.m4a", root);
    analysis_write(reload, "native-completed-audio", 22);
    analysis_write(different, "different-beatport-audio", 24);
    analysis_write(collision, "collision-audio-24", 18);
    assert(st_disk_sig(original) != st_disk_sig(reload));

    fixture_native_phrases = 0;
    fixture_second_stream_deck = 1;
    assert(acquire_loaded(0, both, 0, 44, &job) == AZ_OK && job.needed == AZ_DATA_PHRASES);
    assert(publish_result(0, &job, phrases) == AZ_OK);
    int publishes = fixture_published;
    memcpy(fixture_source_path, reload, sizeof reload);
    ++fixture_stream_id;
    assert(acquire_loaded(0, both, 1, 44, &job) == ANALYSIS_SOURCE_COMPLETE && !job.lease && !job.needed);
    assert(fixture_published == publishes + 1);

    memcpy(fixture_source_path, different, sizeof different);
    assert(acquire_loaded(0, both, 0, 44, &job) == AZ_OK && job.needed == both);
    assert(publish_result(0, &job, combined) == AZ_OK);
    memcpy(fixture_source_path, reload, sizeof reload);
    assert(acquire_loaded(0, both, 0, 44, &job) == ANALYSIS_SOURCE_COMPLETE && !job.lease);

    /* Grid changes invalidate only PSSI/PQTZ. A colliding source cannot displace
     * the slot while this job has it pinned, including during a read-only probe. */
    fixture_grid_shift = 4410;
    assert(acquire_loaded(0, both, 0, 44, &job) == AZ_OK && job.needed == AZ_DATA_PHRASES);
    memcpy(fixture_source_path, collision, sizeof collision);
    az_vocal_snapshot_v1 vocals = {.size = sizeof vocals};
    assert(analysis_vocal_intervals_loaded(owner_at(0), 1, 44, &vocals, NULL, 0) == AZ_NOT_FOUND);
    assert(acquire_loaded(0, both, 1, 44, &other) == AZ_BUSY && !other.lease);
    memcpy(fixture_source_path, reload, sizeof reload);
    assert(publish_result(0, &job, phrases) == AZ_OK);
    assert(acquire_loaded(0, both, 0, 44, &job) == ANALYSIS_SOURCE_COMPLETE && !job.lease);
    memcpy(fixture_source_path, collision, sizeof collision);
    assert(acquire_loaded(0, both, 1, 44, &job) == AZ_OK && job.needed == both);
    analysis_source_release(&job);
    fixture_grid_shift = 0;
    fixture_second_stream_deck = 0;
    memcpy(fixture_source_path, original, sizeof original);
}
int main(void) {
    analysis_owner_thread_bind(analysis_owner_begin(0));
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
    analysis_source_input missing = {0};
    az_track_ref absent = ref;
    absent.track_id = 999;
    assert(acquire_track(0, AZ_DATA_PHRASES, &absent, &missing) == AZ_NOT_FOUND);
    analysis_source_input job = {0};
    assert(acquire_track(-1, AZ_DATA_PHRASES, &ref, &job) == AZ_STALE);
    assert(acquire_track(0, AZ_DATA_PHRASES, &ref, &job) == AZ_OK);
    assert(job.needed == (AZ_DATA_GRID | AZ_DATA_PHRASES) && job.lease);
    uint8_t grid[40] = {0}, phrase[56] = {0};
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
    az_beat baseline_beats[2] = {{.time_s = 0, .bpm = 120, .beat_in_bar = 1},
                                 {.time_s = 0.5, .bpm = 120, .beat_in_bar = 2}};
    az_analysis_phrase_v1 baseline_phrase = {.start_beat = 1, .kind = 1};
    az_analysis_result_v1 baseline_result = {.size = sizeof baseline_result, .version = AZ_DATA_VERSION, .source_rate = 44100, .source_frames = 44100, .beats = baseline_beats, .beat_count = 2, .phrases = {.mood = 1, .end_beat = 2, .count = 1, .entries = &baseline_phrase}};
    az_analysis_result_v1 loaded_result = baseline_result;
    loaded_result.beats = NULL;
    loaded_result.beat_count = 0;
    assert(publish_result(-1, &job, &baseline_result) == AZ_STALE);
    /* A mutated audio file invalidates the lease without writing either container. */
    analysis_write(path, "changed-audio-placeholder", 25);
    fixture_publication_busy = 1; /* Explicitly exercise the bounded admission retry. */
    int changed_audio = publish_result(0, &job, &baseline_result);
    if (changed_audio != AZ_STALE) fprintf(stderr, "Changed audio: expected STALE, got %d\n", changed_audio);
    assert(!fixture_publication_busy && changed_audio == AZ_STALE);
    analysis_source_release(&job);
    assert(acquire_track(0, AZ_DATA_PHRASES, &ref, &job) == AZ_OK);
    assert(publish_result(0, &job, &baseline_result) == AZ_OK);
    analysis_source_release(&job);
    assert(acquire_track(0, AZ_DATA_PHRASES, &ref, &job) == ANALYSIS_SOURCE_COMPLETE);
    size_t n = 0;
    int failed = 0;
    snprintf(path, sizeof path, "%s/sdb1/analysis.DAT", g_media);
    uint8_t *file = read_analysis(path, &n, &failed);
    assert(file && !failed && sa_tag_count(file, n, "PQTZ") == 2);
    free(file);
    snprintf(path, sizeof path, "%s/sdb1/analysis.EXT", g_media);
    file = read_analysis(path, &n, &failed);
    assert(file && !failed && sa_tag_count(file, n, "PSSI") == 1);
    free(file);
    /* Cached-file reuse publishes to native maps during acquisition. It must
     * observe retirement at that publication boundary too. */
    fixture_loaded_ref = ref;
    nonblocking_retirement(0);
    nonblocking_retirement(1);
    acquisition_failure_cleanup(&ref);
    az_analysis_result_v1 short_result = {.size = 4, .version = AZ_DATA_VERSION};
    assert(publish_result(0, &(analysis_source_input) {.lease = 1, .owner = owner_at(0)}, &short_result) == AZ_INVALID);
    /* A row without an analysis location gets complete files and a conditional
     * native-spool update. Apostrophes in audio paths remain literal SQL data. */
    snprintf(path, sizeof path, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    assert(!sqlite3_open(path, &db));
    sql(db, "UPDATE content SET path='/quote''track.wav',analysisDataFilePath=NULL WHERE content_id=10");
    assert(!sqlite3_close(db));
    snprintf(path, sizeof path, "%s/sdb1/quote'track.wav", g_media);
    analysis_write(path, "audio-placeholder", 17);
    job = (analysis_source_input) {0};
    assert(acquire_track(0, AZ_DATA_PHRASES, &ref, &job) == AZ_OK);
    assert(publish_result(0, &job, &baseline_result) == AZ_PENDING);
    analysis_source_input saved_link = job;
    pending_admission_lifetime(&ref, 0);
    pending_admission_lifetime(&ref, 1);
    job = (analysis_source_input) {0};
    int reopened;
    do { reopened = acquire_track(0, AZ_DATA_PHRASES, &ref, &job); } while (reopened == AZ_BUSY);
    assert(reopened == AZ_PENDING && job.lease);
    analysis_source_input reused_link = job;
    snprintf(path, sizeof path, "%s/sdb1/PIONEER/azspool/track-analysis-0000000a-%llu.sql", g_media, (unsigned long long) job.lease);
    FILE *spool = fopen(path, "r");
    char statement[6500];
    assert(spool && fgets(statement, sizeof statement, spool) && fgets(statement, sizeof statement, spool) && !fclose(spool));
    assert(strstr(statement, "path=CAST(X'2f71756f746527747261636b2e776176' AS TEXT)") &&
           strstr(statement, "analysisDataFilePath IS NULL"));
    assert(strchr(statement, ';') == strrchr(statement, ';') && !strchr(statement, 0x01));
    char link_path[PATH_MAX];
    snprintf(link_path, sizeof link_path, "%s", path);
    snprintf(path, sizeof path, "%s/sdb1/PIONEER/rekordbox/exportLibrary.db", g_media);
    assert(!sqlite3_open(path, &db));
    assert(apply_analysis_spool(db, link_path) == 0);
    assert(!sqlite3_close(db));
    await_link(&saved_link, AZ_OK);
    await_link(&reused_link, AZ_OK);
    analysis_source_release(&saved_link);
    analysis_source_release(&reused_link);
    assert(acquire_track(0, AZ_DATA_PHRASES, &ref, &job) == ANALYSIS_SOURCE_COMPLETE && !job.lease);
    assert(!sqlite3_open(path, &db));
    sql(db, "UPDATE content SET analysisDataFilePath='/DJ.DAT' WHERE content_id=10");
    sql(db, statement);
    struct st_resolution manual = {0};
    assert(st_read_row(db, 10, &manual) == AZ_OK && !strcmp(manual.analysis, "/DJ.DAT"));
    assert(!sqlite3_close(db));
    /* Native-only lease pins its grid, rejects a replaced load, then publishes
     * both a missing persistent grid and the native phrase notification. */
    fixture_loaded_ref = ref;
    fixture_grid_count = 0;
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_BUSY);
    fixture_grid_count = 2;
    fixture_released = 0;
    nonblocking_retirement(0);
    nonblocking_retirement(1);
    core_source_lifetime(&loaded_result);
    nonblocking_publication_retirement(&loaded_result, 0);
    nonblocking_publication_retirement(&loaded_result, 1);
    for (int scenario = 0; scenario < 7; scenario++) busy_publication(&loaded_result, scenario);
    puts("Native publication continuation: durable files retained through BUSY; same lease completes without rewrites; changed result, files, load and owners refused");
    assert(acquire_loaded(-1, AZ_DATA_PHRASES, 0, 44, &job) == AZ_STALE);
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 43, &job) == AZ_STALE);
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_OK);
    assert(job.needed == AZ_DATA_PHRASES && job.beat_count == 2);
    assert(job.beats[0].beat_in_bar == 1 && job.beats[1].beat_in_bar == 2);
    assert(compare_timestamps(job.beats[1].time_s, 0.5, 1, 44100, 1) == AZ_TIME_EQUAL);
    char snapshot[1024];
    memcpy(snapshot, job.audio_path, sizeof snapshot);
    fixture_current = 0;
    assert(publish_result(0, &job, &loaded_result) == AZ_STALE);
    assert(!fixture_published);
    analysis_source_release(&job);
    assert(access(snapshot, F_OK) && fixture_released == 1);
    fixture_current = 1;
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_OK);
    assert(publish_result(0, &job, &loaded_result) == AZ_OK);
    assert(fixture_published == 1 && fixture_released == 2);
    /* Cache hit refreshes native state immediately instead of running analysis. */
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == ANALYSIS_SOURCE_COMPLETE && !job.lease);
    assert(fixture_published == 2 && fixture_released == 3);
    /* Same row count is insufficient: a shifted disk grid must not receive
     * phrases inferred against the pinned current native grid. */
    snprintf(path, sizeof path, "%s/sdb1/DJ.EXT", g_media);
    analysis_write(path, "", 0);
    snprintf(path, sizeof path, "%s/sdb1/DJ.DAT", g_media);
    failed = 0;
    file = read_analysis(path, &n, &failed);
    assert(file && !failed);
    sa_put32(file + 28 + 28, 100);
    analysis_write(path, file, n);
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_OK);
    assert(publish_result(0, &job, &loaded_result) == AZ_STALE);
    analysis_source_release(&job);
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
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == ANALYSIS_SOURCE_COMPLETE && !job.lease);
    assert(fixture_published == 3); /* populated DAT wins over empty EXT */
    fixture_published = 2; /* independent streaming assertions below */
    snprintf(st_test_state, sizeof st_test_state, "%s/settings", root);
    assert(!mkdir(st_test_state, 0700));
    snprintf(fixture_source_path, sizeof fixture_source_path, "%s/12345.m4a", root);
    analysis_write(fixture_source_path, "native-completed-audio", 22);
    fixture_source_kind = AZ_STATUS_SOURCE_STREAMING;
    fixture_source_ready = 0;
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_BUSY);
    fixture_source_ready = 1;
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_OK);
    assert(!job.track.track_id && !job.track.source_id);
    analysis_source_input streaming_job = job;
    job = (analysis_source_input) {0};
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_BUSY);
    fixture_source_ready = 0;
    assert(publish_result(0, &streaming_job, &loaded_result) == AZ_STALE);
    analysis_source_release(&streaming_job);
    fixture_source_ready = 1;
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_OK);
    assert(publish_result(0, &job, &loaded_result) == AZ_OK);
    assert(fixture_published == 3 && fixture_source_closed == 3);
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == ANALYSIS_SOURCE_COMPLETE);
    assert(fixture_published == 4 && fixture_source_closed == 4);
    uint8_t malformed[40] = {0};
    memcpy(malformed, "PMAI", 4);
    sa_put32(malformed + 4, 28);
    sa_put32(malformed + 8, 40);
    memcpy(malformed + 28, "PSSI", 4);
    sa_put32(malformed + 32, 12);
    sa_put32(malformed + 36, 12);
    snprintf(path, sizeof path, "%s/analysis/beatport-v2-70/analysis.EXT", st_test_state);
    analysis_write(path, malformed, sizeof malformed);
    assert(acquire_loaded(0, AZ_DATA_PHRASES, 0, 44, &job) == AZ_FAILED);
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
    assert(analysis_vocal_intervals(owner_at(0), &ref, &vocals, intervals, 4) == AZ_NOT_FOUND);
    display_read(AZ_NOT_FOUND, 0);
    assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == AZ_OK);
    assert(job.needed == AZ_ANALYSIS_VOCALS);
    uint8_t pvdi[46] = {0};
    memcpy(pvdi, "PVDI", 4);
    sa_put32(pvdi + 4, 24);
    sa_put32(pvdi + 8, 46);
    sa_put32(pvdi + 12, 1024);
    sa_put16(pvdi + 16, 22050);
    sa_put16(pvdi + 18, 1);
    sa_put32(pvdi + 20, 22);
    memset(pvdi + 24 + 3, 4, 8);
    memset(pvdi + 24 + 20, 4, 2);
    az_vocal_frame_v1 vocal_frames[22] = {0};
    for (unsigned i = 0; i < 22; i++) vocal_frames[i].activity = pvdi[24 + i];
    az_analysis_result_v1 vocal_result = {.size = sizeof vocal_result, .version = AZ_DATA_VERSION, .source_rate = 44100, .source_frames = 44100, .vocals = {.sample_rate = 22050, .hop_samples = 1024, .method = AZ_VOCAL_LSTM, .count = 22, .frames = vocal_frames}};
    char vocals_path[1024], saved_dat[1024], saved_ext[1024];
    snprintf(vocals_path, sizeof vocals_path, "%s/sdb1/DJ.2EX", g_media);
    snprintf(saved_dat, sizeof saved_dat, "%s/sdb1/DJ.DAT", g_media);
    snprintf(saved_ext, sizeof saved_ext, "%s/sdb1/DJ.EXT", g_media);
    uint64_t before_dat = st_disk_sig(saved_dat), before_ext = st_disk_sig(saved_ext);
    int published_before = fixture_published;
    assert(publish_result(0, &job, &vocal_result) == AZ_OK);
    assert(st_disk_sig(saved_dat) == before_dat && st_disk_sig(saved_ext) == before_ext && fixture_published == published_before);
    assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == ANALYSIS_SOURCE_COMPLETE && !job.lease && !job.needed);
    assert(analysis_vocal_intervals(owner_at(-1), &ref, &vocals, intervals, 4) == AZ_STALE);
    assert(analysis_vocal_intervals(owner_at(0), &ref, &vocals, NULL, 0) == AZ_LIMIT && vocals.interval_count == 2);
    assert(analysis_vocal_intervals_loaded(owner_at(0), 0, 43, &vocals, intervals, 4) == AZ_STALE);
    assert(analysis_vocal_intervals_loaded(owner_at(0), 0, 44, &vocals, intervals, 4) == AZ_OK);
    assert(vocals.method == AZ_VOCAL_LSTM && vocals.interval_count == 2);
    assert(compare_timestamps(intervals[0].start_s, 3072.0 / 22050, 1, 22050, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(intervals[1].stop_s, 1.0, 1, 44100, 1) == AZ_TIME_EQUAL);
    display_read(AZ_OK, 22);
    display_masked(vocals_path);
    fixture_current = 0;
    display_read(AZ_STALE, 0);
    fixture_current = 1;
    /* Typed LSTM output floors resampled length before hop rounding. */
    for (int source = 0; source < 2; source++) {
        assert(!unlink(vocals_path));
        assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == AZ_OK);
        az_vocal_frame_v1 frames[9];
        for (unsigned i = 0; i < 9; i++) frames[i].activity = 4;
        az_analysis_result_v1 result = {.size = sizeof result, .version = AZ_DATA_VERSION, .source_rate = source ? 48000 : 44100, .source_frames = source ? 20063 : 18433, .vocals = {.sample_rate = 22050, .hop_samples = 1024, .method = AZ_VOCAL_LSTM, .count = 9, .frames = frames}};
        struct sa_tags native = {0};
        uint8_t *encoded = sa_encode_native(&result, &native);
        assert(encoded && native.vn == 33 && !memcmp(native.vocal, "PVDI", 4));
        assert(sa_u32(native.vocal + 20) == 9);
        free(encoded);
        result.vocals.method = AZ_VOCAL_SPECTRAL;
        assert(publish_result(0, &job, &result) == AZ_INVALID);
        result.vocals.method = AZ_VOCAL_LSTM;
        assert(publish_result(-1, &job, &result) == AZ_STALE);
        frames[0].activity = 5;
        assert(publish_result(0, &job, &result) == AZ_INVALID);
        frames[0].activity = 4;
        result.vocals.count = UINT32_MAX;
        assert(publish_result(0, &job, &result) == AZ_INVALID);
        result.vocals.count = 9;
        result.vocals.frames = NULL;
        assert(publish_result(0, &job, &result) == AZ_INVALID);
        result.vocals.frames = frames;
        fixture_current = 0;
        assert(publish_result(0, &job, &result) == AZ_STALE);
        fixture_current = 1;
        assert(publish_result(0, &job, &result) == AZ_OK);
        assert(analysis_vocal_intervals_loaded(owner_at(0), 0, 44, &vocals, intervals, 4) == AZ_OK);
        assert(vocals.method == AZ_VOCAL_LSTM && vocals.frame_count == 9 && vocals.interval_count == 1);
        assert(compare_timestamps(vocals.analyzed_s, (double) (source ? 20063 : 18433) / (source ? 48000 : 44100),
                                  1, source ? 48000 : 44100, 1) == AZ_TIME_EQUAL);
    }
    assert(!unlink(vocals_path));
    assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == AZ_OK);
    assert(publish_result(0, &job, &vocal_result) == AZ_OK);
    /* Existing PVDI is preserved when phrases and the DAT are absent. */
    uint64_t before_vocals = st_disk_sig(vocals_path);
    assert(!unlink(saved_dat) && !unlink(saved_ext));
    fixture_native_phrases = 0;
    assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == AZ_OK && job.needed == AZ_DATA_PHRASES);
    az_analysis_phrase_v1 typed_phrase = {.start_beat = 1, .kind = 1};
    az_analysis_result_v1 phrase_result = {.size = sizeof phrase_result, .version = AZ_DATA_VERSION, .source_rate = 44100, .source_frames = 44100, .phrases = {.mood = 1, .end_beat = 2, .count = 1, .entries = &typed_phrase}};
    struct sa_tags typed_tags = {0};
    uint8_t *typed_bytes = sa_encode_native(&phrase_result, &typed_tags);
    assert(typed_bytes && typed_tags.pn == 56 && !memcmp(typed_tags.phrase, phrase, 56));
    free(typed_bytes);
    typed_phrase.variant1 = 256;
    assert(publish_result(0, &job, &phrase_result) == AZ_INVALID);
    typed_phrase.variant1 = 0;
    assert(publish_result(0, &job, &phrase_result) == AZ_OK);
    assert(st_disk_sig(vocals_path) == before_vocals);
    /* No ANLZ at all: one loaded job publishes grid, phrases and vocals. */
    assert(!unlink(saved_dat) && !unlink(saved_ext) && !unlink(vocals_path));
    assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == AZ_OK && job.needed == (AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS));
    az_analysis_result_v1 combined = phrase_result;
    combined.vocals = vocal_result.vocals;
    /* Malformed and stale publications leave all target files untouched. */
    vocal_frames[0].activity = 5;
    assert(publish_result(0, &job, &combined) == AZ_INVALID);
    vocal_frames[0].activity = 0;
    fixture_current = 0;
    assert(publish_result(0, &job, &combined) == AZ_STALE);
    fixture_current = 1;
    assert(!st_disk_sig(saved_dat) && !st_disk_sig(saved_ext) && !st_disk_sig(vocals_path));
    assert(publish_result(0, &job, &combined) == AZ_OK);
    /* Saved phrases can refresh, then a vocal-only job retains the new map. */
    assert(!unlink(vocals_path));
    assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == AZ_OK && job.needed == AZ_ANALYSIS_VOCALS);
    memset(vocal_frames, 0, sizeof vocal_frames);
    assert(publish_result(0, &job, &vocal_result) == AZ_OK);
    assert(analysis_vocal_intervals(owner_at(0), &ref, &vocals, NULL, 0) == AZ_OK && !vocals.interval_count && vocals.frame_count == 22);
    assert(acquire_track(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, &ref, &job) == ANALYSIS_SOURCE_COMPLETE);
    /* A PVDI created while a job is running invalidates the lease. */
    assert(!unlink(vocals_path));
    fixture_native_phrases = 1;
    assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == AZ_OK);
    size_t direct_n = 0;
    uint8_t *direct = sa_merge(NULL, 0, pvdi, 46, &direct_n);
    assert(direct);
    analysis_write(vocals_path, direct, direct_n);
    free(direct);
    assert(publish_result(0, &job, &vocal_result) == AZ_STALE);
    analysis_source_release(&job);
    assert(analysis_vocal_intervals(owner_at(0), &ref, &vocals, intervals, 4) == AZ_OK && vocals.method == AZ_VOCAL_PVDI);
    /* Streaming vocal-only output is readable through the loaded API. */
    fixture_source_kind = AZ_STATUS_SOURCE_STREAMING;
    snprintf(path, sizeof path, "%s/analysis/beatport-v2-70/analysis.EXT", st_test_state);
    assert(!unlink(path));
    assert(acquire_loaded(0, AZ_DATA_PHRASES | AZ_ANALYSIS_VOCALS, 0, 44, &job) == AZ_OK && job.needed == AZ_ANALYSIS_VOCALS);
    assert(publish_result(0, &job, &vocal_result) == AZ_OK);
    assert(analysis_vocal_intervals_loaded(owner_at(0), 0, 44, &vocals, NULL, 0) == AZ_OK && !vocals.interval_count);
    display_read(AZ_OK, 22); /* Beatport, even with automatic analysis disabled. */
    beatport_reload_cache(root, &phrase_result, &combined);
    puts("Beatport cache: redownload, deck change, intervening track, grid change and slot collision passed");
    az_analysis_result_v1 reused = baseline_result;
    reused.vocals = vocal_result.vocals;
    for (int scenario = 0; scenario < 5; scenario++) loaded_link_reuse(ref, &reused, scenario);
    for (int scenario = 0; scenario < 4; scenario++) loaded_reuse_freshness(ref, &reused, scenario);
    native_only_phrase_reuse(ref);
    failed_refresh_releases_link(ref, &reused);
    puts("Loaded saved reuse: autonomous link completion, deferred partial linkage, native-only availability and owner/source refusal passed");
    link_failure_case(ref, &baseline_result, 0);
    link_failure_case(ref, &baseline_result, 1);
    puts("Link outcomes: real transaction commit failure and later row edit preserve saved files without false success");
    puts("Vocal SDK: PVDI preference, vocal-only/combined publication, instrumentals, stale races and Beatport reads passed");
    return 0;
}
