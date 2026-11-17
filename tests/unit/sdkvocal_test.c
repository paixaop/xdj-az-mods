/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdk_vocals.h"
#include <assert.h>
#include <stdio.h>
static void time_is(double got, double expected, unsigned rate) {
    int order = compare_timestamps(got, expected, 1, rate, 1);
    assert(order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL);
}
int main(void) {
    uint8_t tag[32] = {0};
    memcpy(tag, "PVDI", 4); sa_put32(tag + 4, 24); sa_put32(tag + 8, sizeof tag);
    sa_put32(tag + 12, 1024); tag[16] = 0x56; tag[17] = 0x22; tag[19] = 1; sa_put32(tag + 20, 8);
    const uint8_t confidence[8] = {0, 3, 4, 2, 0, 4, 4, 4}; memcpy(tag + 24, confidence, 8);
    size_t n = 0; uint8_t *file = sa_merge(NULL, 0, tag, sizeof tag, &n); assert(file);
    az_vocal_snapshot_v1 v = {0}; az_vocal_interval rows[4] = {{123, 456}};
    assert(sv_file(file, n, &v, NULL, 0) == AZ_LIMIT && v.interval_count == 2);
    assert(sv_file(file, n, &v, rows, 1) == AZ_LIMIT); time_is(rows[0].start_s, 123, 22050);
    assert(sv_file(file, n, &v, rows, 4) == AZ_OK && v.method == AZ_VOCAL_PVDI);
    time_is(rows[0].start_s, 1024.0 / 22050, 22050); time_is(rows[0].stop_s, 3072.0 / 22050, 22050);
    time_is(rows[1].start_s, 5120.0 / 22050, 22050); time_is(rows[1].stop_s, 8192.0 / 22050, 22050);
    /* No hardcoded PVDI sample rate. */
    file[28 + 16] = 0xbb; file[28 + 17] = 0x80;
    assert(sv_file(file, n, &v, rows, 4) == AZ_OK); time_is(rows[0].start_s, 1024.0 / 48000, 48000);
    file[28 + 16] = 0x56; file[28 + 17] = 0x22;
    uint8_t marker[32] = {0}; memcpy(marker, "AZVD", 4); sa_put32(marker + 4, 32); sa_put32(marker + 8, 32);
    sa_put32(marker + 12, 1); sa_put32(marker + 16, AZ_VOCAL_SPECTRAL); sa_put32(marker + 20, sv_hash(tag, sizeof tag));
    sa_put32(marker + 24, 15000); sa_put32(marker + 28, 44100);
    size_t mn; uint8_t *marked = sa_merge(file, n, marker, sizeof marker, &mn); assert(marked);
    assert(sv_file(marked, mn, &v, rows, 4) == AZ_OK && v.method == AZ_VOCAL_SPECTRAL);
    time_is(rows[1].stop_s, 15000.0 / 44100, 44100); free(marked);
    /* A populated all-zero timeline is completed instrumental analysis. */
    memset(file + 28 + 24, 0, 8);
    assert(sv_file(file, n, &v, NULL, 0) == AZ_OK && !v.interval_count && v.info.availability == AZ_AVAIL_EMPTY);
    assert(sa_tag_count(file, n, "PVDI") == 8);
    file[28 + 19] = 2; assert(sv_file(file, n, &v, rows, 4) == AZ_UNSUPPORTED);
    file[28 + 19] = 1; file[28 + 24] = 5; assert(sv_file(file, n, &v, rows, 4) == AZ_FAILED);
    file[28 + 24] = 0; sa_put32(file + 28 + 20, 9); assert(sv_file(file, n, &v, rows, 4) == AZ_FAILED);
    sa_put32(file + 28 + 20, 8); sa_put32(file + 28 + 12, 0); assert(sv_file(file, n, &v, rows, 4) == AZ_FAILED);
    free(file);
    assert(sv_file(NULL, 0, &v, rows, 4) == AZ_NOT_FOUND);
    sa_put32(tag + 8, 24); sa_put32(tag + 20, 0);
    file = sa_merge(NULL, 0, tag, 24, &n); assert(file);
    assert(sv_file(file, n, &v, rows, 4) == AZ_NOT_FOUND); free(file);
    puts("PVDI intervals: timing, capacity, instrumental, provenance and malformed data passed");
    return 0;
}
