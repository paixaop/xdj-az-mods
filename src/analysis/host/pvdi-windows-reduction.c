/* SPDX-License-Identifier: GPL-3.0-only */
#include "pvdi-windows-reduction.h"
#include <string.h>

/* Raw .text offsets from the authenticated Windows x64 releases, starting at 7.2.17. */
static const struct {
    size_t sum, recurrence;
} profiles[] = {
    {0x011e9450, 0x011eafe0},
    {0x011e9e60, 0x011eb9f0},
    {0x012cda20, 0x012cf5b0},
    {0x01352290, 0x01353e20},
};

#define MATCH(bytes, literal) (memcmp((bytes), (literal), sizeof(literal) - 1) == 0)

static unsigned vex_destination(const uint8_t *opcode)
{
    unsigned modrm = opcode[0] == 0xc4 ? 4 : 3;
    return ((opcode[modrm] >> 3) & 7u) + ((opcode[1] & 0x80) ? 0 : 8);
}

static unsigned vex_first_input(const uint8_t *opcode)
{
    return ((opcode[1] ^ 0xffu) >> 3) & 15u;
}

static int input_bank(unsigned reg, unsigned low, unsigned high)
{
    if (reg == low)
        return 0;
    return reg == high ? TA_WINDOWS_REDUCTION_LANES / 2 : -1;
}

static bool cross_banks(const uint8_t *operation, unsigned low, unsigned high,
                        uint8_t pairs[TA_WINDOWS_REDUCTION_PAIRS * 2])
{
    if (operation[0] != 0xc5 || !(operation[1] & 4) || operation[2] != 0x58 ||
        (operation[3] & 0xc0) != 0xc0)
        return false;
    int left = input_bank(vex_first_input(operation), low, high);
    int right = input_bank(operation[3] & 7, low, high);
    if (left < 0 || right < 0 || left == right)
        return false;
    for (int i = 0; i < TA_WINDOWS_REDUCTION_LANES / 2; ++i) {
        pairs[i * 2] = (uint8_t)(left + i);
        pairs[i * 2 + 1] = (uint8_t)(right + i);
    }
    return true;
}

static bool finish_tree(const uint8_t *finish, uint8_t *pairs)
{
    if (finish[0] != 0xc5 || finish[2] != 0x58 || (finish[3] & 0xc0) != 0xc0)
        return false;
    unsigned inputs = (vex_first_input(finish) << 4) | (finish[3] & 7u);
    if (inputs != 0x01 && inputs != 0x10)
        return false;
    pairs[12] = (uint8_t)(12 + (inputs == 0x01));
    pairs[13] = (uint8_t)(12 + (inputs == 0x10));
    return true;
}

static bool horizontal_tree(const uint8_t *tree, uint8_t *pairs)
{
    const uint8_t *horizontal = tree + 4;
    const uint8_t *extract = tree + 8;
    if (!MATCH(horizontal, "\xc5\xfd\x7c\xc8") || !MATCH(extract, "\xc4\xe3\x7d\x19\xc8\x01") ||
        vex_destination(tree) != vex_first_input(horizontal))
        return false;
    /* VHADDPD adds adjacent lanes, then the last scalar add consumes the high half first. */
    pairs[8] = 8;
    pairs[9] = 9;
    pairs[10] = 10;
    pairs[11] = 11;
    return finish_tree(tree + 14, pairs);
}

static bool sum_width(const uint8_t *sum)
{
    return MATCH(sum + 0x1d, "\x48\x83\xf8") && MATCH(sum + 0x3d, "\x49\x83\xc0") &&
           sum[0x20] == TA_WINDOWS_REDUCTION_LANES && sum[0x40] == sum[0x20];
}

static bool sum_recipe(const uint8_t *sum, uint8_t *output)
{
    if (!sum_width(sum))
        return false;
    const uint8_t *first_add = sum + 0x30;
    const uint8_t *second_add = sum + 0x34;
    if (!MATCH(first_add, "\xc5\xfd\x58\x01") || !MATCH(second_add, "\xc5\xf5\x58\x49\x20") ||
        !MATCH(sum + 0x58, "\xc5\xeb\x58\xd1") || !MATCH(sum + 0x61, "\xc5\xeb\x58\x11"))
        return false;
    const uint8_t modes[] = {1, sum[0x20], TA_WINDOWS_REDUCTION_PAIRS, 0, 0, 0, 1};
    memcpy(output, modes, sizeof modes);
    uint8_t *pairs = output + sizeof modes;
    return cross_banks(sum + 0x46, vex_destination(first_add), vex_destination(second_add), pairs) &&
           horizontal_tree(sum + 0x46, pairs);
}

static bool recurrence_update(const uint8_t *recurrence)
{
    /* Two ordered subtractions preceding each FMA. Tail product and addition are separate. */
    return MATCH(recurrence + 0x275, "\xc5\xfd\x5c\x4c\x08\xe0\xc4\xc1\x75\x5c\xd2\xc5\xed\x58\xde") &&
           MATCH(recurrence + 0x284, "\xc4\xe2\xe5\xb8\xe3") &&
           MATCH(recurrence + 0x29b, "\xc4\x62\xe5\xb8\xc3") &&
           MATCH(recurrence + 0x2e0, "\xc4\xa1\x7b\x10\x04\x00\xc5\xfb\x5c\x08\xc4\xc1\x73\x5c\xd1\xc5\xeb\x58\xdd") &&
           MATCH(recurrence + 0x2f3, "\xc5\xe3\x59\xc3\xc5\xdb\x58\xe0");
}

static bool recurrence_recipe(const uint8_t *recurrence, uint8_t *output)
{
    if (!MATCH(recurrence + 0x234, "\x83\xfa") || !MATCH(recurrence + 0x2a0, "\x41\x83\xc1") ||
        recurrence[0x236] != TA_WINDOWS_REDUCTION_LANES || recurrence[0x2a3] != recurrence[0x236] ||
        !recurrence_update(recurrence))
        return false;
    const uint8_t modes[] = {2, recurrence[0x236], TA_WINDOWS_REDUCTION_PAIRS, 1, 1, 2, 0};
    memcpy(output, modes, sizeof modes);
    uint8_t *pairs = output + sizeof modes;
    unsigned low = vex_destination(recurrence + 0x284), high = vex_destination(recurrence + 0x29b);
    return cross_banks(recurrence + 0x2b1, low, high, pairs) &&
           horizontal_tree(recurrence + 0x2b1, pairs);
}

bool pvdi_windows_reduction_extract(const uint8_t *pe, size_t pe_size, size_t build,
                                    uint8_t output[TA_MODEL_WINDOWS_REDUCTION_BYTES])
{
    if (!pe || !output || build >= sizeof profiles / sizeof profiles[0])
        return false;
    if (pe_size < profiles[build].recurrence + 0x2fb)
        return false;
    uint8_t bytes[TA_MODEL_WINDOWS_REDUCTION_BYTES] = {
        'P', 'V', 'D', 'I', 'R', 'D', 'X', '1', TA_WINDOWS_REDUCTION_RECORD_BYTES, 2
    };
    uint8_t *first = bytes + TA_WINDOWS_REDUCTION_HEADER_BYTES;
    if (!sum_recipe(pe + profiles[build].sum, first) ||
        !recurrence_recipe(pe + profiles[build].recurrence, first + TA_WINDOWS_REDUCTION_RECORD_BYTES))
        return false;
    memcpy(output, bytes, sizeof bytes);
    return true;
}
