/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual native module proof only. No forwarding test seam, hook installer,
 * connection/database API, key, filename query or firmware function emulation.
 * Run each case in a new isolated coherent AArch64 Linux process. The wrapper
 * verifies the exact library artifact hash before the explicit dlopen below. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/common/azconnectionhooks.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <dlfcn.h>
#endif
#define REQUIRE(x, why)                                                                        \
    do {                                                                                       \
        if (!(x)) {                                                                            \
            fprintf(stderr,                                                                    \
                    "FAIL connection-profile: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
            abort();                                                                           \
        }                                                                                      \
    } while (0)

#if !defined(__linux__) || !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
int main() {
    puts("SKIP: real profile proof requires coherent little-endian AArch64 Linux runtime");
    return 77;
}
#else


const unsigned char expected_text[32] = {
    0x24, 0x3c, 0x4e, 0x20, 0xf5, 0x52, 0xc9, 0x7e, 0xe0, 0xaa, 0x8d, 0xe4, 0x52, 0x75, 0x01, 0xd7,
    0xa3, 0xdd, 0x5a, 0x22, 0x3e, 0x8e, 0x01, 0x27, 0x35, 0x74, 0x01, 0x7f, 0x7a, 0xfc, 0x39, 0x44};
void *load(const char *path) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) fprintf(stderr, "DLOPEN_FAILURE: %s\n", dlerror());
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
    REQUIRE(!az_connection_prepare_profile() && errno == EDOM && !az_connection_ready(), "missing module refuses proof without enabling hooks or changing errno");
}
void wrong(const char *path) {
    void *handle = load(path);
    az_sql_version_number_fn number = (az_sql_version_number_fn) dlsym(handle, "sqlite3_libversion_number");
    az_sql_version_fn version = (az_sql_version_fn) dlsym(handle, "sqlite3_libversion");
    __auto_type calls = ((unsigned *) (dlsym(handle, "profile_forbidden_calls")));
    REQUIRE(number && version && calls && number() == 3033000 && !strcmp(version(), "3.33.0"),
            "lookalike has genuine callable getters with expected names and versions");
    REQUIRE(*calls == 0, "lookalike has executed no connection or digest API");
    REQUIRE(!az_connection_prepare_profile() && !az_connection_ready(), "names and correct getter values cannot substitute for exact module proof");
    REQUIRE(*calls == 0, "wrong-layout module refused before forbidden connection/digest calls");
    REQUIRE(dlclose(handle) == 0, "close only the probe's lookalike code reference");
}
void valid(const char *path, bool cold_first) {
    if (cold_first) absent();
    void *handle = load(path);
    REQUIRE(!az_connection_ready(), "loading the module never installs lifecycle hooks");
    if (cold_first) {
        REQUIRE(!az_connection_prepare_profile() && !az_connection_ready(), "failed first preparation cannot be retried into a different proof");
        REQUIRE(dlclose(handle) == 0, "close only the caller reference after cold refusal");
        return;
    }
    errno = ERANGE;
    const struct az_sql_profile *profile = az_connection_prepare_profile();
    REQUIRE(profile && errno == ERANGE && !az_connection_ready(), "sealed real validator mints profile with hooks unavailable and unchanged errno");
    REQUIRE(az_connection_prepare_profile() == profile, "one immutable proof reused before any redirect");
    void *control = dlsym(handle, "sqlite3_file_control");
    Dl_info info = {0};
    REQUIRE(control && dladdr(control, &info) && az_sql_profile_base(profile) == ((uintptr_t) (info.dli_fbase)),
            "profile belongs to the actual explicitly loaded function module");
    REQUIRE(((void *) (az_sql_profile_file_control(profile))) == control &&
                ((uintptr_t) (control)) - az_sql_profile_base(profile) == 0x244b0,
            "read-only file-control address is the checked actual export");
    REQUIRE(az_sql_profile_range_count(profile) && az_sql_profile_range_count(profile) <= 32 && !az_sql_profile_range(profile, az_sql_profile_range_count(profile)),
            "immutable bounded module range view");
    for (unsigned i = 0; i < az_sql_profile_range_count(profile); ++i) {
        const struct az_sql_range *range = az_sql_profile_range(profile, i);
        REQUIRE(range && range->begin < range->end && (range->flags & 4) &&
                    az_sql_profile_contains(profile, range->begin, range->end - range->begin, 4),
                "actual retained readable mapping");
    }
    REQUIRE(!az_sql_profile_contains(profile, UINTPTR_MAX - 1, 8, 4), "range arithmetic rejects overflow");
    REQUIRE(az_sql_profile_open_entry(profile) == az_sql_profile_base(profile) + 0x647b0 &&
                az_sql_profile_close_entry(profile) == az_sql_profile_base(profile) + 0x512c0 &&
                az_sql_profile_contains(profile, az_sql_profile_open_entry(profile), 16, 5) && az_sql_profile_contains(profile, az_sql_profile_close_entry(profile), 16, 5),
            "exact reviewed internal entries remain readable executable code");
    REQUIRE(az_sql_profile_io_rva(profile, 0) == 0xb3d98 && az_sql_profile_io_rva(profile, 1) == 0xb3e30 && az_sql_profile_io_rva(profile, 2) == 0xb3ec8 &&
                az_sql_profile_io_rva(profile, 3) == 0 && az_sql_profile_vfs_open_rva(profile) == 0x53968 && az_sql_profile_read_rva(profile) == 0x1513c &&
                az_sql_profile_size_rva(profile) == 0x1bdf4 && az_sql_profile_control_rva(profile) == 0x536b0,
            "verified read-only physical adapter RVAs");
    az_sql_version_number_fn number = (az_sql_version_number_fn) dlsym(handle, "sqlite3_libversion_number");
    az_sql_version_fn version = (az_sql_version_fn) dlsym(handle, "sqlite3_libversion");
    REQUIRE(number && version && number() == 3033000 && !strcmp(version(), "3.33.0"), "real native public getters execute");
    az_crypto_sha256_fn sha = (az_crypto_sha256_fn) dlsym(handle, "EVP_sha256");
    az_crypto_digest_fn digest = (az_crypto_digest_fn) dlsym(handle, "EVP_Digest");
    unsigned char hash[32] = {0};
    unsigned length = 0;
    REQUIRE(sha && digest && az_sql_profile_contains(profile, az_sql_profile_base(profile) + 0xf980, 481192, 5) &&
                digest(((const void *) (az_sql_profile_base(profile) + 0xf980)), 481192, hash, &length, sha(), NULL) &&
                length == sizeof hash && !memcmp(hash, expected_text, sizeof hash),
            "actual mapped native module text matches retained SHA256");
    REQUIRE(dlclose(handle) == 0, "probe releases only its own explicit module reference");
    REQUIRE(dladdr(control, &info) && az_connection_prepare_profile() == profile && number() == 3033000 && !az_connection_ready(),
            "sealed proof holds code lifetime after caller dlclose without enabling hooks");
}
int main(int argc, char **argv) {

    REQUIRE(argc >= 2, "explicit isolated probe case");
    if (!strcmp(argv[1], "missing")) {
        REQUIRE(argc == 2, "missing case has no module path");
        absent();
    } else {
        REQUIRE(argc == 3, "explicit read-only code-module path");
        if (!strcmp(argv[1], "wrong")) wrong(argv[2]);
        else if (!strcmp(argv[1], "valid")) valid(argv[2], false);
        else if (!strcmp(argv[1], "cold")) valid(argv[2], true);
        else REQUIRE(false, "known isolated module probe case");
    }
    printf("PASS connection-profile case=%s: native getter/hash proof only; hooks unavailable; no DB/key API\n", argv[1]);
    return 0;
}
#endif
