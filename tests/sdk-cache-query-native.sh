#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: GNU ELF native source integration required'; exit 77; }
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-query-native.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer)
case ${SAN:-plain} in plain|'') ;; asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all) ;; *) exit 2 ;; esac
"${HOST_CC:-cc}" -std=c11 "${flags[@]}" -Isrc/common -c src/azmod/sdk_cache_entry.c -o "$task_tmp/entry.o"
"${HOST_CXX:-g++}" -std=c++11 "${flags[@]}" -pthread -Isrc/common -rdynamic -DAZSDK_CACHE_BIRTH_TESTING \
    tests/unit/sdkcachequerynative_test.cpp src/azmod/sdk_cache_message.cpp src/azmod/sdk_connection_hooks.cpp \
    "$task_tmp/entry.o" -ldl -o "$task_tmp/test"
failures=0
for profile in 1 2; do
    result=0
    (ulimit -c 0; "$task_tmp/test" "$profile") >"$task_tmp/$profile.log" 2>&1 || result=$?
    cat "$task_tmp/$profile.log"
    if [ "${EXPECT_REENTRY_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/FAIL cache-birth:/ { p; q; }' "$task_tmp/$profile.log")
        if [ "$result" = 0 ] || [ "$result" = 77 ] ||
           ! grep -q 'NATIVE_QUERY_ROWS_OK.*sequence=1500' "$task_tmp/$profile.log" ||
           [[ "$first_failure" != *'unfinished outer native query must hide nested reply evidence'* ]] ||
           grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer|undefined symbol' "$task_tmp/$profile.log"; then
            failures=$((failures + 1))
        else echo "EXPECTED_RUNTIME_RED build=$profile: nested query leaked evidence before outer native completion"; fi
    elif [ "${EXPECT_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/FAIL cache-birth:/ { p; q; }' "$task_tmp/$profile.log")
        if [ "$result" = 0 ] || [ "$result" = 77 ] ||
           ! grep -q "NATIVE_SELECTION_OWNER_OK build=$profile" "$task_tmp/$profile.log" ||
           ! grep -q "NATIVE_QUERY_ROWS_OK build=$profile" "$task_tmp/$profile.log" ||
           [[ "$first_failure" != *'missing actual native query/reply scoped evidence after real rows'* ]] ||
           grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer|undefined symbol' "$task_tmp/$profile.log"; then
            echo "FAIL: profile $profile did not reach intended actual-query evidence RED" >&2
            failures=$((failures + 1))
        else
            echo "EXPECTED_RUNTIME_RED build=$profile: actual native rows returned; query/reply ownership observation missing"
        fi
    elif [ "$result" != 0 ]; then failures=$((failures + 1)); fi
done
[ "$failures" = 0 ]
