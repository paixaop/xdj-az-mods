#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual process storage and an independently allocated native weak control.
 * The late callback checks the free flag before touching retained state. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "native_abi_fixture.h"
#define REQUIRE(x, why)                                        \
    do {                                                       \
        if (!(x)) {                                            \
            fprintf(stderr, "FAIL source-storage: %s\n", why); \
            abort();                                           \
        }                                                      \
    } while (0)
#ifndef SOURCES_INC
#define SOURCES_INC "../../src/azmod/sdk_native_sources.c"
#endif
#include SOURCES_INC
static bool control_freed;
static unsigned build;
static void control_free(void *p) {
    control_freed = true;
    free(p);
}
static const void *const control_vtable[] = {fixture_noop, control_free, fixture_noop, control_free, fixture_deleter};
static void late_native_shutdown(void) {
    REQUIRE(!control_freed, "SourceState destroyed before later native shutdown hook");
    SourceState *state = &source_state();
    REQUIRE(!pthread_mutex_lock(&state->registry_mutex), "late hook locks actual process mutex");
    REQUIRE(!az_native_alive(state->entries[0].mount.control), "late hook sees actual expired native owner without a revived allocation");
    state->next_epoch = 1;
    REQUIRE(!pthread_mutex_unlock(&state->registry_mutex), "late hook releases actual process mutex");
    printf("PASS source-storage build=%u: same state and mutex survive native shutdown interval\n", build);
}
int main(int argc, char **argv) {
    REQUIRE(argc == 2 && (argv[1][0] == '1' || argv[1][0] == '2'), "independent profile process");
    build = (unsigned) (argv[1][0] - '0');
    REQUIRE(!atexit(late_native_shutdown), "late native callback registration");
    struct fixture_owner owner = fixture_make(sizeof(uintptr_t), control_vtable);
    struct az_native_owner view;
    memcpy(&view, &owner, sizeof view);
    az_native_weak_copy(&source_state().entries[0].mount, &view);
    fixture_drop(&owner);
    REQUIRE(!control_freed && !az_native_alive(source_state().entries[0].mount.control), "actual native strong lifetime ended and genuine weak control remains");
    printf("NATIVE_STATE_STORAGE_OK build=%u: real GNU weak control retained before shutdown\n", build);
    fflush(stdout);
}
