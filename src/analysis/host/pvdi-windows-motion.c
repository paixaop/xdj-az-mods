/* SPDX-License-Identifier: GPL-3.0-only */
#include "pvdi-windows-motion.h"

#include <string.h>

enum {
    reference_inference = 0x13a9c40,
    code_rva = 0x1000,
    code_offset = 0x400,
    max_additions = 7
};

enum motion_record {
    weighted_energy = 1, magnitude_energy, log_flatness, magnitude_flatness,
    negative_correlation, nonnegative_correlation, finish_energy, finish_flatness,
    finish_correlation, argmax
};

enum sample { magnitude = 1, window, log_magnitude, prior, following };
enum accumulation { acc_plus_value = 1, value_plus_acc, fma_into_acc };
enum tail { no_tail, scalar_mul_acc, scalar_mul_acc_reversed };
enum motion_operation {
    ordered_gt = 1, unordered_or_lt, add_epsilon_before_division, log_after_epsilon_add,
    exponent_after_mean_multiply, mean_fma_epsilon, divide_then_multiply, computed_center,
    keep_right_on_unordered_or_equal, first_index_among_equal, seed_lanes_from_first_vector,
    masked_final_vector, scalar_after_full_vectors
};

struct motion_profile {
    size_t size;
    uint32_t inference, text_bytes, rdata_rva, rdata_offset, rdata_bytes;
};

/* Section geometry and inference RVAs of the authenticated Windows PE cohort. */
static const struct motion_profile profiles[] = {
    {100441520, 0x1240e00, 0x335a600, 0x335c000, 0x335aa00, 0x27b8000},
    {100561840, 0x1241810, 0x3372800, 0x3374000, 0x3372c00, 0x27bc400},
    {103540656, 0x13253d0, 0x3511a00, 0x3513000, 0x3511e00, 0x28cc800},
    {104436656, 0x13a9c40, 0x35c5800, 0x35c7000, 0x35c5c00, 0x28eba00}
};

struct extraction {
    const uint8_t *image;
    const struct motion_profile *profile;
    uint32_t displacement;
    bool valid;
    uint8_t recipe[PVDI_WINDOWS_MOTION_BYTES];
    size_t used;
};

struct opcode_check { uint32_t address; size_t size; const char *code; };
#define CHECK(address, code) {(address), sizeof(code) - 1, (code)}
#define COUNT(array) (sizeof(array) / sizeof(*(array)))

static uint32_t read32(const uint8_t *bytes)
{
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static const uint8_t *native_range(struct extraction *work, uint32_t rva, size_t length,
                                   uint32_t section_rva, uint32_t offset, uint32_t section_bytes)
{
    static const uint8_t empty[16] = {0};
    uint32_t relative = rva - section_rva;
    if (length > section_bytes || relative > section_bytes - length) {
        work->valid = false;
        return empty;
    }
    return work->image + offset + relative;
}

static const uint8_t *code_at(struct extraction *work, uint32_t address, size_t length)
{
    return native_range(work, address, length, code_rva, code_offset, work->profile->text_bytes);
}

static const uint8_t *code_ref(struct extraction *work, uint32_t address, size_t length)
{
    return code_at(work, address + work->displacement, length);
}

static bool check_opcodes(struct extraction *work, uint32_t base, const struct opcode_check *checks, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const struct opcode_check *check = &checks[i];
        const uint8_t *native = code_at(work, base + check->address, check->size);
        if (memcmp(native, check->code, check->size) != 0)
            return false;
    }
    return work->valid;
}

static uint32_t branch_target(struct extraction *work, uint32_t address, uint8_t opcode)
{
    const uint8_t *p = code_at(work, address, 5);
    if (*p != opcode)
        work->valid = false;
    return address + 5 + read32(p + 1);
}

static const uint8_t *constant_at(struct extraction *work, uint32_t instruction)
{
    const uint8_t *p = code_at(work, instruction, 8);
    if ((p[0] != 0xc5 && p[0] != 0xf2) || (p[3] & 0xc7) != 5)
        work->valid = false;
    uint32_t rva = instruction + 8 + read32(p + 4);
    const struct motion_profile *m = work->profile;
    return native_range(work, rva, 8, m->rdata_rva, m->rdata_offset, m->rdata_bytes);
}

static void emit(struct extraction *work, unsigned value)
{
    if (work->used >= sizeof(work->recipe) || value > UINT8_MAX) {
        work->valid = false;
        return;
    }
    work->recipe[work->used++] = (uint8_t)value;
}

static void emit16(struct extraction *work, unsigned value)
{
    emit(work, value & 255);
    emit(work, value >> 8);
}

static void emit_constant(struct extraction *work, const uint8_t *data)
{
    for (size_t i = 0; i < 8; ++i)
        emit(work, data[i]);
}

static void emit_reference(struct extraction *work, uint32_t instruction)
{
    emit_constant(work, constant_at(work, instruction + work->displacement));
}

static void record(struct extraction *work, enum motion_record tag, unsigned size)
{
    emit(work, tag);
    emit16(work, size);
}

struct pair { uint8_t left, right; };
struct reduction_tree {
    unsigned leaves, length;
    struct pair additions[max_additions];
};

struct vector { uint8_t lane[4]; unsigned width; };
struct avx_operands { unsigned dst, lhs, rhs, width; };

static unsigned register_operand(const uint8_t *p, unsigned modrm)
{
    return ((~p[1] & 0x80) >> 4) | ((p[modrm] >> 3) & 7);
}

static bool packed_operation(const uint8_t *p, uint8_t opcode)
{
    return p[0] == 0xc5 && p[2] == opcode && (p[1] & 3) == 1 && (p[3] & 0xc0) == 0xc0;
}

static struct avx_operands packed_operands(struct extraction *work, uint32_t rva, uint8_t opcode)
{
    const uint8_t *p = code_ref(work, rva, 4);
    if (!packed_operation(p, opcode))
        work->valid = false;
    return (struct avx_operands){register_operand(p, 3), ((~p[1]) & 0x78) >> 3, p[3] & 7, p[1] & 4 ? 4 : 2};
}

static bool vector_fma(const uint8_t *p)
{
    return p[0] == 0xc4 && (p[1] & 0x1f) == 2 && (p[2] & 0x87) == 0x85 && p[3] == 0xb8;
}

static unsigned fma_register(struct extraction *work, uint32_t address)
{
    const uint8_t *p = code_ref(work, address, 5);
    if (!vector_fma(p))
        work->valid = false;
    return register_operand(p, 4);
}

static unsigned magnitude_register(struct extraction *work, uint32_t address)
{
    struct avx_operands operands = packed_operands(work, address, 0x58);
    if (operands.dst != operands.lhs || operands.width != 4)
        work->valid = false;
    return operands.dst;
}

static uint8_t add_node(struct extraction *work, struct reduction_tree *tree, uint8_t left, uint8_t right)
{
    for (unsigned i = 0; i < tree->length; ++i) {
        if (tree->additions[i].left == left && tree->additions[i].right == right)
            return (uint8_t)(tree->leaves + i);
    }
    if (tree->length >= max_additions) {
        work->valid = false;
        return 0;
    }
    tree->additions[tree->length] = (struct pair){left, right};
    return (uint8_t)(tree->leaves + tree->length++);
}

static void packed_add(struct extraction *work, struct reduction_tree *tree, struct vector *regs, uint32_t address)
{
    struct avx_operands op = packed_operands(work, address, 0x58);
    struct vector left = regs[op.lhs], right = regs[op.rhs], result = {.width = op.width};
    if (left.width < op.width || right.width < op.width)
        work->valid = false;
    for (unsigned i = 0; i < op.width; ++i)
        result.lane[i] = add_node(work, tree, left.lane[i], right.lane[i]);
    regs[op.dst] = result;
}

static void horizontal_add(struct extraction *work, struct reduction_tree *tree, struct vector *regs, uint32_t address)
{
    struct avx_operands op = packed_operands(work, address, 0x7c);
    struct vector left = regs[op.lhs], right = regs[op.rhs], result = {.width = 4};
    if (left.width != 4 || right.width != 4 || op.width != 4)
        work->valid = false;
    for (unsigned i = 0; i < 4; i += 2) {
        result.lane[i] = add_node(work, tree, left.lane[i], left.lane[i + 1]);
        result.lane[i + 1] = add_node(work, tree, right.lane[i], right.lane[i + 1]);
    }
    regs[op.dst] = result;
}

static bool half_extract_operation(const uint8_t *p)
{
    return p[0] == 0xc4 && (p[1] & 0x1f) == 3 && p[3] == 0x19 && (p[4] & 0xc0) == 0xc0 && p[5] <= 1;
}

static void extract_half(struct extraction *work, struct vector *regs, uint32_t address)
{
    const uint8_t *p = code_ref(work, address, 6);
    if (!half_extract_operation(p)) {
        work->valid = false;
        return;
    }
    struct vector src = regs[register_operand(p, 4)];
    if (src.width != 4)
        work->valid = false;
    unsigned dst = ((~p[1] & 0x20) >> 2) | (p[4] & 7);
    unsigned lane = 2 * p[5];
    regs[dst] = (struct vector){{src.lane[lane], src.lane[lane + 1]}, 2};
}

struct reduction {
    enum motion_record tag;
    enum accumulation update;
    enum sample first, second;
    enum tail tail;
    unsigned lanes, low_register, high_register;
    uint32_t combine, horizontal, extract, final;
};

static uint8_t build_tree(struct extraction *work, const struct reduction *s, struct reduction_tree *tree)
{
    struct vector regs[16] = {0};
    regs[s->low_register] = (struct vector){{0, 1, 2, 3}, 4};
    if (s->lanes == 8) {
        regs[s->high_register] = (struct vector){{4, 5, 6, 7}, 4};
        packed_add(work, tree, regs, s->combine);
    }
    horizontal_add(work, tree, regs, s->horizontal);
    extract_half(work, regs, s->extract);
    packed_add(work, tree, regs, s->final);
    return regs[packed_operands(work, s->final, 0x58).dst].lane[0];
}

static void reduction_record(struct extraction *work, const struct reduction *source)
{
    struct reduction_tree tree = {.leaves = source->lanes};
    uint8_t root = build_tree(work, source, &tree);
    if (tree.length + 1 != source->lanes) {
        work->valid = false;
        return;
    }
    record(work, source->tag, 7 + 2 * tree.length);
    emit(work, source->lanes);
    emit(work, source->update);
    emit(work, source->first);
    emit(work, source->second);
    emit(work, source->tail);
    emit(work, tree.length);
    for (unsigned i = 0; i < tree.length; ++i) {
        emit(work, tree.additions[i].left);
        emit(work, tree.additions[i].right);
    }
    emit(work, root);
}

static uint32_t immediate32(struct extraction *work, uint32_t address, unsigned offset)
{
    return read32(code_ref(work, address, offset + 4) + offset);
}

static bool energy_window(struct extraction *work)
{
    static const struct opcode_check site[] = {
        CHECK(0x13ae1b4, "\xc5\xfd\x10\x09"),
        CHECK(0x13ae1ca, "\x48\x8d\x49\x40"),
        CHECK(0x13ae1d2, "\xc5\xfd\x10\x49\xe0"),
        CHECK(0x13ae1a8, "\xc5\xe9\x57\xd2\xc5\xd9\x57\xe4\xc5\xd1\x57\xed"),
        CHECK(0x13ae16d, "\xc5\xe1\x57\xdb"),
        CHECK(0x13ae1bc, "\x41\x83\xc2"),
        CHECK(0x13abbb3, "\x41\xb9"),
        CHECK(0x13ae203, "\xc4\x41\x7b\x58\xc0"), /* Post-reduction scalar epsilon. */
        CHECK(0x13ae217, "\xc4\xc1\x63\x5e\xc0"),
        CHECK(0x13abbc9, "\xc5\xf9\x2f\xc6\x0f\x86")
    };
    if (!check_opcodes(work, work->displacement, site, COUNT(site)))
        return false;
    uint32_t target = branch_target(work, 0x13abbc4 + work->displacement, 0xe8);
    unsigned lanes = *code_ref(work, 0x13ae1bf, 1);
    unsigned width = immediate32(work, 0x13abbb3, 2);
    if (target != 0x13ae150 + work->displacement || lanes != 8 || !width || width % lanes)
        return false;

    struct reduction weighted = {weighted_energy, fma_into_acc, magnitude, window, no_tail, lanes,
        fma_register(work, 0x13ae1c0), fma_register(work, 0x13ae1d7),
        0x13ae1fb, 0x13ae1ff, 0x13ae208, 0x13ae20e};
    struct reduction plain = {magnitude_energy, acc_plus_value, magnitude, 0, no_tail, lanes,
        magnitude_register(work, 0x13ae1ce), magnitude_register(work, 0x13ae1dd),
        0x13ae1e9, 0x13ae1ed, 0x13ae1f1, 0x13ae1f7};
    reduction_record(work, &weighted);
    reduction_record(work, &plain);
    record(work, finish_energy, 20);
    emit(work, add_epsilon_before_division);
    emit(work, ordered_gt);
    emit16(work, width);
    emit_reference(work, 0x13ae165);
    emit_reference(work, 0x13abb4e);
    return work->valid;
}

static bool flatness_window(struct extraction *work)
{
    static const struct opcode_check site[] = {
        CHECK(0x13abbd3, "\xc5\xf9\x57\xc0"),
        CHECK(0x13abbdf, "\xc5\xf1\x57\xc9"),
        CHECK(0x13abbf1, "\xc4\xc1\x7d\x10\x06"),
        CHECK(0x13abbf6, "\xc5\xfd\x58\x05"),
        CHECK(0x13abbfe, "\xe8"), /* Log after adding log epsilon to the input vector. */
        CHECK(0x13abc03, "\xc5\xfd\x58\x95\xa0\x00\x00\x00"),
        CHECK(0x13abc18, "\xc5\xf5\x58\x85\xc0\x00\x00\x00"),
        CHECK(0x13abc28, "\x4d\x8d\x76"),
        CHECK(0x13abbeb, "\x41\xbc"),
        CHECK(0x13abc2c, "\x49\x83\xec\x01\x75\xbf"),
        CHECK(0x13abc40, "\xc5\xfb\x59\xc7"),
        CHECK(0x13abc47, "\xe8"), /* Exponent after scaling the sum. */
        CHECK(0x13abc66, "\xc4\xc2\xe9\xa9\xc8\xc5\xfb\x5e\xc1"),
        CHECK(0x13abc73, "\xc4\xc1\x79\x2f\xc1\x73")
    };
    if (!check_opcodes(work, work->displacement, site, COUNT(site)))
        return false;
    unsigned lanes = *code_ref(work, 0x13abc2b, 1) / 8u;
    unsigned iterations = immediate32(work, 0x13abbeb, 2);
    if (lanes != 4 || !iterations || iterations > UINT16_MAX / lanes)
        return false;

    struct reduction logs = {log_flatness, value_plus_acc, log_magnitude, 0, no_tail, lanes,
        register_operand(code_ref(work, 0x13abc03, 4), 3), 0,
        0, 0x13abc32, 0x13abc36, 0x13abc3c};
    struct reduction plain = {magnitude_flatness, value_plus_acc, magnitude, 0, no_tail, lanes,
        register_operand(code_ref(work, 0x13abc4c, 4), 3), 0,
        0, 0x13abc54, 0x13abc58, 0x13abc5e};
    reduction_record(work, &logs);
    reduction_record(work, &plain);
    record(work, finish_flatness, 38);
    emit(work, log_after_epsilon_add);
    emit(work, exponent_after_mean_multiply);
    emit(work, mean_fma_epsilon);
    emit(work, unordered_or_lt);
    emit16(work, iterations * lanes);
    emit_reference(work, 0x13abbf6);
    emit_reference(work, 0x13abb56);
    emit_reference(work, 0x13abb5e);
    emit_reference(work, 0x13abb66);
    return work->valid;
}

static bool correlation_reductions(struct extraction *work)
{
    static const struct opcode_check site[] = {
        CHECK(0x13acdca, "\x4d\x8d\x04\xc8\x2b\xd8"),
        CHECK(0x13ace88, "\x4d\x8d\x04\xfe"),
        CHECK(0x13acf2c, "\x49\xff\xc3\x41\xff\xc2"),
        CHECK(0x13ace10, "\xc5\xfd\x10\x09"),
        CHECK(0x13ace19, "\xc5\xfd\x10\x49\x20"),
        CHECK(0x13ace2c, "\x49\x83\xc1"),
        CHECK(0x13ace50, "\xc5\xfb\x10\x04\x08\xc5\xfb\x59\x09"),
        CHECK(0x13ace5d, "\xc5\xeb\x58\xd1"),
        CHECK(0x13aced0, "\xc5\xfd\x10\x08"),
        CHECK(0x13aced9, "\xc5\xfd\x10\x48\x20"),
        CHECK(0x13aceec, "\x49\x83\xc1"),
        CHECK(0x13acf10, "\xc5\xfb\x10\x04\x08\xc5\xfb\x59\x09"),
        CHECK(0x13acf1d, "\xc5\xeb\x58\xd1")
    };
    if (!check_opcodes(work, work->displacement, site, COUNT(site)))
        return false;
    unsigned negative_lanes = *code_ref(work, 0x13ace2f, 1);
    unsigned positive_lanes = *code_ref(work, 0x13aceef, 1);
    if (negative_lanes != 8 || positive_lanes != 8)
        return false;
    struct reduction negative = {negative_correlation, fma_into_acc, following, prior,
        scalar_mul_acc_reversed, negative_lanes, fma_register(work, 0x13ace14), fma_register(work, 0x13ace1e),
        0x13ace35, 0x13ace39, 0x13ace3d, 0x13ace43};
    struct reduction positive = {nonnegative_correlation, fma_into_acc, following, prior,
        scalar_mul_acc, positive_lanes, fma_register(work, 0x13aced4), fma_register(work, 0x13acede),
        0x13acef5, 0x13acef9, 0x13acefd, 0x13acf03};
    reduction_record(work, &negative);
    reduction_record(work, &positive);
    return work->valid;
}

static bool scaling_operation(const uint8_t *p)
{
    return p[0] == 0xc5 && p[2] == 0x59 && (p[3] & 0xc7) == 0x85 && (~p[1] & 0x78) == 0x20;
}

static unsigned scaling_mask(struct extraction *work, uint32_t rva, uint32_t first)
{
    const uint8_t *p = code_ref(work, rva, 8);
    if (!scaling_operation(p)) {
        work->valid = false;
        return 0;
    }
    unsigned width = p[1] & 4 ? 4 : (p[1] & 3) == 3 ? 1 : 2;
    uint32_t offset = read32(p + 4) - first;
    if (offset % 8 || offset / 8 + width > 16) {
        work->valid = false;
        return 0;
    }
    return ((1u << width) - 1) << (offset / 8);
}

static unsigned normalized_candidates(struct extraction *work, uint32_t first)
{
    static const uint32_t sites[] = {0x13ae553, 0x13ae55b, 0x13ae56b, 0x13ae57b};
    unsigned mask = 0;
    for (size_t i = 0; i < COUNT(sites); ++i)
        mask |= scaling_mask(work, sites[i], first);
    return mask;
}

struct candidate_range {
    uint32_t width, low, high, first_stack, center_stack, end_stack;
};

static bool valid_candidates(struct candidate_range c)
{
    return c.high >= c.low && c.high - c.low < 16 && c.width / 2 >= c.low && c.width / 2 <= c.high;
}

static bool valid_candidate_stack(struct candidate_range c)
{
    if (c.end_stack <= c.first_stack || c.center_stack < c.first_stack || c.center_stack >= c.end_stack)
        return false;
    return c.center_stack - c.first_stack == (c.width / 2 - c.low) * 8 &&
           c.end_stack - c.first_stack == (c.high - c.low + 1) * 8;
}

static bool correlation_selection(struct extraction *work)
{
    static const struct opcode_check site[] = {
        CHECK(0x13ae539, "\xe8"),
        CHECK(0x13acdae, "\xd1\xf8"), /* Correlation center is half the band width. */
        CHECK(0x13ae546, "\xc5\xfb\x5e\xa5") /* Reciprocal of the computed center. */
    };
    if (!check_opcodes(work, work->displacement, site, COUNT(site)))
        return false;
    uint32_t helper = branch_target(work, 0x13ae539 + work->displacement, 0xe8);
    if (helper != 0x13acd80 + work->displacement)
        return false;
    struct candidate_range c = {.width = immediate32(work, 0x13ae52e, 4),
        .low = immediate32(work, 0x13ae520, 4), .high = immediate32(work, 0x13ae513, 4),
        .first_stack = immediate32(work, 0x13ae59d, 3), .center_stack = immediate32(work, 0x13ae546, 4),
        .end_stack = immediate32(work, 0x13ae596, 3)};
    if (!valid_candidates(c) || !valid_candidate_stack(c))
        return false;
    unsigned mask = normalized_candidates(work, c.first_stack);
    if (mask != (1u << (c.high - c.low + 1)) - 1)
        return false;
    record(work, finish_correlation, 20);
    emit(work, divide_then_multiply);
    emit(work, computed_center);
    emit16(work, c.width);
    emit16(work, c.low);
    emit16(work, c.high);
    emit16(work, c.width / 2);
    emit16(work, mask);
    emit_reference(work, 0x13ae53e);
    return work->valid;
}

struct selection_paths { uint32_t dispatch, wide, narrow; };

static bool selection_dispatch(struct extraction *work, struct selection_paths *paths)
{
    static const struct opcode_check site[] = {
        CHECK(0x10, "\x48\x83\xf9"), CHECK(0x27, "\x48\x83\xf9"),
        CHECK(0x16, "\x41\xf6\xc0"), CHECK(0x2d, "\x41\xf6\xc0"),
        CHECK(0x54, "\x66\x0f\x2f\xc1\x76")
    };
    paths->dispatch = branch_target(work, 0x13ae5a7 + work->displacement, 0xe8);
    if (!check_opcodes(work, paths->dispatch, site, COUNT(site)))
        return false;
    paths->wide = branch_target(work, paths->dispatch + 0x22, 0xe9);
    paths->narrow = branch_target(work, paths->dispatch + 0x39, 0xe9);
    return work->valid;
}

static bool check_selection_paths(struct extraction *work, const struct selection_paths *paths)
{
    static const struct opcode_check wide[] = {
        CHECK(0x75, "\xc5\xf5\xc2\xc4\x1e"), CHECK(0x88, "\xc5\xf5\x5f\xe4"),
        CHECK(0xd3, "\xc5\xfd\x5f\xd4"), CHECK(0xdd, "\xc5\xf5\x5f\xc2"),
        CHECK(0x27, "\xc5\xfd\x10\x09"), CHECK(0xad, "\xc4\xc2\x6d\x2d\x00"),
        CHECK(0xb2, "\xc4\xe3\x75\x4b\xd8\x20"), CHECK(0xbd, "\xc5\xf5\xdb\xc2"),
        CHECK(0x11e, "\x48\x0f\x42\xc8")
    };
    static const struct opcode_check narrow[] = {
        CHECK(0x4b, "\x66\x0f\xc2\xc1\x01"), CHECK(0x50, "\x66\x0f\x5f\xcc"),
        CHECK(0x72, "\x66\x0f\x5f\xec"), CHECK(0x10, "\x0f\x10\x21"),
        CHECK(0xf0, "\xf2\x41\x0f\x10\x00\x66\x0f\x2f\xc3\x76"),
        CHECK(0xb0, "\x48\x0f\x43\xc8")
    };
    return check_opcodes(work, paths->wide, wide, COUNT(wide)) &&
           check_opcodes(work, paths->narrow, narrow, COUNT(narrow));
}

static void emit_selection_permutations(struct extraction *work, const struct selection_paths *paths)
{
    uint8_t adjacent = code_at(work, paths->wide + 0xce, 5)[4];
    uint8_t halves = code_at(work, paths->wide + 0xd7, 6)[5];
    uint8_t narrow = code_at(work, paths->narrow + 0x6d, 5)[4];
    for (unsigned i = 0; i < 4; ++i) {
        emit(work, (i & 2) + ((adjacent >> i) & 1));
        emit(work, (halves >> (2 * i)) & 3);
    }
    for (unsigned i = 0; i < 2; ++i)
        emit(work, (narrow >> i) & 1);
}

static bool emit_argmax(struct extraction *work)
{
    struct selection_paths paths;
    if (!selection_dispatch(work, &paths) || !check_selection_paths(work, &paths))
        return false;
    const uint8_t *score = constant_at(work, paths.dispatch + 0x3e);
    const uint8_t *wide_score = constant_at(work, paths.wide + 0x2b);
    const uint8_t *narrow_score = constant_at(work, paths.narrow + 0x4);
    if (memcmp(score, wide_score, 8) || memcmp(score, narrow_score, 8))
        return false;
    record(work, argmax, 26);
    emit(work, code_at(work, paths.dispatch + 0x10, 4)[3] / 8u);
    emit(work, code_at(work, paths.dispatch + 0x27, 4)[3] / 8u);
    emit(work, ordered_gt);
    emit(work, keep_right_on_unordered_or_equal);
    emit(work, first_index_among_equal);
    emit_constant(work, score);
    emit_selection_permutations(work, &paths);
    emit(work, seed_lanes_from_first_vector);
    emit(work, masked_final_vector);
    emit(work, scalar_after_full_vectors);
    return work->valid;
}

static bool extract_recipe(struct extraction *work)
{
    static const uint8_t magic[] = {'P', 'V', 'D', 'I', 'M', 'O', 'T', '1'};
    memcpy(work->recipe, magic, sizeof magic);
    work->used = sizeof magic;
    return energy_window(work) && flatness_window(work) && correlation_reductions(work) &&
           correlation_selection(work) && emit_argmax(work) && work->used == sizeof(work->recipe);
}

bool pvdi_windows_motion_extract(const uint8_t *pe, size_t pe_size, unsigned cohort,
                                 uint8_t *recipe, size_t recipe_capacity, size_t *recipe_size)
{
    if (!recipe_size)
        return false;
    *recipe_size = 0;
    if (!pe || !recipe || recipe_capacity < PVDI_WINDOWS_MOTION_BYTES || cohort >= COUNT(profiles))
        return false;
    const struct motion_profile *profile = &profiles[cohort];
    if (pe_size != profile->size)
        return false;
    struct extraction work = {.image = pe, .profile = profile,
        .displacement = profile->inference - reference_inference, .valid = true};
    if (!extract_recipe(&work))
        return false;
    memcpy(recipe, work.recipe, work.used);
    *recipe_size = work.used;
    return true;
}
