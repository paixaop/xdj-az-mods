/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SDK_VOCALS_H
#define AZMOD_SDK_VOCALS_H
#include "azmod-mod.h"
#include "sdk_analysis_write.h"
#include "../common/aztimestamp.h"
#define SV_FRAMES_MAX 262144u
static inline uint32_t sv_hash(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static inline uint64_t sv_expected_count(uint32_t frames, uint32_t source_rate,
                                         uint32_t rate, uint32_t hop, uint32_t method) {
    if (!source_rate || !rate || !hop) return 0;
    if (method == AZ_VOCAL_LSTM) return (((uint64_t) frames * rate / source_rate) + hop - 1) / hop;
    return ((uint64_t) frames * rate + (uint64_t) source_rate * hop - 1) / ((uint64_t) source_rate * hop);
}
/* PVDI: version/flags:u16, hop:u16, rate:u16, stride:u16, count:u32, confidence[count].
 * This is a quantized detector score, not a probability or waveform. */
static inline int sv_tag_header(const uint8_t *p, size_t n) {
    return p && n >= 24 && !memcmp(p, "PVDI", 4) && sa_u32(p + 4) == 24 && sa_u32(p + 8) == n;
}
static inline int sv_timeline_shape(uint32_t hop, uint32_t rate, uint32_t count, size_t size) {
    return hop && rate && hop <= rate && count <= SV_FRAMES_MAX && count == size - 24;
}
static inline int sv_tag(const uint8_t *p, size_t n, az_vocal_snapshot_v1 *v) {
    if (!sv_tag_header(p, n)) return AZ_FAILED;
    if (sa_u16(p + 12) != 0 || sa_u16(p + 18) != 1) return AZ_UNSUPPORTED;
    uint32_t hop = sa_u16(p + 14), rate = sa_u16(p + 16), count = sa_u32(p + 20);
    if (!sv_timeline_shape(hop, rate, count, n)) return AZ_FAILED;
    if (!sa_vocal_scores_valid(p + 24, count)) return AZ_FAILED;
    v->frame_count = count;
    v->hop_samples = hop;
    v->sample_rate = rate;
    v->threshold = AZ_ANALYSIS_POLICY_SAVED_VOCAL_THRESHOLD;
    v->analyzed_s = (double) count * hop / rate;
    return count ? AZ_OK : AZ_NOT_FOUND;
}
struct sv_tags {
    const uint8_t *vocal, *marker;
};
static inline int sv_select_tag(struct sv_tags *tags, const uint8_t *tag) {
    if (!memcmp(tag, "PVDI", 4)) {
        if (tags->vocal) return AZ_FAILED;
        tags->vocal = tag;
    }
    if (!memcmp(tag, "AZVD", 4)) {
        if (tags->marker || sa_u32(tag + 4) != 32 || sa_u32(tag + 8) != 32) return AZ_FAILED;
        tags->marker = tag;
    }
    return AZ_OK;
}
static inline int sv_find_tags(const uint8_t *file, size_t n, struct sv_tags *tags) {
    for (size_t at = sa_u32(file + 4); at < n; at += sa_u32(file + at + 8)) {
        int rc = sv_select_tag(tags, file + at);
        if (rc) return rc;
    }
    return tags->vocal ? AZ_OK : AZ_NOT_FOUND;
}
static inline int sv_marker_matches(const struct sv_tags *tags) {
    const uint8_t *marker = tags->marker;
    return marker && sa_u32(marker + 12) == 1 &&
           (sa_u32(marker + 16) == AZ_VOCAL_SPECTRAL || sa_u32(marker + 16) == AZ_VOCAL_LSTM) &&
           sa_u32(marker + 20) == sv_hash(tags->vocal, sa_u32(tags->vocal + 8));
}
static inline int sv_lstm_shape(const az_vocal_snapshot_v1 *v, uint32_t method) {
    return method != AZ_VOCAL_LSTM || (v->sample_rate == 22050 && v->hop_samples == 1024 && v->frame_count >= 9);
}
static inline int sv_provenance(const struct sv_tags *tags, az_vocal_snapshot_v1 *v) {
    v->method = AZ_VOCAL_PVDI;
    if (!sv_marker_matches(tags)) return AZ_OK;
    const uint8_t *marker = tags->marker;
    uint32_t frames = sa_u32(marker + 24), rate = sa_u32(marker + 28), method = sa_u32(marker + 16);
    if (!frames || !rate || v->frame_count != sv_expected_count(frames, rate, v->sample_rate, v->hop_samples, method) ||
        !sv_lstm_shape(v, method)) return AZ_FAILED;
    v->analyzed_s = (double) frames / rate;
    v->method = method;
    return AZ_OK;
}
static inline uint32_t sv_interval_count(const uint8_t *scores, const az_vocal_snapshot_v1 *v) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < v->frame_count; i++)
        if (scores[i] >= v->threshold && (!i || scores[i - 1] < v->threshold)) count++;
    return count;
}
static inline int sv_interval(const az_vocal_snapshot_v1 *v, uint32_t start, uint32_t end, az_vocal_interval *out) {
    double stop = (double) end * v->hop_samples / v->sample_rate;
    int order = compare_timestamps(stop, v->analyzed_s, 1, v->sample_rate,
                                   AZ_ANALYSIS_POLICY_SAVED_VOCAL_TOLERANCE_SAMPLES);
    if (order == AZ_TIME_INVALID) return AZ_FAILED;
    if (order == AZ_TIME_AFTER && AZ_ANALYSIS_POLICY_CLIP_SAVED_VOCAL_END)
        stop = v->analyzed_s;
    *out = (az_vocal_interval) {(double) start * v->hop_samples / v->sample_rate, stop};
    return AZ_OK;
}
static inline int sv_intervals(const uint8_t *scores, const az_vocal_snapshot_v1 *v, az_vocal_interval *intervals) {
    uint32_t start = 0, dest = 0;
    for (uint32_t i = 0; i <= v->frame_count; i++) {
        int active = i < v->frame_count && scores[i] >= v->threshold;
        int previous = i && scores[i - 1] >= v->threshold;
        if (active && !previous) start = i;
        if (!active && previous) {
            int rc = sv_interval(v, start, i, &intervals[dest++]);
            if (rc) return rc;
        }
    }
    return AZ_OK;
}
static inline int sv_read_timeline(const struct sv_tags *tags, az_vocal_snapshot_v1 *v,
                                   az_vocal_interval *intervals, uint32_t capacity) {
    int rc = sv_tag(tags->vocal, sa_u32(tags->vocal + 8), v);
    if (rc) return rc;
    rc = sv_provenance(tags, v);
    if (rc) return rc;
    v->interval_count = sv_interval_count(tags->vocal + 24, v);
    v->info.availability = v->interval_count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    v->info.provenance = AZ_PROV_FILE;
    if (v->interval_count > capacity) return AZ_LIMIT;
    return sv_intervals(tags->vocal + 24, v, intervals);
}
/* Validates the entire container and refuses ambiguous duplicate tags. */
static inline int sv_file(const uint8_t *file, size_t n, az_vocal_snapshot_v1 *v,
                          az_vocal_interval *intervals, uint32_t capacity) {
    if (!v || (!intervals && capacity)) return AZ_INVALID;
    if (!sa_framed(file, n)) return AZ_FAILED;
    if (!file) return AZ_NOT_FOUND;
    struct sv_tags tags = {0};
    int rc = sv_find_tags(file, n, &tags);
    return rc ? rc : sv_read_timeline(&tags, v, intervals, capacity);
}
#endif
