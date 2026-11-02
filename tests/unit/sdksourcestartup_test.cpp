/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual canonical SourceState/native module plus an exact extracted startup
 * config caller. Only flag-file access in the copied caller/helper is faked.
 * Disposable :memory: handles only; no EP147/file DB/key/physical control. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include "../../src/azmod/sdk_native_sources.cpp"
#define REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr, \
    "FAIL source-startup: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); std::abort(); } } while (0)
extern "C" void azsdk_test_run_startup_config(int);
extern "C" int azsdk_test_startup_sources_on(void);
extern "C" unsigned azsdk_test_connection_island_count(void);
namespace { unsigned flag_mask, physical_flag; }
extern "C" int azsdk_test_startup_access(const char *path, int mode) {
    REQUIRE(path && mode == F_OK, "only exact startup flag-file existence checks are substituted");
    if (!std::strcmp(path, "/home/root/settings/azmod.track-data.on")) return flag_mask & 1 ? 0 : -1;
    if (!std::strcmp(path, "/home/root/settings/azmod.native-sources.on")) return flag_mask & 2 ? 0 : -1;
    if (!std::strcmp(path, "/home/root/settings/azmod.native-connections.on")) return flag_mask & 4 ? 0 : -1;
    if (!std::strcmp(path, "/home/root/settings/azmod.native-source-files.on")) return physical_flag ? 0 : -1;
    REQUIRE(false, "no other filesystem/API operation is faked"); return -1;
}
#if !defined(__linux__) || !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
int main() { std::puts("SKIP: exact native startup requires coherent little-endian AArch64 Linux"); return 77; }
#else
namespace {
bool known(sqlite3 *connection, std::uint64_t *generation = nullptr) {
    azsdk_connection_epoch::Receipt receipt {UINT64_MAX};
    const bool found = source_state().connection_epochs.read(connection, receipt);
    REQUIRE(found || receipt.generation == UINT64_MAX, "unknown canonical scalar lookup preserves its output");
    if (found && generation) *generation = receipt.generation;
    return found;
}
}
int main(int argc, char **argv) {
    REQUIRE(argc == 5, "explicit exact module/build/flag mask/physical policy");
    const int build = std::atoi(argv[2]); flag_mask = unsigned(std::atoi(argv[3])); physical_flag = unsigned(std::atoi(argv[4]));
    REQUIRE((build == 1 || build == 2) && flag_mask <= 7 && physical_flag <= 1, "bounded independent startup scenario");
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) std::fprintf(stderr, "DLOPEN_FAILURE: %s\n", dlerror());
    REQUIRE(module, "exact module loads under explicitly coherent native dependencies");
    using Open = int (*)(const char *, sqlite3 **, int, const char *);
    using Close = int (*)(sqlite3 *);
    auto native_open = reinterpret_cast<Open>(dlsym(module, "sqlite3_open_v2"));
    auto native_close = reinterpret_cast<Close>(dlsym(module, "sqlite3_close"));
    REQUIRE(native_open && native_close, "actual native public memory connection APIs");
    const auto *proof = azsdk_connection_hooks::prepare_profile();
    REQUIRE(proof && !azsdk_connection_hooks::ready(), "real sealed prepatch module proof precedes startup redirects");
    sqlite3 *baseline = nullptr;
    REQUIRE(native_open(":memory:", &baseline, 6, nullptr) == 0 && baseline && !known(baseline) &&
            native_close(baseline) == 0, "unmodified real native memory open/close baseline");
    sqlite3 *preexisting = nullptr;
    REQUIRE(native_open(":memory:", &preexisting, 6, nullptr) == 0 && preexisting && !known(preexisting),
            "pre-startup live handle has no fabricated observed incarnation");
    std::printf("NATIVE_STARTUP_BASELINE_OK build=%d mask=%u physical=%u memory_open_close=1\n", build, flag_mask, physical_flag);
    std::fflush(stdout);
    // Runs the exact real flag/config block through just before msg_thread_note.
    // This call is the fixture's boundary before subsequent application work.
    azsdk_test_run_startup_config(build);
    const bool diagnostics = (flag_mask & 3) == 3, opted_in = flag_mask == 7;
    REQUIRE(bool(azsdk_test_startup_sources_on()) == diagnostics &&
            bool(source_state().enabled.load(std::memory_order_acquire)) == diagnostics,
            "actual extracted caller preserves guarded source diagnostics behavior");
    REQUIRE(bool(azsdk_sources_physical_ready()) == (diagnostics && physical_flag),
            "physical profile preparation is independently opt-in; no physical metadata API is called");
    sqlite3 *after = nullptr;
    REQUIRE(native_open(":memory:", &after, 6, nullptr) == 0 && after,
            "actual next application open preserves native result/output after startup boundary");
    std::uint64_t generation = 0;
    const bool observed = known(after, &generation);
    REQUIRE(native_close(after) == 0, "actual application close preserves native success before observation assertion");
    std::printf("NATIVE_STARTUP_BOUNDARY_OK build=%d mask=%u physical=%u post_open_close=1 canonical_known=%u\n",
                build, flag_mask, physical_flag, unsigned(observed));
    std::fflush(stdout);
    if (opted_in) {
        REQUIRE(observed && generation, "missing canonical SourceState receipt after opted-in azmod_init boundary");
        REQUIRE(azsdk_connection_hooks::ready() && azsdk_test_connection_island_count() == 2 && !known(after),
                "both actual island entries installed and application close retired its recorded generation");
        REQUIRE(!known(preexisting), "startup never adopts a native allocation opened before hooks");
        sqlite3 *live = nullptr;
        std::uint64_t next = 0;
        REQUIRE(native_open(":memory:", &live, 6, nullptr) == 0 && known(live, &next) && next > generation,
                "later real native open belongs to the same canonical nonreset namespace");
        REQUIRE(azsdk_sources_configure(0, build, 0) == AZS_UNSUPPORTED,
                "source diagnostics disable separately from installed connection forwarding");
        REQUIRE(native_close(live) == 0 && !known(live), "installed close keeps retiring while diagnostics are off");
        live = nullptr;
        REQUIRE(native_open(":memory:", &live, 6, nullptr) == 0 && known(live, &generation) && generation > next,
                "successful native births continue through diagnostics disable without resetting epochs");
        REQUIRE(native_close(live) == 0 && !known(live), "diagnostics-off memory handle is released and retired");
        REQUIRE(azsdk_sources_configure(1, build, physical_flag ? unsigned(AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER) : 0u) == AZS_OK &&
                bool(azsdk_sources_physical_ready()) == bool(physical_flag), "physical preparation reuses sealed proof after native code redirects");
    } else {
        REQUIRE(!observed && !azsdk_connection_hooks::ready() && azsdk_test_connection_island_count() == 0,
                "any missing opt-in prerequisite leaves actual module entries uninstalled and receipts unknown");
    }
    REQUIRE(native_close(preexisting) == 0 && !known(preexisting), "unknown preexisting memory handle still releases normally");
    REQUIRE(azsdk_connection_hooks::prepare_profile() == proof && dlclose(module) == 0,
            "same retained code proof survives caller module reference release");
    std::printf("PASS source-startup build=%d mask=%u physical=%u: extracted caller, canonical State and native memory-only operations\n",
                build, flag_mask, physical_flag);
    return 0;
}
#endif
