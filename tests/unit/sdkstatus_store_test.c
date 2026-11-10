/* SPDX-License-Identifier: GPL-3.0-only */
static double saved_positions[64];
static unsigned npositions;
static double first_beats[128];
static unsigned nbeats;
static double shared_positions[64];
static unsigned shared_count;
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE 1
#include "azmod-mod.h"
#include "sdktrack_fixture.h"
#include "sdk_status_layout.h"
#include "sdk_status_proof.h"
static unsigned proof_database, proof_other, proof_baselines;
static unsigned publications[4], loss[4], retired[4];
static unsigned ready[4][AZ_STATUS_TOPIC_COUNT][8], sent[4][AZ_STATUS_TOPIC_COUNT];
static uint64_t last_epoch[4], loss_topics[4], revision[4][AZ_STATUS_TOPIC_COUNT];
static char last_key[4][32], last_format[4][32];
static uint8_t preview[4];
static az_status_grid_ref_v1 saved_map[4];
static pthread_mutex_t test_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cv = PTHREAD_COND_INITIALIZER;
static int ds_publish_file(unsigned deck, uint64_t epoch, const az_track_ref *ref, uint64_t observed,
                           const struct ds_field *fields, size_t count, uint64_t lost, unsigned reason) {
    assert(ref->source_id && observed);
    pthread_mutex_lock(&test_mu);
    last_epoch[deck] = epoch;
    if (lost) {
        assert(reason == AZ_STATUS_CAPTURE_GAP || reason == AZ_STATUS_REVISION_LOST);
        loss[deck]++;
        loss_topics[deck] |= lost;
        if (ds_proof_context.kind == DS_PROOF_GAP) {
            assert(ds_proof_context.binding_cut && ds_proof_context.binding_serial);
            if (ds_proof_context.cause == DS_PROOF_DATABASE) {
                assert(ds_proof_context.connection && ds_proof_context.db_before && ds_proof_context.db_after);
                assert(ds_proof_context.db_before != ds_proof_context.db_after);
                proof_database++;
            } else proof_other++;
        }
    }
    if (count && ds_proof_context.kind == DS_PROOF_BASELINE) {
        assert(ds_proof_context.binding_cut && ds_proof_context.binding_serial && ds_proof_context.topics);
        proof_baselines++;
    }
    if (count) publications[deck]++;
    for (size_t i = 0; i < count; i++) {
        const struct ds_field *f = &fields[i];
        assert(f->info.scope_id == epoch && f->info.scope == AZ_STATUS_LOAD_SCOPE && f->info.provider_id);
        assert(f->info.info.provenance == AZ_PROV_FILE ||
               (f->topic == AZ_STATUS_PHRASES && f->field == AZ_STATUS_PHRASES_SAVED_GRID &&
                f->info.info.provenance == AZ_PROV_DERIVED));
        sent[deck][f->topic]++;
        ready[deck][f->topic][f->field] = f->info.info.availability;
        revision[deck][f->topic] = f->info.info.revision;
        if (!f->value) continue;
        if (f->topic == AZ_STATUS_METADATA && (f->field == AZ_STATUS_METADATA_DURATION_S || f->field == AZ_STATUS_METADATA_DURATION_PRECISION_S)) {
            assert(f->clock && f->clock->units_per_second == 1 && f->clock->window_rate == 1 && f->clock->window_samples == 1 && f->clock->ticks);
            int order = compare_timestamps(*f->clock->ticks, *(const double *) f->value, 1, 1, 1);
            assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
        }
        if (f->topic == AZ_STATUS_GRID || f->topic == AZ_STATUS_CUES) {
            assert(f->clock && f->clock->units_per_second == 1000 && f->clock->window_rate == 1000 && f->clock->window_samples == 1 && f->clock->ticks);
            for (unsigned j = 0; j < f->info.count; j++) {
                double seconds = f->topic == AZ_STATUS_GRID ? ((const az_beat *) f->value)[j].time_s : ((const az_cue_entry *) f->value)[j].start_s;
                int64_t ticks = f->clock->ticks[f->topic == AZ_STATUS_GRID ? j : 2 * j];
                int order = compare_timestamps((long double) ticks, (long double) seconds * 1000, 1000, 1000, 1);
                assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
                if (f->topic == AZ_STATUS_CUES && (((const az_cue_entry *) f->value)[j].valid & AZ_CUE_END)) {
                    order = compare_timestamps((long double) f->clock->ticks[2 * j + 1], (long double) ((const az_cue_entry *) f->value)[j].end_s * 1000, 1000, 1000, 1);
                    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
                }
            }
        }
        if (f->topic == AZ_STATUS_OVERVIEW) {
            const az_waveform *wave = f->value;
            assert(f->clock && f->clock->units_per_second == 1 && f->clock->window_rate == 1 && f->clock->window_samples == 1 && f->clock->ticks);
            int order = compare_timestamps(*f->clock->ticks, wave->seconds_per_sample * wave->sample_count, 1, 1, 1);
            assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
        }
        if (f->topic == AZ_STATUS_PHRASES && f->field == AZ_STATUS_PHRASES_SAVED_GRID)
            saved_map[deck] = *(const az_status_grid_ref_v1 *) f->value;
        if (!deck && f->topic == AZ_STATUS_GRID && f->field == AZ_STATUS_GRID_SAVED &&
            f->info.count && nbeats < 128) first_beats[nbeats++] = ((const az_beat *) f->value)->time_s;
        if (!deck && f->topic == AZ_STATUS_CUES && f->field == AZ_STATUS_CUES_SAVED_MEMORY &&
            f->info.count == 1 && npositions < 64)
            saved_positions[npositions++] = ((const az_cue_entry *) f->value)->start_s;
        if (deck == 3 && f->topic == AZ_STATUS_CUES && f->field == AZ_STATUS_CUES_SAVED_MEMORY &&
            f->info.count == 1 && shared_count < 64)
            shared_positions[shared_count++] = ((const az_cue_entry *) f->value)->start_s;
        if (f->topic == AZ_STATUS_METADATA && f->field == AZ_STATUS_METADATA_FORMAT)
            snprintf(last_format[deck], sizeof last_format[deck], "%s", (const char *) f->value);
        if (f->topic == AZ_STATUS_KEY) snprintf(last_key[deck], sizeof last_key[deck], "%s", (const char *) f->value);
        if (f->topic == AZ_STATUS_OVERVIEW) {
            const az_waveform *w = f->value;
            assert(w->sample_count == 2 && w->samples);
            int extent = compare_timestamps((long double) w->seconds_per_sample * w->sample_count, 0.,
                                             1, f->clock->window_rate, f->clock->window_samples);
            assert(extent != AZ_TIME_INVALID && extent == AZ_TIME_AFTER);
            preview[deck] = w->samples[1];
        }
    }
    pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    return AZ_OK;
}
static int ds_retire_file_binding(unsigned deck, uint64_t epoch, const az_track_ref *ref) {
    assert(epoch && ref->source_id);
    retired[deck]++;
    return AZ_OK;
}
static unsigned bindings;
static az_track_ref bound_ref;
static int ds_publish(unsigned deck, uint64_t epoch, unsigned flags, uint64_t now, const struct ds_field *fields, size_t count,
                      const az_status_occurrence_v1 *occ, const az_status_phrase_transition_v1 *phrase, uint64_t lost, unsigned reason) {
    assert(deck < 4 && epoch && !flags && now && count == 2 && !occ && !phrase && !lost && !reason);
    assert(fields[0].topic == AZ_STATUS_LOAD && fields[0].field == AZ_STATUS_LOAD_TRACK);
    bound_ref = *(const az_track_ref *) fields[0].value;
    bindings++;
    return AZ_OK;
}
static unsigned origin_refresh[4];
void azfile_native_refresh(unsigned deck, uint64_t epoch) {
    assert(deck < 4 && epoch);
    ++origin_refresh[deck];
}
#include "sdk_status_store.c"
/* The host fixture gives its two simulated media synthetic mount IDs. Check
 * the actual host SQLite Unix fd against its device/inode on both ports. */
int azsdk_sources_reader_fd(void *db, uint64_t mount, uint64_t device, uint64_t inode) {
    struct stat sb;
    int fd = libfd_fd_of(db);
    return mount && fd >= 0 && !fstat(fd, &sb) && device == (uint64_t) sb.st_dev &&
                   inode == (uint64_t) sb.st_ino
               ? fd
               : -1;
}
int azsdk_sources_descriptor_identity(int fd, uint64_t mount, uint64_t device, uint64_t inode) {
    struct stat sb;
    return mount && fd >= 0 && !fstat(fd, &sb) && device == (uint64_t) sb.st_dev && inode == (uint64_t) sb.st_ino;
}
static uintptr_t sm_ptr(uintptr_t p, unsigned n) { return *(uintptr_t *) (p + n); }
#include "sdk_status_save.c"
static struct azsdk_source_sample fake_native;
static int fake_owner_rc = AZS_OK;
static unsigned fake_busy_reads;
int azjs_lookup(uint64_t token, struct azjs_origin *out) {
    if (token != 73) return 0;
    out->owner = 85;
    out->track_id = 10;
    return 1;
}
int azsdk_sources_status_read(uint64_t epoch, void *out, size_t size) {
    assert(epoch == 85 && size == sizeof fake_native);
    if (fake_busy_reads) {
        --fake_busy_reads;
        return AZS_BUSY;
    }
    memcpy(out, &fake_native, size);
    return fake_owner_rc;
}

static void replace(const char *path, const uint8_t *bytes, size_t n) {
    char tmp[PATH_MAX];
    assert(snprintf(tmp, sizeof tmp, "%s.tmp", path) < (int) sizeof tmp);
    FILE *f = fopen(tmp, "wb");
    assert(f && fwrite(bytes, 1, n, f) == n && !fclose(f));
    assert(!rename(tmp, path));
}
static void watch(unsigned deck, uint64_t topics) {
    /* Deterministic cuts, the same monitor and parsing functions as both workers. */
    g_sdk_sq.ok = -1;
    sdk_status_store_interest(deck, topics);
    g_sdk_sq.ok = 1;
}
static void wait_key(unsigned deck, const char *key) {
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += 4;
    pthread_mutex_lock(&test_mu);
    while (strcmp(last_key[deck], key))
        assert(pthread_cond_timedwait(&test_cv, &test_mu, &end) == 0);
    pthread_mutex_unlock(&test_mu);
}
#ifdef __linux__
static int (*real_step)(void *);
static struct {
    const char *root, *path;
    const uint16_t *audio, *analysis;
    size_t n;
    uint8_t *bytes;
    size_t size;
} delayed_grid;
static int finish_grid_while_observing(void *statement) {
    g_sdk_sq.step = real_step;
    struct sw_batch commit = {.lane = SS_SAVED_GRID};
    sw_start(&commit, delayed_grid.root, strlen(delayed_grid.root), delayed_grid.audio, delayed_grid.n, delayed_grid.analysis, 6);
    assert(commit.count == 2);
    delayed_grid.bytes[31] = 8;
    FILE *file = fopen(delayed_grid.path, "r+b");
    assert(file && fwrite(delayed_grid.bytes, 1, delayed_grid.size, file) == delayed_grid.size && !fclose(file));
    sw_finish(&commit);
    return real_step(statement);
}
#endif
/* Physical companion pairs with both a memory loop and a hot loop. The row
 * offsets differ in DAT/EXT and have no relation to native vector ordinals. */
static size_t origin_container(uint8_t *data, unsigned ext, unsigned duplicate, struct azn_file_cue_row rows[2]) {
    size_t pos = 12;
    memset(data, 0, 320);
    memcpy(data, "PMAI", 4);
    data[7] = 12;
    for (unsigned hot = 0; hot < 2; hot++) {
        uint8_t *tag = data + pos;
        unsigned header = ext ? 20 : 24;
        unsigned count = !hot && duplicate ? 2 : 1;
        unsigned total = ext ? 48 : 40, extent = header + total * count;
        memcpy(tag, ext ? "PCO2" : "PCOB", 4);
        tag[7] = header;
        tag[11] = extent;
        tag[15] = hot;
        tag[ext ? 17 : 19] = count;
        uint8_t *atom = tag + header;
        memcpy(atom, ext ? "PCP2" : "PCPT", 4);
        atom[7] = ext ? 16 : 28;
        atom[11] = total;
        atom[15] = hot ? 8 : 0;
        atom[ext ? 16 : 28] = 2;
        layered_put32(atom + (ext ? 20 : 32), 1000 + hot);
        layered_put32(atom + (ext ? 24 : 36), 2000 + hot);
        if (count == 2) memcpy(atom + total, atom, total);
        struct azn_file_atom *stamp = ext ? &rows[hot].pcp2 : &rows[hot].pcpt;
        stamp->header = atom[7];
        stamp->total = total;
        azsha256_hex(atom + 14, total - 14, stamp->digest);
        pos += extent;
    }
    if (!ext) {
        memcpy(data + pos, "PQTZ", 4);
        data[pos + 7] = data[pos + 11] = 24;
        pos += 24;
    }
    layered_put32(data + 8, (uint32_t) pos);
    return pos;
}
static int file_time_equal(double actual, double expected) {
    int order = compare_timestamps(actual, expected, 1, 1000, 1);
    assert(order != AZ_TIME_INVALID);
    return order == AZ_TIME_EQUAL;
}
static void integer_source_clock_cases(void) {
    uint8_t dat[320], ext[320];
    struct azn_file_cue_row rows[2] = {0};
    size_t sizes[] = {origin_container(dat, 0, 0, rows), origin_container(ext, 1, 0, rows)};
    layered_put32(dat + 12 + 24 + 32, 1002);
    layered_put32(ext + 12 + 20 + 20, 1003);
    const uint8_t *inputs[] = {dat, ext};
    struct st_data *data = malloc(sizeof *data);
    assert(data);
    st_data_init(data);
    int64_t raw_cues[2 * ST_CUES] = {0}, raw_beats[2] = {0};
    data->cue_ticks = raw_cues;
    assert(!st_parse_selected(data, inputs, sizes, AZ_DATA_CUES, 1));
    int cue_order = compare_timestamps(raw_cues[0], 1003, 1000, 1000, 1);
    assert(cue_order != AZ_TIME_INVALID && cue_order == AZ_TIME_EQUAL);
    assert(data->view.cue_count == 1 && !(data->cues[0].valid & AZ_CUE_AUTO_ACTIVE));
    uint8_t grid[52] = {[0] = 'P', [1] = 'M', [2] = 'A', [3] = 'I', [7] = 12, [11] = 52, [12] = 'P', [13] = 'Q', [14] = 'T', [15] = 'Z', [19] = 24, [23] = 40, [35] = 2};
    layered_put32(grid + 40, 1002);
    layered_put32(grid + 48, 1003);
    inputs[0] = grid;
    inputs[1] = NULL;
    sizes[0] = sizeof grid;
    sizes[1] = 0;
    st_data_init(data);
    data->beat_ticks = raw_beats;
    assert(!st_parse_component(data, inputs, sizes, AZ_DATA_GRID) && data->view.beat_count == 2);
    int beat_order = compare_timestamps(raw_beats[1], 1003, 1000, 1000, 1);
    assert(beat_order != AZ_TIME_INVALID && beat_order == AZ_TIME_EQUAL);
    free(data);
}
static void original_inputs_opened(uint64_t read, unsigned inputs) {
    for (unsigned i = 0; i < FO_READS; i++) {
        struct fo_read *r = fo_reads[i];
        if (!r || r->id != read) continue;
        for (unsigned f = 0; f < 2; f++) {
            if (!(inputs & (1u << f)) || r->files[f].rc) continue;
            int fd = open(r->files[f].path, O_RDONLY | O_CLOEXEC);
            assert(fd >= 0 && fo_opened(read, fd) == (1u << f) && fcntl(fd, F_GETFD) >= 0);
            close(fd);
        }
        return;
    }
    assert(!"missing original native read");
}
static void origin_cases(const struct st_source *src, sqlite3 *writer, const char *dat, const char *ext) {
    char root[1100];
    snprintf(root, sizeof root, "%s/PIONEER", src->mount);
    uint16_t audio[30], analysis[] = {'/', 'a', '.', 'D', 'A', 'T'};
    const char *name = "/Contents/a/ten.mp3";
    size_t n = strlen(name);
    for (unsigned i = 0; i < n; i++) audio[i] = (uint16_t) name[i];
    struct azn_file_cue_row rows[2] = {0};
    uint8_t dat_bytes[320], ext_bytes[320];
    size_t dn = origin_container(dat_bytes, 0, 0, rows), en = origin_container(ext_bytes, 1, 0, rows);
    replace(dat, dat_bytes, dn);
    replace(ext, ext_bytes, en);
    const struct azn_file_cue_callbacks *cb = azfile_status_callbacks();
    assert(cb->version == 2);
    uint64_t token[AZN_FILE_CUE_MAX], id, other[AZN_FILE_CUE_MAX], stale_token[AZN_FILE_CUE_MAX];
    az_status_cue_ref_v1 ref = {0};
    assert(!cb->begin(root, strlen(root) - 1, audio, n, analysis, 6));
    audio[1] = 'X';
    assert(!cb->begin(root, strlen(root), audio, n, analysis, 6));
    audio[1] = 'C';
    sql(writer, "INSERT INTO content SELECT 90,title,artist_id_artist,album_id,key_id,path,bpmx100,length,analysisDataFilePath,fileType FROM content WHERE content_id=10");
    assert(!cb->begin(root, strlen(root), audio, n, analysis, 6));
    sql(writer, "DELETE FROM content WHERE content_id=90");
    /* FileData may be created by GRID before a cue getter exists. Its
     * physical input remains the authority for the later typed merge. */
    const struct azn_file_input_api *inputs = cb->inputs();
    assert(inputs && inputs->version == 1);
    uint64_t physical[2] = {0};
    id = inputs->physical_begin(dat, strlen(dat), NULL, 0);
    assert(id && !inputs->seal(id, 1)); /* a path without the original fd is insufficient */
    id = inputs->physical_begin(dat, strlen(dat), NULL, 0);
    assert(id);
    original_inputs_opened(id, 1);
    replace(dat, dat_bytes, dn);
    int replaced_fd = open(dat, O_RDONLY | O_CLOEXEC);
    assert(replaced_fd >= 0 && !inputs->opened(id, replaced_fd));
    close(replaced_fd);
    assert(!inputs->seal(id, 1)); /* equal-byte replacement between opens is a different input */
    id = inputs->physical_begin(dat, strlen(dat), NULL, 0);
    assert(id);
    original_inputs_opened(id, 1);
    original_inputs_opened(id, 1); /* stock parser reopens the same physical file */
    physical[0] = inputs->seal(id, 1);
    id = inputs->physical_begin(ext, strlen(ext), NULL, 0);
    assert(id);
    original_inputs_opened(id, 2);
    original_inputs_opened(id, 2);
    physical[1] = inputs->seal(id, 1);
    assert(physical[0] && physical[1]);
    id = inputs->join(physical);
    assert(id);
    cb->complete(id, rows, 2, other);
    assert(other[0] && other[1]);
    /* The cache still holds its old token after an equal-byte replacement.
     * A new getter cannot claim that it read the replacement. */
    replace(dat, dat_bytes, dn);
    assert(!inputs->join(physical));
    inputs->hold(physical, 2, 0);
    sql(writer, "INSERT INTO content SELECT 90,title,artist_id_artist,album_id,key_id,'/different.mp3',bpmx100,length,analysisDataFilePath,fileType FROM content WHERE content_id=10");
    assert(!inputs->physical_begin(dat, strlen(dat), NULL, 0));
    sql(writer, "DELETE FROM content WHERE content_id=90");
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id && cb->opened);
    int wrong_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(wrong_fd >= 0);
    assert(!cb->opened(id, wrong_fd) && fcntl(wrong_fd, F_GETFD) >= 0);
    close(wrong_fd);
    cb->complete(id, rows, 2, other);
    assert(!other[0] && !other[1]);
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    original_inputs_opened(id, 2);
    cb->complete(id, rows, 2, other);
    assert(!other[0] && !other[1]);
    int old_fd = open(dat, O_RDONLY | O_CLOEXEC);
    assert(old_fd >= 0);
    replace(dat, dat_bytes, dn);
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    assert(!cb->opened(id, old_fd) && fcntl(old_fd, F_GETFD) >= 0);
    close(old_fd);
    cb->complete(id, rows, 2, other);
    assert(!other[0] && !other[1]);
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    original_inputs_opened(id, 3);
    cb->complete(id, rows, 2, stale_token);
    assert(stale_token[0]);
    replace(dat, dat_bytes, dn);
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    original_inputs_opened(id, 3);
    cb->complete(id, rows, 2, token);
    assert(token[0] && token[1] && token[0] != token[1]);
    assert(!azfile_status_ref(2, 50, token[0], &ref));
    unsigned held = FO_READS;
    for (unsigned i = 0; i < FO_READS; i++)
        if (fo_reads[i] && fo_reads[i]->id == id) held = i;
    assert(held < FO_READS && cb->hold);
    /* Content custody and the copied working property independently retain
     * the same original read, even before the first subscription. The audio
     * callback must succeed while the storage mutex is already held. */
    pthread_mutex_lock(&ss_mu);
    cb->hold(token, 2, 1);
    cb->hold(token, 2, 1);
    pthread_mutex_unlock(&ss_mu);
    assert((atomic_load(&fo_custody[held].state) & FO_COUNT_MASK) == 2);
    for (unsigned i = 0; i < FO_READS + 2; i++) {
        uint64_t next = cb->begin(root, strlen(root), audio, n, analysis, 6);
        assert(next);
        original_inputs_opened(next, 3);
        cb->complete(next, rows, 2, other);
        assert(other[0]);
    }
    cb->hold(token, 2, 0);
    assert((atomic_load(&fo_custody[held].state) & FO_COUNT_MASK) == 1);
    uint64_t states[FO_READS];
    for (unsigned i = 0; i < FO_READS; i++) states[i] = atomic_load(&fo_custody[i].state);
    cb->hold(stale_token, 2, 1); /* evicted token must not acquire the reused slot */
    for (unsigned i = 0; i < FO_READS; i++) assert(states[i] == atomic_load(&fo_custody[i].state));
    watch(2, SS_TOPIC(CUES));
    sdk_status_store_binding(2, 50, &src->ref, 1);
    ss_monitor_deck(2);
    ss_work();
    assert(!azfile_status_ref(2, 50, stale_token[0], &ref));
    assert(azfile_status_ref(2, 50, token[0], &ref) && ref.provenance == AZ_PROV_FILE &&
           ref.cue_field == AZ_STATUS_CUES_SAVED_MEMORY && ref.entry.entry_id == 1 && ref.entry.type == AZ_CUE_LOOP);
    assert(azfile_status_ref(2, 50, token[1], &ref) && ref.cue_field == AZ_STATUS_CUES_SAVED_HOT &&
           ref.entry.slot == 8 && file_time_equal(ref.entry.start_s, 1.001) && ref.collection_revision == revision[2][AZ_STATUS_CUES]);
    assert(!azfile_status_ref(2, 51, token[1], &ref) && !azfile_status_ref(1, 50, token[1], &ref));
#ifdef __linux__
    /* A committed metadata change breaks DB observation but cannot destroy
     * the native import's independently watched, unchanged cue inputs. */
    unsigned losses = loss[2];
    sql(writer, "UPDATE content SET bpmx100=bpmx100+1 WHERE content_id=10");
    ss_monitor_deck(2);
    ss_work();
    assert(loss[2] > losses);
    assert(azfile_status_ref(2, 50, token[0], &ref) && ref.provenance == AZ_PROV_FILE &&
           ref.cue_field == AZ_STATUS_CUES_SAVED_MEMORY);
    assert(azfile_status_ref(2, 50, token[1], &ref) && ref.cue_field == AZ_STATUS_CUES_SAVED_HOT);
    sql(writer, "UPDATE content SET bpmx100=bpmx100-1 WHERE content_id=10");
    ss_monitor_deck(2);
    ss_work();
    assert(azfile_status_ref(2, 50, token[1], &ref));
    uint64_t cue_revision = ref.collection_revision;
    losses = loss[2];
    struct sw_batch grid_edit = {.lane = SS_SAVED_GRID};
    sw_start(&grid_edit, root, strlen(root), audio, n, analysis, 6);
    assert(grid_edit.count == 1);
    dat_bytes[dn - 5] = 4;
    FILE *grid_file = fopen(dat, "r+b");
    assert(grid_file && fwrite(dat_bytes, 1, dn, grid_file) == dn && !fclose(grid_file));
    sw_finish(&grid_edit);
    ss_monitor_deck(2);
    ss_work();
    assert(loss[2] == losses && azfile_status_ref(2, 50, token[1], &ref) && ref.collection_revision == cue_revision);
#endif
    /* Identical bytes and row number on another physical USB cannot transfer
     * its importer origin to this deck's qualified source. */
    struct st_source foreign;
    assert(!st_source_read(1, &foreign));
    char foreign_root[1100], foreign_file[1024];
    snprintf(foreign_root, sizeof foreign_root, "%s/PIONEER", foreign.mount);
    assert(st_source_path(foreign.mount, "/a.DAT", foreign_file));
    replace(foreign_file, dat_bytes, dn);
    assert(st_source_path(foreign.mount, "/a.EXT", foreign_file));
    replace(foreign_file, ext_bytes, en);
    id = cb->begin(foreign_root, strlen(foreign_root), audio, n, analysis, 6);
    assert(id);
    original_inputs_opened(id, 3);
    cb->complete(id, rows, 2, other);
    assert(other[0] && !azfile_status_ref(2, 50, other[0], &ref));
    /* Churn the bounded completed-read registry. The positively resolved
     * current-loop read remains held while unrelated reads are evicted. */
    for (unsigned i = 0; i < FO_READS + 2; i++) {
        id = cb->begin(foreign_root, strlen(foreign_root), audio, n, analysis, 6);
        assert(id);
        original_inputs_opened(id, 3);
        cb->complete(id, rows, 2, other);
        assert(other[0]);
    }
    assert(azfile_status_ref(2, 50, token[0], &ref));
    ss_pins[2].epoch = 50;
    ss_pins[2].token = 73;
    fake_native.flags = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_PHYSICAL;
    fake_native.physical_status = AZS_PHYSICAL_OK;
    fake_native.mount_id = src->mount_id;
    fake_native.device = src->device;
    fake_native.inode = src->inode;
    assert(azfile_status_ref(2, 50, token[0], &ref));
    ++fake_native.mount_id;
    assert(!azfile_status_ref(2, 50, token[0], &ref));
    --fake_native.mount_id;
    ss_pins[2].retired = 1;
    assert(!azfile_status_ref(2, 50, token[0], &ref));
    memset(&ss_pins[2], 0, sizeof ss_pins[2]);
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    struct azn_file_cue_row forged = rows[0];
    forged.pcpt.digest[0] ^= 1;
    original_inputs_opened(id, 3);
    cb->complete(id, &forged, 1, other);
    assert(!other[0]);
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    struct azn_file_cue_row repeats[] = {rows[0], rows[0]};
    original_inputs_opened(id, 3);
    cb->complete(id, repeats, 2, other);
    assert(!other[0] && !other[1]);
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    sql(writer, "UPDATE content SET title=title || 'x' WHERE content_id=10");
    original_inputs_opened(id, 3);
    cb->complete(id, rows, 2, other);
    assert(!other[0] && !other[1]);
    /* A replaced physical entry invalidates the old import, even if rows
     * have the same times. A duplicated raw PCPT rejects just its own row. */
    dn = origin_container(dat_bytes, 0, 1, rows);
    replace(dat, dat_bytes, dn);
    assert(!azfile_status_ref(2, 50, token[0], &ref));
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    original_inputs_opened(id, 3);
    cb->complete(id, rows, 2, other);
    assert(!other[0] && other[1]);
    cb->hold(token, 2, 0);
    assert(!(atomic_load(&fo_custody[held].state) & FO_COUNT_MASK));
    sdk_status_store_binding(2, 0, NULL, 2);
    ss_monitor_deck(2);
    assert(!azfile_status_ref(2, 50, other[1], &ref));
    watch(2, 0);
}

static struct azn_file_tag map_claim(const uint8_t *tag, unsigned first) {
    struct azn_file_tag value = {.name = hc_be32(tag), .header = hc_be32(tag + 4), .total = hc_be32(tag + 8), .first = first};
    azsha256_hex(tag + first, value.total - first, value.digest);
    return value;
}
static int map_busy_once(void *statement) {
    (void) statement;
    g_sdk_sq.step = real_step;
    return SQLITE_BUSY;
}
static void map_origin_cases(const struct st_source *src, sqlite3 *writer, const char *dat, const char *ext, const uint8_t *phrases) {
    uint8_t grid[128] = {0}, phrase[68];
    memcpy(phrase, phrases, sizeof phrase);
    memcpy(grid, "PMAI", 4);
    grid[7] = 12;
    grid[11] = 68;
    memcpy(grid + 12, "PQTZ", 4);
    grid[19] = 24;
    grid[23] = 56;
    grid[35] = 4;
    for (unsigned i = 0; i < 4; i++) {
        grid[37 + i * 8] = (uint8_t) (i + 1);
        grid[38 + i * 8] = 0x2e;
        grid[39 + i * 8] = 0xe0;
        layered_put32(grid + 40 + i * 8, i * 500);
    }
    replace(dat, grid, 68);
    replace(ext, phrase, sizeof phrase);
    char root[1100];
    snprintf(root, sizeof root, "%s/PIONEER", src->mount);
    const char *path = "/Contents/a/ten.mp3";
    size_t n = strlen(path);
    uint16_t audio[40], analysis[] = {'/', 'a', '.', 'D', 'A', 'T'};
    for (unsigned i = 0; i < n; i++) audio[i] = (uint16_t) path[i];
    const struct azn_file_map_callbacks *cb = azfile_map_callbacks();
    struct azn_file_tag claims[] = {map_claim(grid + 12, 14), map_claim(phrase + 12, 0)};
    uint64_t token[3], id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    original_inputs_opened(id, 3);
    cb->complete(id, claims, 2, token);
    assert(token[0] && token[1]);
    cb->hold(token, 2, 1);
    uint64_t pair[] = {token[0], 0, token[1]};
    sdk_status_store_binding(2, 60, NULL, 3);
    azfile_map_pair(2, 60, pair); /* genuine paired consumer may precede LOAD.track */
    watch(2, SS_TOPIC(PHRASES));
    sdk_status_store_binding(2, 60, &src->ref, 4);
    ss_monitor_deck(2);
    ss_work();
    assert(ready[2][AZ_STATUS_GRID][AZ_STATUS_GRID_SAVED] == AZ_AVAIL_READY);
    assert(ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_READY);
    assert(saved_map[2].load_epoch == 60 && saved_map[2].provider_id == ss_decks[2].provider[1] &&
           saved_map[2].revision == ss_decks[2].revision[AZ_STATUS_GRID] &&
           saved_map[2].provenance == AZ_PROV_FILE && saved_map[2].grid_field == AZ_STATUS_GRID_SAVED);
    /* A committed DB gap still reports lost history, but independent ANLZ
     * watchers prove that these original physical map inputs did not change. */
    unsigned db_loss = loss[2];
    sql(writer, "UPDATE key SET name='unrelated map history'");
    ss_monitor_deck(2);
    ss_work();
    assert(loss[2] > db_loss && ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_READY);
    /* A temporary failed DB read has no file image. Preserve the original
     * input witness through withdrawal, then bind only to the recovered exact
     * file identity; the failure must not itself manufacture a replacement. */
    real_step = g_sdk_sq.step;
    g_sdk_sq.step = map_busy_once;
    ss_monitor_deck(2);
    ss_work();
    assert(g_sdk_sq.step == real_step && ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_STALE);
    ss_monitor_deck(2);
    ss_work();
    assert(ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_READY);
    uint64_t other[3];
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    claims[0].digest[0] ^= 1;
    original_inputs_opened(id, 3);
    cb->complete(id, claims, 2, other);
    assert(!other[0] && !other[1]);
    claims[0].digest[0] ^= 1;
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    claims[0] = map_claim(grid + 12, 13); /* no native framing admits this slice */
    original_inputs_opened(id, 3);
    cb->complete(id, claims, 2, other);
    assert(!other[0] && !other[1]);
    claims[0] = map_claim(grid + 12, 14);
#ifdef __linux__
    /* A positively owned grid translation preserves the historical pairing.
     * The new saved target revision must follow its actual GRID publication. */
    unsigned before = loss[2];
    struct sw_batch offset = {.lane = SS_SAVED_GRID};
    sw_start(&offset, root, strlen(root), audio, n, analysis, 6);
    assert(offset.count == 1);
    grid[31] = 6;
    FILE *f = fopen(dat, "r+b");
    assert(f && fwrite(grid, 1, 68, f) == 68 && !fclose(f));
    sw_finish(&offset);
    ss_monitor_deck(2);
    ss_work();
    assert(loss[2] == before && ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_READY);
    assert(saved_map[2].revision == ss_decks[2].revision[AZ_STATUS_GRID]);
    /* A covered cue-only commit keeps both map atom identities. */
    uint64_t edition = saved_map[2].revision;
    struct sw_batch cues = {0};
    sw_start(&cues, root, strlen(root), audio, n, analysis, 6);
    assert(cues.count == 1);
    memcpy(grid + 68, "PCOB", 4);
    grid[75] = grid[79] = 24;
    grid[11] = 92;
    f = fopen(dat, "r+b");
    assert(f && fwrite(grid, 1, 92, f) == 92 && !fclose(f));
    sw_finish(&cues);
    ss_monitor_deck(2);
    ss_work();
    assert(loss[2] == before && ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_READY &&
           saved_map[2].revision == edition);
    sql(writer, "UPDATE key SET name='after certified cue edit'");
    ss_monitor_deck(2);
    ss_work();
    assert(loss[2] > before && ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_READY);
#endif
    /* An unrelated PSSI replacement revokes only its original tag token.
     * A later genuine pair may still use the original untouched grid atom. */
    phrase[39] = 4;
    replace(ext, phrase, sizeof phrase);
    ss_monitor_deck(2);
    ss_work();
    assert(ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_STALE);
    claims[1] = map_claim(phrase + 12, 12);
    id = cb->begin(root, strlen(root), audio, n, analysis, 6);
    assert(id);
    original_inputs_opened(id, 3);
    cb->complete(id, &claims[1], 1, other);
    assert(other[0]);
    cb->hold(other, 1, 1);
    pair[2] = other[0];
    azfile_map_pair(2, 60, pair);
    ss_work();
    assert(ready[2][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED_GRID] == AZ_AVAIL_READY);
    sdk_status_store_binding(2, 0, NULL, 5);
    ss_monitor_deck(2);
    watch(2, 0);
    assert(!fm_pairs[2].epoch);
    cb->hold(token, 2, 0);
    cb->hold(other, 1, 0);
    sql(writer, "UPDATE key SET name='stick A'");
}
int main(int argc, char **argv) {
    atomic_store(&ds_proof_enabled, 1);
    assert(argc == 2);
    integer_source_clock_cases();
    struct ss_image before_duration = {0}, after_duration = {0};
    before_duration.metadata.valid = after_duration.metadata.valid = AZ_META_DURATION;
    before_duration.duration_ticks = after_duration.duration_ticks = 0;
    after_duration.metadata.duration_s = -0.0;
    assert(!ss_metadata_changes(&before_duration, &after_duration));
    assert(ss_image_same(&before_duration, &after_duration));
    before_duration.duration_ticks = after_duration.duration_ticks = 178;
    before_duration.metadata.duration_s = 178;
    after_duration.metadata.duration_s = 178.5;
    assert(!ss_metadata_changes(&before_duration, &after_duration));
    assert(ss_image_same(&before_duration, &after_duration));
    ++after_duration.duration_ticks; /* Exactly one library source unit is distinct. */
    assert(ss_metadata_changes(&before_duration, &after_duration) == (SS_META | SS_TOPIC(OVERVIEW)));
    assert(!ss_image_same(&before_duration, &after_duration));
    after_duration = before_duration;
    after_duration.metadata.valid = 0;
    assert(ss_metadata_changes(&before_duration, &after_duration) == (SS_META | SS_TOPIC(OVERVIEW)));
    after_duration = before_duration;
    strcpy(after_duration.metadata.title, "changed");
    assert(ss_metadata_changes(&before_duration, &after_duration) == SS_META);
    after_duration = before_duration;
    ++after_duration.file[0].inode;
    assert(!ss_image_same(&before_duration, &after_duration));
    puts("PASS metadata duration source ticks, signed zero, resolution boundary and overview invalidation");
    char real_root[PATH_MAX];
    assert(realpath(argv[1], real_root));
    snprintf(g_media, sizeof g_media, "%s/media", real_root);
    snprintf(g_sysblock, sizeof g_sysblock, "%s/sys", real_root);
    snprintf(g_mounts, sizeof g_mounts, "%s/mounts", real_root);
    snprintf(st_test_mountinfo, sizeof st_test_mountinfo, "%s/mountinfo", real_root);
    assert(!mkdir(g_media, 0700) && !mkdir(g_sysblock, 0700));
    make_stick("sdb1", "A");
    plug("sdb1", "1-1", "sdb1");
    make_stick("sda1", "B");
    plug("sda1", "4-1", "sda1");
    mountinfo(real_root, 100);
    assert(sdk_sq_resolve());
    g_sdk_sq.key = st_test_database_key;
    struct st_source sources[2];
    sqlite3 *writers[2];
    char dat[1024], ext[1024];
    uint8_t grid[86] = {
        [0] = 'P', [1] = 'M', [2] = 'A', [3] = 'I', [7] = 12, [11] = 86, [12] = 'P', [13] = 'Q', [14] = 'T', [15] = 'Z', [19] = 24, [23] = 40, [35] = 2, [37] = 1, [38] = 0x2e, [39] = 0xe0, [45] = 2, [46] = 0x2e, [47] = 0xe0, [50] = 1, [51] = 0xf4, [52] = 'P', [53] = 'W', [54] = 'A', [55] = 'V', [59] = 20, [63] = 22, [67] = 2, [72] = 23, [73] = 102, [74] = 'X', [75] = 'X', [76] = 'X', [77] = 'X', [81] = 12, [85] = 12};
    uint8_t phrase[68] = {
        [0] = 'P', [1] = 'M', [2] = 'A', [3] = 'I', [7] = 12, [11] = 68, [12] = 'P', [13] = 'S', [14] = 'S', [15] = 'I', [19] = 32, [23] = 56, [27] = 24, [29] = 1, [31] = 2, [39] = 3, [47] = 1, [49] = 7};
    for (int i = 0; i < 2; i++) {
        assert(!st_source_read(i, &sources[i]));
        sources[i].ref.track_id = 10;
        char db[1024];
        assert(st_source_path(sources[i].mount, "/PIONEER/rekordbox/exportLibrary.db", db));
        assert(!sqlite3_open(db, &writers[i]));
        sql(writers[i], "ALTER TABLE content ADD COLUMN analysisDataFilePath TEXT");
        if (!i) sql(writers[i], "ALTER TABLE content ADD COLUMN fileType INTEGER; UPDATE content SET fileType=4");
        sql(writers[i], "UPDATE content SET analysisDataFilePath='/a.DAT' WHERE content_id=10");
        sql(writers[i], i ? "UPDATE key SET name='stick B'" : "UPDATE key SET name='stick A'");
    }
    assert(st_source_path(sources[0].mount, "/a.DAT", dat));
    assert(st_source_path(sources[0].mount, "/a.EXT", ext));
    origin_cases(&sources[0], writers[0], dat, ext);
    map_origin_cases(&sources[0], writers[0], dat, ext, phrase);
    replace(dat, grid, sizeof grid);
    struct stat stat_dat;
    assert(!stat(dat, &stat_dat));
    size_t checked_n = 0;
    assert(!st_read_file_checked(dat, &checked_n, stat_dat.st_dev, stat_dat.st_ino + 1, st_stat_sig(&stat_dat)));
    phrase[29] = 2; /* malformed PSSI payload within intact framing */
    replace(ext, phrase, sizeof phrase);
    sdk_status_store_binding(0, 1, &sources[0].ref, 1);
    watch(0, SS_TOPIC(GRID) | SS_TOPIC(KEY) | SS_TOPIC(OVERVIEW) | SS_TOPIC(METADATA));
    ss_monitor_deck(0);
    ss_work();
    assert(ss_monitors[0].db && libfd_owner(ss_monitors[0].db_fd) == LIBFD_STATUS);
    assert(!strcmp(last_key[0], "stick A") && !strcmp(last_format[0], "AAC") && preview[0] == 102);
    assert(ready[0][AZ_STATUS_GRID][AZ_STATUS_GRID_SAVED] == AZ_AVAIL_READY);
    /* A firmware write has the actual accessor root plus both native file
     * IDs. Only its successfully completed bytes may recover an in-place edit. */
    char access_root[1024];
    snprintf(access_root, sizeof access_root, "%s/PIONEER", sources[0].mount);
    const char *audio = ss_decks[0].image.metadata.path + strlen(sources[0].mount);
    uint16_t au[1024], an[] = {'/', 'a', '.', 'D', 'A', 'T'};
    size_t audio_n = strlen(audio);
    for (size_t i = 0; i < audio_n; i++) au[i] = (unsigned char) audio[i];
    struct sw_batch save = {0}, wrong = {0};
    sw_start(&wrong, access_root, strlen(access_root) - 1, au, audio_n, an, 6);
    assert(!wrong.count);
    sw_start(&save, access_root, strlen(access_root), au, audio_n, an, 6);
    assert(save.count == 1);
    grid[73] = 79;
    FILE *edit = fopen(dat, "r+b");
    assert(edit && fwrite(grid, 1, sizeof grid, edit) == sizeof grid && !fclose(edit));
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_GRID][AZ_STATUS_GRID_SAVED] == AZ_AVAIL_READY && preview[0] == 102);
    sw_finish(&save);
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_GRID][AZ_STATUS_GRID_SAVED] == AZ_AVAIL_READY && preview[0] == 79);
    grid[73] = 102;
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    ss_work();
    unsigned wanted = ss_decks[0].topics;
    watch(0, wanted | SS_TOPIC(CUES));
    uint8_t memo[150] = {0};
    memcpy(memo, grid, sizeof grid);
    memo[11] = sizeof memo;
    uint8_t *section = memo + sizeof grid;
    memcpy(section, "PCOB", 4);
    section[7] = 24;
    section[11] = 64;
    section[19] = 1;
    uint8_t *point = section + 24;
    memcpy(point, "PCPT", 4);
    point[7] = 28;
    point[11] = 40;
    point[28] = 1;
    replace(dat, memo, sizeof memo);
    ss_monitor_deck(0);
    ss_work();
    watch(3, SS_TOPIC(CUES));
    sdk_status_store_binding(3, 2, &sources[0].ref, 1);
    ss_monitor_deck(3);
    ss_work();
    ss_pins[0].epoch = 1;
    ss_pins[0].token = 73;
    fake_native.flags = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_PHYSICAL;
    fake_native.physical_status = AZS_PHYSICAL_OK;
    fake_native.mount_id = sources[0].mount_id;
    fake_native.device = sources[0].device;
    fake_native.inode = sources[0].inode;
    npositions = shared_count = 0;
    unsigned saved_grid_publications = sent[0][AZ_STATUS_GRID];
    for (unsigned n = 1; n <= 2; n++) {
        struct sw_batch commit = {0};
        sw_start(&commit, access_root, strlen(access_root), au, audio_n, an, 6);
        assert(commit.count == 2);
        layered_put32(point + 32, 1000 * n);
        edit = fopen(dat, "r+b");
        assert(edit && fwrite(memo, 1, sizeof memo, edit) == sizeof memo && !fclose(edit));
        sw_finish(&commit);
    }
    assert(ss_saved_count == 2 && !npositions);
    assert(ss_saved_bytes == 2 * (sizeof memo + sizeof phrase));
    ss_monitor_deck(0);
    ss_monitor_deck(3);
    fake_busy_reads = 1;
    ss_work();
    assert(ss_saved_count == 2 && !npositions);
    assert(shared_count >= 2 && file_time_equal(shared_positions[0], 1.0) && file_time_equal(shared_positions[1], 2.0));
    ss_work();
    assert(npositions >= 2 && file_time_equal(saved_positions[0], 1.0) && file_time_equal(saved_positions[1], 2.0));
    assert(!ss_saved_count && !ss_saved_bytes);
    memset(&ss_pins[0], 0, sizeof ss_pins[0]);
#ifdef __linux__
    assert(sent[0][AZ_STATUS_GRID] == saved_grid_publications);
#else
    (void) saved_grid_publications;
#endif
    unsigned shared_cues = sent[3][AZ_STATUS_CUES], before_overview = sent[0][AZ_STATUS_OVERVIEW];
    nbeats = 0;
    for (unsigned n = 0; n < 2; n++) {
        struct sw_batch offset = {.lane = SS_SAVED_GRID};
        sw_start(&offset, access_root, strlen(access_root), au, audio_n, an, 6);
        assert(offset.count == 2);
        memo[30] = n ? 0xff : 0;
        memo[31] = n ? 0xfa : 6;
        edit = fopen(dat, "r+b");
        assert(edit && fwrite(memo, 1, sizeof memo, edit) == sizeof memo && !fclose(edit));
        sw_finish(&offset);
    }
    assert(ss_saved_count == 2 && ss_saved_bytes == 2 * sizeof memo && !nbeats);
    ss_monitor_deck(0);
    ss_monitor_deck(3);
    ss_work();
    assert(nbeats >= 2 && file_time_equal(first_beats[0], .006) && file_time_equal(first_beats[1], -.006));
#ifdef __linux__
    assert(sent[3][AZ_STATUS_CUES] == shared_cues && sent[0][AZ_STATUS_OVERVIEW] == before_overview);
#else
    (void) shared_cues;
    (void) before_overview;
#endif
#ifdef __linux__
    /* A read via an O_RDWR descriptor closes without MODIFY. A second real
     * native grid commit then lands after the first observer drain, while
     * that pass is reading SQLite. Both named commits must stay GRID-only. */
    unsigned before_race = loss[0];
    shared_cues = sent[3][AZ_STATUS_CUES];
    before_overview = sent[0][AZ_STATUS_OVERVIEW];
    struct sw_batch first = {.lane = SS_SAVED_GRID};
    sw_start(&first, access_root, strlen(access_root), au, audio_n, an, 6);
    memo[30] = 0;
    memo[31] = 7;
    edit = fopen(dat, "r+b");
    assert(edit && fwrite(memo, 1, sizeof memo, edit) == sizeof memo && !fclose(edit));
    sw_finish(&first);
    int rw = open(dat, O_RDWR);
    assert(rw >= 0 && !close(rw));
    delayed_grid.root = access_root;
    delayed_grid.path = dat;
    delayed_grid.audio = au;
    delayed_grid.analysis = an;
    delayed_grid.n = audio_n;
    delayed_grid.bytes = memo;
    delayed_grid.size = sizeof memo;
    real_step = g_sdk_sq.step;
    g_sdk_sq.step = finish_grid_while_observing;
    ss_monitor_deck(0);
    assert(g_sdk_sq.step == real_step);
    ss_monitor_deck(0);
    ss_monitor_deck(3);
    ss_work();
    assert(loss[0] == before_race && sent[3][AZ_STATUS_CUES] == shared_cues && sent[0][AZ_STATUS_OVERVIEW] == before_overview);
    assert(file_time_equal(first_beats[nbeats - 1], .008));
    assert(revision[0][AZ_STATUS_GRID] == ss_decks[0].completed[0].revision);
    before_race = loss[0];
    rw = open(dat, O_RDWR);
    assert(rw >= 0 && !close(rw));
    ss_monitor_deck(0);
    ss_monitor_deck(3);
    ss_work();
    assert(loss[0] == before_race && sent[3][AZ_STATUS_CUES] == shared_cues && sent[0][AZ_STATUS_OVERVIEW] == before_overview);
#endif
    /* Exhaust the retained revisions with no further edit to wake recovery.
     * Nothing from before the loss may replay after the resulting baseline. */
    unsigned prior_loss = loss[0];
    loss_topics[0] = 0;
    npositions = 0;
    for (unsigned n = 1; n <= SS_SAVE_QUEUE + 1; n++) {
        struct sw_batch commit = {0};
        sw_start(&commit, access_root, strlen(access_root), au, audio_n, an, 6);
        assert(commit.count == 2);
        layered_put32(point + 32, n * 1000);
        edit = fopen(dat, "r+b");
        assert(edit && fwrite(memo, 1, sizeof memo, edit) == sizeof memo && !fclose(edit));
        sw_finish(&commit);
    }
    assert(loss[0] == prior_loss + 1 && loss_topics[0] == SS_TOPIC(CUES) && !npositions);
    assert(ss_saved_count == SS_SAVE_QUEUE && !ss_decks[0].pinned[SS_SAVED_CUES]);
    ss_monitor_deck(0);
    ss_monitor_deck(3);
    ss_work();
    assert(!ss_saved_count && !ss_saved_bytes && npositions);
    for (unsigned i = 0; i < npositions; i++) assert(file_time_equal(saved_positions[i], SS_SAVE_QUEUE + 1.0));
    watch(3, 0);
    sdk_status_store_binding(3, 0, NULL, 2);
    ss_monitor_deck(3);
    watch(0, wanted);
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    ss_work();
    unsigned grid_sent = sent[0][AZ_STATUS_GRID];
    uint64_t grid_rev = revision[0][AZ_STATUS_GRID];
    watch(0, ss_decks[0].topics | SS_TOPIC(PHRASES));
    ss_monitor_deck(0);
    ss_work();
    assert(sent[0][AZ_STATUS_GRID] == grid_sent && revision[0][AZ_STATUS_GRID] == grid_rev);
    assert(ready[0][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED] == AZ_AVAIL_FAILED);
    phrase[29] = 1;
    replace(ext, phrase, sizeof phrase);
    grid[35] = 10;
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED] == AZ_AVAIL_READY);
    assert(ready[0][AZ_STATUS_GRID][AZ_STATUS_GRID_SAVED] == AZ_AVAIL_FAILED && preview[0] == 102);
    grid[85] = 13;
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_OVERVIEW][AZ_STATUS_OVERVIEW_SAVED] == AZ_AVAIL_FAILED);
    assert(ready[0][AZ_STATUS_PHRASES][AZ_STATUS_PHRASES_SAVED] == AZ_AVAIL_READY);
    grid[85] = 12;
    grid[35] = 2;
    grid[73] = 77;
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    ss_work();
    assert(preview[0] == 77);
#ifdef __linux__
    /* Parsing is paused through two renames ending at identical contents. */
    unsigned before_gap = loss[0];
    grid[73] = 99;
    replace(dat, grid, sizeof grid);
    grid[73] = 77;
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    assert(loss[0] > before_gap && preview[0] == 77);
    ss_work();
    assert(preview[0] == 77);
    /* Force a lost notifier with identical final bytes. Its capture boundary
     * must reach the reducer before parsing or a further filesystem edit. */
    int fake[2];
    assert(!pipe(fake) && fcntl(fake[0], F_SETFL, O_NONBLOCK) == 0);
    int saved_fd = ss_monitors[0].fd;
    ss_monitors[0].fd = fake[0];
    struct inotify_event overflow = {.wd = -1, .mask = IN_Q_OVERFLOW};
    assert(write(fake[1], &overflow, sizeof overflow) == sizeof overflow);
    before_gap = loss[0];
    ss_monitor_deck(0);
    assert(loss[0] > before_gap);
    ss_monitors[0].fd = saved_fd;
    close(fake[0]);
    close(fake[1]);
    ss_work();
    assert(ready[0][AZ_STATUS_OVERVIEW][AZ_STATUS_OVERVIEW_SAVED] == AZ_AVAIL_STALE);
    replace(dat, grid, sizeof grid);
    replace(ext, phrase, sizeof phrase);
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_OVERVIEW][AZ_STATUS_OVERVIEW_SAVED] == AZ_AVAIL_READY);
#endif
    uint8_t partial_cues[60] = {
        [0] = 'P', [1] = 'M', [2] = 'A', [3] = 'I', [7] = 12, [11] = 60, [12] = 'P', [13] = 'C', [14] = 'O', [15] = 'B', [19] = 24, [23] = 24, [27] = 1, [36] = 'P', [37] = 'C', [38] = 'O', [39] = 'B', [43] = 24, [47] = 24, [55] = 1};
    watch(0, ss_decks[0].topics | SS_TOPIC(CUES));
    replace(dat, partial_cues, sizeof partial_cues);
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_CUES][AZ_STATUS_CUES_SAVED_HOT] == AZ_AVAIL_EMPTY);
    assert(ready[0][AZ_STATUS_CUES][AZ_STATUS_CUES_SAVED_MEMORY] == AZ_AVAIL_FAILED);
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    ss_work();
    sql(writers[0], "UPDATE content SET length=NULL WHERE content_id=10");
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_OVERVIEW][AZ_STATUS_OVERVIEW_SAVED] == AZ_AVAIL_UNSUPPORTED);
    sql(writers[0], "UPDATE content SET length=0 WHERE content_id=10");
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_OVERVIEW][AZ_STATUS_OVERVIEW_SAVED] == AZ_AVAIL_UNSUPPORTED);
    grid[67] = 0;
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_OVERVIEW][AZ_STATUS_OVERVIEW_SAVED] == AZ_AVAIL_EMPTY);
    grid[67] = 2;
    sql(writers[0], "UPDATE content SET length=100 WHERE content_id=10");
    replace(dat, grid, sizeof grid);
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_OVERVIEW][AZ_STATUS_OVERVIEW_SAVED] == AZ_AVAIL_READY);
    /* Native withdrawal must never re-offer metadata for the preceding epoch. */
    unsigned key_before = sent[0][AZ_STATUS_KEY];
    sdk_status_store_reseed(0, 10, SS_TOPIC(KEY));
    ss_work();
    assert(sent[0][AZ_STATUS_KEY] == key_before);
    sdk_status_store_reseed(0, 1, SS_TOPIC(KEY));
    ss_work();
    assert(sent[0][AZ_STATUS_KEY] == key_before + 1);
    sql(writers[0], "UPDATE content SET fileType=6 WHERE content_id=10");
    ss_monitor_deck(0);
    ss_work();
    assert(!strcmp(last_format[0], "ALAC"));
    sql(writers[0], "UPDATE content SET fileType=999 WHERE content_id=10");
    ss_monitor_deck(0);
    ss_work();
    assert(ready[0][AZ_STATUS_METADATA][AZ_STATUS_METADATA_FORMAT] == AZ_AVAIL_UNSUPPORTED);
    /* Same numeric content ID, independent storage, no selection by title/ID. */
    watch(1, SS_TOPIC(KEY));
    sdk_status_store_binding(1, 2, &sources[1].ref, 2);
    ss_monitor_deck(1);
    ss_work();
    assert(!strcmp(last_key[1], "stick B"));
    watch(2, SS_TOPIC(KEY));
    sdk_status_store_binding(2, 3, &sources[0].ref, 6);
    ss_monitor_deck(2);
    ss_work();
    assert(!strcmp(last_key[2], "stick A"));
    /* Out-of-order A -> B -> A bindings reject both the old epoch and late eject. */
    sdk_status_store_binding(1, 4, &sources[0].ref, 4);
    sdk_status_store_binding(1, 5, &sources[1].ref, 5);
    sdk_status_store_binding(1, 0, NULL, 4);
    ss_monitor_deck(1);
    ss_work();
    assert(last_epoch[1] == 5 && !strcmp(last_key[1], "stick B"));
    /* A durable DB activity token survives B -> A while parsing is stopped.
     * Rollback and uncommitted writes never publish a speculative row. */
    for (int wal = 0; wal < 2; wal++) {
        if (wal) {
            ss_monitor_reset(&ss_monitors[0]);
            ss_monitor_reset(&ss_monitors[2]);
            sql(writers[0], "PRAGMA journal_mode=WAL; UPDATE content SET title='WAL seed' WHERE content_id=10");
            ss_monitor_deck(0);
            ss_work();
        }
        unsigned prior = loss[0], pub = publications[0];
        sql(writers[0], "BEGIN; UPDATE key SET name='uncommitted'");
        ss_monitor_deck(0);
        ss_work();
        sql(writers[0], "ROLLBACK");
        ss_monitor_deck(0);
        ss_work();
        assert(loss[0] == prior && publications[0] == pub);
        sql(writers[0], "UPDATE key SET name='B'; UPDATE key SET name='stick A'");
        ss_monitor_deck(0);
        assert(loss[0] > prior && !strcmp(last_key[0], "stick A"));
        ss_work();
        assert(!strcmp(last_key[0], "stick A"));
        sql(writers[0], "UPDATE key SET name='visible'");
        ss_monitor_deck(0);
        ss_work();
        assert(!strcmp(last_key[0], "visible"));
        sql(writers[0], "UPDATE key SET name='stick A'");
        ss_monitor_deck(0);
        ss_work();
        assert(!strcmp(last_key[0], "stick A"));
    }
    assert(!(loss_topics[0] & SS_TOPIC(LOOP)));
    /* A confirmed missing or ambiguous committed row withdraws the reference.
     * The original pack cannot bind a later occupant of the same numeric ID. */
    for (unsigned n = 0; n < 2; n++) {
        /* Retirement must be identical with diagnostic capture disabled. */
        atomic_store(&ds_proof_enabled, n);
        az_track_ref row = sources[0].ref;
        row.track_id = 12;
        sql(writers[0], "DELETE FROM content WHERE content_id=12; INSERT INTO content(content_id,title) VALUES(12,'test')");
        sdk_status_store_binding(2, 30 + n, &row, 30 + n);
        ss_monitor_deck(2);
        ss_work();
        assert(!ss_decks[2].retired);
        ss_pins[2].epoch = 30 + n;
        ss_pins[2].token = 73;
        sql(writers[0], n ? "DELETE FROM content WHERE content_id=12" : "INSERT INTO content(content_id,title) VALUES(12,'ambiguous')");
        ss_monitor_deck(2);
        ss_work();
        assert(ss_decks[2].retired && ss_pins[2].retired && retired[2] == n + 1);
        sql(writers[0], "DELETE FROM content WHERE content_id=12; INSERT INTO content(content_id,title) VALUES(12,'replacement')");
        assert(!ss_resolve_pin(2));
    }
    atomic_store(&ds_proof_enabled, 1);
    memset(&ss_pins[2], 0, sizeof ss_pins[2]);
    mountinfo(real_root, 101);
    ss_monitor_deck(0);
    ss_work();
    assert(retired[0] == 1 && ss_decks[0].retired && !ss_decks[0].pending);
    for (int i = 0; i < 4; i++) {
        ss_monitor_reset(&ss_monitors[i]);
        sdk_status_store_binding(i, 0, NULL, 100);
        watch(i, 0);
    }
    /* Match the firmware connection's open file to exactly one current port.
     * The duplicate ID on the other stick is never a candidate by itself. */
    fake_native.flags = AZS_CURRENT | AZS_OWNER_OBSERVED | AZS_PHYSICAL;
    fake_native.mount_id = sources[1].mount_id;
    fake_native.device = sources[1].device;
    fake_native.inode = sources[1].inode;
    sdk_status_store_binding(3, 6, NULL, 101);
    watch(3, SS_TOPIC(LOAD));
    ss_native_load(3, 6, 72);
    assert(ss_resolve_pin(3) && ss_pins[3].retired && !bindings);
    ss_native_load(3, 6, 72); /* A later seed cannot resurrect a retired pack. */
    assert(!ss_resolve_pin(3));
    ss_native_load(3, 6, 73);
    fake_owner_rc = AZS_BUSY;
    assert(ss_resolve_pin(3) && !ss_pins[3].retired && !bindings);
    fake_owner_rc = AZS_OK;
    assert(ss_resolve_pin(3) && bindings == 1 && st_ref_equal(&bound_ref, &sources[1].ref));
    fake_native.flags &= ~AZS_PHYSICAL;
    assert(ss_resolve_pin(3) && retired[3] == 1 && !ss_pins[3].retired);
    fake_native.flags |= AZS_PHYSICAL;
    assert(ss_resolve_pin(3) && bindings == 2 && st_ref_equal(&bound_ref, &sources[1].ref));
    fake_owner_rc = AZS_STALE;
    assert(ss_resolve_pin(3) && retired[3] == 2);
    assert(!ss_resolve_pin(3));
    fake_owner_rc = AZS_OK;
    ss_native_load(3, 0, 0);
    watch(3, 0);
    /* Normal interest starts both workers without a ticket, tick or getter.
     * The independent observer wakes the parser again after a committed row edit. */
    sql(writers[1], "PRAGMA busy_timeout=2000; PRAGMA journal_mode=WAL; UPDATE key SET name='stick B'");
    sdk_status_store_binding(3, 7, &sources[1].ref, 102);
    sdk_status_store_interest(3, SS_TOPIC(KEY));
    wait_key(3, "stick B");
    int write_rc = sqlite3_exec(writers[1], "UPDATE key SET name='after commit'", NULL, NULL, NULL);
    if (write_rc) fprintf(stderr, "writer sqlite code %d/%d: %s\n", write_rc, sqlite3_extended_errcode(writers[1]), sqlite3_errmsg(writers[1]));
    assert(!write_rc);
    wait_key(3, "after commit");
    sdk_status_store_binding(3, 0, NULL, 103);
    sdk_status_store_interest(3, 0);
    assert(!sqlite3_close(writers[0]) && !sqlite3_close(writers[1]));
    pthread_mutex_lock(&test_mu);
    assert(proof_database && proof_other && proof_baselines);
    pthread_mutex_unlock(&test_mu);
    puts("PASS provenance: exact database-version cause, distinct file/source uncertainty and fenced baseline context");
    puts("status store: isolated DAT/EXT payloads and framing, PWAV bytes, two ports, epoch fencing, WAL, rollback and source loss passed");
    return 0;
}
