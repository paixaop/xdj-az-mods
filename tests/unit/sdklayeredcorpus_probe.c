/* SPDX-License-Identifier: GPL-3.0-only */
/* Read-only operator-supplied 2EX corpus. The reference envelope walker and
 * subset inverse do not call the sww_* or hc_* helpers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "../../src/azmod/sdk_analysis.h"
#include "../../src/azmod/sdk_wave_provider.h"

enum { CORPUS_WINDOW = 4096, RAW_ENCODING = 3, LAYERED_ENCODING = 8 };
_Static_assert(AZ_WAVE_WINDOW_MAX == CORPUS_WINDOW, "public bounded window contract");
_Static_assert((unsigned) AZ_WAVE_PWV7_BANDS == (unsigned) RAW_ENCODING,
               "original raw encoding remains 3");
_Static_assert((unsigned) AZ_WAVE_PWV7_LAYERED_RAW == (unsigned) LAYERED_ENCODING,
               "derived encoding is append-only 8");
_Static_assert(sizeof(((struct st_data *) 0)->detail) >= CORPUS_WINDOW * 6,
               "real backend owns a complete bounded layered window");

static uint64_t checks, total_windows, total_columns, zero_columns, by_layers[3];
static const char *active_path;
static uint32_t active_column;
#define CHECK(condition)                                                         \
    do {                                                                         \
        ++checks;                                                                \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL layered-corpus:%d column=%" PRIu32 " %s; %s\n", \
                    __LINE__, active_column, #condition, active_path);            \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

struct raw_pwv7 {
    size_t data_offset;
    uint32_t count;
};
struct guarded_data {
    uint8_t before[32];
    struct st_data data;
    uint8_t after[32];
};
static uint32_t corpus_be32(const uint8_t *p) {
    return (uint32_t) p[0] * 16777216u + (uint32_t) p[1] * 65536u +
           (uint32_t) p[2] * 256u + p[3];
}
/* Independent full-envelope walk: never ask the implementation for an index,
 * sample count, file/tag bounds, byte order or PWV7 source data offset. */
static struct raw_pwv7 corpus_walk(const uint8_t *p, size_t size) {
    struct raw_pwv7 out = {0};
    CHECK(size >= 12 && !memcmp(p, "PMAI", 4));
    CHECK(corpus_be32(p + 8) == size);
    size_t offset = corpus_be32(p + 4);
    CHECK(offset >= 12 && offset <= size);
    unsigned found = 0;
    while (offset < size) {
        CHECK(size - offset >= 12);
        size_t header = corpus_be32(p + offset + 4);
        size_t length = corpus_be32(p + offset + 8);
        CHECK(header >= 12 && header <= length && length <= size - offset);
        if (!memcmp(p + offset, "PWV7", 4)) {
            CHECK(++found == 1 && header == 24);
            CHECK(corpus_be32(p + offset + 12) == 3);
            CHECK(corpus_be32(p + offset + 20) == 0x00960000u);
            out.count = corpus_be32(p + offset + 16);
            CHECK((length - header) % 3 == 0);
            CHECK(out.count == (length - header) / 3);
            out.data_offset = offset + header;
        }
        offset += length;
    }
    CHECK(offset == size && found == 1 && out.count > 0);
    return out;
}
static uint8_t *corpus_read(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    CHECK(fd >= 0);
    struct stat sb;
    CHECK(fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode));
    CHECK(sb.st_size >= 12 && (uint64_t) sb.st_size <= 16u * 1024u * 1024u);
    *size = (size_t) sb.st_size;
    uint8_t *p = malloc(*size);
    CHECK(p != NULL);
    size_t offset = 0;
    while (offset < *size) {
        ssize_t n = read(fd, p + offset, *size - offset);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0);
        offset += (size_t) n;
    }
    uint8_t tail;
    ssize_t n;
    do n = read(fd, &tail, 1); while (n < 0 && errno == EINTR);
    CHECK(n == 0 && close(fd) == 0);
    return p;
}
static void corpus_prepare(struct guarded_data *g, uint32_t encoding, uint32_t first) {
    memset(g->before, 0x69, sizeof g->before);
    memset(g->after, 0x96, sizeof g->after);
    st_data_init(&g->data);
    /* Detail timing must ignore unrelated metadata duration. */
    g->data.view.metadata.valid = AZ_META_DURATION;
    g->data.view.metadata.duration_s = 987654.0;
    g->data.wave_query = (az_wave_query) {sizeof(az_wave_query), AZ_DATA_VERSION,
                                         encoding, first, CORPUS_WINDOW, 0};
    memset(g->data.detail, 0xa5, sizeof g->data.detail);
    /* Detect a write immediately beyond the detail buffer inside st_data. */
    memset(g->data.beats, 0x7c, 32);
}
static void corpus_view(const struct guarded_data *g, const struct raw_pwv7 *raw,
                        uint32_t encoding, uint32_t first, uint32_t count, uint32_t stride) {
    const struct st_data *d = &g->data;
    const az_wave_snapshot *v = &d->wave_view;
    CHECK(v->size == sizeof *v && v->version == AZ_DATA_VERSION);
    CHECK(v->columns == d->detail && v->encoding == encoding);
    CHECK(v->first_column == first && v->column_count == count && v->total_columns == raw->count);
    CHECK(v->bytes_per_column == stride && v->data_bytes == count * stride);
    CHECK(v->info.availability == AZ_AVAIL_READY);
    CHECK(v->reserved[0] == 0 && v->reserved[1] == 0);
    CHECK(isfinite(v->start_s) && fabs(v->start_s * 150.0 - first) < 1e-7);
    CHECK(fabs(v->seconds_per_column - 1.0 / 150.0) < 1e-15);
    for (size_t i = (size_t) count * stride; i < sizeof d->detail; ++i)
        CHECK(d->detail[i] == 0xa5);
    for (size_t i = 0; i < sizeof g->before; ++i) CHECK(g->before[i] == 0x69);
    for (size_t i = 0; i < sizeof g->after; ++i) CHECK(g->after[i] == 0x96);
    for (size_t i = 0; i < 32; ++i) CHECK(((const uint8_t *) d->beats)[i] == 0x7c);
}
/* Invert symbolic IDs to input-byte membership. A/B/C are raw byte0/1/2,
 * not an independently certified physical band/frequency or display color. */
static unsigned corpus_subset(unsigned id) {
    switch (id) {
    case 1: return 4; /* C */
    case 2: return 2; /* B */
    case 3: return 1; /* A */
    case 4: return 6; /* BC */
    case 5: return 5; /* AC */
    case 6: return 3; /* AB */
    case 8: return 7; /* ABC */
    default: return 0;
    }
}
/* No sorting, ranking, >= source comparisons or call to the encoder. Decode
 * the output by assigning each component its absolute level on first inclusion
 * in the strictly growing subset chain, then compare with the immutable input. */
static void corpus_inverse(const uint8_t record[6], const uint8_t original[3]) {
    unsigned header = record[0] | (unsigned) record[1] << 8;
    unsigned layers = header >> 12;
    CHECK(layers >= 1 && layers <= 3 && record[5] == 0);
    uint8_t reconstructed[3] = {0, 0, 0};
    if (record[2] == 0) {
        CHECK(header == 0x1008 && record[3] == 0 && record[4] == 0);
        ++zero_columns;
    } else {
        unsigned seen = 0, previous = 256;
        for (unsigned layer = 0; layer < layers; ++layer) {
            unsigned subset = corpus_subset((header >> (4 * layer)) & 15);
            unsigned magnitude = record[2 + layer];
            CHECK(subset != 0 && magnitude > 0 && magnitude < previous);
            CHECK((subset & seen) == seen && subset != seen);
            unsigned newly_included = subset & ~seen;
            for (unsigned component = 0; component < 3; ++component)
                if (newly_included & (1u << component)) reconstructed[component] = (uint8_t) magnitude;
            seen = subset;
            previous = magnitude;
        }
        for (unsigned layer = layers; layer < 3; ++layer) {
            CHECK(((header >> (4 * layer)) & 15) == 0);
            CHECK(record[2 + layer] == 0);
        }
    }
    CHECK(!memcmp(reconstructed, original, sizeof reconstructed));
    ++by_layers[layers - 1];
}
static void corpus_file(const char *path) {
    active_path = path;
    active_column = 0;
    size_t size;
    uint8_t *bytes = corpus_read(path, &size);
    struct raw_pwv7 raw = corpus_walk(bytes, size);
    struct guarded_data *g = malloc(sizeof *g);
    CHECK(g != NULL);
    uint64_t windows = 0;
    for (uint32_t first = 0; first < raw.count;) {
        active_column = first;
        uint32_t count = raw.count - first;
        if (count > CORPUS_WINDOW) count = CORPUS_WINDOW;
        corpus_prepare(g, RAW_ENCODING, first);
        CHECK(st_wave_read(path, &g->data) == AZ_OK);
        corpus_view(g, &raw, RAW_ENCODING, first, count, 3);
        CHECK(!memcmp(g->data.detail, bytes + raw.data_offset + (size_t) first * 3,
                      (size_t) count * 3));
        if (total_windows == 0)
            puts("LAYERED_CORPUS_BASELINE_OK real_st_wave_read=1 raw_encoding=3 independent_PWV7=1");
        corpus_prepare(g, LAYERED_ENCODING, first);
        CHECK(st_wave_read(path, &g->data) == AZ_OK);
        corpus_view(g, &raw, LAYERED_ENCODING, first, count, 6);
        for (uint32_t column = 0; column < count; ++column) {
            active_column = first + column;
            corpus_inverse(g->data.detail + (size_t) column * 6,
                           bytes + raw.data_offset + (size_t) active_column * 3);
        }
        ++windows;
        ++total_windows;
        total_columns += count;
        first += count;
    }
    printf("layered-corpus: columns=%" PRIu32 " windows=%" PRIu64 " PASS; %s\n", raw.count, windows, path);
    free(g);
    free(bytes);
}
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /absolute/provided/ANLZ0000.2EX [...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; ++i) corpus_file(argv[i]);
    printf("WAVE_LAYER_CORPUS_PASS files=%d windows=%" PRIu64 " columns=%" PRIu64
           " allzero=%" PRIu64 " layers1=%" PRIu64 " layers2=%" PRIu64 " layers3=%" PRIu64
           " checks=%" PRIu64 "\n", argc - 1, total_windows, total_columns, zero_columns,
           by_layers[0], by_layers[1], by_layers[2], checks);
    return 0;
}
