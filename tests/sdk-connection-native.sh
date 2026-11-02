#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Separate immutable actual-native memory-only redirect packet. No emulator,
# hardware, production installation, real database or key operations.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then echo 'SKIP: GNU Linux packet required'; exit 77; fi
case ${MODE:-all} in build|run|all) ;; *) echo 'MODE must be build, run or all' >&2; exit 2 ;; esac
: "${OUT_DIR:?set OUT_DIR to a new retained native packet directory}"
mkdir -p "$OUT_DIR"
task_out=$(cd "$OUT_DIR" && pwd)
if [ "${MODE:-all}" != run ]; then
    if [ -e "$task_out/connection-native" ] || [ -e "$task_out/build-inputs.txt" ]; then
        echo 'FAIL: preserve earlier packet attempts; choose a new OUT_DIR' >&2; exit 2
    fi
    # Compile the real C installer as C11. No copied installer model or source
    # edits, and no forwarding/profile testing seam in the C++ dependency.
    python3 - "$task_out" <<'PY'
from pathlib import Path
import json,sys
p=Path(sys.argv[1]); src=Path('src/azmod/inline_hooks.c').resolve()
(p/'island-wrapper.c').write_text('''#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define dbg(...) ((void)0)
#include '''+json.dumps(str(src))+'''
int azsdk_test_connection_island(uintptr_t entry, void *hook, void **original) {
    return azm_install_island_hook_at(entry, hook, original, "connection-native-fixture");
}
unsigned azsdk_test_connection_island_count(void) {
    unsigned count = 0;
    int n = __atomic_load_n(&g_azm_rec_n, __ATOMIC_ACQUIRE);
    if (n > AZM_REC_MAX) return 0;
    for (int i = 0; i < n; ++i)
        if (__atomic_load_n(&g_azm_rec[i].path, __ATOMIC_ACQUIRE) == AZM_ISLAND) ++count;
        else return 0;
    return count;
}
''')
PY
    "${HOST_CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread \
        -c "$task_out/island-wrapper.c" -o "$task_out/island-wrapper.o"
    flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread)
    "${HOST_CXX:-c++}" "${flags[@]}" tests/unit/sdkconnectionnative_test.cpp \
        src/azmod/sdk_connection_hooks.cpp "$task_out/island-wrapper.o" -ldl -o "$task_out/connection-native"
    "${HOST_CXX:-c++}" "${flags[@]}" -DAZSDK_NATIVE_UNREDIRECTED_CONTROL tests/unit/sdkconnectionnative_test.cpp \
        src/azmod/sdk_connection_hooks.cpp "$task_out/island-wrapper.o" -ldl -o "$task_out/connection-native-unredirected"
    {
        "${HOST_CXX:-c++}" --version
        sha256sum tests/unit/sdkconnectionnative_test.cpp tests/sdk-connection-native.sh \
            src/common/azconnectionepoch.h src/common/azconnectionhooks.h src/azmod/sdk_connection_hooks.cpp \
            src/azmod/inline_hooks.c src/common/azprot.h "$task_out/island-wrapper.c" \
            "$task_out/island-wrapper.o" "$task_out/connection-native" "$task_out/connection-native-unredirected"
    } >"$task_out/build-inputs.txt"
    if command -v readelf >/dev/null 2>&1; then
        readelf -h -l -d -V "$task_out/connection-native" >"$task_out/probe-elf-requirements.txt"
        readelf -h -l -d -V "$task_out/connection-native-unredirected" >"$task_out/control-elf-requirements.txt"
    fi
fi
if [ "${MODE:-all}" = build ]; then echo "NATIVE_CONNECTION_PACKET_BUILT=$task_out"; exit 0; fi
: "${MODULE_PATH:?set MODULE_PATH to the explicit exact retained native SQLCipher ELF}"
case ${MODULE_BUILD:-} in
1) expected=4b41d53a1b50d14e0d01cb08384546ba5f561a71cb4bd7a2784628095521f15d ;;
2) expected=9d0fd0efd8caf4053df104ef02ef45dc18f45092552bae98f62a52808a610b5b ;;
*) echo 'MODULE_BUILD must select exact1.30=1 or2.00=2 artifact' >&2; exit 2 ;;
esac
actual=$(sha256sum "$MODULE_PATH"); actual=${actual%% *}
if [ "$actual" != "$expected" ]; then echo 'FAIL: exact original module whole-file SHA mismatch' >&2; exit 1; fi
: "${RUNTIME_LOADER:?set explicitly qualified coherent native loader}"
: "${RUNTIME_LIBRARY_PATH:?set its explicitly qualified coherent native library directory}"
task_runtime_parent=${RUNTIME_OUT_DIR:-$task_out}
mkdir -p "$task_runtime_parent"
task_run=$(mktemp -d "$task_runtime_parent/connection-native-build$MODULE_BUILD.XXXXXX")
{
    uname -a
    printf 'MODULE_BUILD=%s\nMODULE_PATH=%s\nRUNTIME_LOADER=%s\nRUNTIME_LIBRARY_PATH=%s\n' \
        "$MODULE_BUILD" "$MODULE_PATH" "$RUNTIME_LOADER" "$RUNTIME_LIBRARY_PATH"
    sha256sum "$MODULE_PATH" "$RUNTIME_LOADER" "$task_out/connection-native" "$task_out/connection-native-unredirected"
} >"$task_run/runtime-inputs.txt"
run_native() {
    LD_PRELOAD= LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" OPENSSL_CONF=/dev/null \
        "$RUNTIME_LOADER" --library-path "$RUNTIME_LIBRARY_PATH" "$@"
}
status=0
(ulimit -c 0; run_native "$task_out/connection-native-unredirected" "$MODULE_PATH" roundtrip) \
    >"$task_run/control-unredirected.log" 2>&1 || status=$?
cat "$task_run/control-unredirected.log"
if [ "$status" = 0 ] || [ "$status" = 77 ] ||
    ! grep -q NATIVE_MEMORY_BASELINE_OK "$task_run/control-unredirected.log" ||
    ! grep -q 'missing adapter-owned actual native successful connection birth' "$task_run/control-unredirected.log" ||
    grep -Eq 'DLOPEN_FAILURE|Segmentation fault|runtime error:|undefined symbol' "$task_run/control-unredirected.log"; then
    echo "FAIL: unredirected actual-native control exit=$status lacked exact birth assertion" >&2; exit 1
fi
echo 'EXPECTED_RUNTIME_RED: genuine native memory APIs succeeded; absent redirects could not create an observed birth'
for scenario in roundtrip busy zombie failed partial; do
    (ulimit -c 0; run_native "$task_out/connection-native" "$MODULE_PATH" "$scenario") \
        >"$task_run/$scenario.log" 2>&1
    cat "$task_run/$scenario.log"
done
echo "NATIVE_CONNECTION_COMPLETE build=$MODULE_BUILD packet=$task_out runtime_evidence=$task_run"
