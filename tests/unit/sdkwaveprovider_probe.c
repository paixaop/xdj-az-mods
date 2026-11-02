/* SPDX-License-Identifier: GPL-3.0-only */
/* Operator-supplied local EXT/2EX files only. No exported track/firmware bytes are
 * embedded here. The reference walker does not use sww_* or hc_* helpers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "../../src/azmod/sdk_analysis.h"

static unsigned checks;
static size_t read_calls, read_bytes, largest_request, short_limit;
static int inject_eintr;
static char temporary[64];
#define CHECK(x)                                                       \
    do {                                                               \
        checks++;                                                      \
        if (!(x)) {                                                    \
            fprintf(stderr, "sdkwaveprovider:%d: %s\n", __LINE__, #x); \
            exit(1);                                                   \
        }                                                              \
    } while (0)
static void cleanup(void) {
    if (temporary[0]) unlink(temporary);
}
/* Observe the real fd read path and exercise its short-read/EINTR retry logic. */
static ssize_t counted_pread(int fd, void *out, size_t count, off_t offset) {
    read_calls++;
    if (count > largest_request) largest_request = count;
    if (inject_eintr) {
        inject_eintr = 0;
        errno = EINTR;
        return -1;
    }
    if (short_limit && count > short_limit) count = short_limit;
    ssize_t n = pread(fd, out, count, offset);
    if (n > 0) read_bytes += (size_t) n;
    return n;
}
#define pread counted_pread
#include "../../src/azmod/sdk_wave_provider.h"
#undef pread

struct raw_wave {
    size_t tag_offset, data_offset, last_tag, tags;
    uint32_t encoding, stride, header_word, count, found;
};
static uint32_t raw_be32(const uint8_t *p) {
    return (uint32_t) p[0] * 16777216u + (uint32_t) p[1] * 65536u +
           (uint32_t) p[2] * 256u + p[3];
}
static void raw_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}
/* Independent full-envelope reference: field offsets are from the documented
 * PMAI/detail-tag layouts, not values computed by the provider or its parser. */
static struct raw_wave raw_walk(const uint8_t *p, size_t n, uint32_t encoding) {
    struct raw_wave w = {0};
    CHECK(encoding >= 1 && encoding <= 3);
    w.encoding = encoding;
    w.stride = encoding == 1 ? 1 : encoding == 2 ? 2
                                                 : 3;
    w.header_word = encoding == 2 ? 0x00960305u : 0x00960000u;
    const char *tag = encoding == 1 ? "PWV3" : encoding == 2 ? "PWV5"
                                                             : "PWV7";
    CHECK(n >= 12 && memcmp(p, "PMAI", 4) == 0);
    CHECK(raw_be32(p + 8) == n);
    size_t off = raw_be32(p + 4);
    CHECK(off >= 12 && off <= n);
    while (off < n) {
        CHECK(n - off >= 12);
        size_t header = raw_be32(p + off + 4), length = raw_be32(p + off + 8);
        CHECK(header >= 12 && header <= length && length <= n - off);
        w.last_tag = off;
        w.tags++;
        if (memcmp(p + off, tag, 4) == 0) {
            CHECK(++w.found == 1 && header == 24);
            CHECK(raw_be32(p + off + 12) == w.stride);
            w.count = raw_be32(p + off + 16);
            CHECK((length - header) % w.stride == 0 && w.count == (length - header) / w.stride);
            CHECK(raw_be32(p + off + 20) == w.header_word);
            w.tag_offset = off;
            w.data_offset = off + header;
        }
        off += length;
    }
    return w;
}
static void reset_reads(void) {
    read_calls = read_bytes = largest_request = 0;
    short_limit = 0;
    inject_eintr = 0;
}
static void query_init(struct st_data *data, uint32_t encoding, uint32_t first, uint32_t count) {
    st_data_init(data);
    data->wave_query = (az_wave_query) {sizeof(az_wave_query), AZ_DATA_VERSION,
                                        encoding, first, count, 0};
}
static void valid_window(const char *path, const uint8_t *raw, const struct raw_wave *w,
                         struct st_data *data, uint32_t first, uint32_t requested,
                         int fragmented) {
    CHECK(first < w->count);
    query_init(data, w->encoding, first, requested);
    memset(data->detail, 0xa5, sizeof data->detail);
    reset_reads();
    if (fragmented) {
        short_limit = 5;
        inject_eintr = 1;
    }
    CHECK(st_wave_read(path, data) == AZ_OK);
    uint32_t expected = w->count - first;
    if (expected > requested) expected = requested;
    const az_wave_snapshot *v = &data->wave_view;
    CHECK(v->size == sizeof *v && v->version == AZ_DATA_VERSION);
    CHECK(v->columns == data->detail && v->encoding == w->encoding);
    CHECK(v->first_column == first && v->column_count == expected && v->total_columns == w->count);
    size_t expected_bytes = (size_t) expected * w->stride;
    CHECK(v->bytes_per_column == w->stride && v->data_bytes == expected_bytes);
    CHECK(v->info.availability == AZ_AVAIL_READY);
    CHECK(fabs(v->start_s * 150.0 - first) < 1e-8);
    CHECK(fabs(v->seconds_per_column - 1.0 / 150.0) < 1e-15);
    for (size_t i = 0; i < expected_bytes; i++) CHECK(v->columns[i] == raw[w->data_offset + (size_t) first * w->stride + i]);
    int tail_untouched = 1;
    for (size_t i = expected_bytes; i < sizeof data->detail; i++)
        if (data->detail[i] != 0xa5) tail_untouched = 0;
    CHECK(tail_untouched);
    CHECK(data->beats[0].time_s == 0 && data->beats[0].bpm == 0);
    /* Only envelopes, one full target header and the selected byte span are read.
     * Actual bytes remain the same under forced fragmentation and one EINTR. */
    CHECK(read_bytes == 12 + w->tags * 12 + 24 + expected_bytes);
    CHECK(read_bytes <= 12 + w->tags * 12 + 24 + (size_t) w->stride * AZ_WAVE_WINDOW_MAX);
    CHECK(largest_request <= (expected_bytes > 24 ? expected_bytes : 24));
    CHECK(read_calls >= w->tags + 3);
    if (fragmented) CHECK(inject_eintr == 0 && read_calls > w->tags + 3);
}
static void failure(const char *path, struct st_data *data, uint32_t encoding, uint32_t first,
                    uint32_t count, int expected) {
    query_init(data, encoding, first, count);
    reset_reads();
    CHECK(st_wave_read(path, data) == expected);
    CHECK(data->wave_view.column_count == 0 && data->wave_view.data_bytes == 0);
    CHECK(data->wave_view.info.availability != AZ_AVAIL_READY);
}
/* Derived files are temporary, outside the repository and removed even on a
 * failed check. The operator's source file is opened read-only and never edited. */
static void derived_failure(const uint8_t *raw, size_t bytes, struct st_data *data,
                            uint32_t encoding, int expected) {
    CHECK(temporary[0] == 0);
    snprintf(temporary, sizeof temporary, "/tmp/az-sdk-waveprobe-XXXXXX");
    int fd = mkstemp(temporary);
    CHECK(fd >= 0);
    size_t off = 0;
    while (off < bytes) {
        ssize_t n = write(fd, raw + off, bytes - off);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0);
        off += (size_t) n;
    }
    CHECK(close(fd) == 0);
    failure(temporary, data, encoding, 0, 32, expected);
    CHECK(unlink(temporary) == 0);
    temporary[0] = 0;
}
static void probe_file(const char *path) {
    unsigned start_checks = checks;
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long extent = ftell(file);
    CHECK(extent >= 12 && extent <= 16 * 1024 * 1024);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    size_t size = (size_t) extent;
    uint8_t *raw = malloc(size), *changed = malloc(size);
    struct st_data *data = malloc(sizeof *data);
    CHECK(raw != NULL && changed != NULL && data != NULL);
    CHECK(fread(raw, 1, size, file) == size && fclose(file) == 0);
    unsigned present = 0;
    for (uint32_t encoding = 1; encoding <= 3; encoding++) {
        struct raw_wave w = raw_walk(raw, size, encoding);
        if (!w.found) {
            failure(path, data, encoding, 0, 1, AZ_UNSUPPORTED);
            CHECK(read_bytes == 12 + w.tags * 12); /* No selected-header or payload read. */
            continue;
        }
        present++;
        CHECK(w.count > 150);
        valid_window(path, raw, &w, data, 0, 32, 0);
        valid_window(path, raw, &w, data, 150, 32, 0);
        valid_window(path, raw, &w, data, w.count - 2, 2, 0);
        valid_window(path, raw, &w, data, 0, AZ_WAVE_WINDOW_MAX, 0);
        valid_window(path, raw, &w, data, w.count - 7, AZ_WAVE_WINDOW_MAX, 0);
        valid_window(path, raw, &w, data, w.count / 2, 257, 0);
        valid_window(path, raw, &w, data, 150, 32, 1);
        failure(path, data, encoding, w.count, 1, AZ_INVALID);
        failure(path, data, encoding, UINT32_MAX, 2, AZ_INVALID);
        failure(path, data, encoding, 0, 0, AZ_INVALID);
        failure(path, data, encoding, 0, AZ_WAVE_WINDOW_MAX + 1, AZ_LIMIT);
        memcpy(changed, raw, size);
        memcpy(changed + w.tag_offset, "PWV9", 4);
        derived_failure(changed, size, data, encoding, AZ_UNSUPPORTED);
        memcpy(changed, raw, size);
        raw_put32(changed + w.tag_offset + 20, w.header_word + 1);
        derived_failure(changed, size, data, encoding, AZ_UNSUPPORTED);
        memcpy(changed, raw, size);
        raw_put32(changed + w.tag_offset + 16, UINT32_MAX);
        derived_failure(changed, size, data, encoding, AZ_FAILED);
        CHECK(w.last_tag > w.tag_offset);
        memcpy(changed, raw, size);
        raw_put32(changed + w.last_tag + 4, 0);
        derived_failure(changed, size, data, encoding, AZ_FAILED);
        derived_failure(raw, size - 1, data, encoding, AZ_FAILED);
        printf("sdkwaveprovider: encoding=%u columns=%u stride=%u PASS; %s\n", encoding, w.count, w.stride, path);
    }
    CHECK(present != 0);
    printf("sdkwaveprovider: %u checks PASS; bytes=%zu formats=%u; %s\n", checks - start_checks, size, present, path);
    free(data);
    free(changed);
    free(raw);
}
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /absolute/local/ANLZ0000.EXT [ANLZ0000.2EX ...]\n", argv[0]);
        return 2;
    }
    CHECK(atexit(cleanup) == 0);
    for (int i = 1; i < argc; i++) probe_file(argv[i]);
    printf("sdkwaveprovider: total=%u checks PASS\n", checks);
    return 0;
}
