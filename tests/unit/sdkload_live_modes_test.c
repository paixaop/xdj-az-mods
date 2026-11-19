/* SPDX-License-Identifier: GPL-3.0-only */
/* A native status consumer needs the same live listener when the public live
 * service is disabled. Constructor admission must still own a valid unit. */
#define AZ_STATUS_NATIVE_PRODUCER 1
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static _Atomic int g_sdk_live_on, g_status_play_on;
static _Atomic uintptr_t g_djpu_vt = 0x10000;
static unsigned expected_deck, registrations;
static void *expected_unit;
static int azsdk_live_register(int deck, void *unit) {
    assert(deck == (int) expected_deck && unit == expected_unit);
    ++registrations;
    return 1;
}
#include LIFE_MODES_INC

int main(void) {
    static const struct {
        int public_service, native_status, listening;
    } modes[] = {{0, 0, 0}, {0, 1, 1}, {1, 0, 1}, {1, 1, 1}};
    uintptr_t unit = 0x10000, other = 0x10000;
    expected_unit = &unit;
    for (unsigned deck = 0; deck < 4; ++deck) {
        expected_deck = deck;
        for (unsigned mode = 0; mode < sizeof modes / sizeof *modes; ++mode) {
            atomic_store(&g_sdk_live_on, modes[mode].public_service);
            atomic_store(&g_status_play_on, modes[mode].native_status);
            for (unsigned owner = 0; owner < 4; ++owner) {
                /* Newly claimed, already owned, owned by a different unit,
                 * and wrong native type after a claim. */
                int claimed = owner == 0 || owner == 3;
                void *prior = owner == 1 ? (void *) &unit : (void *) &other;
                unit = owner == 3 ? 0x20000 : 0x10000;
                registrations = 0;
                djpuctor_live(deck, &unit, claimed, prior);
                assert(registrations == (unsigned) (owner < 2 ? modes[mode].listening : 0));
            }
        }
    }
    puts("PASS live listener admission: public/native modes, one registration, owner and vtable guards on four decks");
}
