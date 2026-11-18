/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual SourceState + exact native memory-only connections. A missing
 * startup bridge only forwards genuine native APIs, without an observer model. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include "../../src/azmod/sdk_native_sources.c"
typedef struct sqlite3 sqlite3;
typedef int (*SourceInstaller)(uintptr_t, void *, void **, void *);
int azsdk_sources_install_connections(SourceInstaller, void *) __attribute__((weak));
int azsdk_test_connection_island(uintptr_t, void *, void **);
#define REQUIRE(x, why)                                                  \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL source-connection-native: %s\n", why); \
            abort();                                                     \
        }                                                                \
    } while (0)

int installer(uintptr_t entry, void *replacement, void **original, void *context) {
    (void) context;
    return azsdk_test_connection_island(entry, replacement, original);
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3, "explicit native module and build");
    int build = atoi(argv[2]);
    REQUIRE(build == 1 || build == 2, "supported build label");
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        fprintf(stderr, "DLOPEN_FAILURE %s\n", dlerror());
        return 77;
    }
    __auto_type open = ((int (*)(const char *, sqlite3 **, int, const char *))(dlsym(module, "sqlite3_open_v2")));
    __auto_type close = ((int (*)(sqlite3 *))(dlsym(module, "sqlite3_close")));
    REQUIRE(open && close, "exact native public APIs");
    const bool late = getenv("AZSDK_TEST_LATE_PHYSICAL") != NULL;
    REQUIRE(azsdk_sources_configure(1, build, late ? 0u : (unsigned) (AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER)) == AZS_OK,
            "SourceState configured before actual native installation");
    REQUIRE((bool) (azsdk_sources_physical_ready()) == !late, "physical metadata remains independently opt-in");
    sqlite3 *baseline = NULL;
    REQUIRE(open(":memory:", &baseline, 6, NULL) == 0 && baseline && close(baseline) == 0, "genuine native memory baseline");
    printf("NATIVE_SOURCE_STATE_BASELINE_OK memory=1 initial_physical=%u\n", !late);
    fflush(stdout);
    __auto_type bridge = azsdk_sources_install_connections;
    if (bridge) (void) bridge(installer, NULL);
    sqlite3 *db = NULL;
    REQUIRE(open(":memory:", &db, 6, NULL) == 0 && db, "genuine native open after startup bridge");
    struct az_connection_receipt receipt = {0};
    REQUIRE(az_connection_read(&source_state().connection_epochs, db, &receipt) && receipt.generation, "missing SourceState actual native connection installation bridge");
    __auto_type generation = receipt.generation;
    REQUIRE(azsdk_sources_configure(0, build, 0) == AZS_UNSUPPORTED, "source diagnostics disable independently");
    REQUIRE(close(db) == 0 && !az_connection_read(&source_state().connection_epochs, db, &receipt), "installed close retirement cannot be disabled with diagnostics");
    db = NULL;
    REQUIRE(open(":memory:", &db, 6, NULL) == 0 && az_connection_read(&source_state().connection_epochs, db, &receipt) && receipt.generation > generation, "startup bridge retains one canonical nonreset state while diagnostics off");
    REQUIRE(azsdk_sources_configure(1, build, AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER) == AZS_OK && azsdk_sources_physical_ready(), "first or reused physical view consumes sealed proof after our own code redirects");
    REQUIRE(close(db) == 0, "final native disposable connection cleanup");
    puts("PASS source-connection-native: shared proof, canonical process state, actual native startup forwarding/retirement; no EP147/file DB/key");
}
