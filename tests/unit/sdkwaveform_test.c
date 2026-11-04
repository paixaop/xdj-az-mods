/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic ANLZ containers only. No firmware, audio or exported track bytes. */
#include "sdk_waveform.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x)                                                   \
    do {                                                           \
        checks++;                                                  \
        if (!(x)) {                                                \
            fprintf(stderr, "sdkwaveform:%d: %s\n", __LINE__, #x); \
            exit(1);                                               \
        }                                                          \
    } while (0)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}
enum {
    FILE_HEADER = 36,
    PREFIX = 16,
    WAVE = FILE_HEADER + PREFIX,
    DATA = WAVE + 24
};
/* Nondefault container header and an unrelated tag precede the target. */
static uint8_t *fixture(uint32_t count, size_t *size) {
    *size = DATA + (size_t) count + 12;
    uint8_t *p = calloc(1, *size);
    CHECK(p != NULL);
    put32(p, SDKW_PMAI);
    put32(p + 4, FILE_HEADER);
    put32(p + 8, (uint32_t) *size);
    memcpy(p + FILE_HEADER, "JUNK", 4);
    put32(p + FILE_HEADER + 4, 12);
    put32(p + FILE_HEADER + 8, PREFIX);
    put32(p + WAVE, SDKW_PWV3);
    put32(p + WAVE + 4, 24);
    put32(p + WAVE + 8, count + 24);
    put32(p + WAVE + 12, 1);
    put32(p + WAVE + 16, count);
    put32(p + WAVE + 20, SDKW_PWV3_WORD);
    for (uint32_t i = 0; i < count; i++) p[DATA + i] = (uint8_t) ((i & 31) | (((i / 32) & 7) << 5));
    memcpy(p + DATA + count, "TAIL", 4);
    put32(p + DATA + count + 4, 12);
    put32(p + DATA + count + 8, 12);
    return p;
}
/* Deliberately broken negative control: publishes the first matching tag before
 * it discovers a malformed later tag. Never used by the production helpers. */
static int broken_first_match(const uint8_t *p, size_t size) {
    struct sww_file file;
    if (sww_file_header(p, size, size, &file) != SDKW_OK) return SDKW_MALFORMED;
    for (uint32_t off = file.header_size; off < file.size;) {
        struct sww_tag t;
        if (sww_tag_header(p + off, size - off, &file, off, &t) != SDKW_OK) return SDKW_MALFORMED;
        if (t.fourcc == SDKW_PWV3) return SDKW_OK;
        off += t.size;
    }
    return SDKW_NOT_FOUND;
}
static void expect_index_error(uint8_t *p, size_t size, int expected) {
    struct sww_index out, before;
    memset(&out, 0xa5, sizeof out);
    memcpy(&before, &out, sizeof out);
    CHECK(sww_find_pwv3(p, size, &out) == expected);
    CHECK(memcmp(&out, &before, sizeof out) == 0);
}
static void test_ranges(void) {
    size_t size;
    uint8_t *p = fixture(70000, &size);
    struct sww_index index, streamed;
    CHECK(sww_find_pwv3(p, size, &index) == SDKW_OK);
    CHECK(index.sample_count == 70000 && index.tag.offset == WAVE && index.entry_size == 1);
    struct sww_file file;
    struct sww_tag tag;
    CHECK(sww_file_header(p, 12, size, &file) == SDKW_OK);
    CHECK(sww_tag_header(p + WAVE, 12, &file, WAVE, &tag) == SDKW_OK);
    CHECK(sww_index_pwv3(p + WAVE, 24, &tag, &streamed) == SDKW_OK);
    CHECK(memcmp(&index, &streamed, sizeof index) == 0);
    struct sww_window window;
    CHECK(sww_plan_window(&streamed, 150, 32, &window) == SDKW_OK);
    CHECK(window.byte_offset == DATA + 150 && window.byte_count == 32);
    CHECK(window.start_s == 1.0 && window.seconds_per_sample == 1.0 / 150.0);
    struct sww_sample samples[SDKW_WINDOW_MAX], direct[SDKW_WINDOW_MAX];
    memset(samples, 0xa5, sizeof samples);
    CHECK(sww_decode_pwv3(p + window.byte_offset, window.byte_count, 32, samples, SDKW_WINDOW_MAX) == SDKW_OK);
    for (uint32_t i = 0; i < 32; i++) {
        CHECK(samples[i].raw == p[DATA + 150 + i]);
        CHECK(samples[i].height == ((150 + i) & 31));
        CHECK(samples[i].intensity == (((150 + i) / 32) & 7));
        CHECK(samples[i].reserved == 0);
    }
    /* Raw-as-height and a fixed file offset both fail on this synthetic source. */
    CHECK(samples[0].raw != samples[0].height);
    CHECK(p[28 + 24 + 150] != samples[0].raw);
    struct sww_window full;
    CHECK(sww_read_memory(p, size, 150, 32, direct, SDKW_WINDOW_MAX, &full) == SDKW_OK);
    CHECK(memcmp(samples, direct, 32 * sizeof samples[0]) == 0);
    CHECK(full.byte_offset == window.byte_offset && full.sample_count == 32);
    uint8_t copy[32];
    CHECK(sww_copy_window(&window, p + window.byte_offset, window.byte_count, copy, sizeof copy) == SDKW_OK);
    CHECK(memcmp(copy, p + DATA + 150, sizeof copy) == 0);
    CHECK(sww_plan_window(&index, 75, 1, &window) == SDKW_OK && window.start_s == 0.5);
    CHECK(sww_plan_window(&index, 65535, SDKW_WINDOW_MAX, &window) == SDKW_OK);
    CHECK(window.sample_count == SDKW_WINDOW_MAX && window.byte_count == SDKW_WINDOW_MAX);
    CHECK(sww_read_memory(p, size, 69999, SDKW_WINDOW_MAX, samples, SDKW_WINDOW_MAX, &window) == SDKW_OK);
    CHECK(window.sample_count == 1 && window.total_samples == 70000 && window.first_sample == 69999);
    CHECK(samples[0].raw == p[DATA + 69999]);
    struct sww_window before;
    memcpy(&before, &window, sizeof window);
    CHECK(sww_plan_window(&index, 70000, 1, &window) == SDKW_OUT_OF_RANGE);
    CHECK(sww_plan_window(&index, UINT32_MAX, 2, &window) == SDKW_OUT_OF_RANGE);
    CHECK(sww_plan_window(&index, 0, UINT32_MAX, &window) == SDKW_LIMIT);
    CHECK(sww_plan_window(&index, 0, SDKW_WINDOW_MAX + 1, &window) == SDKW_LIMIT);
    CHECK(sww_plan_window(&index, 0, 0, &window) == SDKW_INVALID);
    CHECK(memcmp(&window, &before, sizeof window) == 0);
    free(p);
}
static void test_decode_bounds(void) {
    /* Synthetic independent bit-pattern oracle. */
    const uint8_t raw[] = {0xff, 0xe1, 0x80, 0x1f, 0x20, 0x00};
    const uint8_t height[] = {31, 1, 0, 31, 0, 0}, intensity[] = {7, 7, 4, 0, 1, 0};
    struct sww_sample samples[6], before[6];
    CHECK(sww_decode_pwv3(raw, sizeof raw, 6, samples, 6) == SDKW_OK);
    for (unsigned i = 0; i < 6; i++) CHECK(samples[i].height == height[i] && samples[i].intensity == intensity[i]);
    memset(samples, 0xa5, sizeof samples);
    memcpy(before, samples, sizeof samples);
    for (size_t n = 0; n < sizeof raw; n++) {
        CHECK(sww_decode_pwv3(raw, n, 6, samples, 6) == SDKW_MALFORMED);
        CHECK(memcmp(samples, before, sizeof samples) == 0);
    }
    CHECK(sww_decode_pwv3(raw, 6, 6, samples, 5) == SDKW_LIMIT);
    CHECK(sww_decode_pwv3(raw, 6, UINT32_MAX, samples, 6) == SDKW_LIMIT);
    CHECK(sww_decode_pwv3(NULL, 0, 0, NULL, 0) == SDKW_EMPTY);
    CHECK(sww_decode_pwv3(NULL, 6, 6, samples, 6) == SDKW_INVALID);
    CHECK(memcmp(samples, before, sizeof samples) == 0);
    struct sww_window w = {SDKW_ENCODING_PWV3, 6, 0, 6, 100, 6, 0, 1.0 / 150.0};
    uint8_t copy[6], old[6];
    memset(copy, 0xa5, sizeof copy);
    memcpy(old, copy, sizeof copy);
    CHECK(sww_copy_window(&w, raw, 5, copy, 6) == SDKW_MALFORMED);
    CHECK(sww_copy_window(&w, raw, 6, copy, 5) == SDKW_LIMIT);
    w.first_sample = UINT32_MAX;
    CHECK(sww_copy_window(&w, raw, 6, copy, 6) == SDKW_INVALID);
    w.first_sample = 0;
    w.byte_count = SIZE_MAX;
    CHECK(sww_copy_window(&w, raw, 6, copy, 6) == SDKW_INVALID);
    w.byte_count = 6;
    w.encoding = 99;
    CHECK(sww_copy_window(&w, raw, 6, copy, 6) == SDKW_UNSUPPORTED);
    CHECK(memcmp(copy, old, sizeof copy) == 0);
}
static void test_bad_envelopes(void) {
    size_t n;
    uint8_t *p = fixture(8, &n);
    struct sww_file file;
    struct sww_tag tag;
    CHECK(sww_file_header(p, 12, n, &file) == SDKW_OK);
    CHECK(sww_tag_header(p + WAVE, 12, &file, WAVE, &tag) == SDKW_OK);
    for (size_t cut = 0; cut < n; cut++) expect_index_error(p, cut, SDKW_MALFORMED);
    for (size_t cut = 0; cut < 12; cut++) {
        struct sww_file unused;
        CHECK(sww_file_header(p, cut, n, &unused) == SDKW_MALFORMED);
    }
    for (size_t cut = 0; cut < 24; cut++) {
        struct sww_index unused;
        CHECK(sww_index_pwv3(p + WAVE, cut, &tag, &unused) == SDKW_MALFORMED);
    }
    put32(p, 0);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p, SDKW_PMAI);
    put32(p + 8, (uint32_t) n - 1);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + 8, (uint32_t) n);
    put32(p + 4, 11);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + 4, FILE_HEADER);
    put32(p + WAVE + 4, 20);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + WAVE + 4, 11);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + WAVE + 4, 33);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + WAVE + 4, 28);
    expect_index_error(p, n, SDKW_UNSUPPORTED);
    put32(p + WAVE + 4, 24);
    put32(p + WAVE + 8, UINT32_MAX);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + WAVE + 8, 32);
    put32(p + WAVE + 16, UINT32_MAX);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + WAVE + 16, 8);
    put32(p + WAVE + 12, 0);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + WAVE + 12, 2);
    put32(p + WAVE + 16, 4);
    expect_index_error(p, n, SDKW_UNSUPPORTED);
    put32(p + WAVE + 16, 3);
    expect_index_error(p, n, SDKW_MALFORMED);
    put32(p + WAVE + 12, 1);
    put32(p + WAVE + 16, 8);
    put32(p + WAVE + 20, 300u << 16);
    expect_index_error(p, n, SDKW_UNSUPPORTED);
    put32(p + WAVE + 20, SDKW_PWV3_WORD);
    put32(p + DATA + 8 + 4, 0); /* malformed tag after otherwise valid waveform */
    CHECK(broken_first_match(p, n) == SDKW_OK);
    expect_index_error(p, n, SDKW_MALFORMED);
    struct sww_sample blocked[8], old_samples[8];
    struct sww_window blocked_window, old_window;
    memset(blocked, 0xa5, sizeof blocked);
    memcpy(old_samples, blocked, sizeof blocked);
    memset(&blocked_window, 0xa5, sizeof blocked_window);
    memcpy(&old_window, &blocked_window, sizeof old_window);
    CHECK(sww_read_memory(p, n, 0, 8, blocked, 8, &blocked_window) == SDKW_MALFORMED);
    CHECK(memcmp(blocked, old_samples, sizeof blocked) == 0);
    CHECK(memcmp(&blocked_window, &old_window, sizeof blocked_window) == 0);
    put32(p + DATA + 8 + 4, 12);
    memcpy(p + WAVE, "PWV5", 4);
    expect_index_error(p, n, SDKW_NOT_FOUND);
    put32(p + WAVE, SDKW_PWV3);
    struct sww_index index, before;
    CHECK(sww_find_pwv3(p, n, &index) == SDKW_OK);
    memcpy(&before, &index, sizeof index);
    uint8_t changed[24];
    memcpy(changed, p + WAVE, 24);
    put32(changed + 8, 24);
    CHECK(sww_index_pwv3(changed, 24, &tag, &index) == SDKW_MALFORMED);
    CHECK(memcmp(&index, &before, sizeof index) == 0);
    CHECK(sww_tag_header(p + WAVE, 12, &file, UINT64_MAX, &tag) == SDKW_MALFORMED);
    CHECK(sww_file_header(p, 12, (size_t) SDKW_FILE_MAX + 1, &file) == SDKW_LIMIT);
    free(p);
    p = fixture(7, &n);
    put32(p + WAVE + 12, 2);
    put32(p + WAVE + 16, 3);
    expect_index_error(p, n, SDKW_MALFORMED); /* Seven payload bytes cannot hold 2-byte entries. */
    free(p);
}
static void test_empty_duplicate_and_limits(void) {
    size_t n;
    uint8_t *p = fixture(0, &n);
    struct sww_index index = {0};
    CHECK(sww_find_pwv3(p, n, &index) == SDKW_EMPTY);
    struct sww_window w;
    CHECK(sww_plan_window(&index, 0, 1, &w) == SDKW_EMPTY && w.sample_count == 0);
    CHECK(sww_plan_window(&index, 1, 1, &w) == SDKW_OUT_OF_RANGE);
    CHECK(sww_read_memory(p, n, 0, 1, NULL, 0, &w) == SDKW_EMPTY);
    CHECK(sww_copy_window(&w, NULL, 0, NULL, 0) == SDKW_EMPTY);
    /* Replace 12-byte tail with another canonical empty 24-byte waveform tag. */
    p = realloc(p, n + 12);
    CHECK(p != NULL);
    memcpy(p + n - 12, p + WAVE, 24);
    n += 12;
    put32(p + 8, (uint32_t) n);
    expect_index_error(p, n, SDKW_AMBIGUOUS);
    free(p);
    n = 12 + (SDKW_TAGS_MAX + 1u) * 12u;
    p = calloc(1, n);
    CHECK(p != NULL);
    put32(p, SDKW_PMAI);
    put32(p + 4, 12);
    put32(p + 8, (uint32_t) n);
    for (size_t off = 12; off < n; off += 12) {
        memcpy(p + off, "JUNK", 4);
        put32(p + off + 4, 12);
        put32(p + off + 8, 12);
    }
    expect_index_error(p, n, SDKW_LIMIT);
    free(p);
}
/* New formats use independently generated, asymmetric synthetic field values. */
static uint8_t *encoded_fixture(uint32_t encoding, uint32_t count, size_t *size) {
    CHECK(encoding == SDKW_ENCODING_PWV5 || encoding == SDKW_ENCODING_PWV7);
    uint32_t stride = encoding == SDKW_ENCODING_PWV5 ? 2 : 3;
    uint64_t payload = (uint64_t) count * stride;
    CHECK(payload <= SDKW_FILE_MAX - DATA - 12);
    *size = DATA + (size_t) payload + 12;
    uint8_t *p = calloc(1, *size);
    CHECK(p != NULL);
    put32(p, SDKW_PMAI);
    put32(p + 4, FILE_HEADER);
    put32(p + 8, (uint32_t) *size);
    memcpy(p + FILE_HEADER, "JUNK", 4);
    put32(p + FILE_HEADER + 4, 12);
    put32(p + FILE_HEADER + 8, PREFIX);
    memcpy(p + WAVE, encoding == SDKW_ENCODING_PWV5 ? "PWV5" : "PWV7", 4);
    put32(p + WAVE + 4, 24);
    put32(p + WAVE + 8, 24 + (uint32_t) payload);
    put32(p + WAVE + 12, stride);
    put32(p + WAVE + 16, count);
    put32(p + WAVE + 20, encoding == SDKW_ENCODING_PWV5 ? 0x00960305u : 0x00960000u);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *raw = p + DATA + (size_t) i * stride;
        if (stride == 2) {
            uint32_t red = (i + 1) % 8, green = (i / 3 + 2) % 8, blue = (i / 7 + 3) % 8;
            uint32_t height = (i * 3 + 4) % 32, low = (i / 11) % 4;
            uint16_t word = (uint16_t) (red * 8192 + green * 1024 + blue * 128 + height * 4 + low);
            raw[0] = (uint8_t) (word / 256);
            raw[1] = (uint8_t) word;
        } else {
            raw[0] = (uint8_t) (i * 5 + 1);
            raw[1] = (uint8_t) (i * 7 + 53);
            raw[2] = (uint8_t) (i * 11 + 149);
        }
    }
    memcpy(p + DATA + (size_t) payload, "TAIL", 4);
    put32(p + DATA + (size_t) payload + 4, 12);
    put32(p + DATA + (size_t) payload + 8, 12);
    return p;
}
static void expect_detail_error(uint8_t *p, size_t size, uint32_t encoding, int expected) {
    struct sww_index out, before;
    memset(&out, 0xa5, sizeof out);
    memcpy(&before, &out, sizeof out);
    CHECK(sww_find_detail(p, size, encoding, &out) == expected);
    CHECK(memcmp(&out, &before, sizeof out) == 0);
}
static void test_color_decode_edges(void) {
    const uint8_t packed[] = {0x29, 0x91, 0xff, 0xff, 0, 0, 0x80, 2};
    const uint16_t word[] = {0x2991, 0xffff, 0, 0x8002};
    const uint8_t red[] = {1, 7, 0, 4}, green[] = {2, 7, 0, 0}, blue[] = {3, 7, 0, 0};
    const uint8_t height[] = {4, 31, 0, 0}, low[] = {1, 3, 0, 2};
    struct sww_rgb_sample rgb[4], before_rgb[4];
    CHECK(sww_decode_pwv5(packed, sizeof packed, 4, rgb, 4) == SDKW_OK);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(rgb[i].raw == word[i]);
        CHECK(rgb[i].red == red[i] && rgb[i].green == green[i] && rgb[i].blue == blue[i]);
        CHECK(rgb[i].height == height[i] && rgb[i].low_bits == low[i] && rgb[i].reserved == 0);
    }
    /* Deliberately wrong little-endian interpretation changes the RGB fields. */
    uint16_t reversed = (uint16_t) (packed[1] * 256u + packed[0]);
    CHECK(reversed != rgb[0].raw && reversed / 8192 != rgb[0].red);
    memset(rgb, 0xa5, sizeof rgb);
    memcpy(before_rgb, rgb, sizeof rgb);
    for (size_t n = 0; n < sizeof packed; n++) {
        CHECK(sww_decode_pwv5(packed, n, 4, rgb, 4) == SDKW_MALFORMED);
        CHECK(memcmp(rgb, before_rgb, sizeof rgb) == 0);
    }
    CHECK(sww_decode_pwv5(packed, sizeof packed, 4, rgb, 3) == SDKW_LIMIT);
    CHECK(sww_decode_pwv5(packed, sizeof packed, UINT32_MAX, rgb, 4) == SDKW_LIMIT);
    CHECK(sww_decode_pwv5(NULL, 0, 0, NULL, 0) == SDKW_EMPTY);
    CHECK(sww_decode_pwv5(NULL, 8, 4, rgb, 4) == SDKW_INVALID);
    CHECK(memcmp(rgb, before_rgb, sizeof rgb) == 0);
    const uint8_t bands[] = {11, 97, 203, 255, 0, 128, 0, 0, 0};
    struct sww_band_sample band[3], before_band[3];
    CHECK(sww_decode_pwv7(bands, sizeof bands, 3, band, 3) == SDKW_OK);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(band[i].low == bands[i * 3] && band[i].mid == bands[i * 3 + 1] && band[i].high == bands[i * 3 + 2]);
        CHECK(band[i].reserved == 0);
    }
    /* Deliberately wrong mid/high/low ordering cannot match the asymmetric sentinel. */
    CHECK(band[0].low != bands[2] && band[0].mid != bands[0] && band[0].high != bands[1]);
    CHECK(band[1].low == 255); /* No 5-bit clamp, scaling or PWVC gain. */
    memset(band, 0xa5, sizeof band);
    memcpy(before_band, band, sizeof band);
    for (size_t n = 0; n < sizeof bands; n++) {
        CHECK(sww_decode_pwv7(bands, n, 3, band, 3) == SDKW_MALFORMED);
        CHECK(memcmp(band, before_band, sizeof band) == 0);
    }
    CHECK(sww_decode_pwv7(bands, sizeof bands, 3, band, 2) == SDKW_LIMIT);
    CHECK(sww_decode_pwv7(bands, sizeof bands, UINT32_MAX, band, 3) == SDKW_LIMIT);
    CHECK(sww_decode_pwv7(NULL, 0, 0, NULL, 0) == SDKW_EMPTY);
    CHECK(sww_decode_pwv7(NULL, 9, 3, band, 3) == SDKW_INVALID);
    CHECK(memcmp(band, before_band, sizeof band) == 0);
}
static void test_encoded_ranges(void) {
    for (uint32_t encoding = SDKW_ENCODING_PWV5; encoding <= SDKW_ENCODING_PWV7; encoding++) {
        uint32_t stride = encoding == SDKW_ENCODING_PWV5 ? 2 : 3;
        size_t size;
        uint8_t *p = encoded_fixture(encoding, 70000, &size);
        struct sww_index index, streamed;
        CHECK(sww_find_detail(p, size, encoding, &index) == SDKW_OK);
        CHECK(index.entry_size == stride && index.sample_count == 70000 && index.encoding == encoding);
        struct sww_file file;
        struct sww_tag tag;
        CHECK(sww_file_header(p, 12, size, &file) == SDKW_OK);
        CHECK(sww_tag_header(p + WAVE, 12, &file, WAVE, &tag) == SDKW_OK);
        CHECK(sww_index_detail(p + WAVE, 24, &tag, encoding, &streamed) == SDKW_OK);
        CHECK(memcmp(&index, &streamed, sizeof index) == 0);
        struct sww_window window = {0};
        CHECK(sww_plan_window(&index, 150, 32, &window) == SDKW_OK);
        CHECK(window.byte_offset == DATA + 150 * stride && window.byte_count == 32 * stride);
        CHECK(window.start_s == 1.0 && window.seconds_per_sample == 1.0 / 150.0);
        /* Deliberately wrong byte-per-column offset must differ for both formats. */
        CHECK(window.byte_offset != DATA + 150);
        CHECK(sww_plan_window(&index, 65535, SDKW_WINDOW_MAX, &window) == SDKW_OK);
        CHECK(window.byte_count == SDKW_WINDOW_MAX * stride && window.sample_count == SDKW_WINDOW_MAX);
        uint8_t copy[SDKW_WINDOW_MAX * 3], untouched[SDKW_WINDOW_MAX * 3];
        CHECK(sww_copy_window(&window, p + window.byte_offset, window.byte_count, copy, sizeof copy) == SDKW_OK);
        CHECK(memcmp(copy, p + DATA + (size_t) 65535 * stride, window.byte_count) == 0);
        if (encoding == SDKW_ENCODING_PWV5) {
            struct sww_rgb_sample decoded[SDKW_WINDOW_MAX];
            CHECK(sww_decode_pwv5(copy, window.byte_count, window.sample_count, decoded, SDKW_WINDOW_MAX) == SDKW_OK);
            for (uint32_t k = 0; k < window.sample_count; k++) {
                uint32_t i = 65535 + k;
                CHECK(decoded[k].red == (i + 1) % 8);
                CHECK(decoded[k].green == (i / 3 + 2) % 8 && decoded[k].blue == (i / 7 + 3) % 8);
                CHECK(decoded[k].height == (i * 3 + 4) % 32 && decoded[k].low_bits == (i / 11) % 4);
                CHECK(decoded[k].raw == (uint16_t) (copy[k * 2] * 256u + copy[k * 2 + 1]));
            }
        } else {
            struct sww_band_sample decoded[SDKW_WINDOW_MAX];
            CHECK(sww_decode_pwv7(copy, window.byte_count, window.sample_count, decoded, SDKW_WINDOW_MAX) == SDKW_OK);
            for (uint32_t k = 0; k < window.sample_count; k++) {
                uint32_t i = 65535 + k;
                CHECK(decoded[k].low == (uint8_t) (i * 5 + 1));
                CHECK(decoded[k].mid == (uint8_t) (i * 7 + 53));
                CHECK(decoded[k].high == (uint8_t) (i * 11 + 149) && decoded[k].reserved == 0);
            }
        }
        memset(copy, 0xa5, sizeof copy);
        memcpy(untouched, copy, sizeof copy);
        CHECK(sww_copy_window(&window, p + window.byte_offset, window.byte_count - 1, copy, sizeof copy) == SDKW_MALFORMED);
        CHECK(sww_copy_window(&window, p + window.byte_offset, window.byte_count, copy, window.byte_count - 1) == SDKW_LIMIT);
        CHECK(memcmp(copy, untouched, sizeof copy) == 0);
        CHECK(sww_plan_window(&index, 69999, SDKW_WINDOW_MAX, &window) == SDKW_OK);
        CHECK(window.sample_count == 1 && window.byte_count == stride && window.byte_offset == DATA + (uint64_t) 69999 * stride);
        CHECK(sww_copy_window(&window, p + window.byte_offset, stride, copy, sizeof copy) == SDKW_OK);
        CHECK(memcmp(copy, p + DATA + (size_t) 69999 * stride, stride) == 0);
        struct sww_window old;
        memcpy(&old, &window, sizeof old);
        CHECK(sww_plan_window(&index, UINT32_MAX, SDKW_WINDOW_MAX, &window) == SDKW_OUT_OF_RANGE);
        CHECK(sww_plan_window(&index, 0, UINT32_MAX, &window) == SDKW_LIMIT);
        CHECK(sww_plan_window(&index, 0, 0, &window) == SDKW_INVALID);
        CHECK(memcmp(&old, &window, sizeof old) == 0);
        struct sww_index forged = index;
        forged.entry_size = UINT32_MAX;
        CHECK(sww_plan_window(&forged, 150, 32, &window) == SDKW_INVALID);
        expect_detail_error(p, size, 99, SDKW_UNSUPPORTED);
        expect_detail_error(p, size, encoding == SDKW_ENCODING_PWV5 ? SDKW_ENCODING_PWV7 : SDKW_ENCODING_PWV5, SDKW_NOT_FOUND);
        expect_index_error(p, size, SDKW_NOT_FOUND); /* Mono helper still selects mono only. */
        free(p);
    }
}
static void test_encoded_bad_and_empty(void) {
    for (uint32_t encoding = SDKW_ENCODING_PWV5; encoding <= SDKW_ENCODING_PWV7; encoding++) {
        uint32_t stride = encoding == SDKW_ENCODING_PWV5 ? 2 : 3;
        size_t size;
        uint8_t *p = encoded_fixture(encoding, 4, &size);
        uint32_t canonical = encoding == SDKW_ENCODING_PWV5 ? 0x00960305u : 0x00960000u;
        put32(p + WAVE + 12, 0);
        expect_detail_error(p, size, encoding, SDKW_MALFORMED);
        put32(p + WAVE + 12, 1);
        put32(p + WAVE + 16, 4 * stride);
        expect_detail_error(p, size, encoding, SDKW_UNSUPPORTED);
        put32(p + WAVE + 12, stride);
        put32(p + WAVE + 16, UINT32_MAX);
        expect_detail_error(p, size, encoding, SDKW_MALFORMED);
        put32(p + WAVE + 16, 4);
        put32(p + WAVE + 20, canonical + 1);
        expect_detail_error(p, size, encoding, SDKW_UNSUPPORTED);
        put32(p + WAVE + 20, canonical);
        struct sww_file file;
        struct sww_tag tag;
        struct sww_index index = {0};
        CHECK(sww_file_header(p, 12, size, &file) == SDKW_OK);
        CHECK(sww_tag_header(p + WAVE, 12, &file, WAVE, &tag) == SDKW_OK);
        CHECK(sww_index_detail(p + WAVE, 23, &tag, encoding, &index) == SDKW_MALFORMED);
        put32(p + DATA + 4 * stride + 4, 0);
        expect_detail_error(p, size, encoding, SDKW_MALFORMED);
        put32(p + DATA + 4 * stride + 4, 12);
        /* Preserve a valid following envelope while making target bytes misaligned. */
        memmove(p + DATA + 4 * stride - 1, p + DATA + 4 * stride, 12);
        size--;
        put32(p + 8, (uint32_t) size);
        put32(p + WAVE + 8, 24 + 4 * stride - 1);
        expect_detail_error(p, size, encoding, SDKW_MALFORMED);
        free(p);
        p = encoded_fixture(encoding, 0, &size);
        CHECK(sww_find_detail(p, size, encoding, &index) == SDKW_EMPTY);
        struct sww_window window = {0};
        CHECK(sww_plan_window(&index, 0, 1, &window) == SDKW_EMPTY && window.byte_count == 0);
        CHECK(sww_copy_window(&window, NULL, 0, NULL, 0) == SDKW_EMPTY);
        CHECK(sww_plan_window(&index, 1, 1, &window) == SDKW_OUT_OF_RANGE);
        p = realloc(p, size + 12);
        CHECK(p != NULL);
        memcpy(p + size - 12, p + WAVE, 24);
        size += 12;
        put32(p + 8, (uint32_t) size);
        expect_detail_error(p, size, encoding, SDKW_AMBIGUOUS);
        free(p);
    }
}
static uint32_t oracle32(const uint8_t *p) {
    return (uint32_t) p[0] * 16777216u + (uint32_t) p[1] * 65536u + (uint32_t) p[2] * 256u + p[3];
}
/* Optional external files are read-only operator inputs; no asset bytes are
 * copied into source. This raw oracle never calls a sww_* layout/field helper. */
static void real_detail_probe(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long extent = ftell(f);
    CHECK(extent >= 12 && extent <= 16 * 1024 * 1024);
    CHECK(fseek(f, 0, SEEK_SET) == 0);
    size_t size = (size_t) extent;
    uint8_t *p = malloc(size);
    CHECK(p != NULL);
    CHECK(fread(p, 1, size, f) == size && fclose(f) == 0);
    CHECK(memcmp(p, "PMAI", 4) == 0 && oracle32(p + 8) == size);
    size_t offset = oracle32(p + 4);
    CHECK(offset >= 12 && offset <= size);
    unsigned found = 0;
    while (offset < size) {
        CHECK(size - offset >= 12);
        size_t header = oracle32(p + offset + 4), length = oracle32(p + offset + 8);
        CHECK(header >= 12 && header <= length && length <= size - offset);
        uint32_t encoding = !memcmp(p + offset, "PWV5", 4) ? 2 : !memcmp(p + offset, "PWV7", 4) ? 3
                                                                                                : 0;
        if (encoding) {
            CHECK(header == 24);
            uint32_t stride = encoding == 2 ? 2 : 3, total = oracle32(p + offset + 16);
            CHECK(oracle32(p + offset + 12) == stride);
            CHECK((length - header) % stride == 0 && total == (length - header) / stride && total > 150);
            CHECK(oracle32(p + offset + 20) == (encoding == 2 ? 0x00960305u : 0x00960000u));
            struct sww_index index = {0};
            CHECK(sww_find_detail(p, size, encoding, &index) == SDKW_OK);
            CHECK(index.tag.offset == offset && index.sample_count == total && index.entry_size == stride);
            uint32_t first[] = {0, 150, total / 2, total - 1};
            uint32_t requested[] = {4096, 32, 257, 4096};
            for (unsigned q = 0; q < 4; q++) {
                struct sww_window w = {0};
                CHECK(sww_plan_window(&index, first[q], requested[q], &w) == SDKW_OK);
                uint32_t count = total - first[q];
                if (count > requested[q]) count = requested[q];
                CHECK(w.byte_offset == offset + header + (uint64_t) first[q] * stride);
                CHECK(w.byte_count == (size_t) count * stride && w.sample_count == count);
                CHECK(w.start_s == first[q] / 150.0 && w.seconds_per_sample == 1.0 / 150.0);
                uint8_t copied[4096 * 3];
                CHECK(sww_copy_window(&w, p + w.byte_offset, w.byte_count, copied, sizeof copied) == SDKW_OK);
                CHECK(memcmp(copied, p + offset + header + (size_t) first[q] * stride, w.byte_count) == 0);
                if (encoding == 2) {
                    struct sww_rgb_sample decoded[4096];
                    CHECK(sww_decode_pwv5(copied, w.byte_count, count, decoded, 4096) == SDKW_OK);
                    for (uint32_t k = 0; k < count; k++) {
                        const uint8_t *raw = p + offset + header + (size_t) (first[q] + k) * 2;
                        uint16_t word = (uint16_t) (raw[0] * 256u + raw[1]);
                        CHECK(decoded[k].raw == word && decoded[k].red == word / 8192);
                        CHECK(decoded[k].green == word / 1024 % 8 && decoded[k].blue == word / 128 % 8);
                        CHECK(decoded[k].height == word / 4 % 32 && decoded[k].low_bits == word % 4);
                    }
                } else {
                    struct sww_band_sample decoded[4096];
                    CHECK(sww_decode_pwv7(copied, w.byte_count, count, decoded, 4096) == SDKW_OK);
                    for (uint32_t k = 0; k < count; k++) {
                        const uint8_t *raw = p + offset + header + (size_t) (first[q] + k) * 3;
                        CHECK(decoded[k].low == raw[0] && decoded[k].mid == raw[1] && decoded[k].high == raw[2]);
                    }
                }
            }
            found++;
        }
        offset += length;
    }
    CHECK(found != 0);
    printf("sdkwaveform: external raw oracle PASS (%u detail tags): %s\n", found, path);
    free(p);
}

int main(int argc, char **argv) {
    test_ranges();
    test_decode_bounds();
    test_bad_envelopes();
    test_empty_duplicate_and_limits();
    unsigned mono_checks = checks;
    CHECK(mono_checks == 473);
    test_color_decode_edges();
    test_encoded_ranges();
    test_encoded_bad_and_empty();
    for (int i = 1; i < argc; i++) real_detail_probe(argv[i]);
    printf("sdkwaveform: mono=%u total=%u checks PASS (6 deliberately broken controls rejected)\n", mono_checks, checks);
    return 0;
}
