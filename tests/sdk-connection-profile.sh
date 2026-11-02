#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Separate immutable packet for actual native module proof. No hook install,
# database/connection/key API, emulator edits or automatic runtime selection.
# MODE=build prepares binaries; MODE=run uses an explicitly coherent runtime.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: build/run packet requires GNU Linux; no native module work performed'
    exit 77
fi
case ${MODE:-all} in build|run|all) ;; *) echo 'MODE must be build, run or all' >&2; exit 2 ;; esac
: "${OUT_DIR:?set OUT_DIR to a retained new packet directory}"
mkdir -p "$OUT_DIR"
task_out=$(cd "$OUT_DIR" && pwd)
if [ "${MODE:-all}" != run ]; then
    if [ -e "$task_out/connection-profile" ] || [ -e "$task_out/build-inputs.txt" ]; then
        echo 'FAIL: preserve earlier build attempts; choose a new OUT_DIR' >&2
        exit 2
    fi
    cat >"$task_out/wrong-profile.c" <<'C'
/* Version-compatible names deliberately lack the exact native module layout. */
#include <stddef.h>
unsigned profile_forbidden_calls;
int sqlite3_libversion_number(void) { return 3033000; }
const char *sqlite3_libversion(void) { return "3.33.0"; }
int sqlite3_file_control(void *p, const char *n, int o, void *v) { (void)p;(void)n;(void)o;(void)v; ++profile_forbidden_calls; return 21; }
int sqlite3_open(const char *p, void **o) { (void)p;(void)o; ++profile_forbidden_calls; return 21; }
int sqlite3_open_v2(const char *p, void **o, int f, const char *v) { (void)p;(void)o;(void)f;(void)v; ++profile_forbidden_calls; return 21; }
int sqlite3_open16(const void *p, void **o) { (void)p;(void)o; ++profile_forbidden_calls; return 21; }
int sqlite3_close(void *p) { (void)p; ++profile_forbidden_calls; return 21; }
int sqlite3_close_v2(void *p) { (void)p; ++profile_forbidden_calls; return 21; }
const void *EVP_sha256(void) { ++profile_forbidden_calls; return 0; }
int EVP_Digest(const void *p, size_t n, unsigned char *o, unsigned *l, const void *m, void *e) { (void)p;(void)n;(void)o;(void)l;(void)m;(void)e; ++profile_forbidden_calls; return 0; }
C
    "${HOST_CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread \
        tests/unit/sdkconnectionprofile_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_out/connection-profile"
    "${HOST_CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -shared -fPIC \
        -Wl,-soname,libsqlcipher.so.0 "$task_out/wrong-profile.c" -o "$task_out/wrong-profile.so"
    {
        "${HOST_CXX:-c++}" --version
        sha256sum tests/unit/sdkconnectionprofile_test.cpp tests/sdk-connection-profile.sh \
            src/common/azconnectionepoch.h src/common/azconnectionhooks.h src/azmod/sdk_connection_hooks.cpp \
            "$task_out/connection-profile" "$task_out/wrong-profile.c" "$task_out/wrong-profile.so"
    } >"$task_out/build-inputs.txt"
    if command -v readelf >/dev/null 2>&1; then
        readelf -h -l -V "$task_out/connection-profile" >"$task_out/probe-elf-requirements.txt"
        readelf -h -d -V "$task_out/wrong-profile.so" >"$task_out/lookalike-elf-requirements.txt"
    fi
fi
if [ "${MODE:-all}" = build ]; then echo "PROFILE_PACKET_BUILT=$task_out"; exit 0; fi
: "${MODULE_PATH:?set MODULE_PATH to the explicit exact retained SQLCipher ELF}"
case ${MODULE_BUILD:-} in
1) expected=4b41d53a1b50d14e0d01cb08384546ba5f561a71cb4bd7a2784628095521f15d ;;
2) expected=9d0fd0efd8caf4053df104ef02ef45dc18f45092552bae98f62a52808a610b5b ;;
*) echo 'MODULE_BUILD must select exact1.30=1 or2.00=2 artifact' >&2; exit 2 ;;
esac
actual=$(sha256sum "$MODULE_PATH"); actual=${actual%% *}
if [ "$actual" != "$expected" ]; then echo 'FAIL: module whole-file SHA differs from selected retained original' >&2; exit 1; fi
task_runtime_parent=${RUNTIME_OUT_DIR:-$task_out}
mkdir -p "$task_runtime_parent"
task_run=$(mktemp -d "$task_runtime_parent/connection-profile-build$MODULE_BUILD.XXXXXX")
run=("$task_out/connection-profile")
if [ -n "${RUNTIME_LOADER:-}" ]; then
    : "${RUNTIME_LIBRARY_PATH:?explicit loader needs its coherent library search path}"
    run=("$RUNTIME_LOADER" --library-path "$RUNTIME_LIBRARY_PATH" "$task_out/connection-profile")
fi
{
    uname -a
    printf 'MODULE_BUILD=%s\nMODULE_PATH=%s\nRUNTIME_LOADER=%s\nRUNTIME_LIBRARY_PATH=%s\n' \
        "$MODULE_BUILD" "$MODULE_PATH" "${RUNTIME_LOADER:-native environment}" "${RUNTIME_LIBRARY_PATH:-native environment}"
    sha256sum "$MODULE_PATH" "$task_out/connection-profile" "$task_out/wrong-profile.so"
} >"$task_run/runtime-inputs.txt"
for scenario in missing wrong valid cold; do
    args=("$scenario")
    if [ "$scenario" = wrong ]; then args+=("$task_out/wrong-profile.so"); fi
    if [ "$scenario" = valid ] || [ "$scenario" = cold ]; then args+=("$MODULE_PATH"); fi
    # Every case gets a new process and no inherited mod preload. Library search
    # paths/loader must be qualified by the caller before this run is authorized.
    LD_PRELOAD= LD_LIBRARY_PATH="${RUNTIME_LIBRARY_PATH:-${LD_LIBRARY_PATH:-}}" \
        OPENSSL_CONF=/dev/null "${run[@]}" "${args[@]}" >"$task_run/$scenario.log" 2>&1
    cat "$task_run/$scenario.log"
done
echo "PROFILE_PROOF_COMPLETE build=$MODULE_BUILD packet=$task_out runtime_evidence=$task_run"
