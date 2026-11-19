/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/aznative_calls.h"
#include <assert.h>
#include <stdio.h>
/* Only their addresses are used here. The separate TU owns their native ABI. */
extern void native_word_result(void), native_pair_result(void), native_string_result(void);
int main(void) {
    const uintptr_t sentinel = UINT64_C(0x8877665544332211);
    const uintptr_t argument = 0x12345678;
    struct {
        uintptr_t before;
        struct az_native_string value;
        uintptr_t after;
    } string = {sentinel, {0}, sentinel};
    string.value = az_native_string_allocate((az_native_string_allocate_fn) native_string_result, argument);
    assert(string.before == sentinel && (uintptr_t) string.value.data == argument && string.after == sentinel);
    assert((uintptr_t) az_native_player_acquire((az_native_player_acquire_fn) native_word_result,
                                                (struct az_native_player_interface *) argument) == argument);
    assert((uintptr_t) az_native_location_acquire((az_native_location_acquire_fn) native_word_result,
                                                  (const struct az_native_player *) argument) == argument);
    struct {
        uintptr_t before;
        struct az_native_text value;
        uintptr_t after;
    } text = {sentinel, {0}, sentinel};
    text.value = az_native_editor_text((az_native_editor_text_fn) native_word_result, (struct az_native_editor *) argument);
    assert(text.before == sentinel && (uintptr_t) text.value.text == argument && text.after == sentinel);
    assert((uintptr_t) az_native_phrase_make((az_native_phrase_make_fn) native_word_result,
                                             (const struct az_native_phrase_atom_ref *) argument) == argument);
    struct {
        uintptr_t before;
        struct az_native_owner value;
        uintptr_t after;
    } owner = {sentinel, {0}, sentinel};
    owner.value = az_native_track_state_acquire((az_native_track_state_fn) native_pair_result,
                                                (const struct az_native_track *) argument);
    assert(owner.before == sentinel && (uintptr_t) owner.value.object == argument &&
           (uintptr_t) owner.value.control == UINT64_C(0x1122334455667788) && owner.after == sentinel);
    puts("PASS typed native results: independent 8/16-byte return ABI, scalar allocation size, operation arguments and destination canaries");
}
