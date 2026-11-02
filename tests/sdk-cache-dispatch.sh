#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: Linux GNU ownership required'; exit 77; }
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-dispatch.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
cflags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Isrc/common -fexceptions)
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -rdynamic -DAZSDK_CACHE_BIRTH_TESTING)
case ${SAN:-plain} in
plain|'') ;;
asan) cflags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer); flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) cflags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer); flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or ubsan' >&2; exit 2 ;;
esac
# The C forwarding-only weak default is linked unchanged alongside the actual
# production C++ message/dispatcher module. No test dispatcher overrides it.
"${HOST_CC:-cc}" "${cflags[@]}" -c src/azmod/sdk_cache_entry.c -o "$task_tmp/entry.o"
"${HOST_CXX:-g++}" "${flags[@]}" tests/unit/sdkcachedispatch_test.cpp \
    "${CACHE_DISPATCH_SOURCE:-src/azmod/sdk_cache_message.cpp}" src/azmod/sdk_connection_hooks.cpp "$task_tmp/entry.o" -ldl -o "$task_tmp/test"
failures=0
for profile in 1 2; do
    status=0
    (ulimit -c 0; "$task_tmp/test" "$profile") >"$task_tmp/$profile.log" 2>&1 || status=$?
    cat "$task_tmp/$profile.log"
    if [ "${EXPECT_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/FAIL cache-birth:/ { p; q; }' "$task_tmp/$profile.log")
        if [ "$status" = 0 ] || [ "$status" = 77 ] ||
           ! grep -q "NATIVE_FORWARD_OK build=$profile owner16=16 rows24=24" "$task_tmp/$profile.log" ||
           ! grep -q "DISPATCH_FORWARD_BASELINE_OK build=$profile" "$task_tmp/$profile.log" ||
           ! grep -q "NATIVE_DISPATCH_FORWARD_OK build=$profile kind=8" "$task_tmp/$profile.log" ||
           [[ "$first_failure" != *'missing mutation retirement before native original'* ]] ||
           grep -Eq 'Segmentation fault|runtime error:|undefined symbol|ERROR: (AddressSanitizer|ThreadSanitizer)' "$task_tmp/$profile.log"; then
            echo "FAIL: profile $profile did not reach the intended real dispatcher retirement assertion" >&2
            failures=$((failures + 1))
        else
            echo "EXPECTED_RUNTIME_RED profile=$profile: real native original ran while old cache witness remained accepted"
        fi
    elif [ "$status" != 0 ]; then failures=$((failures + 1)); fi
done
[ "$failures" = 0 ]
