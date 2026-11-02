#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Real native-owner forwarding before a concrete missing-resolver runtime RED.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then echo 'SKIP: cache selection requires Linux GNU ownership'; exit 77; fi
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-selection.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -DAZSDK_CACHE_SELECTION_TESTING)
if [ -f src/common/azcacheselection.h ]; then flags+=(-DAZSDK_HAVE_CACHE_SELECTION); fi
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
"${HOST_CXX:-g++}" "${flags[@]}" tests/unit/sdkcacheselection_test.cpp -o "$task_tmp/test"
failures=0
for profile in 1 2; do
    status=0
    (ulimit -c 0; "$task_tmp/test" "$profile") >"$task_tmp/profile-$profile.log" 2>&1 || status=$?
    cat "$task_tmp/profile-$profile.log"
    if [ "${EXPECT_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/FAIL cache-selection:/ { p; q; }' "$task_tmp/profile-$profile.log")
        if [ "$status" = 0 ] || [ "$status" = 77 ] ||
           ! grep -q "NATIVE_SELECTION_FORWARD_OK build=$profile shared16=16 vector24=24 forwards=1 value=101" "$task_tmp/profile-$profile.log" ||
           [[ "$first_failure" != *'missing fresh selected-owner pins and real ownership result'* ]] ||
           grep -Eq 'ERROR: (AddressSanitizer|ThreadSanitizer)|runtime error:|undefined symbol|Segmentation fault|Illegal instruction|Bus error' "$task_tmp/profile-$profile.log"; then
            echo "FAIL: profile $profile did not reach the concrete missing selected-owner observation" >&2
            failures=$((failures + 1)); continue
        fi
        echo "EXPECTED_RUNTIME_RED profile=$profile: real native query forwarded; fresh selected ownership unavailable"
    elif [ "$status" != 0 ]; then failures=$((failures + 1)); fi
done
[ "$failures" = 0 ]
