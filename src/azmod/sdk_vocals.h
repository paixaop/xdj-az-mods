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
/* PVDI v1: hop:u32, rate:u16, version:u16, count:u32, confidence[count].
 * This is a quantized detector score, not a probability or waveform. */
static inline int sv_tag(const uint8_t *p, size_t n, az_vocal_snapshot_v1 *v) {
    if (!p || n < 24 || memcmp(p, "PVDI", 4) || sa_u32(p + 4) != 24 || sa_u32(p + 8) != n) return AZ_FAILED;
    if (sa_u16(p + 18) != 1) return AZ_UNSUPPORTED;
    uint32_t hop = sa_u32(p + 12), rate = sa_u16(p + 16), count = sa_u32(p + 20);
    if (!hop || !rate || hop > rate || count > SV_FRAMES_MAX || count != n - 24) return AZ_FAILED;
    for (uint32_t i = 0; i < count; i++) if (p[24 + i] > 4) return AZ_FAILED;
    v->frame_count = count; v->hop_samples = hop; v->sample_rate = rate; v->threshold = 3;
    v->analyzed_s = (double) count * hop / rate;
    return count ? AZ_OK : AZ_NOT_FOUND;
}
/* Validates the entire container and refuses ambiguous duplicate tags. */
static inline int sv_file(const uint8_t *file, size_t n, az_vocal_snapshot_v1 *v,
                           az_vocal_interval *intervals, uint32_t capacity) {
    if (!v || (!intervals && capacity)) return AZ_INVALID;
    if (!sa_framed(file, n)) return AZ_FAILED;
    if (!file) return AZ_NOT_FOUND;
    const uint8_t *tag = NULL, *marker = NULL;
    uint32_t length = 0;
    for (size_t at = sa_u32(file + 4); at < n; at += sa_u32(file + at + 8)) {
        if (!memcmp(file + at, "PVDI", 4)) {
            if (tag) return AZ_FAILED;
            tag = file + at; length = sa_u32(tag + 8);
        }
        if (!memcmp(file + at, "AZVD", 4)) {
            if (marker || sa_u32(file + at + 4) != 32 || sa_u32(file + at + 8) != 32) return AZ_FAILED;
            marker = file + at;
        }
    }
    if (!tag) return AZ_NOT_FOUND;
    int rc = sv_tag(tag, length, v);
    if (rc) return rc;
    v->method = AZ_VOCAL_PVDI;
    if (marker && sa_u32(marker + 12) == 1 && sa_u32(marker + 16) == AZ_VOCAL_SPECTRAL &&
        sa_u32(marker + 20) == sv_hash(tag, length)) {
        uint32_t frames = sa_u32(marker + 24), rate = sa_u32(marker + 28);
        if (!frames || !rate || v->frame_count !=
            ((uint64_t) frames * v->sample_rate + (uint64_t) rate * v->hop_samples - 1) /
            ((uint64_t) rate * v->hop_samples)) return AZ_FAILED;
        double duration = (double) frames / rate;
        int order = compare_timestamps(duration, v->analyzed_s, 1, rate, 1);
        /* Generated coverage is rounded up by at most one PVDI frame. */
        if (order == AZ_TIME_INVALID || order == AZ_TIME_AFTER ||
            v->analyzed_s - duration > (double) v->hop_samples / v->sample_rate) return AZ_FAILED;
        v->analyzed_s = duration; v->method = AZ_VOCAL_SPECTRAL;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < v->frame_count; i++)
        if (tag[24 + i] >= v->threshold && (!i || tag[23 + i] < v->threshold)) count++;
    v->interval_count = count;
    v->info.availability = count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    v->info.provenance = AZ_PROV_FILE;
    if (count > capacity) return AZ_LIMIT;
    uint32_t start = 0, dest = 0;
    for (uint32_t i = 0; i <= v->frame_count; i++) {
        int active = i < v->frame_count && tag[24 + i] >= v->threshold;
        int previous = i && tag[23 + i] >= v->threshold;
        if (active && !previous) start = i;
        if (!active && previous) {
            double stop = (double) i * v->hop_samples / v->sample_rate;
            int order = compare_timestamps(stop, v->analyzed_s, 1, v->sample_rate, 1);
            if (order == AZ_TIME_INVALID) return AZ_FAILED;
            if (order == AZ_TIME_AFTER) stop = v->analyzed_s;
            intervals[dest++] = (az_vocal_interval) {(double) start * v->hop_samples / v->sample_rate, stop};
        }
    }
    return AZ_OK;
}
#endif
