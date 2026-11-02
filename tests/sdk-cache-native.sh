#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: GNU ELF source integration required'; exit 77; }
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-native.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer)
case ${SAN:-plain} in plain|'') ;; asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all) ;; *) exit 2 ;; esac
"${HOST_CC:-cc}" -std=c11 "${flags[@]}" -Isrc/common -c src/azmod/sdk_cache_entry.c -o "$task_tmp/entry.o"
"${HOST_CXX:-g++}" -std=c++11 "${flags[@]}" -pthread -Isrc/common -rdynamic -DAZSDK_CACHE_BIRTH_TESTING \
    tests/unit/sdkcachenative_test.cpp src/azmod/sdk_cache_message.cpp src/azmod/sdk_connection_hooks.cpp \
    "$task_tmp/entry.o" -ldl -o "$task_tmp/test"
for profile in 1 2; do
    result=0
    (ulimit -c 0; "$task_tmp/test" "$profile") >"$task_tmp/$profile.log" 2>&1 || result=$?
    cat "$task_tmp/$profile.log"
    if [ "${EXPECT_ADMISSION_RED:-0}" = 1 ]; then
        [ "$result" != 0 ] && [ "$result" != 77 ]
        grep -q 'NATIVE_SELECTION_OWNER_OK' "$task_tmp/$profile.log"
        grep -q 'production route must not observe before complete startup admission' "$task_tmp/$profile.log"
        ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$task_tmp/$profile.log"
        echo "EXPECTED_RUNTIME_RED build=$profile: native forwarding admitted observation before startup completion"
    elif [ "${EXPECT_SELECTION_RED:-0}" = 1 ]; then
        [ "$result" != 0 ] && [ "$result" != 77 ]
        grep -q 'NATIVE_SELECTION_OWNER_OK' "$task_tmp/$profile.log"
        grep -q 'native selected-cache publication must preserve its genuine cache birth' "$task_tmp/$profile.log"
        ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$task_tmp/$profile.log"
        echo "EXPECTED_RUNTIME_RED build=$profile: native selected store destroys the just-recorded birth"
    elif [ "${EXPECT_RED:-0}" = 1 ]; then
        [ "$result" != 0 ] && [ "$result" != 77 ]
        grep -q 'NATIVE_SELECTION_OWNER_OK' "$task_tmp/$profile.log"
        grep -q 'missing actual owned message/selection/Collector source-birth adapter' "$task_tmp/$profile.log"
        ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$task_tmp/$profile.log"
        echo "EXPECTED_RUNTIME_RED build=$profile: real native owners/results forward; source birth missing"
    else [ "$result" = 0 ]; fi
done
