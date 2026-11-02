/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEROW_H
#define AZCACHEROW_H
#include <cstddef>
#include <cstdint>
#include <cstring>

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
namespace azsdk_cache_row {
namespace detail {
constexpr std::size_t carrier_bytes = 24, slot_bytes = 8, row_bytes = 0x98, row_quota = 16;
inline bool address(std::uintptr_t value, std::size_t bytes = 1) noexcept {
    const std::uint64_t maximum = UINT64_C(0x0000ffffffffffff);
    return value >= 0x10000 && !(value & 7u) && value <= maximum &&
           bytes && bytes - 1 <= maximum - value;
}
template<class T> inline T scalar(std::uintptr_t value) noexcept {
    T out; std::memcpy(&out, reinterpret_cast<const void *>(value), sizeof out); return out;
}
} // namespace detail
inline bool copy_owned_tag23(const void *owned_carrier, std::uint32_t &out) noexcept {
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ || UINTPTR_MAX != UINT64_MAX
    (void)owned_carrier; (void)out; return false;
#else
    const auto carrier = reinterpret_cast<std::uintptr_t>(owned_carrier);
    if (!detail::address(carrier, detail::carrier_bytes)) return false;
    std::uintptr_t span[3]; std::memcpy(span, owned_carrier, sizeof span);
    const auto begin = span[0], end = span[1], capacity = span[2];
    if (!detail::address(begin) || !detail::address(end) || !detail::address(capacity) ||
        end <= begin || capacity < end || (end - begin) % detail::slot_bytes || (capacity - begin) % detail::slot_bytes) return false;
    const auto count = (end - begin) / detail::slot_bytes;
    /* Observation quota for the proved families, not a global native maximum. */
    if (count > detail::row_quota) return false;
    bool found = false;
    std::uint32_t value = 0;
    for (std::uintptr_t i = 0; i < count; ++i) {
        const auto row = detail::scalar<std::uintptr_t>(begin + i * detail::slot_bytes);
        if (!detail::address(row, detail::row_bytes)) return false;
        if (detail::scalar<std::uint8_t>(row + 4) != 0x23) continue;
        if (found) return false;
        value = detail::scalar<std::uint32_t>(row);
        found = true;
    }
    if (!found) return false;
    out = value;
    return true;
#endif
}
} // namespace azsdk_cache_row
#endif
