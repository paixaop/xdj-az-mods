/* SPDX-License-Identifier: GPL-3.0-only */
#include "pvdi-windows-source.h"
#include "pvdi-windows-reduction.h"
#include "../../common/azsha256.h"

/* The shipped JUCE resource predates the EVP provider interface. */
#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/blowfish.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    blowfish_block_bytes = 8,
    windows_key_max_bytes = 56,
    encrypted_graph_bytes = 6758760,
    affine_group_columns = 4,
    affine_groups = (PVDI_WINDOWS_AFFINE_COUNT + affine_group_columns - 1) / affine_group_columns
};

struct windows_profile {
    const char *version;
    const char *executable_sha256;
    uint32_t executable_bytes;
    uint32_t fixed_version_offset;
    uint32_t graph_key_offset;
    uint32_t file_version_high, file_version_low;
};

struct network_layout {
    uint32_t chebyshev;
    uint32_t mean[affine_groups], deviation[affine_groups];
    uint32_t thresholds[TA_VOCAL_THRESHOLD_COUNT], output_class;
};

/* Each identity is from the corresponding AlphaTheta-signed Windows x64 PE.
 * All addresses refer to raw file offsets. */
static const struct windows_profile windows_profiles[] = {
    {"7.2.17.0", "ea1725a0eca7975cb7d132e8f0d5a9e1300dd32c6356e8f3ad6621e96f0e98a9",
     100441520, 0x5e4fe48, 0x3c5ae00, 0x00070002, 0x00110000},
    {"7.2.18.0", "a99896cf26d5998e6ad4177796a467b83df14bf8ae7207df21ed01251e402493",
     100561840, 0x5e6d248, 0x3c73c80, 0x00070002, 0x00120000},
    {"7.2.19.0", "c9ed23e974c51c75e2ec069fbd2679a3dbe606be9e79e9cbbd13d6418ab11c37",
     103540656, 0x613f848, 0x3e9ee78, 0x00070002, 0x00130000},
    {"7.2.20.0", "37d2b6a57770a42d9604d8872b1d8dae4f1fffa15483189a92ba58f9dd4de9ef",
     104436656, 0x6217848, 0x3f546a8, 0x00070002, 0x00140000}
};

static const char encrypted_graph_sha256[] =
    "38818f0970a5c763eb3d7557e67431dcc58f5739c6f79adbaf61ddceeb4a472a";
static const char graph_sha256[] =
    "4f1009aea543aaebd27a232d26de057780019e710a4c2075dd2e5ca9b01d4ef6";

#include "pvdi-windows-network-offsets.inc"

_Static_assert(sizeof chebyshev_operands / sizeof chebyshev_operands[0] == PVDI_WINDOWS_CHEBYSHEV_COUNT,
               "All Chebyshev coefficients require a native operand");
_Static_assert(sizeof windows_networks / sizeof windows_networks[0] ==
               sizeof windows_profiles / sizeof windows_profiles[0], "A coefficient layout is required for each build");

static bool supports_executable_size(long size)
{
    for (size_t i = 0; i < sizeof windows_profiles / sizeof windows_profiles[0]; i++) {
        if (size == (long)windows_profiles[i].executable_bytes)
            return true;
    }
    return false;
}

static long stream_size(FILE *file)
{
    if (fseek(file, 0, SEEK_END))
        return -1;
    long size = ftell(file);
    return fseek(file, 0, SEEK_SET) ? -1 : size;
}

static bool digest_matches(const uint8_t *data, size_t size, const char *expected)
{
    char actual[65];
    azsha256_hex(data, size, actual);
    return strcmp(actual, expected) == 0;
}

static bool read_exact(FILE *file, uint8_t *data, size_t size)
{
    return fread(data, 1, size, file) == size && fgetc(file) == EOF && !ferror(file);
}

static uint8_t *read_verified(FILE *file, size_t size, const char *sha256)
{
    uint8_t *data = malloc(size);
    if (data && read_exact(file, data, size) && digest_matches(data, size, sha256))
        return data;
    free(data);
    return NULL;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t read_le64(const uint8_t *data)
{
    return read_le32(data) | (uint64_t)read_le32(data + 4) << 32;
}

static void write_le32(uint8_t *data, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) {
        data[i] = (uint8_t)value;
        value >>= 8;
    }
}

static bool version_matches(const uint8_t *data, const struct windows_profile *profile)
{
    const uint8_t *value = data + profile->fixed_version_offset;
    return read_le32(value) == UINT32_C(0xfeef04bd) &&
           read_le32(value + 8) == profile->file_version_high &&
           read_le32(value + 12) == profile->file_version_low;
}

static const struct windows_profile *verify_executable(const uint8_t *data, size_t size)
{
    char sha256[65];
    azsha256_hex(data, size, sha256);
    for (size_t i = 0; i < sizeof windows_profiles / sizeof windows_profiles[0]; i++) {
        const struct windows_profile *profile = &windows_profiles[i];
        if (size == profile->executable_bytes && strcmp(sha256, profile->executable_sha256) == 0 &&
            version_matches(data, profile))
            return profile;
    }
    return NULL;
}

static const struct windows_profile *read_executable(FILE *file, long size, uint8_t **data)
{
    if (!supports_executable_size(size))
        return NULL;
    *data = malloc((size_t)size);
    if (!*data || !read_exact(file, *data, (size_t)size))
        return NULL;
    return verify_executable(*data, (size_t)size);
}

static enum pvdi_windows_status load_executable(FILE *file, struct pvdi_windows_source *source,
                                                const struct windows_profile **selected)
{
    long size = stream_size(file);
    if (size < 0)
        return PVDI_WINDOWS_EXECUTABLE_IO;
    uint8_t *data = NULL;
    const struct windows_profile *profile = read_executable(file, size, &data);
    if (!profile) {
        free(data);
        return PVDI_WINDOWS_UNSUPPORTED_EXECUTABLE;
    }
    source->version = profile->version;
    source->executable = data;
    source->executable_bytes = profile->executable_bytes;
    *selected = profile;
    return PVDI_WINDOWS_OK;
}

static bool decrypt_graph(uint8_t *resource, const uint8_t *key)
{
    /* The loader supplies the UTF-8 literal length, excluding its terminator. */
    const uint8_t *end = memchr(key, 0, windows_key_max_bytes + 1);
    if (end == NULL || end == key)
        return false;
    BF_KEY schedule;
    BF_set_key(&schedule, (int)(end - key), key);
    for (size_t i = 0; i < encrypted_graph_bytes; i += blowfish_block_bytes) {
        /* JUCE applies Blowfish to two little-endian native words per block. */
        BF_LONG block[] = {read_le32(resource + i), read_le32(resource + i + 4)};
        BF_decrypt(block, &schedule);
        write_le32(resource + i, block[0]);
        write_le32(resource + i + 4, block[1]);
    }
    return true;
}

static bool clear_graph_matches(const uint8_t *graph)
{
    enum { padding = encrypted_graph_bytes - PVDI_WINDOWS_GRAPH_BYTES };
    _Static_assert(padding == 2, "Check the complete graph padding");
    return graph[PVDI_WINDOWS_GRAPH_BYTES] == padding &&
           graph[PVDI_WINDOWS_GRAPH_BYTES + 1] == padding &&
           digest_matches(graph, PVDI_WINDOWS_GRAPH_BYTES, graph_sha256);
}

static uint8_t *load_graph(FILE *file, const uint8_t *key)
{
    uint8_t *resource = read_verified(file, encrypted_graph_bytes, encrypted_graph_sha256);
    if (resource && decrypt_graph(resource, key) && clear_graph_matches(resource))
        return resource;
    free(resource);
    return NULL;
}

static double fp64(const uint8_t *bytes)
{
    uint64_t bits = read_le64(bytes);
    double result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

static double fp32(const uint8_t *bytes)
{
    uint32_t bits = read_le32(bytes);
    float result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

static double i32(const uint8_t *bytes)
{
    return (int32_t)read_le32(bytes);
}

static double i8(const uint8_t *bytes)
{
    return (int8_t)*bytes;
}

static double u8(const uint8_t *bytes)
{
    return *bytes;
}

static double u16(const uint8_t *bytes)
{
    return bytes[0] | (uint32_t)bytes[1] << 8;
}

static double power_of_two(const uint8_t *bytes)
{
    return ldexp(1.0, *bytes);
}

static double xor_zero(const uint8_t *bytes)
{
    return memcmp(bytes, "\x33\xd2", 2) == 0 ? 0.0 : NAN;
}

static double decrement(const uint8_t *bytes)
{
    return memcmp(bytes, "\xff\xc8", 2) == 0 ? 1.0 : NAN;
}

static double half(const uint8_t *bytes)
{
    return memcmp(bytes, "\xd1\xf8", 2) == 0 ? 2.0 : NAN;
}

static double lea_twice(const uint8_t *bytes)
{
    return memcmp(bytes, "\x8d\x14\x1b", 3) == 0 ? 2.0 : NAN;
}

static bool is_scalar_add_same(const uint8_t *bytes)
{
    unsigned first = ((bytes[1] ^ 0xffu) >> 3) & 15u;
    unsigned second = bytes[3] & 7;
    return bytes[0] == 0xc5 && bytes[2] == 0x58 &&
           (bytes[3] & 0xc0) == 0xc0 && first == second;
}

static double add_same(const uint8_t *bytes)
{
    return memcmp(bytes, "\x03\xc0", 2) == 0 || is_scalar_add_same(bytes) ? 2.0 : NAN;
}

static double span_times_reciprocal(double span)
{
    return span * (1.0 / span);
}

static double damping_from_pole_angle(double start, double step, unsigned stage)
{
    double value = cos(start + stage * step);
    return value + value;
}

#define WIN_OFFSET(...) ((const uint32_t[]){__VA_ARGS__}[build])

static void copy_vocal_operands(struct pvdi_windows_vocal *parameters, const uint8_t *pe,
                                size_t build)
{
#define VOCAL_OPERAND(field, expression) \
    parameters->value[field] = (expression); \
    parameters->present[field] = true;
#include "pvdi-windows-vocal-offsets.inc"
#undef VOCAL_OPERAND
}

static double sib_scale(const uint8_t *bytes)
{
    return (bytes[0] & 0x3f) == 5 ? (double)(1u << (bytes[0] >> 6)) : NAN;
}

static double x64_index_scale(const uint8_t *bytes)
{
    return (double)(1u << (bytes[0] >> 6));
}

static double x64_instruction_value(const uint8_t *bytes, const char *opcode, size_t size, double value)
{
    return memcmp(bytes, opcode, size) == 0 ? value : NAN;
}

static double beat_adjacency_mask(const uint8_t *mask_bytes, const uint8_t *target_byte,
                                  const uint8_t *window_bytes)
{
    int32_t window = (int32_t)read_le32(window_bytes);
    if (window < 1 || window > UINT8_MAX)
        return NAN;
    uint32_t source_mask = read_le32(mask_bytes);
    uint32_t mask = source_mask & INT32_MAX;
    uint32_t target = (uint32_t)-(int8_t)*target_byte;
    for (uint32_t distance = 1; distance <= (uint32_t)window; ++distance) {
        if ((((distance - target) & source_mask) == 0) != ((distance & mask) == target))
            return NAN;
    }
    return mask;
}

static double opening_score_bit(const uint8_t *bar, const uint8_t *first, const uint8_t *late)
{
    if (*bar < *first || *bar >= *late || *bar - *first >= 32)
        return NAN;
    return (double)(UINT32_C(1) << (*bar - *first));
}

static double rhythm_run_role_mask(const uint8_t *mask, const uint8_t *extra_role)
{
    if (*extra_role >= 64)
        return NAN;
    return (double)(read_le64(mask) | UINT64_C(1) << *extra_role);
}

static uint64_t rhythm_role_range(const uint8_t *negative_base, const uint8_t *span)
{
    int first = -(int8_t)*negative_base;
    if (first < 0 || first + *span >= 64)
        return 0;
    return (UINT64_MAX >> (63 - *span)) << first;
}

static double simd_zero(const uint8_t *bytes)
{
    return memcmp(bytes, "\xc4\x41\x39\x57\xc0", 5) == 0 ? 0.0 : NAN;
}

static double double_period(const uint8_t *bytes)
{
    /* vaddsd xmm10, xmm10, xmm10 */
    return memcmp(bytes, "\xc4\xc1\x2b\x58\xd2", 5) == 0 ? 2.0 : NAN;
}

static double single_filter_call(const uint8_t *pe, uint32_t call, uint32_t target)
{
    return pe[call] == 0xe8 && call + 5 + read_le32(pe + call + 1) == target ? 1.0 : NAN;
}

static double filter_calls(const uint8_t *pe, uint32_t first)
{
    /* The four consecutive reset calls must point to the same function. */
    static const unsigned call_offsets[] = {0, 0x15, 0x30, 0x4b};
    uint32_t expected_target = first + 5 + read_le32(pe + first + 1);
    size_t count = sizeof call_offsets / sizeof call_offsets[0];
    for (size_t i = 0; i < count; ++i) {
        uint32_t at = first + call_offsets[i];
        if (pe[at] != 0xe8 || at + 5 + read_le32(pe + at + 1) != expected_target)
            return NAN;
    }
    return (double)count;
}

static void copy_phrase_operands(struct pvdi_windows_phrase *parameters, const uint8_t *pe, size_t build)
{
#define PHRASE_OPERAND(field, expression) \
    parameters->value[field] = (expression); \
    parameters->present[field] = true;
#include "pvdi-windows-phrase-offsets.inc"
#undef PHRASE_OPERAND
}

#undef WIN_OFFSET

static bool operands_valid(const double *values, const bool *present, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (present[i] && !isfinite(values[i]))
            return false;
    }
    return true;
}

static void copy_normalization(struct pvdi_windows_network *network, const struct network_layout *layout,
                               const uint8_t *pe)
{
    for (size_t column = 0; column < PVDI_WINDOWS_AFFINE_COUNT; ++column) {
        size_t group = column / affine_group_columns;
        size_t offset = sizeof(double) * (column % affine_group_columns);
        double mean = fp64(pe + layout->mean[group] + offset);
        double deviation = fp64(pe + layout->deviation[group] + offset);
        network->scale[column] = 1.0 / deviation;
        network->offset[column] = -mean * network->scale[column];
    }
}

static bool copy_network_operands(struct pvdi_windows_network *network, const struct network_layout *layout,
                                  const uint8_t *pe)
{
    unsigned class_byte = pe[layout->output_class];
    if (class_byte % sizeof(float) != 0)
        return false;
    network->output_class = class_byte / sizeof(float);
    for (size_t i = 0; i < TA_VOCAL_THRESHOLD_COUNT; ++i)
        network->thresholds[i] = (float)fp32(pe + layout->thresholds[i]);
    for (size_t i = 0; i < PVDI_WINDOWS_CHEBYSHEV_COUNT; ++i)
        network->window[i] = fp64(pe + layout->chebyshev + chebyshev_operands[i]);
    copy_normalization(network, layout, pe);
    return true;
}

static bool load_vocal_operands(struct pvdi_windows_source *source, size_t build)
{
    size_t motion_size;
    struct pvdi_windows_vocal *vocal = &source->vocal;
    copy_vocal_operands(vocal, source->executable, build);
    return operands_valid(vocal->value, vocal->present, TA_VOCAL_PARAMETER_COUNT) &&
           pvdi_windows_motion_extract(source->executable, source->executable_bytes, (unsigned)build,
                                       vocal->motion, sizeof vocal->motion, &motion_size) &&
           motion_size == sizeof vocal->motion;
}

static bool load_native_operands(struct pvdi_windows_source *source, size_t build)
{
    copy_phrase_operands(&source->phrase, source->executable, build);
    return load_vocal_operands(source, build) &&
           operands_valid(source->phrase.value, source->phrase.present, TA_PHRASE_PARAMETER_COUNT) &&
           pvdi_windows_reduction_extract(source->executable, source->executable_bytes, build,
                                          source->phrase.reduction) &&
           copy_network_operands(&source->network, &windows_networks[build], source->executable);
}

enum pvdi_windows_status pvdi_windows_load(FILE *executable, FILE *encrypted_graph,
                                          struct pvdi_windows_source *source)
{
    *source = (struct pvdi_windows_source){0};
    const struct windows_profile *profile;
    enum pvdi_windows_status status = load_executable(executable, source, &profile);
    if (status != PVDI_WINDOWS_OK)
        return status;
    if (!load_native_operands(source, (size_t)(profile - windows_profiles))) {
        pvdi_windows_release(source);
        return PVDI_WINDOWS_INVALID_OPERAND;
    }
    source->graph = load_graph(encrypted_graph, source->executable + profile->graph_key_offset);
    if (source->graph)
        return PVDI_WINDOWS_OK;
    pvdi_windows_release(source);
    return PVDI_WINDOWS_GRAPH_MISMATCH;
}

void pvdi_windows_release(struct pvdi_windows_source *source)
{
    free(source->executable);
    free(source->graph);
    *source = (struct pvdi_windows_source){0};
}
