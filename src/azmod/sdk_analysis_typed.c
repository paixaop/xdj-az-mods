/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_analysis_service.c. File encodings stay inside the core. */
static void sa_typed_put16(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}
struct sa_grid_view {
    const uint8_t *bytes;
    uint8_t *file;
    uint32_t size, header, count;
};
static int sa_grid_request_valid(int owner, uint64_t job, const analysis_grid_shape *out,
                                 const az_beat *beats, uint32_t capacity) {
    return owner >= 0 && job && out && st_record_size(out) >= sizeof *out &&
           capacity <= 32768 && (!capacity || beats);
}
static int sa_grid_header_valid(struct sa_grid_view *grid) {
    if (grid->size < 24 || memcmp(grid->bytes, "PQTZ", 4)) return 0;
    grid->header = sa_u32(grid->bytes + 4);
    grid->count = sa_u32(grid->bytes + 20);
    return grid->header >= 24 && grid->header <= grid->size && grid->count && grid->count <= 32768 &&
           grid->count <= (grid->size - grid->header) / 8;
}
static int sa_grid_rows_valid(const struct sa_grid_view *grid) {
    for (uint32_t i = 0; i < grid->count; i++) {
        const uint8_t *row = grid->bytes + grid->header + i * 8;
        if (sa_u16(row) < 1 || sa_u16(row) > 4 || !sa_u16(row + 2)) return 0;
    }
    return 1;
}
static int sa_grid_read(const struct sa_job *job, struct sa_grid_view *grid) {
    grid->bytes = job->native_grid;
    grid->size = (uint32_t) job->native_grid_bytes;
    if (!grid->bytes) {
        size_t file_size = 0;
        int failed = 0;
        grid->file = sa_read(job->dat, &file_size, &failed);
        if (failed) return AZ_FAILED;
        grid->bytes = sa_find_tag(grid->file, file_size, "PQTZ", &grid->size);
    }
    if (!grid->bytes) return AZ_NOT_FOUND;
    if (!sa_grid_header_valid(grid) || !sa_grid_rows_valid(grid)) return AZ_FAILED;
    return AZ_OK;
}
static int sa_grid_copy(const struct sa_grid_view *grid, analysis_grid_shape *out,
                        az_beat *beats, uint32_t capacity) {
    analysis_grid_shape result = {.size = out->size, .version = AZ_DATA_VERSION, .beat_count = grid->count};
    if (capacity < grid->count) {
        *out = result;
        return AZ_LIMIT;
    }
    for (uint32_t i = 0; i < grid->count; i++) {
        const uint8_t *row = grid->bytes + grid->header + i * 8;
        beats[i] = (az_beat) {.time_s = (double) sa_u32(row + 4) * 0.001,
                              .bpm = (double) sa_u16(row + 2) * 0.01,
                              .beat_in_bar = sa_u16(row)};
    }
    *out = result;
    return AZ_OK;
}
static int sdk_analysis_grid(const char *id, uint64_t job, analysis_grid_shape *out,
                             az_beat *beats, uint32_t capacity) {
    int owner = sa_owner(id, "analysis_grid", CM_RA());
    if (!sa_grid_request_valid(owner, job, out, beats, capacity)) return AZ_INVALID;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    struct sa_job *j = sa_find_job(owner, job);
    struct sa_grid_view grid = {0};
    int rc = AZ_STALE;
    if (!j || !sa_current(j)) goto done;
    rc = sa_grid_read(j, &grid);
    if (rc != AZ_OK) goto done;
    /* Validate the complete source and the lease before touching caller output. */
    if (!sa_current(j)) {
        rc = AZ_STALE;
        goto done;
    }
    rc = sa_grid_copy(&grid, out, beats, capacity);
done:
    free(grid.file);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static int sa_result_header_valid(const az_analysis_result_v1 *r) {
    return r && st_record_size(r) >= sizeof *r && r->version == AZ_DATA_VERSION &&
           !r->reserved && !r->reserved2;
}
static int sa_result_source_valid(const az_analysis_result_v1 *r) {
    return (r->source_rate == 44100 || r->source_rate == 48000) && r->source_frames &&
           r->source_frames <= (uint64_t) r->source_rate * 1200;
}
static int sa_result_counts_valid(const az_analysis_result_v1 *r) {
    return r->beat_count <= 32768 && r->phrases.count <= 1024 &&
           r->vocals.count <= SV_FRAMES_MAX && !r->phrases.reserved &&
           (r->phrases.count || r->vocals.count);
}
static int sa_result_buffers_valid(const az_analysis_result_v1 *r) {
    return !!r->beat_count == !!r->beats && !!r->phrases.count == !!r->phrases.entries &&
           !!r->vocals.count == !!r->vocals.frames;
}
static int sa_result_phrase_header_valid(const az_analysis_result_v1 *r) {
    if (!r->phrases.count) return !r->beat_count && !r->phrases.mood && !r->phrases.end_beat;
    return r->phrases.mood >= 1 && r->phrases.mood <= 3 &&
           r->phrases.end_beat && r->phrases.end_beat <= UINT16_MAX;
}
static int sa_result_vocal_header_valid(const az_analysis_result_v1 *r) {
    if (!r->vocals.count) return !r->vocals.sample_rate && !r->vocals.hop_samples && !r->vocals.method;
    return r->vocals.method == AZ_VOCAL_LSTM && r->vocals.sample_rate == 22050 &&
           r->vocals.hop_samples == 1024 && r->vocals.count >= 9 &&
           r->vocals.count == sv_expected_count((uint32_t) r->source_frames, r->source_rate,
                                                22050, 1024, AZ_VOCAL_LSTM);
}
static int sa_result_metadata_valid(const az_analysis_result_v1 *r) {
    return sa_result_header_valid(r) && sa_result_source_valid(r) && sa_result_counts_valid(r) &&
           sa_result_buffers_valid(r) && sa_result_phrase_header_valid(r) && sa_result_vocal_header_valid(r);
}
static int sa_result_beat_time_valid(const az_beat *beat) {
    return isfinite(beat->time_s) && beat->time_s >= 0 && beat->time_s * 1000 + 0.5 <= UINT32_MAX;
}
static int sa_result_beat_tempo_valid(const az_beat *beat) {
    return isfinite(beat->bpm) && round(beat->bpm * 100) >= 1 && round(beat->bpm * 100) <= UINT16_MAX &&
           beat->beat_in_bar >= 1 && beat->beat_in_bar <= 4 && !beat->reserved;
}
static int sa_result_beats_ordered(const az_analysis_result_v1 *r) {
    for (uint32_t i = 1; i < r->beat_count; i++) {
        int order = compare_timestamps(r->beats[i].time_s, r->beats[i - 1].time_s, 1, r->source_rate, 1);
        if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return 0;
        uint64_t current_ms = (uint64_t) (r->beats[i].time_s * 1000 + 0.5);
        uint64_t previous_ms = (uint64_t) (r->beats[i - 1].time_s * 1000 + 0.5);
        order = compare_timestamps(current_ms, previous_ms, 1000, r->source_rate, 1);
        if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return 0;
    }
    return 1;
}
static int sa_result_beats_valid(const az_analysis_result_v1 *r) {
    for (uint32_t i = 0; i < r->beat_count; i++)
        if (!sa_result_beat_time_valid(&r->beats[i]) || !sa_result_beat_tempo_valid(&r->beats[i])) return 0;
    return sa_result_beats_ordered(r);
}
static int sa_result_phrase_beats_fit(const az_analysis_phrase_v1 *p) {
    return p->kind <= UINT16_MAX && p->beat2 <= UINT16_MAX && p->beat3 <= UINT16_MAX &&
           p->beat4 <= UINT16_MAX && p->fill_start_beat <= UINT16_MAX;
}
static int sa_result_phrase_flags_fit(const az_analysis_phrase_v1 *p) {
    return p->variant1 <= UINT8_MAX && p->variant2 <= UINT8_MAX && p->variant3 <= UINT8_MAX &&
           p->structure <= UINT8_MAX && p->fill <= UINT8_MAX;
}
static int sa_result_phrases_valid(const az_analysis_result_v1 *r) {
    for (uint32_t i = 0; i < r->phrases.count; i++) {
        const az_analysis_phrase_v1 *p = &r->phrases.entries[i];
        if (!p->start_beat || p->start_beat >= r->phrases.end_beat ||
            !sa_result_phrase_beats_fit(p) || !sa_result_phrase_flags_fit(p)) return 0;
        if (i && p->start_beat <= r->phrases.entries[i - 1].start_beat) return 0;
    }
    return 1;
}
static int sa_result_vocals_valid(const az_analysis_result_v1 *r) {
    for (uint32_t i = 0; i < r->vocals.count; i++)
        if (r->vocals.frames[i].activity > 4) return 0;
    return 1;
}
static int sa_result_valid(const az_analysis_result_v1 *r) {
    return sa_result_metadata_valid(r) && sa_result_beats_valid(r) &&
           sa_result_phrases_valid(r) && sa_result_vocals_valid(r);
}
static void sa_encode_grid(const az_analysis_result_v1 *r, uint8_t *tag, uint32_t gn) {
    if (!gn) return;
    memcpy(tag, "PQTZ", 4);
    sa_put32(tag + 4, 24);
    sa_put32(tag + 8, gn);
    sa_put32(tag + 16, 0x80000);
    sa_put32(tag + 20, r->beat_count);
    for (uint32_t i = 0; i < r->beat_count; i++) {
        uint8_t *row = tag + 24 + i * 8;
        sa_typed_put16(row, r->beats[i].beat_in_bar);
        sa_typed_put16(row + 2, (uint32_t) round(r->beats[i].bpm * 100));
        sa_put32(row + 4, (uint32_t) (r->beats[i].time_s * 1000 + 0.5));
    }
}
static void sa_encode_phrases(const az_analysis_result_v1 *r, uint8_t *tag, uint32_t pn) {
    if (!pn) return;
    memcpy(tag, "PSSI", 4);
    sa_put32(tag + 4, 32);
    sa_put32(tag + 8, pn);
    sa_typed_put16(tag + 14, 24);
    sa_typed_put16(tag + 16, r->phrases.count);
    sa_typed_put16(tag + 18, r->phrases.mood);
    sa_typed_put16(tag + 26, r->phrases.end_beat);
    for (uint32_t i = 0; i < r->phrases.count; i++) {
        uint8_t *row = tag + 32 + i * 24;
        const az_analysis_phrase_v1 *p = &r->phrases.entries[i];
        sa_typed_put16(row, i + 1);
        sa_typed_put16(row + 2, p->start_beat);
        sa_typed_put16(row + 4, p->kind);
        row[7] = p->variant1;
        row[9] = p->variant2;
        row[11] = p->structure;
        sa_typed_put16(row + 12, p->beat2);
        sa_typed_put16(row + 14, p->beat3);
        sa_typed_put16(row + 16, p->beat4);
        row[19] = p->variant3;
        row[21] = p->fill;
        sa_typed_put16(row + 22, p->fill_start_beat);
    }
}
static void sa_encode_vocals(const az_analysis_result_v1 *r, uint8_t *tag, uint32_t vn) {
    if (!vn) return;
    memcpy(tag, "PVDI", 4);
    sa_put32(tag + 4, 24);
    sa_put32(tag + 8, vn);
    sa_typed_put16(tag + 14, r->vocals.hop_samples);
    sa_typed_put16(tag + 16, r->vocals.sample_rate);
    sa_typed_put16(tag + 18, 1);
    sa_put32(tag + 20, r->vocals.count);
    for (uint32_t i = 0; i < r->vocals.count; i++) tag[24 + i] = r->vocals.frames[i].activity;
}
/* Native tags are the persistence format. There is no inter-component packet:
 * the typed result is validated once, then each tag is encoded directly. */
static uint8_t *sa_encode_native(const az_analysis_result_v1 *r, struct sa_tags *tags) {
    tags->gn = r->beat_count ? 24 + r->beat_count * 8 : 0;
    tags->pn = r->phrases.count ? 32 + r->phrases.count * 24 : 0;
    tags->vn = r->vocals.count ? 24 + r->vocals.count : 0;
    tags->rate = r->source_rate;
    tags->frames = (uint32_t)r->source_frames;
    tags->method = r->vocals.method;
    uint8_t *bytes = calloc(tags->gn + tags->pn + tags->vn, 1);
    if (!bytes) return NULL;
    tags->grid = bytes;
    tags->phrase = bytes + tags->gn;
    tags->vocal = bytes + tags->gn + tags->pn;
    sa_encode_grid(r, bytes, tags->gn);
    sa_encode_phrases(r, bytes + tags->gn, tags->pn);
    sa_encode_vocals(r, bytes + tags->gn + tags->pn, tags->vn);
    return bytes;
}
static int sa_commit_typed(int owner, uint64_t job, const az_analysis_result_v1 *result) {
    if (owner < 0 || !job || !sa_result_valid(result)) return AZ_INVALID;
    struct sa_publication state = {.td = {.dir = -1}, .te = {.dir = -1}, .tv = {.dir = -1}};
    uint8_t *storage = sa_encode_native(result, &state.packet);
    if (!storage) return AZ_LIMIT;
    int rc = AZ_BUSY;
    if (!pthread_mutex_trylock(&sa_mutex)) {
        state.job = sa_find_job(owner, job);
        rc = state.job && sa_current(state.job) ? sa_publication_run(&state) : AZ_STALE;
        sa_publication_drop(&state);
        pthread_mutex_unlock(&sa_mutex);
    }
    free(storage);
    return rc;
}
static int sdk_analysis_commit_result(const char *id, uint64_t job, const az_analysis_result_v1 *result) {
    return sa_commit_typed(sa_owner(id, "analysis_commit_result", CM_RA()), job, result);
}
