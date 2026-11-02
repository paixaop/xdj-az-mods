/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual native module proof only. No forwarding test seam, hook installer,
 * connection/database API, key, filename query or firmware function emulation.
 * Run each case in a new isolated coherent AArch64 Linux process. The wrapper
 * verifies the exact library artifact hash before the explicit dlopen below. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/common/azconnectionhooks.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#if defined(__linux__)
#include <dlfcn.h>
#endif
#define REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr, \
 "FAIL connection-profile: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); std::abort(); } } while (0)

#if !defined(__linux__) || !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
int main() { std::puts("SKIP: real profile proof requires coherent little-endian AArch64 Linux runtime"); return 77; }
#else
namespace {
using namespace azsdk_connection_hooks;
const unsigned char expected_text[32] = {
 0x24,0x3c,0x4e,0x20,0xf5,0x52,0xc9,0x7e,0xe0,0xaa,0x8d,0xe4,0x52,0x75,0x01,0xd7,
 0xa3,0xdd,0x5a,0x22,0x3e,0x8e,0x01,0x27,0x35,0x74,0x01,0x7f,0x7a,0xfc,0x39,0x44};
void *load(const char *path) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) std::fprintf(stderr, "DLOPEN_FAILURE: %s\n", dlerror());
    REQUIRE(handle, "explicit retained module loads in the selected native runtime");
    void *by_name = dlopen("libsqlcipher.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    REQUIRE(by_name == handle, "sealed validator's exact SONAME resolves this explicit loaded module");
    REQUIRE(dlclose(by_name) == 0, "release only the extra SONAME lookup reference");
    return handle;
}
void absent() {
    void *existing = dlopen("libsqlcipher.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (existing) dlclose(existing);
    REQUIRE(!existing, "fresh probe has no preloaded SQLite module");
    errno = EDOM;
    REQUIRE(!prepare_profile() && errno == EDOM && !ready(), "missing module refuses proof without enabling hooks or changing errno");
}
void wrong(const char *path) {
    void *handle = load(path);
    auto number = reinterpret_cast<int (*)()>(dlsym(handle, "sqlite3_libversion_number"));
    auto version = reinterpret_cast<const char *(*)()>(dlsym(handle, "sqlite3_libversion"));
    auto calls = static_cast<unsigned *>(dlsym(handle, "profile_forbidden_calls"));
    REQUIRE(number && version && calls && number() == 3033000 && !std::strcmp(version(), "3.33.0"),
            "lookalike has genuine callable getters with expected names and versions");
    REQUIRE(*calls == 0, "lookalike has executed no connection or digest API");
    REQUIRE(!prepare_profile() && !ready(), "names and correct getter values cannot substitute for exact module proof");
    REQUIRE(*calls == 0, "wrong-layout module refused before forbidden connection/digest calls");
    REQUIRE(dlclose(handle) == 0, "close only the probe's lookalike code reference");
}
void valid(const char *path, bool cold_first) {
    if (cold_first) absent();
    void *handle = load(path);
    REQUIRE(!ready(), "loading the module never installs lifecycle hooks");
    if (cold_first) {
        REQUIRE(!prepare_profile() && !ready(), "failed first preparation cannot be retried into a different proof");
        REQUIRE(dlclose(handle) == 0, "close only the caller reference after cold refusal");
        return;
    }
    errno = ERANGE;
    const SqlProfile *profile = prepare_profile();
    REQUIRE(profile && errno == ERANGE && !ready(), "sealed real validator mints profile with hooks unavailable and unchanged errno");
    REQUIRE(prepare_profile() == profile, "one immutable proof reused before any redirect");
    void *control = dlsym(handle, "sqlite3_file_control");
    Dl_info info = {};
    REQUIRE(control && dladdr(control, &info) && profile->base() == reinterpret_cast<std::uintptr_t>(info.dli_fbase),
            "profile belongs to the actual explicitly loaded function module");
    REQUIRE(reinterpret_cast<void *>(profile->file_control()) == control &&
                reinterpret_cast<std::uintptr_t>(control) - profile->base() == 0x244b0,
            "read-only file-control address is the checked actual export");
    REQUIRE(profile->range_count() && profile->range_count() <= 32 && !profile->range(profile->range_count()),
            "immutable bounded module range view");
    for (unsigned i = 0; i < profile->range_count(); ++i) {
        const Range *range = profile->range(i);
        REQUIRE(range && range->begin < range->end && (range->flags & 4) &&
                    profile->contains(range->begin, range->end - range->begin, 4), "actual retained readable mapping");
    }
    REQUIRE(!profile->contains(UINTPTR_MAX - 1, 8, 4), "range arithmetic rejects overflow");
    REQUIRE(profile->open_entry() == profile->base() + 0x647b0 &&
                profile->close_entry() == profile->base() + 0x512c0 &&
                profile->contains(profile->open_entry(), 16, 5) && profile->contains(profile->close_entry(), 16, 5),
            "exact reviewed internal entries remain readable executable code");
    REQUIRE(profile->io_rva(0) == 0xb3d98 && profile->io_rva(1) == 0xb3e30 && profile->io_rva(2) == 0xb3ec8 &&
                profile->io_rva(3) == 0 && profile->vfs_open_rva() == 0x53968 && profile->read_rva() == 0x1513c &&
                profile->size_rva() == 0x1bdf4 && profile->control_rva() == 0x536b0, "verified read-only physical adapter RVAs");
    auto number = reinterpret_cast<int (*)()>(dlsym(handle, "sqlite3_libversion_number"));
    auto version = reinterpret_cast<const char *(*)()>(dlsym(handle, "sqlite3_libversion"));
    REQUIRE(number && version && number() == 3033000 && !std::strcmp(version(), "3.33.0"), "real native public getters execute");
    auto sha = reinterpret_cast<const void *(*)()>(dlsym(handle, "EVP_sha256"));
    auto digest = reinterpret_cast<int (*)(const void *, std::size_t, unsigned char *, unsigned *, const void *, void *)>(dlsym(handle, "EVP_Digest"));
    unsigned char hash[32] = {}; unsigned length = 0;
    REQUIRE(sha && digest && profile->contains(profile->base() + 0xf980, 481192, 5) &&
                digest(reinterpret_cast<const void *>(profile->base() + 0xf980), 481192, hash, &length, sha(), nullptr) &&
                length == sizeof hash && !std::memcmp(hash, expected_text, sizeof hash), "actual mapped native module text matches retained SHA256");
    REQUIRE(dlclose(handle) == 0, "probe releases only its own explicit module reference");
    REQUIRE(dladdr(control, &info) && prepare_profile() == profile && number() == 3033000 && !ready(),
            "sealed proof holds code lifetime after caller dlclose without enabling hooks");
}
}
int main(int argc, char **argv) {
    static_assert(!std::is_default_constructible<SqlProfile>::value && !std::is_copy_constructible<SqlProfile>::value &&
                      !std::is_destructible<SqlProfile>::value, "only private validator can mint/retire immutable code proof");
    REQUIRE(argc >= 2, "explicit isolated probe case");
    if (!std::strcmp(argv[1], "missing")) { REQUIRE(argc == 2, "missing case has no module path"); absent(); }
    else {
        REQUIRE(argc == 3, "explicit read-only code-module path");
        if (!std::strcmp(argv[1], "wrong")) wrong(argv[2]);
        else if (!std::strcmp(argv[1], "valid")) valid(argv[2], false);
        else if (!std::strcmp(argv[1], "cold")) valid(argv[2], true);
        else REQUIRE(false, "known isolated module probe case");
    }
    std::printf("PASS connection-profile case=%s: native getter/hash proof only; hooks unavailable; no DB/key API\n", argv[1]);
    return 0;
}
#endif
