#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: GNU Linux ELF fixture required'; exit 77; }
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-cue.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -rdynamic -Isrc/common -DAZSDK_CUE_TESTING)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
sources=(tests/unit/sdkcue_test.cpp)
[ ! -f src/azstem/sdk_cue_observer.cpp ] || sources+=(src/azstem/sdk_cue_observer.cpp)
"${HOST_CXX:-g++}" "${flags[@]}" "${sources[@]}" -o "$task_tmp/test"
failures=0
for profile in 1 2; do
    modes=(ordinary capacity)
    [ "${EXPECT_RED:-0}" != 1 ] || modes=(ordinary)
    for mode in "${modes[@]}"; do
        status=0
        args=("$profile"); [ "$mode" != capacity ] || args+=(capacity)
        (ulimit -c 0; "$task_tmp/test" "${args[@]}") >"$task_tmp/$profile-$mode.log" 2>&1 || status=$?
        cat "$task_tmp/$profile-$mode.log"
        if [ "${EXPECT_RED:-0}" = 1 ]; then
            first_failure=$(sed -n '/FAIL cue:/ { p; q; }' "$task_tmp/$profile-$mode.log")
            if [ "$status" = 0 ] || [ "$status" = 77 ] ||
               ! grep -q NATIVE_CUE_CALLBACK_BASELINE_OK "$task_tmp/$profile-$mode.log" ||
               ! grep -q "NATIVE_CUE_UNIT_FORWARD_OK build=$profile deck=0" "$task_tmp/$profile-$mode.log" ||
               [[ "$first_failure" != *'missing actual native cue callback to collected observation'* ]] ||
               grep -Eq 'Segmentation fault|runtime error:|undefined symbol|ERROR: AddressSanitizer' "$task_tmp/$profile-$mode.log"; then
                failures=$((failures + 1))
            else echo "EXPECTED_RUNTIME_RED build=$profile: native typed weak callback works; cue observation missing"; fi
        elif [ "$status" != 0 ]; then failures=$((failures + 1)); fi
    done
done
[ "$failures" = 0 ]
