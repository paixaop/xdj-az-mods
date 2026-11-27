/* SPDX-License-Identifier: GPL-3.0-only */
#include "pvdi-model.h"
#include "../../common/azsha256.h"
#include "../model-format.h"
#ifdef PVDI_WINDOWS_IMPORT
#include "pvdi-windows-source.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    double_bytes = 8,
    header_bytes = TA_MODEL_HEADER_BYTES,
    model_bytes = TA_MODEL_BYTES
};

struct signature {
    const char *sha256;
    uint32_t size;
};

struct file_range {
    uint32_t offset, length;
};

struct graph_tensor {
    struct file_range content, shape;
};

struct graph_cell {
    uint32_t split_count;
    struct file_range gates[TA_VOCAL_GATE_ROLES];
    uint32_t initial_cell, initial_hidden;
};

struct graph_branch {
    struct file_range name, operation, selected_cell;
};

struct graph_topology {
    struct file_range inputs[TA_VOCAL_RECURRENT_CELL_COUNT];
    struct file_range hidden[TA_VOCAL_RECURRENT_CELL_COUNT];
    struct file_range frame_readers[TA_VOCAL_RECURRENT_CELL_COUNT];
    struct graph_branch outputs[TA_VOCAL_RECURRENT_CELL_COUNT];
    struct file_range concat_inputs;
};

struct graph_layout {
    struct signature file;
    struct graph_tensor tensors[TA_VOCAL_MATRIX_COUNT][2];
    uint32_t forget_bias[TA_VOCAL_RECURRENT_CELL_COUNT];
    struct graph_cell cells[TA_VOCAL_RECURRENT_CELL_COUNT];
    uint32_t feature_columns;
    struct graph_topology topology;
};

enum instruction_constant_kind {
    MOVE_HALFWORDS, LOGICAL_MASK, FLOAT_IMMEDIATE, COMPARE_IMMEDIATE, DOUBLE_LOAD_INDEX, MOVE_INTEGER, DOUBLE_TABLE, SUB_IMMEDIATE,
    ADD_IMMEDIATE, WORD_TABLE, ADD_DOUBLE_INDEX, LSL_SCALE, MOVE_NEGATIVE_DISPLACEMENT,
    CONDITIONAL_COMPARE_IMMEDIATE, LSR_SCALE, ALIGN_DOWN_POWER, FLOAT_MINIMUM_INPUT_COUNT, DOUBLE_ZERO,
    SIGNED_BYTE_LOAD_OFFSET, CONDITIONAL_ONE_FALLBACK, ORR_SLOT_INDEX, BASE_BYTE_LOAD_INDEX,
    DOUBLE_PAIR_LOAD_INDEX, WORD_LOAD_INDEX, BYTE_LOAD_INDEX, SUBTRACT_FLAGS_IMMEDIATE,
    ASR_SCALE, LSL_WIDE_SCALE, COMPARE_NEGATIVE_IMMEDIATE, DOUBLE_UNSCALED_BACK_INDEX,
    SIGNED_WORD_SHIFT_SCALE, ADD_ASR_SCALE, BYTE_ZERO_STORE, DOUBLE_PAIR_IMMEDIATE, DOUBLE_PAIR_ZERO,
    SELECT_INVERT_BIAS, REPEATED_CALL_COUNT, WORD_ZERO_STORE, SELECT_INCREMENT,
    FLOAT_WORD_PAIR_INDEX, FLOAT_WORD_UNSCALED_BACK, FLOAT_WORD_TABLE,
    FLOAT_WORD_IMMEDIATE, FLOAT_WORD_MOVI_BYTE_24, ADD_SHIFTED_DOUBLE_INDEX, FLOAT_WORD_MOVE_BITS,
    DOUBLE_POSTINDEX_ZERO_STORE, MATCHING_CALL_COUNT, DOUBLE_SELF_ADD_COUNT, ADD_LSL_SCALE,
    SIGNED_WORD_UNSCALED_BACK_INDEX, SUB_DOUBLE_INDEX, LSR_WIDE_SCALE, WORD_UNSCALED_BACK_INDEX,
    FLOAT_COMPARE_ZERO, WORD_PAIR_STORE_FIRST_INDEX, WORD_PAIR_STORE_SECOND_INDEX, LOW_MASK_PERIOD,
    DOUBLE_PAIR_LOAD_NEXT_INDEX, CONDITIONAL_SET_ZERO, ADD_WORD_INDEX
};

struct instruction_constant {
    uint32_t offset;
    unsigned instructions;
    enum instruction_constant_kind kind;
};

struct rhythm_mask_source {
    unsigned count;
    struct instruction_constant slots[4];
};

struct score_dispatch_source {
    uint32_t table_offset, branch_base, default_target;
    uint32_t weighted_targets[TA_OPENING_SCORE_MASK_COUNT];
};

struct high_export_dispatch_source {
    uint32_t table_offset, branch_base;
    uint32_t targets[TA_HIGH_EXPORT_HANDLER_COUNT];
};

struct delta_tap_source {
    uint32_t after_load, before_load, difference;
};

struct vocal_delta_source {
    struct delta_tap_source taps[3];
    uint32_t second_sum, inner_sum;
};

struct vocal_reflection_source {
    uint32_t group_start, pair_load, reverse, rotate;
    uint32_t right_origin, right_lane;
};

struct motion_reduction_source {
    uint32_t pair_loads, accumulators, reduction, seed_load;
};

struct cqt_reduction_source {
    uint32_t first_load, second_load, real_fma, imag_fma, real_sum, imag_sum;
};

struct phrase_sum_source {
    uint32_t first_load, second_load, accumulators, width, reduction;
};

struct recurrence_reduction_source {
    uint32_t first_load, second_load, accumulators, reduction;
};

struct opening_history_ratio_source {
    uint32_t origin_store, ratio_load;
};

struct pvdi_profile {
    const char *bundle_version;
    struct signature arm64;
    const struct graph_layout *graph;
    uint32_t window_offset, normalization_offset;
    uint32_t final_scale_offset, final_bias_offset;
    struct instruction_constant phrase[TA_PHRASE_OPERAND_COUNT];
    struct rhythm_mask_source rhythm_masks[TA_RHYTHM_MASK_COUNT];
    struct score_dispatch_source opening_score_dispatch;
    struct high_export_dispatch_source high_export_dispatch;
    struct cqt_reduction_source cqt_reduction;
    struct phrase_sum_source phrase_sum, phrase_range_sum;
    struct recurrence_reduction_source recurrence_reduction;
    struct opening_history_ratio_source opening_low_history_ratio, opening_mid_history_ratio;
    struct instruction_constant vocal[TA_VOCAL_DELTA_FIRST_RADIUS];
    struct vocal_delta_source vocal_delta;
    struct vocal_reflection_source vocal_reflection;
    struct motion_reduction_source motion_reduction;
    uint32_t vocal_thresholds[TA_VOCAL_THRESHOLD_COUNT];
    uint32_t vocal_class_address;
};

/* TensorProto tensor_content and TensorShape ranges, in AZPVDI01 order.
 * Versions that ship the identical GraphDef can share this layout. */
static const struct graph_layout graph_7219 = {
    .file = {"4f1009aea543aaebd27a232d26de057780019e710a4c2075dd2e5ca9b01d4ef6", 6758758},
    .tensors = {
        {{{0x2627e, 1167360}, {0x26270, 10}}, {{0x1434b6, 4096}, {0x1434ae, 5}}},
        {{{0x1457fa, 2097152}, {0x1457eb, 10}}, {{0x345a32, 4096}, {0x345a2a, 5}}},
        {{{0x34ec96, 1167360}, {0x34ec88, 10}}, {{0x46bece, 4096}, {0x46bec6, 5}}},
        {{{0x46e212, 2097152}, {0x46e203, 10}}, {{0x66e44a, 4096}, {0x66e442, 5}}},
        {{{0x17d, 131072}, {0x170, 9}}, {{0x20247, 256}, {0x2023c, 8}}},
        {{{0x20420, 512}, {0x20415, 8}}, {{0x206e5, 8}, {0x206db, 8}}}
    },
    /* FP32 TensorProto float_val fields for basic_lstm_cell/Const_2. */
    .forget_bias = {0x144f2b, 0x3474c4, 0x46d943, 0x66fedc},
    /* Split.num_split and input suffixes on Sigmoid_1, Tanh, Add and Sigmoid_2.
     * The last two offsets are FP32 literals feeding the cell and hidden Fill. */
    .cells = {
        {0x144e94, {{0x145268, 5}, {0x145315, 7}, {0x144fd9, 7}, {0x145672, 7}}, 0x2162e, 0x21cda},
        {0x34742d, {{0x347801, 5}, {0x3478ae, 7}, {0x347572, 7}, {0x347c0b, 7}}, 0x2238c, 0x22a52},
        {0x46d8ac, {{0x46dc80, 5}, {0x46dd2d, 7}, {0x46d9f1, 7}, {0x46e08a, 7}}, 0x34a046, 0x34a6f2},
        {0x66fe45, {{0x670219, 5}, {0x6702c6, 7}, {0x66ff8a, 7}, {0x670623, 7}}, 0x34ada4, 0x34b46a}
    },
    .feature_columns = 0x45, /* InputBatch.shape.dim[1].size */
    .topology = {
        /* First input of each BasicLSTMCell concat, and name of each Mul_2 hidden output. */
        .inputs = {{0x144825, 47}, {0x346da1, 76}, {0x46d23d, 47}, {0x66f7b9, 76}},
        .hidden = {{0x145687, 76}, {0x347c20, 76}, {0x46e09f, 76}, {0x670638, 76}},
        .frame_readers = {{0x25fab, 47}, {0x34e9c3, 47}},
        /* Output name, op, and the valid-frame Select input consumed by TensorArrayWrite. */
        .outputs = {
            {{0x349358, 35}, {0x34937d, 9}, {0x347ed0, 76}},
            {{0x671e19, 15}, {0x671e2a, 15}, {0x6708e8, 76}}
        },
        /* All three consecutive ConcatV2 input fields, including the trailing axis. */
        .concat_inputs = {0x671eea, 67}
    }
};

/* Index by the full CFBundleVersion. These are offsets into the thin Mach-O. */
static const struct pvdi_profile profiles[] = {
    {
        .bundle_version = "7.2.19.0342",
        .arm64 = {"2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907", 135191888},
        .graph = &graph_7219,
#define MAC_OFFSET(v7219, v7220) v7219
#include "pvdi-macos-offsets.inc"
#undef MAC_OFFSET
    },
    {
        .bundle_version = "7.2.20.0354",
        .arm64 = {"27c0764ed4a465333bdc43bf212840c7447d585199265ba459eeed5296ddd37a", 135824464},
        .graph = &graph_7219,
#define MAC_OFFSET(v7219, v7220) v7220
#include "pvdi-macos-offsets.inc"
#undef MAC_OFFSET
    }
};

const struct pvdi_profile *pvdi_macos_profile_for_version(const char *version)
{
    for (size_t i = 0; i < sizeof profiles / sizeof profiles[0]; ++i)
        if (strcmp(version, profiles[i].bundle_version) == 0)
            return &profiles[i];
    fprintf(stderr, "Unsupported rekordbox build %s. Supported builds:", version);
    for (size_t i = 0; i < sizeof profiles / sizeof profiles[0]; ++i)
        fprintf(stderr, " %s", profiles[i].bundle_version);
    fputc('\n', stderr);
    return NULL;
}

static bool matches_sha256(const uint8_t *data, const struct signature *signature)
{
    char hex[65];
    azsha256_hex(data, signature->size, hex);
    return strcmp(hex, signature->sha256) == 0;
}

static uint8_t *read_verified(const char *path, struct signature signature, const char *version)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return NULL;
    }
    uint8_t *data = malloc(signature.size);
    bool complete = data && fread(data, 1, signature.size, file) == signature.size &&
                    fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (complete && matches_sha256(data, &signature))
        return data;
    fprintf(stderr, "Input does not match the verified rekordbox %s file: %s\n", version, path);
    free(data);
    return NULL;
}

static bool contains_range(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

static void write_le(uint8_t *output, uint64_t value, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        output[i] = (uint8_t)value;
        value >>= 8;
    }
}

static uint32_t read_instruction(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t move_immediate(uint32_t instruction)
{
    unsigned shift = ((instruction >> 21) & 3) * 16;
    return (uint64_t)((instruction >> 5) & UINT32_C(0xffff)) << shift;
}

static bool apply_move_patches(const uint8_t *instructions, unsigned count, uint64_t *value)
{
    uint32_t expected = UINT32_C(0x72800000) | (read_instruction(instructions) & UINT32_C(0x8000001f));
    for (unsigned i = 1; i < count; ++i) {
        uint32_t instruction = read_instruction(instructions + i * 4);
        if ((instruction & UINT32_C(0xff80001f)) != expected)
            return false;
        unsigned shift = ((instruction >> 21) & 3) * 16;
        *value = (*value & ~(UINT64_C(0xffff) << shift)) | move_immediate(instruction);
    }
    return true;
}

/* MOVZ or MOVN followed by MOVK patches to the same register. */
static bool read_move_constant(const uint8_t *arm64, size_t size, uint32_t offset,
                               unsigned count, uint64_t *value)
{
    if (!contains_range(size, offset, count * 4))
        return false;
    const uint8_t *instructions = arm64 + offset;
    uint32_t first = read_instruction(instructions);
    uint32_t operation = first & UINT32_C(0x7f800000);
    if (operation != UINT32_C(0x52800000) && operation != UINT32_C(0x12800000))
        return false;
    *value = move_immediate(first);
    if (operation == UINT32_C(0x12800000)) {
        uint64_t width = first >> 31 ? UINT64_MAX : UINT32_MAX;
        *value ^= width;
    }
    return apply_move_patches(instructions, count, value);
}

static bool copy_final_normalization(uint8_t *scale, uint8_t *bias, const uint8_t *arm64,
                                     const struct pvdi_profile *profile)
{
    uint64_t scale_bits, bias_bits;
    if (!read_move_constant(arm64, profile->arm64.size, profile->final_scale_offset, 4, &scale_bits) ||
        !read_move_constant(arm64, profile->arm64.size, profile->final_bias_offset, 4, &bias_bits))
        return false;
    write_le(scale, scale_bits, double_bytes);
    write_le(bias, bias_bits, double_bytes);
    return true;
}

static uint64_t low_ones(unsigned count)
{
    return UINT64_MAX >> (64 - count);
}

/* Decode ORR Xd, XZR, #imm, then apply MOVK patches. The logical immediate
 * encodes a rotated run of ones replicated across the 64-bit register. */
static bool read_logical_mask(const uint8_t *instructions, unsigned count, uint64_t *value)
{
    uint32_t instruction = read_instruction(instructions);
    if ((instruction & UINT32_C(0xff8003e0)) != UINT32_C(0xb20003e0))
        return false;
    unsigned imms = (instruction >> 10) & 63;
    unsigned marker = ((instruction >> 16) & 64) | (imms ^ 63);
    if (marker < 2)
        return false;
    unsigned width = 1u << (31 - __builtin_clz(marker));
    unsigned ones = (imms & (width - 1)) + 1;
    if (ones == width)
        return false;
    unsigned rotate = (instruction >> 16) & (width - 1);
    uint64_t pattern = low_ones(ones);
    pattern = ((pattern >> rotate) | (pattern << ((width - rotate) & 63))) & low_ones(width);
    *value = 0;
    for (unsigned shift = 0; shift < 64; shift += width)
        *value |= pattern << shift;
    return apply_move_patches(instructions, count, value);
}

static uint64_t expand_float_immediate(unsigned immediate)
{
    uint64_t exponent = (immediate & 64 ? UINT64_C(0x3fc) : UINT64_C(0x400)) |
                        ((immediate >> 4) & 3);
    return (uint64_t)(immediate >> 7) << 63 | exponent << 52 |
           (uint64_t)(immediate & 15) << 48;
}

static bool read_float_immediate(uint32_t instruction, uint64_t *value)
{
    if ((instruction & UINT32_C(0xffe01fe0)) != UINT32_C(0x1e601000))
        return false;
    *value = expand_float_immediate((instruction >> 13) & 255);
    return true;
}

static bool read_double_pair_immediate(uint32_t instruction, uint64_t *value)
{
    if ((instruction & UINT32_C(0xfff8fc00)) != UINT32_C(0x6f00f400))
        return false;
    unsigned immediate = ((instruction >> 11) & 224) | ((instruction >> 5) & 31);
    *value = expand_float_immediate(immediate);
    return true;
}

/* CMP Wn/Xn, #imm and LDR Dd, [Xn, #imm] both encode a 12-bit immediate.
 * For a double load the unscaled instruction field is the array index. */
static bool read_integer_operand(uint32_t instruction, uint32_t mask, uint32_t opcode,
                                 uint64_t *value)
{
    if ((instruction & mask) != opcode)
        return false;
    double immediate = (instruction >> 10) & 4095;
    memcpy(value, &immediate, sizeof immediate);
    return true;
}

static bool read_integer_move(const uint8_t *arm64, size_t size,
                              struct instruction_constant source, uint64_t *value)
{
    uint64_t integer;
    if (!read_move_constant(arm64, size, source.offset, source.instructions, &integer))
        return false;
    /* The producer adds a negative W-register displacement. The core subtracts its magnitude. */
    if (source.kind == MOVE_NEGATIVE_DISPLACEMENT)
        integer = UINT32_C(0) - (uint32_t)integer;
    double number = (double)integer;
    memcpy(value, &number, sizeof number);
    return true;
}

static bool read_add_array_index(uint32_t instruction, size_t width, uint64_t *value)
{
    if ((instruction & UINT32_C(0xffc00000)) != UINT32_C(0x91000000))
        return false;
    unsigned bytes = (instruction >> 10) & 4095;
    if (bytes % width)
        return false;
    double index = bytes / width;
    memcpy(value, &index, sizeof index);
    return true;
}

static bool read_lsl_scale(uint32_t instruction, int register_bits, uint32_t opcode, uint64_t *value)
{
    /* LSL is UBFM with immr = imms + 1. */
    if ((instruction & UINT32_C(0xffc00000)) != opcode)
        return false;
    unsigned mask = register_bits - 1;
    unsigned immr = (instruction >> 16) & mask;
    unsigned imms = (instruction >> 10) & mask;
    if (!immr || immr != imms + 1)
        return false;
    double scale = UINT64_C(1) << (register_bits - immr);
    memcpy(value, &scale, sizeof scale);
    return true;
}

static bool read_right_shift_scale(uint32_t instruction, unsigned register_bits, uint32_t opcode,
                                   uint64_t *value)
{
    /* LSR and ASR use UBFM and SBFM with imms equal to the last bit. */
    if ((instruction & UINT32_C(0xffc0fc00)) != opcode)
        return false;
    unsigned shift = (instruction >> 16) & (register_bits - 1);
    double scale = UINT64_C(1) << shift;
    memcpy(value, &scale, sizeof scale);
    return true;
}

static bool read_align_down_power(uint32_t instruction, uint64_t *value)
{
    /* AND Wd, Wn with a contiguous high-bit mask clears the low address bits. */
    if ((instruction & UINT32_C(0xffe00000)) != UINT32_C(0x12000000))
        return false;
    unsigned rotation = (instruction >> 16) & 31;
    unsigned length = ((instruction >> 10) & 63) + 1;
    if (rotation != length || length >= 32)
        return false;
    double alignment = UINT32_C(1) << (32 - length);
    memcpy(value, &alignment, sizeof alignment);
    return true;
}

static bool read_signed_byte_offset(uint32_t instruction, uint64_t *value)
{
    if ((instruction & UINT32_C(0xffe00c00)) != UINT32_C(0x38400000))
        return false;
    int offset = (int)((instruction >> 12) & 511);
    double signed_offset = (offset ^ 256) - 256;
    memcpy(value, &signed_offset, sizeof signed_offset);
    return true;
}

static bool read_low_mask_period(uint32_t instruction, uint64_t *value)
{
    /* AND and TST with unrotated low bits implement the remainder period. */
    bool and_mask = (instruction & UINT32_C(0xffff0000)) == UINT32_C(0x12000000);
    bool test_mask = (instruction & UINT32_C(0xffff001f)) == UINT32_C(0x7200001f);
    if (!and_mask && !test_mask)
        return false;
    unsigned last = (instruction >> 10) & 63;
    if (last >= 31)
        return false;
    double period = UINT32_C(1) << (last + 1);
    memcpy(value, &period, sizeof period);
    return true;
}

static bool read_conditional_one_fallback(uint32_t instruction, uint64_t *value)
{
    /* CSINC Wd, Wn, WZR, cond selects zero plus one when the condition fails. */
    if ((instruction & UINT32_C(0x7fff0c00)) != UINT32_C(0x1a9f0400))
        return false;
    double fallback = 1;
    memcpy(value, &fallback, sizeof fallback);
    return true;
}

static bool read_select_increment(uint32_t instruction, uint64_t *value)
{
    /* A failed CSINC condition increments the second source by one. */
    if ((instruction & UINT32_C(0x7fe00c00)) != UINT32_C(0x1a800400))
        return false;
    double increment = 1;
    memcpy(value, &increment, sizeof increment);
    return true;
}

static bool read_orr_slot(uint32_t instruction, uint64_t *value)
{
    /* A native byte load uses a five-bit slot index ORed into its aligned bar base. */
    if ((instruction & UINT32_C(0xffc00000)) != UINT32_C(0xb2400000))
        return false;
    unsigned shift = (64 - ((instruction >> 16) & 63)) & 63;
    unsigned length = ((instruction >> 10) & 63) + 1;
    if (shift + length > 5)
        return false;
    double index = ((1u << length) - 1) << shift;
    memcpy(value, &index, sizeof index);
    return true;
}

static bool read_pair_load_index(uint32_t instruction, uint32_t opcode, int element,
                                 uint64_t *value)
{
    if ((instruction & UINT32_C(0xffc00000)) != opcode)
        return false;
    int index = (int)((instruction >> 15) & 127);
    double signed_index = (index ^ 64) - 64 + element;
    memcpy(value, &signed_index, sizeof signed_index);
    return true;
}

static bool read_word_pair_store_index(uint32_t instruction, unsigned word, uint64_t *value)
{
    if ((instruction & UINT32_C(0xffc00000)) != UINT32_C(0x29000000))
        return false;
    int offset = (int)((instruction >> 15) & 127);
    double index = (offset ^ 64) - 64 + (int)word;
    memcpy(value, &index, sizeof index);
    return true;
}

static bool read_unscaled_back_index(uint32_t instruction, uint32_t opcode, int width, uint64_t *value)
{
    if ((instruction & UINT32_C(0xffe00c00)) != opcode)
        return false;
    int offset = (int)((instruction >> 12) & 511);
    int bytes_back = -((offset ^ 256) - 256);
    if (bytes_back <= 0 || bytes_back % width)
        return false;
    double index = bytes_back / width;
    memcpy(value, &index, sizeof index);
    return true;
}

static bool widen_float_word(uint32_t bits, uint64_t *value)
{
    float single;
    memcpy(&single, &bits, sizeof single);
    double number = single;
    memcpy(value, &number, sizeof number);
    return true;
}

static bool read_movi_float_word(uint32_t instruction, uint64_t *value)
{
    /* MOVI Vd.2S/4S, #byte, LSL #24 constructs an exact FP32 bit pattern. */
    if ((instruction & UINT32_C(0xbff8fc00)) != UINT32_C(0x0f006400))
        return false;
    uint32_t byte = ((instruction >> 11) & 224) | ((instruction >> 5) & 31);
    return widen_float_word(byte << 24, value);
}

static bool read_float_word_operand(uint32_t instruction, enum instruction_constant_kind kind,
                                    uint64_t *value)
{
    switch (kind) {
    case CONDITIONAL_SET_ZERO:
        /* CSET Wd is CSINC Wd, WZR, WZR with the inverse condition. */
        *value = 0;
        return (instruction & UINT32_C(0xffff0fe0)) == UINT32_C(0x1a9f07e0);
    case ADD_WORD_INDEX:
        return read_add_array_index(instruction, sizeof(int32_t), value);
    case FLOAT_WORD_PAIR_INDEX:
        return read_pair_load_index(instruction, UINT32_C(0x2d400000), 0, value);
    case FLOAT_WORD_UNSCALED_BACK:
        return read_unscaled_back_index(instruction, UINT32_C(0xbc400000), sizeof(float), value);
    case FLOAT_WORD_TABLE:
        return widen_float_word(instruction, value);
    case FLOAT_WORD_IMMEDIATE:
        if ((instruction & UINT32_C(0xffe01fe0)) != UINT32_C(0x1e201000))
            return false;
        *value = expand_float_immediate((instruction >> 13) & 255);
        return true;
    case FLOAT_WORD_MOVI_BYTE_24:
        return read_movi_float_word(instruction, value);
    default:
        return false;
    }
}

static bool read_signed_word_shift_scale(uint32_t instruction, uint64_t *value)
{
    /* SBFIZ Xd, Xn, #shift, #32 sign-extends the shifted W-register value. */
    if ((instruction & UINT32_C(0xffc0fc00)) != UINT32_C(0x93407c00))
        return false;
    unsigned rotation = (instruction >> 16) & 63;
    if (rotation <= 32)
        return false;
    double scale = UINT64_C(1) << (64 - rotation);
    memcpy(value, &scale, sizeof scale);
    return true;
}

static bool read_add_asr_scale(uint32_t instruction, uint64_t *value)
{
    /* ADD Wd, Wn, Wm, ASR #shift folds a signed division into the addition. */
    if ((instruction & UINT32_C(0xffe08000)) != UINT32_C(0x0b800000))
        return false;
    double scale = UINT32_C(1) << ((instruction >> 10) & 31);
    memcpy(value, &scale, sizeof scale);
    return true;
}

static bool read_select_invert_bias(uint32_t instruction, uint64_t *value)
{
    /* CSINV Wd, Wn, Wm selects the word complement, equivalent to -Wm - 1. */
    if ((instruction & UINT32_C(0xffe00c00)) != UINT32_C(0x5a800000))
        return false;
    double bias = 1;
    memcpy(value, &bias, sizeof bias);
    return true;
}

static bool read_shifted_double_index(uint32_t instruction, uint64_t *value)
{
    /* ADD Xd, Xn, Xm, LSL #shift indexes binary64 elements with a byte stride. */
    unsigned shift = (instruction >> 10) & 63;
    if ((instruction & UINT32_C(0xffe00000)) != UINT32_C(0x8b000000) || shift < 3)
        return false;
    double stride = UINT64_C(1) << (shift - 3);
    memcpy(value, &stride, sizeof stride);
    return true;
}

static bool read_sub_double_index(uint32_t instruction, uint64_t *value)
{
    if ((instruction & UINT32_C(0xffc00000)) != UINT32_C(0xd1000000))
        return false;
    unsigned bytes = (instruction >> 10) & 4095;
    if (bytes % sizeof(double))
        return false;
    double index = bytes / sizeof(double);
    memcpy(value, &index, sizeof index);
    return true;
}

static bool read_add_lsl_scale(uint32_t instruction, uint64_t *value)
{
    if ((instruction & UINT32_C(0x7fe00000)) != UINT32_C(0x0b000000))
        return false;
    unsigned shift = (instruction >> 10) & 63;
    if (!(instruction >> 31) && shift >= 32)
        return false;
    double scale = UINT64_C(1) << shift;
    memcpy(value, &scale, sizeof scale);
    return true;
}

static bool read_spectral_operand(uint32_t instruction, enum instruction_constant_kind kind,
                                  uint64_t *value)
{
    switch (kind) {
    case WORD_PAIR_STORE_FIRST_INDEX:
        return read_word_pair_store_index(instruction, 0, value);
    case WORD_PAIR_STORE_SECOND_INDEX:
        return read_word_pair_store_index(instruction, 1, value);
    case ADD_LSL_SCALE:
        return read_add_lsl_scale(instruction, value);
    case SIGNED_WORD_UNSCALED_BACK_INDEX:
        return read_unscaled_back_index(instruction, UINT32_C(0xb8800000), sizeof(int32_t), value);
    case WORD_UNSCALED_BACK_INDEX:
        return read_unscaled_back_index(instruction, UINT32_C(0xb8400000), sizeof(uint32_t), value);
    case SUB_DOUBLE_INDEX:
        return read_sub_double_index(instruction, value);
    case LSR_WIDE_SCALE:
        return read_right_shift_scale(instruction, 64, 0xd340fc00u, value);
    case FLOAT_COMPARE_ZERO:
        *value = 0;
        return (instruction & UINT32_C(0xfffffc1f)) == UINT32_C(0x1e602008);
    default:
        return read_float_word_operand(instruction, kind, value);
    }
}

static bool read_geometry_operand(uint32_t instruction, enum instruction_constant_kind kind,
                                  uint64_t *value)
{
    switch (kind) {
    case ADD_ASR_SCALE:
        return read_add_asr_scale(instruction, value);
    case BYTE_ZERO_STORE:
        *value = 0;
        return (instruction & UINT32_C(0xffe0fc1f)) == UINT32_C(0x3820681f);
    case WORD_ZERO_STORE:
        *value = 0;
        return (instruction & UINT32_C(0xffc0001f)) == UINT32_C(0xb900001f);
    case DOUBLE_PAIR_IMMEDIATE:
        return read_double_pair_immediate(instruction, value);
    case DOUBLE_PAIR_ZERO:
        *value = 0;
        return (instruction & UINT32_C(0xffffffe0)) == UINT32_C(0x6f00e400);
    case SELECT_INVERT_BIAS:
        return read_select_invert_bias(instruction, value);
    case SELECT_INCREMENT:
        return read_select_increment(instruction, value);
    case ADD_SHIFTED_DOUBLE_INDEX:
        return read_shifted_double_index(instruction, value);
    default:
        return read_spectral_operand(instruction, kind, value);
    }
}

static bool read_secondary_operand(uint32_t instruction, enum instruction_constant_kind kind,
                                   uint64_t *value)
{
    switch (kind) {
    case DOUBLE_LOAD_INDEX:
        return read_integer_operand(instruction, 0xffc00000u, 0xfd400000u, value);
    case WORD_LOAD_INDEX:
        return read_integer_operand(instruction, 0xffc00000u, 0xb9400000u, value);
    case BYTE_LOAD_INDEX:
        return read_integer_operand(instruction, 0xffc00000u, 0x39400000u, value);
    case DOUBLE_PAIR_LOAD_INDEX:
        return read_pair_load_index(instruction, UINT32_C(0x6d400000), 0, value);
    case SUBTRACT_FLAGS_IMMEDIATE:
        return read_integer_operand(instruction, 0x7fc00000u, 0x71000000u, value);
    case COMPARE_NEGATIVE_IMMEDIATE:
        return read_integer_operand(instruction, 0x7fc0001fu, 0x3100001fu, value);
    case DOUBLE_UNSCALED_BACK_INDEX:
        return read_unscaled_back_index(instruction, UINT32_C(0xfc400000), sizeof(double), value);
    case SIGNED_WORD_SHIFT_SCALE:
        return read_signed_word_shift_scale(instruction, value);
    default:
        return read_geometry_operand(instruction, kind, value);
    }
}

static bool read_extended_operand(uint32_t instruction, enum instruction_constant_kind kind,
                                  uint64_t *value)
{
    switch (kind) {
    case LSR_SCALE:
        return read_right_shift_scale(instruction, 32, 0x53007c00u, value);
    case ASR_SCALE:
        return read_right_shift_scale(instruction, 32, 0x13007c00u, value);
    case ALIGN_DOWN_POWER:
        return read_align_down_power(instruction, value);
    case DOUBLE_ZERO:
        *value = 0;
        return (instruction & UINT32_C(0xffffffe0)) == UINT32_C(0x2f00e400);
    case SIGNED_BYTE_LOAD_OFFSET:
        return read_signed_byte_offset(instruction, value);
    case CONDITIONAL_ONE_FALLBACK:
        return read_conditional_one_fallback(instruction, value);
    case ORR_SLOT_INDEX:
        return read_orr_slot(instruction, value);
    case BASE_BYTE_LOAD_INDEX:
        *value = 0;
        return (instruction & UINT32_C(0xffe0fc00)) == UINT32_C(0x38606800);
    default:
        return read_secondary_operand(instruction, kind, value);
    }
}

static bool read_single_operand(uint32_t instruction, enum instruction_constant_kind kind,
                                uint64_t *value)
{
    switch (kind) {
    case FLOAT_IMMEDIATE:
        return read_float_immediate(instruction, value);
    case COMPARE_IMMEDIATE:
        return read_integer_operand(instruction, 0x7fc0001fu, 0x7100001fu, value);
    case ADD_IMMEDIATE:
        return read_integer_operand(instruction, 0x7fc00000u, 0x11000000u, value);
    case SUB_IMMEDIATE:
        return read_integer_operand(instruction, 0x7fc00000u, 0x51000000u, value);
    case ADD_DOUBLE_INDEX:
        return read_add_array_index(instruction, sizeof(double), value);
    case LSL_SCALE:
        return read_lsl_scale(instruction, 32, 0x53000000u, value);
    case LSL_WIDE_SCALE:
        return read_lsl_scale(instruction, 64, 0xd3400000u, value);
    case WORD_TABLE: {
        double number = instruction;
        memcpy(value, &number, sizeof number);
        return true;
    }
    default:
        return read_extended_operand(instruction, kind, value);
    }
}

static bool read_conditional_compare(uint32_t instruction, uint64_t *value)
{
    if ((instruction & UINT32_C(0x7fe00c10)) != UINT32_C(0x7a400800))
        return false;
    double immediate = (instruction >> 16) & 31;
    memcpy(value, &immediate, sizeof immediate);
    return true;
}

static bool read_float_minimum_input_count(const uint8_t *instructions, unsigned count,
                                           uint64_t *value)
{
    /* The native reduction starts with two inputs, then adds one per FMINNM.
       FCMP marks the comparison of the result against the first tail ratio. */
    if (count < 2)
        return false;
    for (unsigned i = 0; i + 1 < count; ++i)
        if ((read_instruction(instructions + 4 * i) & UINT32_C(0xffe0fc00)) != UINT32_C(0x1e607800))
            return false;
    uint32_t compare = read_instruction(instructions + 4 * (count - 1));
    if ((compare & UINT32_C(0xffe0fc1f)) != UINT32_C(0x1e602000))
        return false;
    double inputs = count;
    memcpy(value, &inputs, sizeof inputs);
    return true;
}

static int64_t branch_target_words(uint32_t instruction, unsigned ordinal)
{
    uint32_t delta = instruction & UINT32_C(0x03ffffff);
    return (int64_t)ordinal + delta - ((delta & UINT32_C(0x02000000)) << 1);
}

static bool read_repeated_call_count(const uint8_t *instructions, unsigned count, bool exclusive, uint64_t *value)
{
    int64_t first = branch_target_words(read_instruction(instructions), 0);
    unsigned calls = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t instruction = read_instruction(instructions + 4 * i);
        if ((instruction & UINT32_C(0xfc000000)) != UINT32_C(0x94000000)) continue;
        if (branch_target_words(instruction, i) != first) {
            if (exclusive) return false;
            continue;
        }
        calls++;
    }
    if (!calls) return false;
    double number = calls;
    memcpy(value, &number, sizeof number);
    return true;
}

static bool decode_derived_operand(const uint8_t *instructions, struct instruction_constant source,
                                   uint64_t *value)
{
    switch (source.kind) {
    case DOUBLE_PAIR_LOAD_NEXT_INDEX:
        return read_pair_load_index(read_instruction(instructions), UINT32_C(0x6d400000), 1, value);
    case FLOAT_MINIMUM_INPUT_COUNT:
        return read_float_minimum_input_count(instructions, source.instructions, value);
    case REPEATED_CALL_COUNT:
        return read_repeated_call_count(instructions, source.instructions, true, value);
    case MATCHING_CALL_COUNT:
        return read_repeated_call_count(instructions, source.instructions, false, value);
    case DOUBLE_SELF_ADD_COUNT: {
        uint32_t add = read_instruction(instructions);
        if ((add & 0xffe0fc00u) != 0x1e602800u || ((add >> 5) & 31) != ((add >> 16) & 31))
            return false;
        double terms = 2;
        memcpy(value, &terms, sizeof terms);
        return true;
    }
    case DOUBLE_POSTINDEX_ZERO_STORE:
        *value = 0;
        return (read_instruction(instructions) & UINT32_C(0xffe00c1f)) == UINT32_C(0xf800041f);
    default:
        return read_single_operand(read_instruction(instructions), source.kind, value);
    }
}

static bool decode_float_word_move(const uint8_t *arm64, size_t size, struct instruction_constant source,
                                   uint64_t *value)
{
    uint64_t bits;
    if (!read_move_constant(arm64, size, source.offset, source.instructions, &bits) || bits > UINT32_MAX)
        return false;
    return widen_float_word((uint32_t)bits, value);
}

static bool decode_instruction_constant(const uint8_t *arm64, size_t size,
                                         struct instruction_constant source, uint64_t *value)
{
    const uint8_t *instructions = arm64 + source.offset;
    switch (source.kind) {
    case MOVE_HALFWORDS:
        return read_move_constant(arm64, size, source.offset, source.instructions, value);
    case FLOAT_WORD_MOVE_BITS:
        return decode_float_word_move(arm64, size, source, value);
    case LOGICAL_MASK:
        return read_logical_mask(instructions, source.instructions, value);
    case DOUBLE_TABLE:
        *value = read_instruction(instructions) | (uint64_t)read_instruction(instructions + 4) << 32;
        return true;
    case MOVE_INTEGER:
    case MOVE_NEGATIVE_DISPLACEMENT:
        return read_integer_move(arm64, size, source, value);
    case CONDITIONAL_COMPARE_IMMEDIATE:
        return read_conditional_compare(read_instruction(instructions), value);
    case LOW_MASK_PERIOD:
        return read_low_mask_period(read_instruction(instructions), value);
    default:
        return decode_derived_operand(instructions, source, value);
    }
}

static bool read_instruction_constant(const uint8_t *arm64, size_t size,
                                      struct instruction_constant source, uint64_t *value)
{
    if (source.instructions == 0 || !contains_range(size, source.offset, source.instructions * 4))
        return false;
    return decode_instruction_constant(arm64, size, source, value);
}

static bool copy_parameters(uint8_t *output, const uint8_t *arm64, size_t size,
                            const struct instruction_constant *sources, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint64_t bits;
        if (!read_instruction_constant(arm64, size, sources[i], &bits)) {
            fprintf(stderr, "Invalid native parameter at ARM64 file offset %#x\n",
                    sources[i].offset);
            return false;
        }
        write_le(output + i * double_bytes, bits, double_bytes);
    }
    return true;
}

static bool read_rhythm_mask(const uint8_t *arm64, size_t size,
                            const struct rhythm_mask_source *source, uint64_t *value)
{
    if (!source->count || source->count > sizeof source->slots / sizeof source->slots[0])
        return false;
    uint32_t mask = 0;
    for (unsigned i = 0; i < source->count; ++i) {
        uint64_t bits;
        if (!read_instruction_constant(arm64, size, source->slots[i], &bits))
            return false;
        double slot;
        memcpy(&slot, &bits, sizeof slot);
        if (slot < 0 || slot >= 32)
            return false;
        mask |= UINT32_C(1) << (unsigned)slot;
    }
    double result = mask;
    memcpy(value, &result, sizeof result);
    return true;
}

static bool copy_rhythm_masks(uint8_t *output, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    for (unsigned i = 0; i < TA_RHYTHM_MASK_COUNT; ++i) {
        uint64_t bits;
        if (!read_rhythm_mask(arm64, profile->arm64.size, &profile->rhythm_masks[i], &bits)) {
            fprintf(stderr, "Invalid native rhythm pattern %u\n", i);
            return false;
        }
        write_le(output + i * double_bytes, bits, double_bytes);
    }
    return true;
}

static bool opening_dispatch_add(uint32_t target, unsigned slot,
                                 const struct score_dispatch_source *source,
                                 uint32_t masks[TA_OPENING_SCORE_MASK_COUNT])
{
    if (target == source->default_target)
        return true;
    for (unsigned i = 0; i < TA_OPENING_SCORE_MASK_COUNT; ++i)
        if (target == source->weighted_targets[i]) {
            masks[i] |= UINT32_C(1) << slot;
            return true;
        }
    return false;
}

static bool read_opening_dispatch(uint32_t masks[TA_OPENING_SCORE_MASK_COUNT], const uint8_t *arm64,
                                  const struct pvdi_profile *profile)
{
    const struct score_dispatch_source *source = &profile->opening_score_dispatch;
    size_t size = profile->arm64.size;
    uint64_t bits;
    if (!read_instruction_constant(arm64, size, profile->phrase[TA_OPENING_SCORE_DISPATCH_LAST_SLOT], &bits))
        return false;
    double last;
    memcpy(&last, &bits, sizeof last);
    if (last >= 32 || !contains_range(size, source->table_offset, (unsigned)last + 1))
        return false;
    for (unsigned i = 0; i <= (unsigned)last; ++i) {
        unsigned branch = arm64[source->table_offset + i];
        uint32_t target = source->branch_base + (uint32_t)branch * sizeof(uint32_t);
        if (!opening_dispatch_add(target, i, source, masks))
            return false;
    }
    return true;
}

static bool copy_opening_dispatch(uint8_t *output, const uint8_t *arm64,
                                  const struct pvdi_profile *profile)
{
    uint32_t masks[TA_OPENING_SCORE_MASK_COUNT] = {0};
    if (!read_opening_dispatch(masks, arm64, profile)) {
        fputs("Invalid native opening score dispatch\n", stderr);
        return false;
    }
    for (unsigned i = 0; i < TA_OPENING_SCORE_MASK_COUNT; ++i) {
        double mask = masks[i];
        uint64_t bits;
        memcpy(&bits, &mask, sizeof bits);
        write_le(output + i * double_bytes, bits, double_bytes);
    }
    return true;
}

static unsigned high_export_handler(const struct high_export_dispatch_source *source, uint32_t target)
{
    for (unsigned i = 0; i < TA_HIGH_EXPORT_HANDLER_COUNT; i++)
        if (source->targets[i] == target)
            return i;
    return TA_HIGH_EXPORT_HANDLER_COUNT;
}

static bool copy_high_export_dispatch(uint8_t *output, const uint8_t *arm64,
                                      const struct pvdi_profile *profile)
{
    const struct high_export_dispatch_source *source = &profile->high_export_dispatch;
    if (!contains_range(profile->arm64.size, source->table_offset, TA_HIGH_EXPORT_HANDLER_COUNT))
        return false;
    unsigned seen = 0;
    for (unsigned slot = 0; slot < TA_HIGH_EXPORT_HANDLER_COUNT; slot++) {
        unsigned branch = arm64[source->table_offset + slot];
        uint32_t target = source->branch_base + branch * sizeof(uint32_t);
        unsigned handler = high_export_handler(source, target);
        if (handler == TA_HIGH_EXPORT_HANDLER_COUNT)
            return false;
        seen |= 1u << handler;
        double value = handler;
        uint64_t bits;
        memcpy(&bits, &value, sizeof bits);
        write_le(output + slot * double_bytes, bits, double_bytes);
    }
    return seen == (1u << TA_HIGH_EXPORT_HANDLER_COUNT) - 1;
}

static bool copy_phrase_parameters(uint8_t *output, const uint8_t *arm64,
                                   const struct pvdi_profile *profile)
{
    return copy_parameters(output, arm64, profile->arm64.size, profile->phrase, TA_PHRASE_OPERAND_COUNT) &&
           copy_rhythm_masks(output + TA_RHYTHM_MASK_KICK_FIRST * double_bytes, arm64, profile) &&
           copy_opening_dispatch(output + TA_OPENING_SCORE_MASK_TWO * double_bytes, arm64, profile) &&
           copy_high_export_dispatch(output + TA_HIGH_EXPORT_DISPATCH_FIRST * double_bytes, arm64, profile);
}

static bool copy_vocal_thresholds(uint8_t *output, const uint8_t *arm64,
                                  const struct pvdi_profile *profile)
{
    for (size_t i = 0; i < TA_VOCAL_THRESHOLD_COUNT; ++i) {
        uint64_t bits;
        if (!read_move_constant(arm64, profile->arm64.size, profile->vocal_thresholds[i], 2, &bits))
            return false;
        write_le(output + i * 4, bits, 4);
    }
    return true;
}

static bool copy_vocal_coefficients(uint8_t *output, const uint8_t *arm64,
                                    const struct pvdi_profile *profile, unsigned window, unsigned columns)
{
    size_t window_bytes = window * double_bytes;
    size_t paired = columns - 1;
    if (!contains_range(profile->arm64.size, profile->window_offset, window_bytes) ||
        !contains_range(profile->arm64.size, profile->normalization_offset,
                        2 * paired * double_bytes))
        return false;
    memcpy(output, arm64 + profile->window_offset, window_bytes);
    const uint8_t *table = arm64 + profile->normalization_offset;
    uint8_t *scales = output + window_bytes;
    uint8_t *offsets = scales + columns * double_bytes;
    for (size_t i = 0; i < paired; ++i) {
        size_t slot = (i / 2 * 4 + i % 2) * double_bytes;
        memcpy(scales + i * double_bytes, table + slot, double_bytes);
        memcpy(offsets + i * double_bytes, table + slot + 2 * double_bytes, double_bytes);
    }
    return copy_final_normalization(scales + paired * double_bytes,
                                    offsets + paired * double_bytes, arm64, profile);
}

/* Only unsigned dimensions and counts in the supported graph layouts reach here. */
static bool read_graph_varint(const uint8_t *data, size_t size, size_t *pos, uint32_t *value)
{
    *value = 0;
    for (unsigned shift = 0; shift < 28 && *pos < size; shift += 7) {
        uint8_t byte = data[(*pos)++];
        *value |= (uint32_t)(byte & 0x7f) << shift;
        if (byte < 0x80)
            return true;
    }
    return false;
}

static bool read_graph_dimension(const uint8_t *shape, size_t size, size_t *pos, uint32_t *value)
{
    uint32_t length;
    if (*pos >= size || shape[(*pos)++] != 0x12 ||
        !read_graph_varint(shape, size, pos, &length) ||
        !contains_range(size, *pos, length) || length == 0)
        return false;
    size_t end = *pos + length;
    return shape[(*pos)++] == 0x08 && read_graph_varint(shape, end, pos, value) && *pos == end;
}

static bool read_tensor_shape(const uint8_t *graph, size_t size, const struct graph_tensor *tensor,
                              uint32_t *dimensions, size_t rank)
{
    struct file_range shape = tensor->shape;
    if (!contains_range(size, shape.offset, shape.length))
        return false;
    size_t pos = 0;
    for (size_t i = 0; i < rank; i++)
        if (!read_graph_dimension(graph + shape.offset, shape.length, &pos, dimensions + i))
            return false;
    return pos == shape.length;
}

static bool read_matrix_shape(const uint8_t *graph, size_t size, const struct graph_tensor *tensors,
                               bool recurrent, uint32_t dimensions[2])
{
    uint32_t bias[2] = { 0 };
    if (!read_tensor_shape(graph, size, tensors, dimensions, 2) ||
        !read_tensor_shape(graph, size, tensors + 1, bias, recurrent ? 1 : 2))
        return false;
    uint64_t elements = (uint64_t)dimensions[0] * dimensions[1];
    bool bias_matches = recurrent ? bias[0] == dimensions[1] :
                                    bias[0] == 1 && bias[1] == dimensions[1];
    return bias_matches && elements * 4 == tensors[0].content.length &&
           dimensions[1] * UINT64_C(4) == tensors[1].content.length;
}

struct tensor_destination {
    uint8_t *weights;
    size_t size, copied;
};

static bool copy_tensor(struct tensor_destination *to, const uint8_t *graph, size_t size,
                         struct file_range content)
{
    if (!contains_range(size, content.offset, content.length) ||
        !contains_range(to->size, to->copied, content.length))
        return false;
    memcpy(to->weights + to->copied, graph + content.offset, content.length);
    to->copied += content.length;
    return true;
}

static bool copy_matrices(uint8_t *model, const uint8_t *graph, const struct graph_layout *layout,
                          size_t weights_offset)
{
    struct tensor_destination to = {model + weights_offset, TA_MODEL_LEGACY_BYTES - weights_offset, 0};
    for (size_t i = 0; i < TA_VOCAL_MATRIX_COUNT; ++i) {
        const struct graph_tensor *tensors = layout->tensors[i];
        size_t size = layout->file.size;
        uint32_t shape[2];
        if (!read_matrix_shape(graph, size, tensors, i < TA_VOCAL_RECURRENT_CELL_COUNT, shape) ||
            !copy_tensor(&to, graph, size, tensors[0].content) ||
            !copy_tensor(&to, graph, size, tensors[1].content))
            return false;
        uint8_t *destination = model + TA_MODEL_MATRIX_SHAPES_OFFSET + i * 8;
        write_le(destination, shape[0], 4);
        write_le(destination + 4, shape[1], 4);
    }
    return to.copied == to.size;
}

static bool read_gate_port(const uint8_t *graph, size_t size, struct file_range name, uint32_t *port)
{
    if ((name.length != 5 && name.length != 7) ||
        !contains_range(size, name.offset, name.length))
        return false;
    const uint8_t *suffix = graph + name.offset;
    if (memcmp(suffix, "split", 5) != 0)
        return false;
    *port = 0;  /* TensorFlow omits the port for output zero. */
    if (name.length == 5)
        return true;
    if (suffix[5] != ':' || suffix[6] < '0' || suffix[6] > '9')
        return false;
    *port = suffix[6] - '0';
    return true;
}

static bool copy_recurrent_cell(uint8_t *output, const uint8_t *graph, size_t size,
                                 const struct graph_cell *source)
{
    size_t pos = source->split_count;
    uint32_t count;
    if (!read_graph_varint(graph, size, &pos, &count) ||
        !contains_range(size, source->initial_cell, 4) ||
        !contains_range(size, source->initial_hidden, 4))
        return false;
    write_le(output + TA_VOCAL_GATE_COUNT * 4, count, 4);
    for (size_t gate = 0; gate < TA_VOCAL_GATE_ROLES; ++gate) {
        uint32_t port;
        if (!read_gate_port(graph, size, source->gates[gate], &port))
            return false;
        write_le(output + gate * 4, port, 4);
    }
    memcpy(output + TA_VOCAL_INITIAL_CELL * 4, graph + source->initial_cell, 4);
    memcpy(output + TA_VOCAL_INITIAL_HIDDEN * 4, graph + source->initial_hidden, 4);
    return true;
}

static bool equal_graph_names(const uint8_t *graph, size_t size, struct file_range a, struct file_range b)
{
    return a.length != 0 && a.length == b.length &&
           contains_range(size, a.offset, a.length) && contains_range(size, b.offset, b.length) &&
           memcmp(graph + a.offset, graph + b.offset, a.length) == 0;
}

static unsigned graph_node_slot(const uint8_t *graph, size_t size, struct file_range reference,
                                const struct file_range *names)
{
    for (unsigned i = 0; i < TA_VOCAL_RECURRENT_CELL_COUNT; ++i)
        if (equal_graph_names(graph, size, reference, names[i]))
            return i + 1;
    return 0;
}

static bool copy_topology_inputs(uint8_t *model, const uint8_t *graph, const struct graph_layout *layout)
{
    const struct graph_topology *source = &layout->topology;
    size_t size = layout->file.size;
    for (unsigned i = 0; i < TA_VOCAL_RECURRENT_CELL_COUNT; ++i) {
        struct file_range input = source->inputs[i];
        unsigned cell = graph_node_slot(graph, size, input, source->hidden);
        if (cell == 0 && graph_node_slot(graph, size, input, source->frame_readers) == 0)
            return false;
        write_le(model + TA_MODEL_RECURRENT_INPUTS_OFFSET + i * 4, cell, 4);
    }
    return true;
}

static bool graph_operation_is(const uint8_t *graph, size_t size, struct file_range op, const char *name)
{
    size_t length = strlen(name);
    return op.length == length && contains_range(size, op.offset, length) &&
           memcmp(graph + op.offset, name, length) == 0;
}

static const struct graph_branch *find_branch(const uint8_t *graph, const struct graph_layout *layout,
                                              struct file_range reference)
{
    for (unsigned i = 0; i < TA_VOCAL_RECURRENT_CELL_COUNT; ++i) {
        const struct graph_branch *branch = &layout->topology.outputs[i];
        if (equal_graph_names(graph, layout->file.size, reference, branch->name))
            return branch;
    }
    return NULL;
}

static bool copy_topology_branch(uint8_t *output, const uint8_t *graph, const struct graph_layout *layout,
                                 struct file_range reference)
{
    const struct graph_branch *branch = find_branch(graph, layout, reference);
    size_t size = layout->file.size;
    if (!branch)
        return false;
    unsigned terminal = graph_node_slot(graph, size, branch->selected_cell, layout->topology.hidden);
    bool reversed = graph_operation_is(graph, size, branch->operation, "ReverseSequence");
    if (terminal == 0 || (!reversed && !graph_operation_is(graph, size, branch->operation, "Transpose")))
        return false;
    write_le(output, terminal, 4);
    write_le(output + 4, reversed, 4);
    return true;
}

static bool read_concat_input(const uint8_t *graph, struct file_range inputs, size_t *pos,
                              struct file_range *reference)
{
    const uint8_t *fields = graph + inputs.offset;
    uint32_t length;
    if (fields[(*pos)++] != 0x1a || !read_graph_varint(fields, inputs.length, pos, &length) ||
        !contains_range(inputs.length, *pos, length))
        return false;
    *reference = (struct file_range){ inputs.offset + (uint32_t)*pos, length };
    *pos += length;
    return true;
}

static bool copy_topology_branches(uint8_t *model, const uint8_t *graph, const struct graph_layout *layout)
{
    struct file_range inputs = layout->topology.concat_inputs;
    if (!contains_range(layout->file.size, inputs.offset, inputs.length))
        return false;
    uint8_t *output = model + TA_MODEL_RECURRENT_BRANCHES_OFFSET;
    memset(output, 0, TA_VOCAL_RECURRENT_CELL_COUNT * TA_MODEL_RECURRENT_BRANCH_BYTES);
    size_t pos = 0;
    unsigned count = 0;
    while (pos < inputs.length) {
        struct file_range reference;
        if (!read_concat_input(graph, inputs, &pos, &reference))
            return false;
        /* ConcatV2's last input specifies the axis. */
        if (pos == inputs.length) {
            write_le(model + TA_MODEL_RECURRENT_BRANCH_COUNT_OFFSET, count, 4);
            return count != 0;
        }
        if (count == TA_VOCAL_RECURRENT_CELL_COUNT || !copy_topology_branch(output, graph, layout, reference))
            return false;
        output += TA_MODEL_RECURRENT_BRANCH_BYTES;
        count++;
    }
    return false;
}

static bool copy_vocal_graph(uint8_t *model, const uint8_t *graph, const struct graph_layout *layout,
                             unsigned columns, size_t weights_offset)
{
    if (!copy_matrices(model, graph, layout, weights_offset))
        return false;
    write_le(model + TA_MODEL_FEATURE_COLUMNS_OFFSET, columns, 4);
    for (size_t i = 0; i < TA_VOCAL_RECURRENT_CELL_COUNT; ++i) {
        uint8_t *output = model + TA_MODEL_RECURRENT_CELLS_OFFSET + i * TA_VOCAL_CELL_FIELDS * 4;
        if (!copy_recurrent_cell(output, graph, layout->file.size, &layout->cells[i]))
            return false;
    }
    return copy_topology_inputs(model, graph, layout) && copy_topology_branches(model, graph, layout);
}

static bool copy_vocal_class(uint8_t *output, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    if (!contains_range(profile->arm64.size, profile->vocal_class_address, 4))
        return false;
    uint32_t instruction = read_instruction(arm64 + profile->vocal_class_address);
    unsigned byte_offset = (instruction >> 10) & 0xfff;
    if ((instruction & 0xffc00000u) != 0x91000000u || byte_offset % 4 != 0)
        return false;
    write_le(output, byte_offset / 4, 4);
    return true;
}

static bool copy_forget_bias(uint8_t *output, const uint8_t *graph,
                             const struct graph_layout *layout)
{
    for (size_t i = 0; i < TA_VOCAL_RECURRENT_CELL_COUNT; ++i) {
        uint32_t offset = layout->forget_bias[i];
        if (!contains_range(layout->file.size, offset, 4))
            return false;
        memcpy(output + i * 4, graph + offset, 4);
    }
    return true;
}

static void write_header(uint8_t *model, const char *magic, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = header_bytes; i < length; ++i)
        hash = (hash ^ model[i]) * 16777619u;
    memcpy(model, magic, 8);
    write_le(model + 8, length - header_bytes, 4);
    write_le(model + 12, hash, 4);
    memset(model + 16, 0, header_bytes - 16);
}

static bool write_model(const char *path, uint8_t *model)
{
    write_header(model, TA_MODEL_MAGIC, model_bytes);
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return false;
    }
    bool written = fwrite(model, 1, model_bytes, file) == model_bytes;
    int closed = fclose(file);
    if (!written || closed)
        fprintf(stderr, "Could not write the complete model to %s\n", path);
    return written && !closed;
}

static bool read_delta_pair_slot(uint32_t instruction, unsigned reg, unsigned *slot)
{
    unsigned pair = (instruction >> 15) & 0x7f;
    bool second = ((instruction >> 10) & 31) == reg;
    if (pair >= 64 || (!second && (instruction & 31) != reg))
        return false;
    *slot = (pair + second) * 2;
    return true;
}

static bool read_delta_slot(uint32_t instruction, unsigned reg, unsigned *slot)
{
    if ((instruction & 0xffc00000u) == 0xad400000u)
        return read_delta_pair_slot(instruction, reg, slot);
    unsigned bytes = (instruction >> 12) & 0x1ff;
    /* LDR Qt, [Xn, #offset]! in the first column's pre-indexed load. */
    if ((instruction & 0xffe00c00u) != 0x3cc00c00u || (instruction & 31) != reg ||
        bytes >= 256 || bytes % 8 != 0)
        return false;
    *slot = bytes / 8;
    return true;
}

static bool read_profile_word(const uint8_t *arm64, size_t size, uint32_t address, uint32_t *word)
{
    if (!contains_range(size, address, 4))
        return false;
    *word = read_instruction(arm64 + address);
    return true;
}

static bool read_delta_difference(const uint8_t *arm64, size_t size, struct delta_tap_source source,
                                  unsigned endpoints[2], unsigned *reg)
{
    uint32_t subtract, after, before;
    if (!read_profile_word(arm64, size, source.difference, &subtract) ||
        !read_profile_word(arm64, size, source.after_load, &after) ||
        !read_profile_word(arm64, size, source.before_load, &before) ||
        (subtract & 0xffe0fc00u) != 0x4ee0d400u)
        return false;
    *reg = subtract & 31;
    return read_delta_slot(after, (subtract >> 5) & 31, endpoints) &&
           read_delta_slot(before, (subtract >> 16) & 31, endpoints + 1);
}

static bool read_vector_double_add(const uint8_t *arm64, size_t size, uint32_t address, uint32_t *word)
{
    return read_profile_word(arm64, size, address, word) && (*word & 0xffe0fc00u) == 0x4e60d400u;
}

static unsigned delta_add_input_uses(uint32_t word, unsigned reg)
{
    return (((word >> 5) & 31) == reg) + (((word >> 16) & 31) == reg);
}

static void write_vocal_parameter(uint8_t *model, unsigned parameter, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    write_le(model + TA_MODEL_VOCAL_OFFSET + parameter * double_bytes, bits, double_bytes);
}

static bool copy_inner_delta_weights(uint8_t *model, const uint8_t *arm64, const struct pvdi_profile *profile,
                                     unsigned first, unsigned second)
{
    uint32_t doubling, combine;
    const struct vocal_delta_source *source = &profile->vocal_delta;
    if (!read_vector_double_add(arm64, profile->arm64.size, source->second_sum, &doubling) ||
        !read_vector_double_add(arm64, profile->arm64.size, source->inner_sum, &combine))
        return false;
    unsigned first_weight = delta_add_input_uses(combine, first);
    unsigned second_additions = delta_add_input_uses(doubling, second);
    if (first_weight == 0 || second_additions == 0 || delta_add_input_uses(combine, doubling & 31) != 1)
        return false;
    write_vocal_parameter(model, TA_VOCAL_DELTA_FIRST_WEIGHT, first_weight);
    write_vocal_parameter(model, TA_VOCAL_DELTA_SECOND_ADDITIONS, second_additions);
    return true;
}

static bool read_delta_radius(const uint8_t *arm64, const struct pvdi_profile *profile, size_t tap,
                              unsigned row_width, unsigned *reg, unsigned *radius)
{
    unsigned endpoints[2];
    if (!read_delta_difference(arm64, profile->arm64.size, profile->vocal_delta.taps[tap], endpoints, reg) ||
        endpoints[0] <= endpoints[1] || (endpoints[0] - endpoints[1]) % (2 * row_width) != 0)
        return false;
    *radius = (endpoints[0] - endpoints[1]) / (2 * row_width);
    return true;
}

static bool copy_vocal_delta(uint8_t *model, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    double columns;
    memcpy(&columns, model + TA_MODEL_VOCAL_OFFSET + TA_VOCAL_MFCC_COLUMNS * double_bytes, sizeof columns);
    if (!(columns >= 1 && columns <= UINT16_MAX))
        return false;
    unsigned regs[3];
    for (size_t tap = 0; tap < sizeof regs / sizeof *regs; ++tap) {
        unsigned radius;
        if (!read_delta_radius(arm64, profile, tap, (unsigned)columns, &regs[tap], &radius))
            return false;
        write_vocal_parameter(model, TA_VOCAL_DELTA_FIRST_RADIUS + (unsigned)tap, radius);
    }
    return copy_inner_delta_weights(model, arm64, profile, regs[0], regs[1]);
}

static bool first_reflected_byte(uint32_t reverse, uint32_t rotate, unsigned *reg, unsigned *byte)
{
    *reg = reverse & 31;
    uint32_t registers = *reg | (*reg << 5);
    if ((reverse & 0xfffffc00u) != 0x4ea00800u || (reverse & 1023) != registers ||
        (rotate & 0xffe08400u) != 0x6e000000u ||
        (rotate & 0x1f03ffu) != (registers | (*reg << 16)))
        return false;
    unsigned displacement = (rotate >> 11) & 15;
    if (displacement % sizeof(float) != 0)
        return false;
    /* REV64 swaps the two FP32 words in each half; EXT chooses the first lane. */
    *byte = displacement ^ sizeof(float);
    return true;
}

static bool read_left_reflection(const uint8_t *arm64, const struct pvdi_profile *profile,
                                  double center, double *back)
{
    const struct vocal_reflection_source *source = &profile->vocal_reflection;
    size_t size = profile->arm64.size;
    uint64_t group;
    uint32_t pair, reverse, rotate;
    if (!read_move_constant(arm64, size, source->group_start, 1, &group) ||
        !read_profile_word(arm64, size, source->pair_load, &pair) ||
        !read_profile_word(arm64, size, source->reverse, &reverse) ||
        !read_profile_word(arm64, size, source->rotate, &rotate))
        return false;
    unsigned reg, byte, slot;
    if (!first_reflected_byte(reverse, rotate, &reg, &byte) || !read_delta_slot(pair, reg, &slot))
        return false;
    *back = center - ((double)group + slot * double_bytes + byte) / sizeof(float);
    return true;
}

static bool read_right_reflection(const uint8_t *arm64, const struct pvdi_profile *profile, double *back)
{
    const struct vocal_reflection_source *source = &profile->vocal_reflection;
    size_t size = profile->arm64.size;
    uint64_t origin, lane_bits;
    uint32_t instruction;
    if (!read_move_constant(arm64, size, source->right_origin, 1, &origin) ||
        origin < (UINT64_C(1) << 31) || origin > UINT32_MAX ||
        !read_profile_word(arm64, size, source->right_lane, &instruction) ||
        !read_integer_operand(instruction, 0xffc00000u, 0x11000000u, &lane_bits))
        return false;
    double lane;
    memcpy(&lane, &lane_bits, sizeof lane);
    *back = (UINT32_C(0) - (uint32_t)origin) - lane;
    return true;
}

static double vocal_parameter_value(const uint8_t *model, unsigned parameter)
{
    double value;
    memcpy(&value, model + TA_MODEL_VOCAL_OFFSET + parameter * double_bytes, sizeof value);
    return value;
}

static bool copy_frame_reflection(uint8_t *model, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    double left, right;
    double center = vocal_parameter_value(model, TA_VOCAL_FRAME_CENTER);
    if (!read_left_reflection(arm64, profile, center, &left) ||
        !read_right_reflection(arm64, profile, &right))
        return false;
    write_vocal_parameter(model, TA_VOCAL_FRAME_LEFT_BACK, left);
    write_vocal_parameter(model, TA_VOCAL_FRAME_RIGHT_BACK, right);
    return true;
}

static bool read_motion_pair_loads(const uint8_t *arm64, size_t size, uint32_t start, uint8_t *pairs)
{
    uint32_t first, second;
    if (!read_profile_word(arm64, size, start, &first) ||
        !read_profile_word(arm64, size, start + 4, &second) ||
        (first & 0xffc00000u) != 0xad400000u || (second & 0xffc00000u) != 0xad400000u)
        return false;
    unsigned offset = (first >> 15) & 127;
    if (((offset + 2) & 127) != ((second >> 15) & 127) ||
        (first & 0x3e0u) != (second & 0x3e0u))
        return false;
    /* Two pairs of Q registers cover four consecutive double pairs. */
    pairs[first & 31] = 1;
    pairs[(first >> 10) & 31] = 2;
    pairs[second & 31] = 3;
    pairs[(second >> 10) & 31] = 4;
    return true;
}

static bool read_add_accumulators(const uint8_t *arm64, size_t size, uint32_t address,
                                  const uint8_t *pairs, unsigned count, uint8_t *accumulators)
{
    for (unsigned i = 0; i < count; ++i) {
        uint32_t word;
        if (!read_vector_double_add(arm64, size, address + i * 4, &word))
            return false;
        unsigned dest = word & 31, input = (word >> 5) & 31;
        if (((word >> 16) & 31) != dest || pairs[input] == 0 || accumulators[dest] != 0)
            return false;
        accumulators[dest] = pairs[input];
    }
    return true;
}

static bool read_motion_accumulators(const uint8_t *arm64, const struct pvdi_profile *profile,
                                     uint8_t *accumulators)
{
    const struct motion_reduction_source *source = &profile->motion_reduction;
    size_t size = profile->arm64.size;
    uint8_t pairs[32] = {0};
    return read_motion_pair_loads(arm64, size, source->pair_loads, pairs) &&
           read_add_accumulators(arm64, size, source->accumulators, pairs,
                                 TA_VOCAL_MOTION_REDUCE_PAIRS, accumulators);
}

static bool next_reduction_pair(uint32_t add, unsigned previous, const uint8_t *accumulators, uint8_t *pair)
{
    unsigned a = (add >> 5) & 31, b = (add >> 16) & 31;
    if ((a == previous) == (b == previous))
        return false;
    *pair = accumulators[a == previous ? b : a];
    return *pair != 0;
}

static bool read_pair_reduction_order(const uint8_t *arm64, size_t size, uint32_t address,
                                     const uint8_t *accumulators, uint8_t *order, unsigned count)
{
    uint32_t add;
    if (!read_vector_double_add(arm64, size, address, &add))
        return false;
    order[0] = accumulators[(add >> 5) & 31];
    order[1] = accumulators[(add >> 16) & 31];
    if (order[0] == 0 || order[1] == 0)
        return false;
    for (unsigned i = 2; i < count; ++i) {
        unsigned previous = add & 31;
        address += 4;
        if (!read_vector_double_add(arm64, size, address, &add) ||
            !next_reduction_pair(add, previous, accumulators, &order[i]))
            return false;
    }
    uint32_t horizontal;
    return read_profile_word(arm64, size, address + 4, &horizontal) &&
           (horizontal & 0xfffffc00u) == 0x7e70d800u && ((horizontal >> 5) & 31) == (add & 31);
}

static bool motion_seed_element(const uint8_t *arm64, size_t size, uint32_t address, unsigned *element)
{
    if (!contains_range(size, address, 2 * double_bytes))
        return false;
    const uint8_t *table = arm64 + address;
    uint64_t low = read_instruction(table) | (uint64_t)read_instruction(table + 4) << 32;
    uint64_t high = read_instruction(table + 8) | (uint64_t)read_instruction(table + 12) << 32;
    if ((low == 0) == (high == 0))
        return false;
    *element = low == 0;
    return true;
}

static bool read_motion_seed_lane(const uint8_t *arm64, const struct pvdi_profile *profile,
                                  const uint8_t *accumulators, unsigned *lane)
{
    size_t size = profile->arm64.size;
    uint32_t load;
    if (!read_profile_word(arm64, size, profile->motion_reduction.seed_load, &load) ||
        (load & 0xffc00000u) != 0x3dc00000u || accumulators[load & 31] == 0)
        return false;
    /* The epsilon and the other lane originate in the same two-double table. */
    unsigned element;
    if (!motion_seed_element(arm64, size, profile->vocal[TA_VOCAL_MOTION_ENERGY_EPSILON].offset, &element))
        return false;
    *lane = (accumulators[load & 31] - 1) * 2 + element;
    return true;
}

static bool copy_motion_reduction(uint8_t *model, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    uint8_t accumulators[32] = { 0 }, order[TA_VOCAL_MOTION_REDUCE_PAIRS];
    unsigned lane;
    if (!read_motion_accumulators(arm64, profile, accumulators) ||
        !read_pair_reduction_order(arm64, profile->arm64.size, profile->motion_reduction.reduction,
                                   accumulators, order, TA_VOCAL_MOTION_REDUCE_PAIRS) ||
        !read_motion_seed_lane(arm64, profile, accumulators, &lane))
        return false;
    for (unsigned i = 0; i < TA_VOCAL_MOTION_REDUCE_PAIRS; ++i)
        write_vocal_parameter(model, TA_VOCAL_MOTION_REDUCE_PAIR_0 + i, order[i] - 1);
    write_vocal_parameter(model, TA_VOCAL_MOTION_ENERGY_EPSILON_LANE, lane);
    return true;
}

static bool read_postindexed_pair_loads(const uint8_t *arm64, size_t size, uint32_t first_load,
                                         uint32_t second_load, unsigned expected_pairs, uint8_t *pairs)
{
    uint32_t first, second;
    if (!read_profile_word(arm64, size, first_load, &first) ||
        !read_profile_word(arm64, size, second_load, &second) ||
        (first & 0xffc00000u) != 0xad400000u || (second & 0xffc00000u) != 0xacc00000u)
        return false;
    unsigned back_offset = (first >> 15) & 127, stride = (second >> 15) & 127;
    if (stride != expected_pairs || back_offset + stride / 2 != 128 ||
        (first & 0x3e0u) != (second & 0x3e0u))
        return false;
    /* The first load reads the preceding two pairs and the second advances past the group. */
    pairs[first & 31] = 1;
    pairs[(first >> 10) & 31] = 2;
    pairs[second & 31] = 3;
    pairs[(second >> 10) & 31] = 4;
    return true;
}

static void write_pair_order(uint8_t *output, const uint8_t *order, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        double number = order[i] - 1;
        uint64_t bits;
        memcpy(&bits, &number, sizeof number);
        write_le(output + i * double_bytes, bits, double_bytes);
    }
}

static bool read_fma_pairs(const uint8_t *arm64, size_t size, uint32_t address,
                           const uint8_t *pairs, unsigned count, uint8_t *accumulators)
{
    for (unsigned i = 0; i < count; ++i) {
        uint32_t word;
        if (!read_profile_word(arm64, size, address + i * 4, &word) ||
            (word & 0xffe0fc00u) != 0x4e60cc00u)
            return false;
        unsigned dest = word & 31, samples = (word >> 16) & 31;
        if (pairs[samples] == 0 || accumulators[dest] != 0)
            return false;
        accumulators[dest] = pairs[samples];
    }
    return true;
}

static bool copy_cqt_pairs(uint8_t *output, const uint8_t *arm64, size_t size,
                            const uint8_t *pairs, uint32_t fma, uint32_t sum)
{
    uint8_t accumulators[32] = { 0 }, order[TA_OPENING_CQT_PAIR_CAPACITY];
    if (!read_fma_pairs(arm64, size, fma, pairs, TA_OPENING_CQT_PAIR_CAPACITY, accumulators) ||
        !read_pair_reduction_order(arm64, size, sum, accumulators, order, TA_OPENING_CQT_PAIR_CAPACITY))
        return false;
    write_pair_order(output, order, TA_OPENING_CQT_PAIR_CAPACITY);
    return true;
}

static bool copy_cqt_reduction(uint8_t *output, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    const struct cqt_reduction_source *source = &profile->cqt_reduction;
    size_t size = profile->arm64.size;
    uint8_t pairs[32] = { 0 };
    return read_postindexed_pair_loads(arm64, size, source->first_load, source->second_load,
                                       TA_OPENING_CQT_PAIR_CAPACITY, pairs) &&
           copy_cqt_pairs(output + TA_OPENING_CQT_REAL_PAIR_0 * double_bytes, arm64, size,
                          pairs, source->real_fma, source->real_sum) &&
           copy_cqt_pairs(output + TA_OPENING_CQT_IMAG_PAIR_0 * double_bytes, arm64, size,
                          pairs, source->imag_fma, source->imag_sum);
}

static bool read_phrase_sum_order(const uint8_t *arm64, size_t size, struct phrase_sum_source source,
                                   uint8_t *order, uint64_t *width)
{
    const unsigned count = TA_PHRASE_SUM_PAIR_CAPACITY;
    uint8_t pairs[32] = {0}, accumulators[32] = {0};
    struct instruction_constant operand = {source.width, 1, SUBTRACT_FLAGS_IMMEDIATE};
    return read_instruction_constant(arm64, size, operand, width) &&
           read_postindexed_pair_loads(arm64, size, source.first_load, source.second_load, count, pairs) &&
           read_add_accumulators(arm64, size, source.accumulators, pairs, count, accumulators) &&
           read_pair_reduction_order(arm64, size, source.reduction, accumulators, order, count);
}

static bool copy_phrase_sum(uint8_t *output, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    uint8_t full[TA_PHRASE_SUM_PAIR_CAPACITY], range[TA_PHRASE_SUM_PAIR_CAPACITY];
    uint64_t full_width, range_width;
    size_t size = profile->arm64.size;
    if (!read_phrase_sum_order(arm64, size, profile->phrase_sum, full, &full_width) ||
        !read_phrase_sum_order(arm64, size, profile->phrase_range_sum, range, &range_width) ||
        full_width != range_width || memcmp(full, range, sizeof full) != 0)
        return false;
    write_le(output + TA_PHRASE_SUM_GROUP * double_bytes, full_width, double_bytes);
    write_pair_order(output + TA_PHRASE_SUM_PAIR_0 * double_bytes, full, sizeof full);
    return true;
}

static bool copy_recurrence_reduction(uint8_t *output, const uint8_t *arm64,
                                      const struct pvdi_profile *profile)
{
    const struct recurrence_reduction_source *source = &profile->recurrence_reduction;
    size_t size = profile->arm64.size;
    const unsigned count = TA_RECURRENCE_DISTANCE_PAIR_CAPACITY;
    uint8_t pairs[32] = {0}, accumulators[32] = {0}, order[TA_RECURRENCE_DISTANCE_PAIR_CAPACITY];
    if (!read_postindexed_pair_loads(arm64, size, source->first_load, source->second_load, count, pairs) ||
        !read_fma_pairs(arm64, size, source->accumulators, pairs, count, accumulators) ||
        !read_pair_reduction_order(arm64, size, source->reduction, accumulators, order, count))
        return false;
    write_pair_order(output + TA_RECURRENCE_DISTANCE_PAIR_0 * double_bytes, order, count);
    return true;
}

static bool read_opening_history_ratio(const uint8_t *arm64, size_t size,
                                      struct opening_history_ratio_source source, double *index)
{
    uint32_t store, load;
    if (!read_profile_word(arm64, size, source.origin_store, &store) ||
        !read_profile_word(arm64, size, source.ratio_load, &load) ||
        (store & UINT32_C(0xffc00000)) != UINT32_C(0xad000000) ||
        (load & UINT32_C(0xffc00000)) != UINT32_C(0xfd400000))
        return false;
    /* STP Q encodes 16-byte units. LDR D encodes double indices. */
    int first = (int)((store >> 15) & 127);
    int base = ((first ^ 64) - 64) * 2;
    *index = (int)((load >> 10) & 4095) - base;
    return *index >= 0;
}

static bool copy_opening_history_ratio(uint8_t *output, const uint8_t *arm64,
                                      const struct pvdi_profile *profile)
{
    double low, mid;
    size_t size = profile->arm64.size;
    if (!read_opening_history_ratio(arm64, size, profile->opening_low_history_ratio, &low) ||
        !read_opening_history_ratio(arm64, size, profile->opening_mid_history_ratio, &mid) || low != mid)
        return false;
    uint64_t bits;
    memcpy(&bits, &low, sizeof bits);
    write_le(output + TA_OPENING_HISTORY_RATIO_INDEX * double_bytes, bits, double_bytes);
    return true;
}

static bool copy_phrase_payload(uint8_t *output, const uint8_t *arm64, const struct pvdi_profile *profile)
{
    return copy_phrase_parameters(output, arm64, profile) &&
           copy_cqt_reduction(output, arm64, profile) &&
           copy_phrase_sum(output, arm64, profile) &&
           copy_recurrence_reduction(output, arm64, profile) &&
           copy_opening_history_ratio(output, arm64, profile);
}

static bool copy_vocal_payload(uint8_t *model, const uint8_t *graph, const uint8_t *arm64,
                                const struct pvdi_profile *profile)
{
    size_t pos = profile->graph->feature_columns;
    uint32_t columns;
    double window = vocal_parameter_value(model, TA_VOCAL_MOTION_BAND_WIDTH);
    if (!(window >= 1 && window <= UINT16_MAX) || window != (unsigned)window ||
        !read_graph_varint(graph, profile->graph->file.size, &pos, &columns) ||
        columns > UINT16_MAX || columns % 2 == 0)
        return false;
    size_t weights = ta_model_weights_offset((unsigned)window, columns);
    return weights < TA_MODEL_LEGACY_BYTES &&
           copy_vocal_coefficients(model + header_bytes, arm64, profile, (unsigned)window, columns) &&
           copy_vocal_graph(model, graph, profile->graph, columns, weights);
}

static bool copy_vocal_parameters(uint8_t *model, const uint8_t *graph, const uint8_t *arm64,
                                   const struct pvdi_profile *profile)
{
    return copy_parameters(model + TA_MODEL_VOCAL_OFFSET, arm64, profile->arm64.size,
                           profile->vocal, TA_VOCAL_DELTA_FIRST_RADIUS) &&
           copy_vocal_payload(model, graph, arm64, profile) &&
           copy_vocal_delta(model, arm64, profile) &&
           copy_frame_reflection(model, arm64, profile) &&
           copy_motion_reduction(model, arm64, profile) &&
           copy_vocal_thresholds(model + TA_MODEL_THRESHOLD_OFFSET, arm64, profile) &&
           copy_forget_bias(model + TA_MODEL_FORGET_BIAS_OFFSET, graph, profile->graph) &&
           copy_vocal_class(model + TA_MODEL_OUTPUT_CLASS_OFFSET, arm64, profile);
}

static bool build_model(const struct pvdi_profile *profile, const uint8_t *graph,
                        const uint8_t *arm64, const char *output_path)
{
    uint8_t *model = malloc(model_bytes);
    if (!model) {
        perror("Allocate vocal model");
        return false;
    }
    bool valid = copy_phrase_payload(model + TA_MODEL_PHRASE_OFFSET, arm64, profile) &&
                 copy_vocal_parameters(model, graph, arm64, profile);
    if (!valid)
        fprintf(stderr, "Invalid model layout for rekordbox %s\n", profile->bundle_version);
    bool ok = valid && write_model(output_path, model);
    free(model);
    return ok;
}

bool pvdi_import(const struct pvdi_profile *profile, const char *graph_path,
                 const char *arm64_path, const char *output_path)
{
    uint8_t *graph = read_verified(graph_path, profile->graph->file, profile->bundle_version);
    if (!graph)
        return false;
    uint8_t *arm64 = read_verified(arm64_path, profile->arm64, profile->bundle_version);
    bool ok = arm64 && build_model(profile, graph, arm64, output_path);
    free(arm64);
    free(graph);
    return ok;
}

#ifdef PVDI_WINDOWS_IMPORT
static bool windows_phrase_complete(const struct pvdi_windows_phrase *phrase)
{
    for (size_t i = 0; i < TA_PHRASE_PARAMETER_COUNT; ++i) {
        /* Native Windows sums and recurrence use the reduction trailer. */
        bool recipe_field = i >= TA_PHRASE_SUM_PAIR_0 && i <= TA_RECURRENCE_DISTANCE_PAIR_3;
        if (phrase->present[i] == recipe_field) {
            fprintf(stderr, "Unexpected Windows phrase analysis field coverage at %zu\n", i);
            return false;
        }
    }
    return true;
}

static bool windows_vocal_complete(const struct pvdi_windows_vocal *vocal)
{
    for (size_t i = 0; i < TA_VOCAL_PARAMETER_COUNT; ++i) {
        /* Native Windows motion supplies these operations as a recipe. */
        bool recipe_field = i == TA_VOCAL_MOTION_ZERO_LAG || i >= TA_VOCAL_MOTION_REDUCE_PAIR_0;
        if (vocal->present[i] == recipe_field) {
            fprintf(stderr, "Unexpected Windows vocal analysis field %zu\n", i);
            return false;
        }
    }
    return true;
}

static void write_double_array(uint8_t *output, const double *values, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint64_t bits;
        memcpy(&bits, &values[i], sizeof bits);
        write_le(output + i * double_bytes, bits, double_bytes);
    }
}

static void write_windows_vocal_tables(uint8_t *model, const struct pvdi_windows_source *source)
{
    const struct pvdi_windows_network *network = &source->network;
    uint8_t *window = model + header_bytes;
    uint8_t *scale = window + PVDI_WINDOWS_CHEBYSHEV_COUNT * double_bytes;
    uint8_t *offset = scale + PVDI_WINDOWS_AFFINE_COUNT * double_bytes;
    write_double_array(window, network->window, PVDI_WINDOWS_CHEBYSHEV_COUNT);
    write_double_array(scale, network->scale, PVDI_WINDOWS_AFFINE_COUNT);
    write_double_array(offset, network->offset, PVDI_WINDOWS_AFFINE_COUNT);
    write_le(model + TA_MODEL_OUTPUT_CLASS_OFFSET, network->output_class, 4);
    for (size_t i = 0; i < TA_VOCAL_THRESHOLD_COUNT; ++i) {
        uint32_t bits;
        memcpy(&bits, &network->thresholds[i], sizeof bits);
        write_le(model + TA_MODEL_THRESHOLD_OFFSET + i * 4, bits, 4);
    }
}

static bool copy_windows_network(uint8_t *model, const struct pvdi_windows_source *source)
{
    size_t position = graph_7219.feature_columns;
    uint32_t columns;
    if (!read_graph_varint(source->graph, graph_7219.file.size, &position, &columns) ||
        columns != PVDI_WINDOWS_AFFINE_COUNT ||
        source->vocal.value[TA_VOCAL_MOTION_BAND_WIDTH] != PVDI_WINDOWS_CHEBYSHEV_COUNT)
        return false;
    size_t weights = ta_model_weights_offset(PVDI_WINDOWS_CHEBYSHEV_COUNT, columns);
    write_windows_vocal_tables(model, source);
    return copy_vocal_graph(model, source->graph, &graph_7219, columns, weights) &&
           copy_forget_bias(model + TA_MODEL_FORGET_BIAS_OFFSET, source->graph, &graph_7219);
}

static bool write_windows_model(FILE *output, const struct pvdi_windows_source *source)
{
    if (!windows_phrase_complete(&source->phrase) || !windows_vocal_complete(&source->vocal))
        return false;
    uint8_t *model = calloc(TA_MODEL_WINDOWS_BYTES, 1);
    if (!model) {
        perror("Allocate vocal model");
        return false;
    }
    write_double_array(model + TA_MODEL_PHRASE_OFFSET, source->phrase.value, TA_PHRASE_PARAMETER_COUNT);
    write_double_array(model + TA_MODEL_VOCAL_OFFSET, source->vocal.value, TA_VOCAL_PARAMETER_COUNT);
    memcpy(model + TA_MODEL_BYTES, source->vocal.motion, TA_MODEL_WINDOWS_MOTION_BYTES);
    memcpy(model + TA_MODEL_WINDOWS_REDUCTION_OFFSET, source->phrase.reduction, TA_MODEL_WINDOWS_REDUCTION_BYTES);
    bool ok = copy_windows_network(model, source);
    if (ok) {
        write_header(model, TA_MODEL_WINDOWS_MAGIC, TA_MODEL_WINDOWS_BYTES);
        ok = fwrite(model, 1, TA_MODEL_WINDOWS_BYTES, output) == TA_MODEL_WINDOWS_BYTES;
    }
    if (!ok)
        fprintf(stderr, "Could not write complete Windows model from rekordbox %s\n", source->version);
    free(model);
    return ok;
}

bool pvdi_import_windows(FILE *executable, FILE *encrypted_graph, FILE *output)
{
    static const char *const errors[] = {
        [PVDI_WINDOWS_EXECUTABLE_IO] = "Could not read Windows rekordbox executable",
        [PVDI_WINDOWS_UNSUPPORTED_EXECUTABLE] =
            "Unsupported Windows rekordbox executable. Verified x64 builds are 7.2.17.0 through 7.2.20.0",
        [PVDI_WINDOWS_GRAPH_MISMATCH] = "Input does not match the verified Windows vd_param.pb",
        [PVDI_WINDOWS_INVALID_OPERAND] = "Invalid Windows analysis operand"
    };
    struct pvdi_windows_source source;
    enum pvdi_windows_status status = pvdi_windows_load(executable, encrypted_graph, &source);
    if (status != PVDI_WINDOWS_OK) {
        fprintf(stderr, "%s\n", errors[status]);
        return false;
    }
    bool ok = write_windows_model(output, &source);
    pvdi_windows_release(&source);
    return ok;
}
#endif
