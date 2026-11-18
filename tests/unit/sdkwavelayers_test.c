/* SPDX-License-Identifier: GPL-3.0-only */
/* Explicit native-branch schema tables; no mirrored sort/encoder model.
 * Only bytes0..4 are defined per stride6. Native malloc byte5 is not a value.
 * Native leaf oracle decodes H only: not a native encoder/renderer/calibration. */
#include "../../src/azmod/sdk_waveform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x, why)                                                                 \
    do {                                                                                \
        if (!(x)) {                                                                     \
            fprintf(stderr,                                                             \
                    "FAIL wave-layers: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                    \
        }                                                                               \
    } while (0)

#if defined(SDKW_LAYERED3_VERSION)
static int encode(const uint8_t *raw, size_t available, uint32_t count, uint8_t *out, size_t bytes) {
    return sww_encode_pwv7_layers(raw, available, count, out, bytes);
}
#else
static int encode(const uint8_t *, size_t, uint32_t, uint8_t *, size_t) { return SDKW_UNSUPPORTED; }
#endif
#if defined(__aarch64__)
uint32_t sww_layer_native_id_130(const uint8_t *, uint32_t);
uint32_t sww_layer_native_id_200(const uint8_t *, uint32_t);
#endif

unsigned profile;
struct Case {
    uint8_t raw[3];
    uint16_t header;
    uint8_t levels[3];
};
/* Exact branch constants from layered-record-contract-v1. Values stay raw;
 * channel A/B/C label bytes0/1/2, with no new physical frequency semantics. */
const struct Case cases[] = {
    {{255, 128, 1}, 0x3863, {255, 128, 1}}, {{255, 1, 128}, 0x3853, {255, 128, 1}}, {{128, 255, 1}, 0x3862, {255, 128, 1}}, {{1, 255, 128}, 0x3842, {255, 128, 1}}, {{128, 1, 255}, 0x3851, {255, 128, 1}}, {{1, 128, 255}, 0x3841, {255, 128, 1}}, {{254, 127, 0}, 0x2063, {254, 127, 0}}, {{254, 0, 127}, 0x2053, {254, 127, 0}}, {{127, 254, 0}, 0x2062, {254, 127, 0}}, {{0, 254, 127}, 0x2042, {254, 127, 0}}, {{127, 0, 254}, 0x2051, {254, 127, 0}}, {{0, 127, 254}, 0x2041, {254, 127, 0}}, {{255, 255, 1}, 0x2086, {255, 1, 0}}, {{128, 127, 128}, 0x2085, {128, 127, 0}}, {{1, 254, 254}, 0x2084, {254, 1, 0}}, {{255, 128, 128}, 0x2083, {255, 128, 0}}, {{127, 128, 127}, 0x2082, {128, 127, 0}}, {{1, 1, 254}, 0x2081, {254, 1, 0}}, {{128, 128, 0}, 0x1006, {128, 0, 0}}, {{254, 0, 254}, 0x1005, {254, 0, 0}}, {{0, 255, 255}, 0x1004, {255, 0, 0}}, {{255, 0, 0}, 0x1003, {255, 0, 0}}, {{0, 127, 0}, 0x1002, {127, 0, 0}}, {{0, 0, 1}, 0x1001, {1, 0, 0}}, {{0, 0, 0}, 0x1008, {0, 0, 0}}, {{1, 1, 1}, 0x1008, {1, 0, 0}}, {{127, 127, 127}, 0x1008, {127, 0, 0}}, {{128, 128, 128}, 0x1008, {128, 0, 0}}, {{254, 254, 254}, 0x1008, {254, 0, 0}}, {{255, 255, 255}, 0x1008, {255, 0, 0}}};
const size_t case_count = sizeof cases / sizeof cases[0];
uint16_t header(const uint8_t *p) { return (uint16_t) (p[0] | ((uint16_t) p[1] << 8)); }
#if defined(__aarch64__)
uint32_t native_id(const uint8_t *record, unsigned layer) {
    REQUIRE(layer < 3, "oracle caller supplies an owned record and layer0..2");
    return profile == 1 ? sww_layer_native_id_130(record, layer) : sww_layer_native_id_200(record, layer);
}
#endif
void baseline() {
    struct sww_band_sample decoded[2] = {0};
    const uint8_t raw[] = {255, 128, 1, 0, 254, 127};
    REQUIRE(sww_decode_pwv7(raw, sizeof raw, 2, decoded, 2) == SDKW_OK &&
                decoded[0].low == 255 && decoded[0].mid == 128 && decoded[0].high == 1 && !decoded[0].reserved &&
                decoded[1].low == 0 && decoded[1].mid == 254 && decoded[1].high == 127 && !decoded[1].reserved,
            "existing real PWV7 decoder preserves original byte triples");
    unsigned leaf = 0;
#if defined(__aarch64__)
    const uint8_t record[] = {0x63, 0x38, 255, 128, 1, 0xcd};
    uint8_t before[6];
    memcpy(before, record, 6);
    REQUIRE(native_id(record, 0) == 3 && native_id(record, 1) == 6 && native_id(record, 2) == 8 &&
                !memcmp(record, before, 6),
            "four original native decoder words execute read-only before encoder requirement");
    leaf = 1;
#endif
    printf("WAVE_LAYER_BASELINE_OK profile=%u old_decoder=1 native_leaf=%u\n", profile, leaf);
    fflush(stdout);
}
void batch(unsigned context) {
    uint8_t raw[case_count * 3], output[case_count * 6 + 16];
    for (size_t i = 0; i < case_count; ++i) memcpy(raw + i * 3, cases[i].raw, 3);
    for (size_t i = 0; i < sizeof output; ++i) output[i] = (uint8_t) (0x21 + context * 17 + i * 13);
    uint8_t before[sizeof output], input[sizeof raw];
    memcpy(before, output, sizeof before);
    memcpy(input, raw, sizeof input);
    uint8_t *records = output + 8;
    const int rc = encode(raw, sizeof raw, (uint32_t) (case_count), records, case_count * 6);
    REQUIRE(!memcmp(output, before, 8) &&
                !memcmp(output + 8 + case_count * 6, before + 8 + case_count * 6, 8) && !memcmp(raw, input, sizeof raw),
            "encoder preserves input and output extent guards");
    REQUIRE(rc == SDKW_OK && header(records) == cases[0].header &&
                !memcmp(records + 2, cases[0].levels, 3),
            "missing exact raw three-band native layered encoding");
    for (size_t i = 0; i < case_count; ++i) {
        const uint8_t *record = records + i * 6;
        REQUIRE(header(record) == cases[i].header && !memcmp(record + 2, cases[i].levels, 3),
                "explicit six-order/tie/zero branch table matches the five defined bytes");
        REQUIRE(record[5] == before[8 + i * 6 + 5], "each caller padding byte5 survives; no malloc padding value is predicted");
#if defined(__aarch64__)
        for (unsigned layer = 0; layer < (unsigned) (cases[i].header >> 12); ++layer)
            REQUIRE(native_id(record, layer) == ((cases[i].header >> (layer * 4)) & 15u),
                    "audited per-build decoder leaf reads generated H layer IDs");
#endif
    }
}
void statuses_and_window() {
    const uint8_t raw[] = {255, 128, 1, 0, 254, 127};
    uint8_t out[24];
    memset(out, 0xa5, sizeof out);
    uint8_t before[sizeof out];
    memcpy(before, out, sizeof out);
#define unchanged(rc, expected, why) REQUIRE((rc) == (expected) && !memcmp(out, before, sizeof out), why)
    unchanged(encode(NULL, 0, 0, NULL, 0), SDKW_EMPTY, "zero count follows SDKW_EMPTY without pointer access");
    unchanged(encode(NULL, 6, 1, out, 6), SDKW_INVALID, "nonempty null input follows SDKW_INVALID unchanged");
    unchanged(encode(raw, 6, 1, NULL, 6), SDKW_INVALID, "nonempty null output follows SDKW_INVALID unchanged");
    unchanged(encode(raw, 2, 1, out, 6), SDKW_MALFORMED, "short input follows SDKW_MALFORMED unchanged");
    unchanged(encode(raw, 5, 2, out, 12), SDKW_MALFORMED, "all input validation precedes first record writes");
    unchanged(encode(raw, 6, 1, out, 5), SDKW_LIMIT, "output byte capacity includes complete stride6");
    unchanged(encode(raw, 6, 2, out, 11), SDKW_LIMIT, "all output validation precedes first record writes");
    unchanged(encode(raw, 0, SDKW_WINDOW_MAX + 1, out, 0), SDKW_LIMIT, "count beyond SDKW window quota refuses before reads");
    unchanged(encode(raw, 0, UINT32_MAX, out, 0), SDKW_LIMIT, "extreme count refuses before arithmetic or reads");
    uint8_t input[SDKW_WINDOW_MAX * 3], output[SDKW_WINDOW_MAX * 6 + 12];
    memset(input, 128, sizeof input);
    memset(output, 0x6d, sizeof output);
    REQUIRE(encode(input, sizeof input, SDKW_WINDOW_MAX, output, SDKW_WINDOW_MAX * 6) == SDKW_OK,
            "maximum SDKW observation window is supported without changing the public quota");
    for (size_t i = 0; i < SDKW_WINDOW_MAX; ++i)
        REQUIRE(header(output + i * 6) == 0x1008 && output[i * 6 + 2] == 128 && !output[i * 6 + 3] && !output[i * 6 + 4] && output[i * 6 + 5] == 0x6d,
                "maximum window retains raw128 equal-layer schema and each caller byte5");
    for (size_t i = SDKW_WINDOW_MAX * 6; i < sizeof output; ++i) REQUIRE(output[i] == 0x6d, "bytes beyond requested records remain unchanged");
    // Input/output non-overlap is an explicit caller precondition, not a new
    // liveness/alias detector or a behavior tested with overlapping arrays.
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (!strcmp(argv[1], "1") || !strcmp(argv[1], "2")), "separate fixture profiles1/2");
    profile = (unsigned) (argv[1][0] - '0');
    baseline();
    for (unsigned context = 0; context < 4; ++context) batch(context);
    statuses_and_window();
    printf("PASS wave-layers profile=%u: raw schema only, four contexts, five defined bytes, bounded SDKW status behavior\n", profile);
    return 0;
}
