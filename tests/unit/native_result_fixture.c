/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile this independent native ABI oracle with GCC -fpcc-struct-return.
 * It writes exactly 8 or 16 bytes, unlike the C caller's 24-byte carrier. */
#include <stdint.h>
#include <stddef.h>
struct native_word {
    uintptr_t value;
};
struct native_pair {
    uintptr_t first, second;
};
struct native_word native_word_result(const void *argument) {
    return (struct native_word) {(uintptr_t) argument};
}
struct native_word native_string_result(size_t bytes) {
    return (struct native_word) {bytes};
}
struct native_pair native_pair_result(const void *argument) {
    return (struct native_pair) {(uintptr_t) argument, UINT64_C(0x1122334455667788)};
}
