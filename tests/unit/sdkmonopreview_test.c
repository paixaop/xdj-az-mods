/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded PWV2/PWAV windows. Raw reference parsing is independent of sww_*. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdk_analysis.h"
static size_t read_bytes, largest_read;
static ssize_t counted_pread(int fd, void *out, size_t n, off_t off) {
    if (n > largest_read) largest_read = n;
    ssize_t rc = pread(fd, out, n, off);
    if (rc > 0) read_bytes += (size_t) rc;
    return rc;
}
#define pread counted_pread
#include "sdk_wave_provider.h"
#undef pread

/* Numeric IDs make the original implementation compile and fail at its actual
 * unsupported-format result, before the append-only public names are added. */
enum {
    TINY = 6,
    OVERVIEW = 7
};
_Static_assert((unsigned) AZ_WAVE_PWV2_TINY_PREVIEW == (unsigned) TINY && (unsigned) AZ_WAVE_PWAV_PREVIEW == (unsigned) OVERVIEW,
               "append-only public waveform IDs");
static uint32_t be32(const uint8_t *p) {
    return (uint32_t) p[0] * 16777216u + (uint32_t) p[1] * 65536u +
           (uint32_t) p[2] * 256u + p[3];
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static void tag(uint8_t *p, const char *name, uint32_t header, uint32_t size) {
    memcpy(p, name, 4);
    put32(p + 4, header);
    put32(p + 8, size);
}
static uint8_t *fixture(unsigned encoding, uint32_t count, size_t *size) {
    *size = 28 + 12 + 20 + count + 12;
    uint8_t *p = calloc(1, *size);
    assert(p);
    tag(p, "PMAI", 28, (uint32_t) *size);
    tag(p + 28, "BEFR", 12, 12);
    tag(p + 40, encoding == TINY ? "PWV2" : "PWAV", 20, 20 + count);
    put32(p + 52, count);
    put32(p + 56, 0x00010000);
    for (uint32_t i = 0; i < count; ++i) p[60 + i] = (uint8_t) (i * 37u + 11u);
    tag(p + 60 + count, "AFTR", 12, 12);
    return p;
}
static void unchanged_error(uint8_t *p, size_t size, unsigned encoding, int status) {
    struct sww_index before, out;
    memset(&before, 0xa5, sizeof before);
    out = before;
    assert(sww_find_detail(p, size, encoding, &out) == status);
    assert(!memcmp(&out, &before, sizeof out));
}
static void windows(const uint8_t *p, size_t size, unsigned encoding, uint32_t offset,
                    uint32_t count, const char *path) {
    struct sww_index index = {0};
    assert(sww_find_detail(p, size, encoding, &index) == (count ? SDKW_OK : SDKW_EMPTY));
    assert(index.tag.offset == offset && index.entry_size == 1 && index.sample_count == count);
    assert(sww_is_preview(encoding) && sww_header_bytes(encoding) == 20);
    struct sww_window no_clock = {0};
    assert(sww_plan_window(&index, 0, 1, &no_clock) == SDKW_UNSUPPORTED);
    const uint32_t firsts[] = {0, count / 2, count ? count - 1 : 0};
    for (unsigned j = 0; j < 3; ++j) {
        uint32_t first = firsts[j], n = count - first;
        if (n > AZ_WAVE_WINDOW_MAX) n = AZ_WAVE_WINDOW_MAX;
        struct sww_window w = {0};
        assert(sww_plan_preview_window(&index, first, AZ_WAVE_WINDOW_MAX, 301.0, &w) == (count ? SDKW_OK : SDKW_EMPTY));
        assert(w.sample_count == n && w.byte_count == n && w.byte_offset == offset + 20u + first);
        assert(fabs(w.start_s - (count ? 301.0 * first / count : 0)) < 1e-9);
        uint8_t bytes[AZ_WAVE_WINDOW_MAX + 8];
        memset(bytes, 0xa5, sizeof bytes);
        assert(sww_copy_window(&w, p + w.byte_offset, n, bytes, n) == (count ? SDKW_OK : SDKW_EMPTY));
        assert(!memcmp(bytes, p + w.byte_offset, n) && bytes[n] == 0xa5);
        struct st_data *data = malloc(sizeof *data);
        assert(data);
        st_data_init(data);
        data->view.metadata.valid = AZ_META_DURATION;
        data->view.metadata.duration_s = 301;
        data->wave_query = (az_wave_query) {sizeof(az_wave_query), 1, encoding, first, AZ_WAVE_WINDOW_MAX, 0};
        read_bytes = largest_read = 0;
        assert(st_wave_read(path, data) == AZ_OK);
        assert(data->wave_view.encoding == encoding && data->wave_view.column_count == n);
        assert(data->wave_view.total_columns == count && data->wave_view.bytes_per_column == 1);
        assert(!memcmp(data->wave_view.columns, p + offset + 20 + first, n));
        assert(data->wave_view.info.availability == (count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY));
        assert(fabs(data->wave_view.seconds_per_column - (count ? 301.0 / count : 0)) < 1e-12);
        assert(read_bytes <= 12 + 4096 * 12 + 20 + n && largest_read <= (n > 20 ? n : 20));
        data->view.metadata.valid = 0;
        assert(st_wave_read(path, data) == AZ_UNSUPPORTED);
        free(data);
    }
    struct sww_window w = {0};
    assert(sww_plan_preview_window(&index, count ? count : 1, 1, 301, &w) == SDKW_OUT_OF_RANGE);
    assert(sww_plan_preview_window(&index, 0, 1, 0, &w) == SDKW_UNSUPPORTED);
}
static void write_fixture(const char *path, const uint8_t *p, size_t size) {
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(p, 1, size, f) == size && !fclose(f));
}
static void synthetic(void) {
    char path[] = "/tmp/sdk-mono-preview.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && !close(fd));
    for (unsigned encoding = TINY; encoding <= OVERVIEW; ++encoding) {
        for (unsigned count = 0; count <= 70000; count += 70000) {
            size_t size;
            uint8_t *p = fixture(encoding, count, &size);
            write_fixture(path, p, size);
            windows(p, size, encoding, 40, count, path);
            free(p);
        }
        size_t size;
        uint8_t *p = fixture(encoding, 100, &size);
        struct sww_format f = {0};
        assert(sww_format_for(encoding, &f) == SDKW_OK && f.entry_size == 1);
        put32(p + 52, 101);
        unchanged_error(p, size, encoding, SDKW_MALFORMED);
        put32(p + 52, 100);
        put32(p + 56, 0);
        unchanged_error(p, size, encoding, SDKW_UNSUPPORTED);
        put32(p + 56, 0x00010000);
        put32(p + 44, 21);
        unchanged_error(p, size, encoding, SDKW_UNSUPPORTED);
        put32(p + 44, 20);
        put32(p + 44, 16);
        unchanged_error(p, size, encoding, SDKW_MALFORMED);
        put32(p + 44, 20);
        put32(p + size - 8, 13);
        unchanged_error(p, size, encoding, SDKW_MALFORMED);
        put32(p + size - 8, 12);
        unchanged_error(p, size - 1, encoding, SDKW_MALFORMED);
        tag(p + size - 12, encoding == TINY ? "PWV2" : "PWAV", 12, 12);
        unchanged_error(p, size, encoding, SDKW_AMBIGUOUS);
        free(p);
    }
    assert(!unlink(path));
}
static void real_file(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long length = ftell(f);
    assert(length >= 12 && length <= SDKW_FILE_MAX && !fseek(f, 0, SEEK_SET));
    size_t size = (size_t) length;
    uint8_t *p = malloc(size);
    assert(p);
    assert(fread(p, 1, size, f) == size && !fclose(f));
    assert(!memcmp(p, "PMAI", 4) && be32(p + 8) == size);
    unsigned found = 0;
    for (size_t off = be32(p + 4); off < size;) {
        assert(size - off >= 12);
        uint32_t header = be32(p + off + 4), n = be32(p + off + 8);
        assert(header >= 12 && header <= n && n <= size - off);
        unsigned encoding = !memcmp(p + off, "PWV2", 4) ? TINY : !memcmp(p + off, "PWAV", 4) ? OVERVIEW
                                                                                             : 0;
        if (encoding) {
            assert(header == 20 && be32(p + off + 12) == n - 20 && be32(p + off + 16) == 0x00010000);
            windows(p, size, encoding, (uint32_t) off, n - 20, path);
            found++;
        }
        off += n;
    }
    assert(found == 2);
    free(p);
}
int main(int argc, char **argv) {
    synthetic();
    for (int i = 1; i < argc; ++i) real_file(argv[i]);
    printf("mono previews: strict layout, raw windows, source-time policy and %d real files passed\n", argc - 1);
    return 0;
}
