/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEROW_H
#define AZCACHEROW_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Private raw schema reader for the proved primary7/standard16 row families.
 * The caller already owns the complete original 24-byte result, its pointer
 * buffer and every separately allocated 0x98-byte row for this entire call.
 * All must remain stable/serialized. These checks acquire no native liveness
 * and cannot make arbitrary or stale pointers safe. Native code alone owns
 * construction/destruction; this never constructs, moves or destroys a row.
 * A copied tag23 value, including zero, proves no TrackID validity, SQL query,
 * source/connection incarnation, request, client or load identity. The caller
 * must separately establish the supported native variant before assigning any
 * meaning to that scalar. The scoped native cache-query observer uses it
 * without assigning a TrackID or committed-load meaning.
 */


static const size_t azcrow_carrier_bytes = 24, azcrow_slot_bytes = 8, azcrow_row_bytes = 0x98, azcrow_row_quota = 16;
static inline bool azcrow_address(uintptr_t value, size_t bytes) {
    const uint64_t maximum = UINT64_C(0x0000ffffffffffff);
    return value >= 0x10000 && !(value & 7u) && value <= maximum &&
           bytes && bytes - 1 <= maximum - value;
}
#define scalar(T, value) ({ T scalar_value;memcpy(&scalar_value,(const void *)(uintptr_t)(value),sizeof scalar_value);scalar_value; })
static inline bool azcrow_owned_span(const uintptr_t span[3], uintptr_t *count) {
    uintptr_t begin = span[0], end = span[1], capacity = span[2];
    if (!azcrow_address(begin, 1) || !azcrow_address(end, 1) || !azcrow_address(capacity, 1)) return false;
    if (end <= begin || capacity < end || (end - begin) % azcrow_slot_bytes || (capacity - begin) % azcrow_slot_bytes) return false;
    *count = (end - begin) / azcrow_slot_bytes;
    /* Observation quota for the proved families, not a global native maximum. */
    return *count <= azcrow_row_quota;
}
static inline bool azcrow_copy_owned_tag23(const void *owned_carrier, uint32_t *out) {
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ || UINTPTR_MAX != UINT64_MAX
    (void) owned_carrier;
    (void) (*out);
    return false;
#else
    __auto_type carrier = ((uintptr_t) (owned_carrier));
    if (!azcrow_address(carrier, azcrow_carrier_bytes)) return false;
    uintptr_t span[3];
    memcpy(span, owned_carrier, sizeof span);
    uintptr_t begin = span[0], count;
    if (!azcrow_owned_span(span, &count)) return false;
    bool found = false;
    uint32_t value = 0;
    for (uintptr_t i = 0; i < count; ++i) {
        __auto_type row = scalar(uintptr_t, begin + i * azcrow_slot_bytes);
        if (!azcrow_address(row, azcrow_row_bytes)) return false;
        if (scalar(uint8_t, row + 4) != 0x23) continue;
        if (found) return false;
        value = scalar(uint32_t, row);
        found = true;
    }
    if (!found) return false;
    (*out) = value;
    return true;
#undef scalar
#endif
}

#endif
