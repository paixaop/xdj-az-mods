#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: actual GNU ELF source integration required'; exit 77; }
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-reply-native.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer)
case ${SAN:-plain} in plain|'') ;; asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all) ;; *) exit 2 ;; esac
source_cpp=${SDK_CACHE_REPLY_SOURCE_CPP:-"$PWD/src/azmod/sdk_native_sources.cpp"}
case "$source_cpp" in /*) ;; *) echo 'SDK_CACHE_REPLY_SOURCE_CPP must be absolute' >&2; exit 2 ;; esac
[ -f "$source_cpp" ] || { echo 'selected SourceCpp is missing' >&2; exit 2; }
cat >"$task_tmp/hash.c" <<'C'
#include "azsha256.h"
void azsdk_reply_fixture_hash(const void *data, size_t n, char *out) { azsha256_hex(data, n, out); }
C
"${HOST_CC:-cc}" -std=c11 "${flags[@]}" -Isrc/common -c "$task_tmp/hash.c" -o "$task_tmp/hash.o"
"${HOST_CXX:-g++}" -std=c++11 "${flags[@]}" -pthread -Isrc/common -rdynamic -DAZSDK_CACHE_BIRTH_TESTING \
    "-DSOURCES_INC=\"$source_cpp\"" \
    tests/unit/sdkcachereplynative_test.cpp src/azmod/sdk_cache_message.cpp src/azmod/sdk_connection_hooks.cpp \
    "$task_tmp/hash.o" -ldl -o "$task_tmp/test"
failures=0
for profile in 1 2; do
    status=0
    (ulimit -c 0; "$task_tmp/test" "$profile") >"$task_tmp/$profile.log" 2>&1 || status=$?
    cat "$task_tmp/$profile.log"
    if [ "${EXPECT_FRAME_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/FAIL cache-birth:/ { p; q; }' "$task_tmp/$profile.log")
        if [ "$status" = 0 ] || [ "$status" = 77 ] ||
           ! grep -q "NATIVE_REPLY_OWNER_BASELINE_OK build=$profile" "$task_tmp/$profile.log" ||
           [[ "$first_failure" != *'incompatible native frame forwards but must not expose an owned reply receipt'* ]] ||
           grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer|undefined symbol' "$task_tmp/$profile.log"; then
            failures=$((failures + 1))
        else echo "EXPECTED_RUNTIME_RED build=$profile: native forwarding and ownership work; incompatible frame is observed"; fi
    elif [ "${EXPECT_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/FAIL cache-birth:/ { p; q; }' "$task_tmp/$profile.log")
        if [ "$status" = 0 ] || [ "$status" = 77 ] ||
           ! grep -q "NATIVE_REPLY_OWNER_BASELINE_OK build=$profile" "$task_tmp/$profile.log" ||
           ! grep -q "NATIVE_REPLY_FORWARD_OK build=$profile callbacks=1" "$task_tmp/$profile.log" ||
           [[ "$first_failure" != *'missing actual Ticket dispatch to native-owned callback receipt'* ]] ||
           grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer|undefined symbol' "$task_tmp/$profile.log"; then
            failures=$((failures + 1))
        else echo "EXPECTED_RUNTIME_RED build=$profile: native owned reply forwards/cleans; callback ownership receipt missing"; fi
    elif [ "$status" != 0 ]; then failures=$((failures + 1)); fi
done
[ "$failures" = 0 ]
