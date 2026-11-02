/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual retained native SQLCipher plus the real island installer. Only
 * disposable :memory: connections and a constant SELECT statement are used.
 * No source registry, native C++ object, file database or key API is involved.
 * Every scenario runs in a new coherent AArch64 Linux process. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/common/azconnectionhooks.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#define REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr, \
 "FAIL connection-native: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); std::abort(); } } while (0)

#if !defined(__linux__) || !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
int main() { std::puts("SKIP: native connection redirects require coherent little-endian AArch64 Linux"); return 77; }
#else
extern "C" int azsdk_test_connection_island(std::uintptr_t, void *, void **);
extern "C" unsigned azsdk_test_connection_island_count(void);
namespace {
using namespace azsdk_connection_hooks;
using azsdk_connection_epoch::Receipt;
struct sqlite3_stmt;
struct API {
    int (*open)(const char *, sqlite3 **);
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*open16)(const void *, sqlite3 **);
    int (*close)(sqlite3 *);
    int (*close_v2)(sqlite3 *);
    int (*prepare)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
    int (*finalize)(sqlite3_stmt *);
};
template <class T> T symbol(void *handle, const char *name) {
    auto result = reinterpret_cast<T>(dlsym(handle, name));
    REQUIRE(result, "exact loaded module provides the requested native public ABI");
    return result;
}
API api(void *handle) {
    return API {symbol<decltype(API::open)>(handle, "sqlite3_open"),
        symbol<decltype(API::open_v2)>(handle, "sqlite3_open_v2"),
        symbol<decltype(API::open16)>(handle, "sqlite3_open16"),
        symbol<decltype(API::close)>(handle, "sqlite3_close"),
        symbol<decltype(API::close_v2)>(handle, "sqlite3_close_v2"),
        symbol<decltype(API::prepare)>(handle, "sqlite3_prepare_v2"),
        symbol<decltype(API::finalize)>(handle, "sqlite3_finalize")};
}
int open_memory(const API &a, unsigned which, sqlite3 **output) {
    static const char16_t memory16[] = u":memory:";
    static_assert(sizeof(char16_t) == 2, "native UTF-16 input uses exact two-byte code units");
    if (which == 0) return a.open(":memory:", output);
    if (which == 2) return a.open16(memory16, output);
    return a.open_v2(":memory:", output, which == 3 ? 0x10006 : 6, nullptr);
}
bool known(EpochState &state, sqlite3 *connection, std::uint64_t *generation = nullptr) {
    Receipt receipt {UINT64_MAX};
    bool result = state.read(connection, receipt);
    REQUIRE(result || receipt.generation == UINT64_MAX, "refused scalar observation leaves caller output unchanged");
    if (result && generation) *generation = receipt.generation;
    return result;
}
EpochState &epochs() {
    /* Same process-lifetime requirement as production; no atexit destructor
     * may retire the borrowed state before a later installed entry can run. */
    static EpochState *const state = new EpochState;
    return *state;
}
struct Bridge {
    const SqlProfile &profile;
    EpochState &state;
    const API &native;
    bool partial;
    unsigned calls = 0, published_entries = 0;
    void *originals[2] = {};
    std::uint32_t saved[2][4] = {};
    std::uint32_t null_branch = 0;
    Bridge(const SqlProfile &p, EpochState &s, const API &a, bool stop)
        : profile(p), state(s), native(a), partial(stop) {
        std::memcpy(&null_branch, reinterpret_cast<const void *>(p.close_entry() - 4), 4);
    }
};
int installer(std::uintptr_t entry, void *replacement, void **original, void *context) {
    auto &b = *static_cast<Bridge *>(context);
    REQUIRE(b.calls < 2 && entry == (b.calls == 0 ? b.profile.close_entry() : b.profile.open_entry()),
            "adapter requests distinct reviewed close-first then open entries");
    unsigned index = b.calls++ == 0 ? 1 : 0;
    REQUIRE(original && !__atomic_load_n(original, __ATOMIC_ACQUIRE) && replacement,
            "each original slot is empty and distinct before real publication");
    std::memcpy(b.saved[index], reinterpret_cast<const void *>(entry), sizeof b.saved[index]);
    if (index == 0 && b.partial) return -1; // actual close remains installed
#if defined(AZSDK_NATIVE_UNREDIRECTED_CONTROL)
    /* Isolated runtime control: native functions remain genuine and callable,
     * but no code is redirected. The first real birth assertion must catch it. */
    (void)replacement;
    __atomic_store_n(original, reinterpret_cast<void *>(entry), __ATOMIC_RELEASE);
    int result = 1;
#else
    int result = azsdk_test_connection_island(entry, replacement, original);
#endif
    REQUIRE(result == 1 && __atomic_load_n(original, __ATOMIC_ACQUIRE),
            "real island backend publishes a callable original before accepting entry");
    b.originals[index] = __atomic_load_n(original, __ATOMIC_ACQUIRE);
#if !defined(AZSDK_NATIVE_UNREDIRECTED_CONTROL)
    const auto *words = reinterpret_cast<const std::uint32_t *>(entry);
    REQUIRE(words[0] != b.saved[index][0] && (words[0] & 0xfc000000u) == 0x14000000u &&
                !std::memcmp(words + 1, b.saved[index] + 1, 12),
            "actual module redirect changes exactly one reviewed instruction");
    const auto *trampoline = static_cast<const std::uint32_t *>(b.originals[index]);
    std::uintptr_t target = 0;
    std::memcpy(&target, trampoline + 3, sizeof target);
    REQUIRE(trampoline[0] == b.saved[index][0] && trampoline[1] == 0x58000051u &&
                trampoline[2] == 0xd61f0220u && target == entry + 4,
            "real callable original displaces one instruction and returns to original PC");
#endif
    ++b.published_entries;
    REQUIRE(!ready(), "entry publication alone cannot enable successful birth admission");
    if (index == 0) {
        sqlite3 *during = nullptr;
        REQUIRE(b.native.open_v2(":memory:", &during, 6, nullptr) == 0 && during,
                "actual public open can enter immediately inside installer callback");
        REQUIRE(!known(b.state, during), "open entering before both installs finish stays unadmitted");
        REQUIRE(b.native.close(during) == 0 && !known(b.state, during),
                "published close forwards and retires during partial installation");
    }
    return result;
}
void baseline(const API &a, EpochState &state) {
    for (unsigned i = 0; i < 4; ++i) {
        sqlite3 *db = nullptr;
        REQUIRE(open_memory(a, i, &db) == 0 && db, "unmodified native public open creates disposable memory handle");
        REQUIRE(!known(state, db), "pre-observation connection is unknown without a birth");
        REQUIRE((i & 1 ? a.close_v2(db) : a.close(db)) == 0, "unmodified native close forwards successfully");
    }
    std::puts("NATIVE_MEMORY_BASELINE_OK public_open_paths=3 contexts=4 ready=0");
    std::fflush(stdout);
}
void roundtrip(const API &a, EpochState &state) {
    sqlite3 *db[4] = {};
    std::uint64_t generation[4] = {};
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE(open_memory(a, i, &db[i]) == 0 && db[i], "hooked native public open preserves real successful result/output");
        REQUIRE(known(state, db[i], &generation[i]), "missing adapter-owned actual native successful connection birth");
        REQUIRE(generation[i] && (!i || generation[i] > generation[i - 1]), "four independent memory handles get fresh nonzero generations");
    }
    for (unsigned i = 0; i < 4; ++i) {
        REQUIRE((i & 1 ? a.close_v2(db[i]) : a.close(db[i])) == 0, "hooked native close preserves success");
        REQUIRE(!known(state, db[i]), "both native public close paths retire their observed incarnation");
    }
    sqlite3 *again = nullptr;
    REQUIRE(a.open_v2(":memory:", &again, 6, nullptr) == 0 && again, "later real allocation succeeds");
    std::uint64_t next = 0;
    REQUIRE(known(state, again, &next) && next > generation[3], "later allocation cannot reuse an old generation even if its pointer repeats");
    REQUIRE(a.close(again) == 0 && !known(state, again), "later allocation retires normally");
}
void held_statement(const API &a, EpochState &state, bool zombie) {
    sqlite3 *db = nullptr;
    REQUIRE(a.open_v2(":memory:", &db, 6, nullptr) == 0 && known(state, db), "real memory birth precedes close control");
    sqlite3_stmt *statement = nullptr;
    REQUIRE(a.prepare(db, "SELECT 1", -1, &statement, nullptr) == 0 && statement,
            "only constant statement holds the disposable native connection busy");
    int result = zombie ? a.close_v2(db) : a.close(db);
    REQUIRE(result == (zombie ? 0 : 5), "real native zombie success or strict BUSY result is preserved");
    REQUIRE(!known(state, db), "generation retires even when strict close fails or zombie release is deferred");
    REQUIRE(a.finalize(statement) == 0, "release only the constant disposable statement");
    REQUIRE(!known(state, db), "deferred native allocation release cannot revive retired generation");
    if (!zombie) REQUIRE(a.close(db) == 0 && !known(state, db), "strict BUSY retry releases handle without reviving it");
}
void failed_open(const API &a, EpochState &state) {
    sqlite3 *db = nullptr;
    int result = a.open_v2(":memory:", &db, 6, "__azsdk_fixture_no_such_vfs__");
    REQUIRE(result != 0 && db, "real invalid-VFS open produces a failed nonnull memory-only error handle");
    REQUIRE(!known(state, db), "failed nonnull native open never mints a successful birth");
    REQUIRE(a.close(db) == 0 && !known(state, db), "failed native error handle is safely released without admission");
}
}
int main(int argc, char **argv) {
    REQUIRE(argc == 3, "explicit exact module path and isolated scenario");
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) std::fprintf(stderr, "DLOPEN_FAILURE: %s\n", dlerror());
    REQUIRE(handle, "exact retained native module loads with explicitly coherent dependencies");
    const SqlProfile *profile = prepare_profile();
    REQUIRE(profile && !ready(), "sealed immutable whole-text proof precedes every code redirect");
    const API native = api(handle);
    auto &state = epochs();
    baseline(native, state);
    bool partial = !std::strcmp(argv[2], "partial");
    REQUIRE(partial || !std::strcmp(argv[2], "roundtrip") || !std::strcmp(argv[2], "busy") ||
                !std::strcmp(argv[2], "zombie") || !std::strcmp(argv[2], "failed"), "known isolated native scenario");
    sqlite3 *preexisting = nullptr;
    if (partial) {
        REQUIRE(state.forward_open(&preexisting, [&]() { return native.open_v2(":memory:", &preexisting, 6, nullptr); }) == 0 &&
                    known(state, preexisting), "partial-install close control starts from an actually observed real memory birth");
    } else REQUIRE(native.open_v2(":memory:", &preexisting, 6, nullptr) == 0 && preexisting,
                   "preexisting real handle starts without an observed generation");
    Bridge bridge(*profile, state, native, partial);
    errno = EDOM;
    int installed = install(state, Installer {installer, &bridge});
    REQUIRE(errno == EDOM && installed == (partial ? 0 : 1) && ready() == !partial,
            "actual install preserves errno and admits births only after both entries succeed");
    REQUIRE(bridge.calls == 2 && bridge.published_entries == (partial ? 1u : 2u), "both exact distinct installation outcomes are retained");
#if !defined(AZSDK_NATIVE_UNREDIRECTED_CONTROL)
    REQUIRE(azsdk_test_connection_island_count() == bridge.published_entries,
            "every accepted entry used the actual island-only path without fallback");
#endif
    std::uint32_t null_branch = 0;
    std::memcpy(&null_branch, reinterpret_cast<const void *>(profile->close_entry() - 4), 4);
    REQUIRE(null_branch == bridge.null_branch && native.close(nullptr) == 0 && native.close_v2(nullptr) == 0,
            "preceding native null guard remains unchanged and both null closes bypass nonnull hook safely");
    REQUIRE(prepare_profile() == profile, "after redirects consumers reuse the same prepatch proof without rehashing modified text");
    if (partial) {
        REQUIRE(known(state, preexisting), "failed open installation alone does not retire unrelated live handle");
        REQUIRE(native.close(preexisting) == 0 && !known(state, preexisting),
                "installed real close keeps retiring despite unavailable complete adapter");
        sqlite3 *unobserved = nullptr;
        REQUIRE(native.open_v2(":memory:", &unobserved, 6, nullptr) == 0 && !known(state, unobserved),
                "partial installation never admits later successful native opens");
        REQUIRE(native.close_v2(unobserved) == 0 && !known(state, unobserved), "partial close continues exact native forwarding");
    } else {
        REQUIRE(!known(state, preexisting), "enabling hooks cannot retroactively adopt an already-open native handle");
        REQUIRE(native.close(preexisting) == 0, "unknown preexisting handle still closes normally");
        if (!std::strcmp(argv[2], "roundtrip")) roundtrip(native, state);
        else if (!std::strcmp(argv[2], "failed")) failed_open(native, state);
        else held_statement(native, state, !std::strcmp(argv[2], "zombie"));
    }
    REQUIRE(!install(state, Installer {installer, &bridge}) && bridge.calls == 2,
            "process-lifetime binding refuses a second native installation without repatching entries");
    REQUIRE(dlclose(handle) == 0 && prepare_profile() == profile,
            "shared immutable proof retains patched code after releasing only the caller module reference");
    std::printf("PASS connection-native case=%s: actual memory-only API paths, real island redirects, scalar epochs; no file DB/key/source capability\n", argv[2]);
    return 0;
}
#endif
