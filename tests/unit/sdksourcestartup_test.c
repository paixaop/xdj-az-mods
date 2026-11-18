/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual canonical SourceState/native module plus an exact extracted startup
 * config caller. Only flag-file access in the copied caller/helper is faked.
 * Disposable :memory: handles only; no EP147/file DB/key/physical control. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include "../../src/azmod/sdk_native_sources.c"
typedef struct sqlite3 sqlite3;
#define REQUIRE(x, why)                                                                    \
    do {                                                                                   \
        if (!(x)) {                                                                        \
            fprintf(stderr,                                                                \
                    "FAIL source-startup: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                       \
        }                                                                                  \
    } while (0)
void azsdk_test_run_startup_config(int);
int azsdk_test_startup_sources_on(void);
unsigned azsdk_test_connection_island_count(void);
unsigned flag_mask, physical_flag;
int azsdk_test_startup_access(const char *path, int mode) {
    REQUIRE(path && mode == F_OK, "only exact startup flag-file existence checks are substituted");
    if (!strcmp(path, "/home/root/settings/azmod.track-data.on")) return flag_mask & 1 ? 0 : -1;
    if (!strcmp(path, "/home/root/settings/azmod.native-sources.on")) return flag_mask & 2 ? 0 : -1;
    if (!strcmp(path, "/home/root/settings/azmod.native-connections.on")) return flag_mask & 4 ? 0 : -1;
    if (!strcmp(path, "/home/root/settings/azmod.native-source-files.on")) return physical_flag ? 0 : -1;
    REQUIRE(false, "no other filesystem/API operation is faked");
    return -1;
}
#if !defined(__linux__) || !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
int main() {
    puts("SKIP: exact native startup requires coherent little-endian AArch64 Linux");
    return 77;
}
#else

static bool known(sqlite3 *connection, uint64_t *generation) {
    struct az_connection_receipt receipt = {UINT64_MAX};
    const bool found = az_connection_read(&source_state().connection_epochs, connection, &receipt);
    REQUIRE(found || receipt.generation == UINT64_MAX, "unknown canonical scalar lookup preserves its output");
    if (found && generation) *generation = receipt.generation;
    return found;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 5, "explicit exact module/build/flag mask/physical policy");
    const int build = atoi(argv[2]);
    flag_mask = (unsigned) atoi(argv[3]);
    physical_flag = (unsigned) atoi(argv[4]);
    REQUIRE((build == 1 || build == 2) && flag_mask <= 7 && physical_flag <= 1, "bounded independent startup scenario");
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) fprintf(stderr, "DLOPEN_FAILURE: %s\n", dlerror());
    REQUIRE(module, "exact module loads under explicitly coherent native dependencies");
    typedef int (*Open)(const char *, sqlite3 **, int, const char *);
    typedef int (*Close)(sqlite3 *);
    __auto_type native_open = ((Open) (dlsym(module, "sqlite3_open_v2")));
    __auto_type native_close = ((Close) (dlsym(module, "sqlite3_close")));
    REQUIRE(native_open && native_close, "actual native public memory connection APIs");
    const struct az_sql_profile *proof = az_connection_prepare_profile();
    REQUIRE(proof && !az_connection_ready(), "real sealed prepatch module proof precedes startup redirects");
    sqlite3 *baseline = NULL;
    REQUIRE(native_open(":memory:", &baseline, 6, NULL) == 0 && baseline && !known(baseline, NULL) &&
                native_close(baseline) == 0,
            "unmodified real native memory open/close baseline");
    sqlite3 *preexisting = NULL;
    REQUIRE(native_open(":memory:", &preexisting, 6, NULL) == 0 && preexisting && !known(preexisting, NULL),
            "pre-startup live handle has no fabricated observed incarnation");
    printf("NATIVE_STARTUP_BASELINE_OK build=%d mask=%u physical=%u memory_open_close=1\n", build, flag_mask, physical_flag);
    fflush(stdout);
    // Runs the exact real flag/config block through just before msg_thread_note.
    // This call is the fixture's boundary before subsequent application work.
    azsdk_test_run_startup_config(build);
    const bool diagnostics = (flag_mask & 3) == 3, opted_in = flag_mask == 7;
    REQUIRE((bool) (azsdk_test_startup_sources_on()) == diagnostics &&
                (bool) (atomic_load_explicit(&(source_state().enabled), memory_order_acquire)) == diagnostics,
            "actual extracted caller preserves guarded source diagnostics behavior");
    REQUIRE((bool) (azsdk_sources_physical_ready()) == (diagnostics && physical_flag),
            "physical profile preparation is independently opt-in; no physical metadata API is called");
    sqlite3 *after = NULL;
    REQUIRE(native_open(":memory:", &after, 6, NULL) == 0 && after,
            "actual next application open preserves native result/output after startup boundary");
    uint64_t generation = 0;
    const bool observed = known(after, &generation);
    REQUIRE(native_close(after) == 0, "actual application close preserves native success before observation assertion");
    printf("NATIVE_STARTUP_BOUNDARY_OK build=%d mask=%u physical=%u post_open_close=1 canonical_known=%u\n",
           build, flag_mask, physical_flag, (unsigned) (observed));
    fflush(stdout);
    if (opted_in) {
        REQUIRE(observed && generation, "missing canonical SourceState receipt after opted-in azmod_init boundary");
        REQUIRE(az_connection_ready() && azsdk_test_connection_island_count() == 2 && !known(after, NULL),
                "both actual island entries installed and application close retired its recorded generation");
        REQUIRE(!known(preexisting, NULL), "startup never adopts a native allocation opened before hooks");
        sqlite3 *live = NULL;
        uint64_t next = 0;
        REQUIRE(native_open(":memory:", &live, 6, NULL) == 0 && known(live, &next) && next > generation,
                "later real native open belongs to the same canonical nonreset namespace");
        REQUIRE(azsdk_sources_configure(0, build, 0) == AZS_UNSUPPORTED,
                "source diagnostics disable separately from installed connection forwarding");
        REQUIRE(native_close(live) == 0 && !known(live, NULL), "installed close keeps retiring while diagnostics are off");
        live = NULL;
        REQUIRE(native_open(":memory:", &live, 6, NULL) == 0 && known(live, &generation) && generation > next,
                "successful native births continue through diagnostics disable without resetting epochs");
        REQUIRE(native_close(live) == 0 && !known(live, NULL), "diagnostics-off memory handle is released and retired");
        REQUIRE(azsdk_sources_configure(1, build, physical_flag ? (unsigned) (AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER) : 0u) == AZS_OK &&
                    (bool) (azsdk_sources_physical_ready()) == (bool) (physical_flag),
                "physical preparation reuses sealed proof after native code redirects");
    } else {
        REQUIRE(!observed && !az_connection_ready() && azsdk_test_connection_island_count() == 0,
                "any missing opt-in prerequisite leaves actual module entries uninstalled and receipts unknown");
    }
    REQUIRE(native_close(preexisting) == 0 && !known(preexisting, NULL), "unknown preexisting memory handle still releases normally");
    REQUIRE(az_connection_prepare_profile() == proof && dlclose(module) == 0,
            "same retained code proof survives caller module reference release");
    printf("PASS source-startup build=%d mask=%u physical=%u: extracted caller, canonical State and native memory-only operations\n",
           build, flag_mask, physical_flag);
    return 0;
}
#endif
