/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "native_abi_fixture.h"
int overview_scalar_tests(void);
void overview_exception_start(void (*)(long), int);
void overview_exception_observe(long);
void overview_exception_check(void);
void overview_lease_checks(void);
static void throwing_renderer(long self) {
    overview_exception_observe(self);
    fixture_raise();
}
static void original(void *context) { overview_exception_start(throwing_renderer, *(int *) context); }
static void run(int mode) {
    unsigned before = fixture_exception_deletions;
    if (fixture_native_try(original, &mode) || fixture_exception_deletions != before + 1) abort();
    overview_exception_check();
}
int main(int argc, char **argv) {
    bool lifetime_only = argc == 2 && !strcmp(argv[1], "--lifetime-only");
    bool throw_only = argc == 2 && !strcmp(argv[1], "--throw-only");
    if (lifetime_only) run(2);
    else {
        if (!throw_only && overview_scalar_tests()) return 1;
        for (int mode = 0; mode < 5; ++mode) run(mode);
        if (!throw_only) overview_lease_checks();
    }
    puts("overview native unwinding: original propagation, conditional restoration and real retirement passed");
    return 0;
}
