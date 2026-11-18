/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by sdk_analysis_service.c. File encodings stay inside the core. */
static void sa_typed_put16(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}
static int sdk_analysis_grid(const char *id, uint64_t job, az_analysis_grid_v1 *out,
                            az_beat *beats, uint32_t capacity) {
    int owner = sa_owner(id, "analysis_grid", CM_RA());
    if (owner < 0 || !job || !out || st_record_size(out) < sizeof *out ||
        capacity > 32768 || (capacity && !beats)) return AZ_INVALID;
    if (pthread_mutex_trylock(&sa_mutex)) return AZ_BUSY;
    struct sa_job *j = NULL;
    for (unsigned i = 0; i < 8; i++)
        if (sa_jobs[i].id == job && sa_jobs[i].owner == owner) j = &sa_jobs[i];
    int rc = AZ_STALE, failed = 0;
    uint8_t *file = NULL;
    size_t file_size = 0;
    if (!j || !sa_current(j)) goto done;
    const uint8_t *grid = j->native_grid;
    uint32_t size = (uint32_t) j->native_grid_bytes;
    if (!grid) {
        file = sa_read(j->dat, &file_size, &failed);
        if (failed) { rc = AZ_FAILED; goto done; }
        grid = sa_find_tag(file, file_size, "PQTZ", &size);
    }
    if (!grid) { rc = AZ_NOT_FOUND; goto done; }
    if (size < 24 || memcmp(grid, "PQTZ", 4)) { rc = AZ_FAILED; goto done; }
    uint32_t header = sa_u32(grid + 4), count = sa_u32(grid + 20);
    if (header < 24 || header > size || !count || count > 32768 ||
        count > (size - header) / 8) { rc = AZ_FAILED; goto done; }
    /* Validate the complete source before touching caller output. */
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *row = grid + header + i * 8;
        if (sa_u16(row) < 1 || sa_u16(row) > 4 || !sa_u16(row + 2)) {
            rc = AZ_FAILED; goto done;
        }
    }
    if (!sa_current(j)) goto done;
    az_analysis_grid_v1 result = {.size = out->size, .version = AZ_DATA_VERSION,
                                 .beat_count = count};
    if (capacity < count) { *out = result; rc = AZ_LIMIT; goto done; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *row = grid + header + i * 8;
        beats[i] = (az_beat){.time_s = (double) sa_u32(row + 4) * 0.001,
                            .bpm = (double) sa_u16(row + 2) * 0.01,
                            .beat_in_bar = sa_u16(row)};
    }
    *out = result;
    rc = AZ_OK;
done:
    free(file);
    pthread_mutex_unlock(&sa_mutex);
    return rc;
}
static uint8_t *sa_encode_result(const az_analysis_result_v1 *r, uint32_t *size) {
    *size = 0;
    if (!r || st_record_size(r) < sizeof *r || r->version != AZ_DATA_VERSION ||
        r->reserved || r->reserved2 || (r->source_rate != 44100 && r->source_rate != 48000) ||
        !r->source_frames || r->source_frames > (uint64_t) r->source_rate * 1200 ||
        r->beat_count > 32768 || r->phrases.count > 1024 || r->vocals.count > SV_FRAMES_MAX ||
        r->phrases.reserved || (!r->phrases.count && !r->vocals.count)) return NULL;
    if ((r->beat_count ? !r->beats : r->beats != NULL) ||
        (r->phrases.count ? !r->phrases.entries : r->phrases.entries != NULL) ||
        (r->vocals.count ? !r->vocals.frames : r->vocals.frames != NULL)) return NULL;
    if (!r->phrases.count && (r->beat_count || r->phrases.mood || r->phrases.end_beat)) return NULL;
    if (!r->vocals.count && (r->vocals.sample_rate || r->vocals.hop_samples || r->vocals.method)) return NULL;
    if (r->phrases.count && (r->phrases.mood < 1 || r->phrases.mood > 3 ||
                            !r->phrases.end_beat || r->phrases.end_beat > UINT16_MAX)) return NULL;
    if (r->vocals.count && (r->vocals.method != AZ_VOCAL_LSTM ||
        r->vocals.sample_rate != 22050 || r->vocals.hop_samples != 1024 || r->vocals.count < 9 ||
        r->vocals.count != sv_expected_count((uint32_t) r->source_frames, r->source_rate,
                                            22050, 1024, AZ_VOCAL_LSTM))) return NULL;
    for (uint32_t i = 0; i < r->beat_count; i++) {
        const az_beat *beat = &r->beats[i];
        if (!isfinite(beat->time_s) || beat->time_s < 0 ||
            beat->time_s * 1000 + 0.5 > UINT32_MAX || !isfinite(beat->bpm) ||
            round(beat->bpm * 100) < 1 || round(beat->bpm * 100) > UINT16_MAX ||
            beat->beat_in_bar < 1 || beat->beat_in_bar > 4 || beat->reserved) return NULL;
        if (i) {
            int order = compare_timestamps(beat->time_s, r->beats[i - 1].time_s, 1, r->source_rate, 1);
            if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return NULL;
            uint64_t current_ms = (uint64_t) (beat->time_s * 1000 + 0.5);
            uint64_t previous_ms = (uint64_t) (r->beats[i - 1].time_s * 1000 + 0.5);
            order = compare_timestamps(current_ms, previous_ms, 1000, r->source_rate, 1);
            if (order == AZ_TIME_INVALID || order != AZ_TIME_AFTER) return NULL;
        }
    }
    for (uint32_t i = 0; i < r->phrases.count; i++) {
        const az_analysis_phrase_v1 *p = &r->phrases.entries[i];
        if (!p->start_beat || p->start_beat >= r->phrases.end_beat ||
            (i && p->start_beat <= r->phrases.entries[i - 1].start_beat) ||
            p->kind > UINT16_MAX || p->beat2 > UINT16_MAX || p->beat3 > UINT16_MAX ||
            p->beat4 > UINT16_MAX || p->fill_start_beat > UINT16_MAX || p->variant1 > UINT8_MAX ||
            p->variant2 > UINT8_MAX || p->variant3 > UINT8_MAX || p->structure > UINT8_MAX ||
            p->fill > UINT8_MAX) return NULL;
    }
    for (uint32_t i = 0; i < r->vocals.count; i++) if (r->vocals.frames[i].activity > 4) return NULL;
    uint32_t gn = r->beat_count ? 24 + r->beat_count * 8 : 0;
    uint32_t pn = r->phrases.count ? 32 + r->phrases.count * 24 : 0;
    uint32_t vn = r->vocals.count ? 24 + r->vocals.count : 0;
    uint32_t length = 28 + gn + pn + vn;
    *size = length;
    uint8_t *bytes = calloc(length, 1);
    if (!bytes) return NULL;
    memcpy(bytes, "TA03", 4);
    sa_put32(bytes + 4, gn); sa_put32(bytes + 8, pn); sa_put32(bytes + 12, vn);
    sa_put32(bytes + 16, r->source_rate); sa_put32(bytes + 20, (uint32_t) r->source_frames);
    sa_put32(bytes + 24, r->vocals.method);
    if (gn) {
        uint8_t *tag = bytes + 28;
        memcpy(tag, "PQTZ", 4); sa_put32(tag + 4, 24); sa_put32(tag + 8, gn);
        sa_put32(tag + 16, 0x80000); sa_put32(tag + 20, r->beat_count);
        for (uint32_t i = 0; i < r->beat_count; i++) {
            uint8_t *row = tag + 24 + i * 8;
            sa_typed_put16(row, r->beats[i].beat_in_bar);
            sa_typed_put16(row + 2, (uint32_t) round(r->beats[i].bpm * 100));
            sa_put32(row + 4, (uint32_t) (r->beats[i].time_s * 1000 + 0.5));
        }
    }
    if (pn) {
        uint8_t *tag = bytes + 28 + gn;
        memcpy(tag, "PSSI", 4); sa_put32(tag + 4, 32); sa_put32(tag + 8, pn);
        sa_typed_put16(tag + 14, 24); sa_typed_put16(tag + 16, r->phrases.count);
        sa_typed_put16(tag + 18, r->phrases.mood); sa_typed_put16(tag + 26, r->phrases.end_beat);
        for (uint32_t i = 0; i < r->phrases.count; i++) {
            uint8_t *row = tag + 32 + i * 24;
            const az_analysis_phrase_v1 *p = &r->phrases.entries[i];
            sa_typed_put16(row, i + 1); sa_typed_put16(row + 2, p->start_beat); sa_typed_put16(row + 4, p->kind);
            row[7] = p->variant1; row[9] = p->variant2; row[11] = p->structure;
            sa_typed_put16(row + 12, p->beat2); sa_typed_put16(row + 14, p->beat3); sa_typed_put16(row + 16, p->beat4);
            row[19] = p->variant3; row[21] = p->fill; sa_typed_put16(row + 22, p->fill_start_beat);
        }
    }
    if (vn) {
        uint8_t *tag = bytes + 28 + gn + pn;
        memcpy(tag, "PVDI", 4); sa_put32(tag + 4, 24); sa_put32(tag + 8, vn);
        sa_typed_put16(tag + 14, r->vocals.hop_samples); sa_typed_put16(tag + 16, r->vocals.sample_rate);
        sa_typed_put16(tag + 18, 1); sa_put32(tag + 20, r->vocals.count);
        for (uint32_t i = 0; i < r->vocals.count; i++) tag[24 + i] = r->vocals.frames[i].activity;
    }
    *size = length;
    return bytes;
}
static int sdk_analysis_commit_result(const char *id, uint64_t job, const az_analysis_result_v1 *result) {
    int owner = sa_owner(id, "analysis_commit_result", CM_RA());
    if (owner < 0 || !job) return AZ_INVALID;
    uint32_t size = 0;
    uint8_t *bytes = sa_encode_result(result, &size);
    if (!bytes) return size ? AZ_LIMIT : AZ_INVALID;
    int rc = sa_commit(owner, job, bytes, size);
    free(bytes);
    return rc;
}
