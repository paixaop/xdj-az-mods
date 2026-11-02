/* SPDX-License-Identifier: GPL-3.0-only */
/* Stored-data regressions and independent optional user-owned ANLZ comparisons. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdk_analysis.h"
#include "sdk_data_engine.h"
#include "sdk_wave_provider.h"
static unsigned checks;
#define CHECK(test)                                                   \
    do {                                                              \
        checks++;                                                     \
        if (!(test)) {                                                \
            fprintf(stderr, "FAIL stored:%d: %s\n", __LINE__, #test); \
            exit(1);                                                  \
        }                                                             \
    } while (0)
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static void put16(uint8_t *p, unsigned n) {
    p[0] = n >> 8;
    p[1] = n;
}
static void tag(uint8_t *p, const char *name, uint32_t h, uint32_t n) {
    memset(p, 0, n);
    memcpy(p, name, 4);
    put32(p + 4, h);
    put32(p + 8, n);
}
static size_t cue_file(uint8_t *p, int extended, unsigned kind, unsigned count, unsigned entry_size,
                       uint32_t status, uint32_t start, uint32_t end) {
    unsigned h = extended ? 20 : 24;
    size_t n = 28 + h + count * entry_size;
    tag(p, "PMAI", 28, (uint32_t) n);
    tag(p + 28, extended ? "PCO2" : "PCOB", h, (uint32_t) n - 28);
    put32(p + 40, kind);
    put16(p + 28 + (extended ? 16 : 18), count);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *e = p + 28 + h + i * entry_size;
        tag(e, extended ? "PCP2" : "PCPT", extended ? 16 : 28, entry_size);
        put32(e + 12, kind ? 1 : 0);
        if (extended) {
            e[16] = 2;
            put32(e + 20, start);
            put32(e + 24, end);
        } else {
            put32(e + 16, status);
            e[28] = 2;
            put32(e + 32, start);
            put32(e + 36, end);
        }
    }
    return n;
}
static int parse(struct st_data *d, uint8_t *a, size_t na, uint8_t *b, size_t nb) {
    const uint8_t *buffers[2] = {a, b};
    size_t sizes[2] = {na, nb};
    st_data_init(d);
    return st_parse_pair(d, buffers, sizes, AZ_DATA_CUES);
}
static void cues(void) {
    uint8_t a[1024], b[1024];
    struct st_data *d = malloc(sizeof *d);
    CHECK(d);
    CHECK(sizeof *d * 8 < 8u * 1024u * 1024u);
    for (unsigned n = 40; n <= 51; n++) {
        size_t size = cue_file(a, 1, 0, 1, n, 0, 0, 500);
        put16(a + 48 + 36, 4);
        put16(a + 48 + 38, 1); /* Quantization does not imply automatic activation. */
        CHECK(parse(d, a, size, NULL, 0) == AZ_OK);
        CHECK(d->view.cue_count == 1 && d->cues[0].type == AZ_CUE_LOOP);
        CHECK(d->cues[0].start_s == 0 && d->cues[0].end_s == .5);
        CHECK(!(d->cues[0].valid & AZ_CUE_COMMENT));
        CHECK(!(d->cues[0].valid & AZ_CUE_COLOR));
        CHECK((d->cues[0].valid & AZ_CUE_COLOR_ID) && d->cues[0].color_id == 0);
        CHECK(!(d->cues[0].valid & AZ_CUE_AUTO_ACTIVE) && d->cues[0].auto_active == -1);
    }
    size_t na = cue_file(a, 1, 0, 1, 44, 0, 0, 500);
    put32(a + 48 + 40, 4); /* The length exists but its payload does not. */
    CHECK(parse(d, a, na, NULL, 0) == AZ_FAILED);
    na = cue_file(a, 1, 0, 1, 40, 0, 0, 500);
    put32(a + 48 + 4, 12);
    CHECK(parse(d, a, na, NULL, 0) == AZ_FAILED);
    na = cue_file(a, 0, 0, 1, 56, 4, 0, 500);
    put32(a + 52 + 4, 16);
    CHECK(parse(d, a, na, NULL, 0) == AZ_FAILED);
    for (unsigned kind = 0; kind < 2; kind++)
        for (unsigned color = 0; color <= 9; color++) {
            na = cue_file(a, 1, kind, 1, 48, 0, 0, 500);
            a[48 + 0x1c] = color == 9 ? 255 : color;
            a[48 + 44] = 7;
            a[48 + 45] = 0x12;
            a[48 + 46] = 0x34;
            a[48 + 47] = 0x56;
            CHECK(parse(d, a, na, NULL, 0) == AZ_OK);
            CHECK((d->cues[0].valid & AZ_CUE_COLOR) == (kind ? AZ_CUE_COLOR : 0));
            CHECK((d->cues[0].valid & AZ_CUE_COLOR_ID) == (kind ? 0 : AZ_CUE_COLOR_ID));
            CHECK(kind ? d->cues[0].rgb == 0x123456 : d->cues[0].color_id == (color == 9 ? 255 : color));
        }
    const uint32_t statuses[] = {0, 1, 4, 7};
    for (unsigned kind = 0; kind < 2; kind++)
        for (unsigned j = 0; j < sizeof statuses / sizeof *statuses; j++) {
            na = cue_file(a, 0, kind, 1, 56, statuses[j], 0, 500);
            CHECK(parse(d, a, na, NULL, 0) == AZ_OK);
            CHECK(d->cues[0].saved == 1 && d->cues[0].modified == -1);
            CHECK((d->cues[0].valid & AZ_CUE_AUTO_ACTIVE) == (j < 3 ? AZ_CUE_AUTO_ACTIVE : 0));
            CHECK(d->cues[0].auto_active == (j == 2 ? 1 : j < 2 ? 0
                                                                : -1));
        }
    for (unsigned kind = 0; kind < 2; kind++) {
        na = cue_file(a, 0, kind, 1, 56, 4, 0, 500);
        size_t nb = cue_file(b, 1, kind, 1, 40, 0, 0, 500);
        CHECK(parse(d, a, na, b, nb) == AZ_OK && d->view.cue_count == 1);
        CHECK(d->cues[0].auto_active == 1 && (d->cues[0].valid & AZ_CUE_AUTO_ACTIVE));
        put32(a + 52 + 16, 0);
        CHECK(parse(d, a, na, b, nb) == AZ_OK && d->cues[0].auto_active == 0);
        put32(a + 52 + 16, 4);
        put32(a + 52 + 36, 501);
        CHECK(parse(d, a, na, b, nb) == AZ_OK && d->cues[0].auto_active == -1);
        CHECK(!(d->cues[0].valid & AZ_CUE_AUTO_ACTIVE));
        put32(a + 52 + 36, 500);
        if (kind) {
            put32(a + 52 + 12, 2);
            CHECK(parse(d, a, na, b, nb) == AZ_OK && d->cues[0].auto_active == -1);
        }
    }
    na = cue_file(a, 0, 0, 2, 56, 4, 0, 500);
    size_t nb = cue_file(b, 1, 0, 1, 40, 0, 0, 500);
    CHECK(parse(d, a, na, b, nb) == AZ_OK && d->cues[0].auto_active == -1);
    put32(a + 52 + 56 + 16, 0);
    CHECK(parse(d, a, na, b, nb) == AZ_OK && d->cues[0].auto_active == -1);
    CHECK(!(d->cues[0].valid & AZ_CUE_AUTO_ACTIVE));
    na = cue_file(a, 0, 0, 1, 56, 4, 0, 500);
    a[52 + 28] = 1;
    put32(a + 52 + 36, UINT32_MAX);
    CHECK(parse(d, a, na, b, nb) == AZ_OK && d->cues[0].auto_active == -1);
    na = cue_file(a, 0, 1, 1, 56, 4, 0, 500); /* Wrong classification. */
    CHECK(parse(d, a, na, b, nb) == AZ_OK && d->view.cue_count == 2);
    CHECK(d->cues[1].classification == AZ_CUE_MEMORY && d->cues[1].auto_active == -1);
    na = cue_file(a, 0, 0, 1, 56, 4, 0, 500);
    put32(a + 52 + 8, 1000); /* A discarded malformed annotation must not poison PCP2. */
    CHECK(parse(d, a, na, b, nb) == AZ_OK && d->cues[0].auto_active == -1);
    free(d);
}
struct parse_task {
    uint8_t dat[256], ext[256];
    size_t sizes[2];
    int expected, passed;
};
static void *parse_worker(void *arg) {
    struct parse_task *task = arg;
    const uint8_t *buffers[2] = {task->dat, task->ext};
    struct st_data *d = malloc(sizeof *d);
    if (!d) return NULL;
    task->passed = 1;
    for (unsigned i = 0; i < 32; i++) {
        st_data_init(d);
        if (st_parse_pair(d, buffers, task->sizes, AZ_DATA_CUES) != AZ_OK ||
            d->view.cue_count != 1 || d->cues[0].auto_active != task->expected ||
            !(d->cues[0].valid & AZ_CUE_AUTO_ACTIVE)) task->passed = 0;
    }
    free(d);
    return NULL;
}
static void concurrent_parsers(void) {
    struct parse_task tasks[4];
    pthread_t threads[4];
    memset(tasks, 0, sizeof tasks);
    for (unsigned i = 0; i < 4; i++) {
        tasks[i].expected = i & 1u;
        tasks[i].sizes[0] = cue_file(tasks[i].dat, 0, 0, 1, 56, tasks[i].expected ? 4 : 0, 0, 500 + i);
        tasks[i].sizes[1] = cue_file(tasks[i].ext, 1, 0, 1, 40, 0, 0, 500 + i);
        CHECK(!pthread_create(&threads[i], NULL, parse_worker, &tasks[i]));
    }
    for (unsigned i = 0; i < 4; i++) {
        CHECK(!pthread_join(threads[i], NULL));
        CHECK(tasks[i].passed);
    }
}
static void phrase_versions(void) {
    uint8_t p[256];
    size_t n = 28 + 32 + 2 * 24 + 24 + 2 * 8;
    struct st_data *d = malloc(sizeof *d);
    CHECK(d);
    for (unsigned version = 0; version <= 2; version++) {
        tag(p, "PMAI", 28, (uint32_t) n);
        uint8_t *phrase = p + 28;
        tag(phrase, "PSSI", 32, 80);
        put16(phrase + 12, version);
        put16(phrase + 14, 24);
        put16(phrase + 16, 2);
        put16(phrase + 18, 1);
        put16(phrase + 26, 9);
        put16(phrase + 34, 1);
        put16(phrase + 36, 7);
        put16(phrase + 58, 5);
        put16(phrase + 60, 3);
        uint8_t *grid = phrase + 80;
        tag(grid, "PQTZ", 24, 40);
        put32(grid + 20, 2);
        put16(grid + 24, 1);
        put16(grid + 26, 12000);
        put32(grid + 28, 0);
        put16(grid + 32, 2);
        put16(grid + 34, 12000);
        put32(grid + 36, 500);
        const uint8_t *buffers[2] = {p, NULL};
        size_t sizes[2] = {n, 0};
        st_data_init(d);
        CHECK(st_parse_pair(d, buffers, sizes, AZ_DATA_PHRASES | AZ_DATA_GRID) == AZ_OK);
        CHECK(d->view.grid_info.availability == AZ_AVAIL_READY && d->view.beat_count == 2);
        if (version < 2) {
            CHECK(d->view.phrases_info.availability == AZ_AVAIL_READY && d->view.phrase_count == 2);
            CHECK(d->phrases[0].start_beat == 0 && d->phrases[0].end_beat == 4 && d->phrases[0].raw_kind == 7);
            CHECK(d->phrases[1].start_beat == 4 && d->phrases[1].end_beat == 8 && d->phrases[1].raw_mood == 1);
        } else CHECK(d->view.phrases_info.availability == AZ_AVAIL_UNSUPPORTED && !d->view.phrase_count);
        put16(phrase + 12, 1);
        put16(phrase + 14, 64);
        st_data_init(d);
        CHECK(st_parse_pair(d, buffers, sizes, AZ_DATA_PHRASES | AZ_DATA_GRID) == AZ_FAILED);
        put32(phrase + 8, (uint32_t) n); /* An unknown version must not excuse malformed outer lengths. */
        put16(phrase + 12, 2);
        st_data_init(d);
        CHECK(st_parse_pair(d, buffers, sizes, AZ_DATA_PHRASES | AZ_DATA_GRID) == AZ_FAILED);
    }
    free(d);
}
static void preview_requests(void) {
    struct st_engine engine = ST_ENGINE_INIT;
    az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, 1, 1, 1, 1, 0};
    az_wave_query query = {sizeof query, AZ_DATA_VERSION, AZ_WAVE_PWV4_PREVIEW, 0, AZ_WAVE_WINDOW_MAX, 0};
    uint64_t ticket = UINT64_MAX;
    for (unsigned encoding = AZ_WAVE_PWV4_PREVIEW; encoding <= AZ_WAVE_PWAV_PREVIEW; encoding++) {
        query.encoding = encoding;
        CHECK(st_request_with_wave(&engine, 0, &ref, ST_DETAIL, &query, &ticket) == AZ_OK);
        const struct st_ticket *record = st_find(&engine, 0, ticket);
        CHECK(record && engine.slots[record->slot].wave_query.encoding == encoding);
        CHECK(engine.slots[record->slot].wave_query.column_count == AZ_WAVE_WINDOW_MAX);
        CHECK(st_release(&engine, 0, ticket, 0) == AZ_OK);
    }
    const unsigned unsupported[] = {0, AZ_WAVE_PWV7_LAYERED_RAW + 1u, UINT32_MAX};
    for (unsigned i = 0; i < sizeof unsupported / sizeof *unsupported; i++) {
        query.encoding = unsupported[i];
        ticket = UINT64_MAX;
        CHECK(st_request_with_wave(&engine, 0, &ref, ST_DETAIL, &query, &ticket) == AZ_UNSUPPORTED);
        CHECK(ticket == UINT64_MAX);
    }
    query.encoding = AZ_WAVE_PWV6_PREVIEW;
    for (unsigned field = 0; field < 6; field++) {
        az_wave_query invalid = query;
        int expected = AZ_INVALID;
        switch (field) {
        case 0: invalid.size--; break;
        case 1: invalid.version++; break;
        case 2: invalid.reserved = 1; break;
        case 3: invalid.column_count = 0; break;
        case 4:
            invalid.column_count++;
            expected = AZ_LIMIT;
            break;
        case 5:
            invalid.column_count = UINT32_MAX;
            expected = AZ_LIMIT;
            break;
        }
        ticket = UINT64_MAX;
        CHECK(st_request_with_wave(&engine, 0, &ref, ST_DETAIL, &invalid, &ticket) == expected);
        CHECK(ticket == UINT64_MAX);
    }
    st_destroy(&engine);
}
static uint8_t *wave_file(unsigned encoding, unsigned count, size_t *size) {
    unsigned header = encoding == SDKW_ENCODING_PWV6 ? 20 : 24;
    unsigned stride = encoding == SDKW_ENCODING_PWV4 ? 6 : 3;
    *size = 28 + header + (size_t) stride * count;
    uint8_t *p = malloc(*size);
    CHECK(p);
    tag(p, "PMAI", 28, (uint32_t) *size);
    tag(p + 28, encoding == SDKW_ENCODING_PWV4 ? "PWV4" : "PWV6", header, (uint32_t) *size - 28);
    put32(p + 40, stride);
    put32(p + 44, count);
    for (size_t i = 28 + header; i < *size; i++) p[i] = (uint8_t) (i * 17u + 3u);
    return p;
}
static void write_file(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite(p, 1, n, f) == n);
    CHECK(!fclose(f));
}
static void previews(void) {
    char path[] = "/tmp/az-sdk-stored.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(!close(fd));
    struct st_data *data = malloc(sizeof *data);
    CHECK(data);
    for (unsigned encoding = SDKW_ENCODING_PWV4; encoding <= SDKW_ENCODING_PWV6; encoding++) {
        size_t size;
        uint8_t *p = wave_file(encoding, 1200, &size);
        unsigned stride = encoding == SDKW_ENCODING_PWV4 ? 6 : 3;
        struct sww_index index = {0};
        CHECK(sww_find_detail(p, size, encoding, &index) == SDKW_OK);
        CHECK(index.sample_count == 1200 && index.entry_size == stride);
        struct sww_window w, before;
        memset(&w, 0x5a, sizeof w);
        before = w;
        CHECK(sww_plan_window(&index, 150, 32, &w) == SDKW_UNSUPPORTED && !memcmp(&w, &before, sizeof w));
        CHECK(sww_plan_preview_window(&index, 150, 32, 400, &w) == SDKW_OK);
        CHECK(w.start_s == 50 && w.seconds_per_sample == 1.0 / 3.0 && w.byte_count == 32u * stride);
        CHECK(w.byte_offset == 28u + index.tag.header_size + 150u * stride);
        CHECK(sww_plan_preview_window(&index, 1199, 4096, 400, &w) == SDKW_OK && w.sample_count == 1);
        before = w;
        CHECK(sww_plan_preview_window(&index, 1200, 1, 400, &w) == SDKW_OUT_OF_RANGE && !memcmp(&w, &before, sizeof w));
        CHECK(sww_plan_preview_window(&index, 0, 0, 400, &w) == SDKW_INVALID);
        CHECK(sww_plan_preview_window(&index, 0, 4097, 400, &w) == SDKW_LIMIT);
        CHECK(sww_plan_preview_window(&index, 0, 1, 0, &w) == SDKW_UNSUPPORTED);
        CHECK(sww_plan_preview_window(&index, 0, 1, NAN, &w) == SDKW_UNSUPPORTED);
        uint8_t *header = malloc(index.tag.header_size);
        CHECK(header);
        memcpy(header, p + 28, index.tag.header_size);
        struct sww_index from_header;
        CHECK(sww_index_detail(header, index.tag.header_size, &index.tag, encoding, &from_header) == SDKW_OK);
        CHECK(sww_index_detail(header, index.tag.header_size - 1, &index.tag, encoding, &from_header) == SDKW_MALFORMED);
        free(header);
        write_file(path, p, size);
        st_data_init(data);
        data->wave_query = (az_wave_query) {sizeof(az_wave_query), 1, encoding, 150, 32, 0};
        data->view.metadata.valid = AZ_META_DURATION;
        data->view.metadata.duration_s = 400;
        CHECK(st_wave_read(path, data) == AZ_OK);
        CHECK(data->wave_view.encoding == encoding && data->wave_view.bytes_per_column == stride);
        CHECK(data->wave_view.column_count == 32 && data->wave_view.total_columns == 1200 && data->wave_view.data_bytes == stride * 32);
        CHECK(data->wave_view.start_s == 50 && data->wave_view.seconds_per_column == 1.0 / 3.0);
        CHECK(!memcmp(data->wave_view.columns, p + 28 + index.tag.header_size + 150 * stride, 32 * stride));
        data->view.metadata.valid = 0; /* A nonzero but invalid value must not establish the axis. */
        CHECK(st_wave_read(path, data) == AZ_UNSUPPORTED);
        free(p);
        p = wave_file(encoding, 0, &size);
        write_file(path, p, size);
        st_data_init(data);
        data->view.metadata.valid = AZ_META_DURATION;
        data->view.metadata.duration_s = 400;
        data->wave_query = (az_wave_query) {sizeof(az_wave_query), 1, encoding, 0, 1, 0};
        CHECK(st_wave_read(path, data) == AZ_OK && data->wave_view.info.availability == AZ_AVAIL_EMPTY);
        CHECK(!data->wave_view.column_count && !data->wave_view.data_bytes && !data->wave_view.seconds_per_column);
        free(p);
        p = wave_file(encoding, 4096, &size);
        write_file(path, p, size);
        data->wave_query.column_count = 4096;
        CHECK(st_wave_read(path, data) == AZ_OK && data->wave_view.data_bytes == 4096u * stride);
        CHECK(!memcmp(data->wave_view.columns, p + 28 + (encoding == SDKW_ENCODING_PWV6 ? 20 : 24), 4096u * stride));
        free(p);
    }
    CHECK(!unlink(path));
    free(data);
}
static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long bytes = ftell(f);
    CHECK(bytes >= 12 && bytes <= (long) ST_FILE_MAX);
    CHECK(!fseek(f, 0, SEEK_SET));
    *n = (size_t) bytes;
    uint8_t *p = malloc(*n);
    CHECK(p);
    CHECK(fread(p, 1, *n, f) == *n);
    CHECK(!fclose(f));
    return p;
}
static void real_preview(const char *path, unsigned encoding) {
    size_t n;
    uint8_t *p = read_file(path, &n);
    CHECK(!memcmp(p, "PMAI", 4) && be32(p + 8) == n);
    const char *name = encoding == SDKW_ENCODING_PWV4 ? "PWV4" : "PWV6";
    size_t selected = 0;
    unsigned found = 0;
    for (size_t off = be32(p + 4); off < n;) {
        CHECK(n - off >= 12);
        uint32_t h = be32(p + off + 4), bytes = be32(p + off + 8);
        CHECK(h >= 12 && h <= bytes && bytes <= n - off);
        if (!memcmp(p + off, name, 4)) {
            selected = off;
            found++;
        }
        off += bytes;
    }
    if (!found) {
        printf("real %s absent: %s\n", name, path);
        free(p);
        return;
    }
    CHECK(found == 1);
    unsigned h = be32(p + selected + 4), stride = be32(p + selected + 12), count = be32(p + selected + 16);
    CHECK(h == (encoding == SDKW_ENCODING_PWV4 ? 24u : 20u));
    CHECK(stride == (encoding == SDKW_ENCODING_PWV4 ? 6u : 3u));
    CHECK(count && count == (be32(p + selected + 8) - h) / stride);
    struct st_data *data = malloc(sizeof *data);
    CHECK(data);
    st_data_init(data);
    data->view.metadata.valid = AZ_META_DURATION;
    data->view.metadata.duration_s = 400; /* Deliberate reference axis, not measured track duration. */
    uint32_t firsts[] = {0, count / 2, count - 1};
    for (unsigned i = 0; i < sizeof firsts / sizeof *firsts; i++) {
        uint32_t first = firsts[i], returned = count - first;
        if (returned > 4096) returned = 4096;
        data->wave_query = (az_wave_query) {sizeof(az_wave_query), 1, encoding, first, 4096, 0};
        CHECK(st_wave_read(path, data) == AZ_OK);
        CHECK(data->wave_view.column_count == returned && data->wave_view.data_bytes == returned * stride);
        CHECK(!memcmp(data->wave_view.columns, p + selected + h + (size_t) first * stride, returned * stride));
        CHECK(fabs(data->wave_view.start_s - first * (400.0 / count)) < 1e-12);
    }
    printf("real %s: %u columns, %u bytes/column, three streamed windows match raw bytes\n", name, count, stride);
    free(data);
    free(p);
}
static void real_cues(const char *dat, const char *ext) {
    size_t sizes[2];
    uint8_t *p[2] = {read_file(dat, &sizes[0]), read_file(ext, &sizes[1])};
    struct st_data *d = malloc(sizeof *d);
    CHECK(d);
    CHECK(parse(d, p[0], sizes[0], p[1], sizes[1]) == AZ_OK);
    const uint8_t *memory = NULL;
    for (unsigned file = 0; file < 2; file++)
        for (size_t off = be32(p[file] + 4); off < sizes[file];) {
            CHECK(sizes[file] - off >= 12);
            uint32_t length = be32(p[file] + off + 8);
            CHECK(length >= 12 && length <= sizes[file] - off);
            if (!memcmp(p[file] + off, "PCO2", 4) && be32(p[file] + off + 12) == 0) memory = p[file] + off;
            off += length;
        }
    unsigned memory_colors = 0;
    if (memory) {
        size_t off = be32(memory + 4), bytes = be32(memory + 8);
        unsigned count = (unsigned) memory[16] << 8 | memory[17];
        uint32_t at = 0;
        for (unsigned i = 0; i < count; i++) {
            CHECK(bytes - off >= 40);
            const uint8_t *entry = memory + off;
            uint32_t length = be32(entry + 8);
            CHECK(length >= 40 && length <= bytes - off);
            while (at < d->view.cue_count && d->cues[at].classification != AZ_CUE_MEMORY) at++;
            CHECK(at < d->view.cue_count);
            CHECK((d->cues[at].valid & AZ_CUE_COLOR_ID) && d->cues[at].color_id == entry[0x1c]);
            CHECK(!(d->cues[at].valid & AZ_CUE_COLOR));
            memory_colors++;
            at++;
            off += length;
        }
    }
    unsigned memory_loops = 0, active = 0, inactive = 0;
    for (uint32_t i = 0; i < d->view.cue_count; i++) {
        const az_cue_entry *c = &d->cues[i];
        if (c->classification == AZ_CUE_MEMORY && c->type == AZ_CUE_LOOP) {
            memory_loops++;
            if (c->valid & AZ_CUE_AUTO_ACTIVE) {
                if (c->auto_active) active++;
                else inactive++;
            }
        }
    }
    if (getenv("REQUIRE_ACTIVE_MEMORY_LOOP")) CHECK(active > 0);
    if (getenv("REQUIRE_INACTIVE_MEMORY_LOOP")) CHECK(inactive > 0);
    printf("real saved memory loops: %u; Active Loop true=%u false=%u (unknown remains unavailable)\n", memory_loops, active, inactive);
    printf("real memory color IDs: %u values match raw PCP2 bytes\n", memory_colors);
    free(d);
    free(p[0]);
    free(p[1]);
}
static void real_phrase_version(const char *path) {
    size_t n;
    uint8_t *p = read_file(path, &n);
    size_t phrase = 0;
    for (size_t off = be32(p + 4); off < n;) {
        CHECK(n - off >= 12);
        uint32_t length = be32(p + off + 8);
        CHECK(length >= 12 && length <= n - off);
        if (!memcmp(p + off, "PSSI", 4)) phrase = off;
        off += length;
    }
    if (!phrase) {
        free(p);
        return;
    }
    CHECK(be32(p + phrase + 4) >= 32);
    unsigned original_version = (unsigned) p[phrase + 12] << 8 | p[phrase + 13];
    if (original_version > 1) {
        printf("real PSSI unknown version %u remains unsupported\n", original_version);
        free(p);
        return;
    }
    struct st_data *a = malloc(sizeof *a), *b = malloc(sizeof *b);
    CHECK(a && b);
    const uint8_t *buffers[2] = {p, NULL};
    size_t sizes[2] = {n, 0};
    st_data_init(a);
    CHECK(st_parse_pair(a, buffers, sizes, AZ_DATA_PHRASES) == AZ_OK);
    put16(p + phrase + 12, original_version ^ 1u);
    st_data_init(b);
    CHECK(st_parse_pair(b, buffers, sizes, AZ_DATA_PHRASES) == AZ_OK);
    CHECK(a->view.phrase_count == b->view.phrase_count);
    CHECK(!memcmp(a->phrases, b->phrases, a->view.phrase_count * sizeof *a->phrases));
    printf("derived PSSI version header: %u raw phrases match; original file remains unchanged\n", a->view.phrase_count);
    free(a);
    free(b);
    free(p);
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || argc == 3 || argc == 4);
    cues();
    concurrent_parsers();
    phrase_versions();
    preview_requests();
    previews();
    if (argc > 1) {
        real_cues(argv[1], argv[2]);
        real_preview(argv[2], SDKW_ENCODING_PWV4);
        real_phrase_version(argv[2]);
    }
    if (argc == 4) real_preview(argv[3], SDKW_ENCODING_PWV6);
    printf("sdk stored: %u checks passed; optional cue tails, saved activation and bounded previews\n", checks);
    return 0;
}
