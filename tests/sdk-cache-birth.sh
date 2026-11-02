#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Standalone pending-cache-birth runtime RED. Never edits source or native inventory.
# Review the test-only Api contract before adding any production adapter/reset seam.
# EXPECT_RED=1 verifies an actual missing ownership observation on OLD production;
# compiler/linker failure, sanitizer errors and merely missing symbols are NOT RED.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: cache-birth requires Linux ELF and GNU libstdc++'
    exit 77
fi
case "$(uname -m)" in
aarch64|arm64) ;;
*)
    if [ "${ALLOW_HOST_OWNER_TEST:-0}" != 1 ]; then
        echo 'SKIP: exact nontrivial X8/row-sret checks require AArch64; ALLOW_HOST_OWNER_TEST=1 tests owner semantics only'
        exit 77
    fi ;;
esac
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-cache-birth.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -rdynamic -DAZSDK_CACHE_BIRTH_TESTING)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
# The test includes the actual current source implementation unchanged. There
# is no extract/replacement engine and no weak accessor existence assertion.
"${HOST_CXX:-c++}" "${flags[@]}" tests/unit/sdkcachebirth_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_tmp/cache-birth"
failures=0
for profile in 1 2; do
    status=0
    (ulimit -c 0; "$task_tmp/cache-birth" "$profile") >"$task_tmp/profile-$profile.log" 2>&1 || status=$?
    cat "$task_tmp/profile-$profile.log"
    if [ "${EXPECT_MODE_RED:-0}" = 1 ]; then
        if [ "$status" = 0 ] || [ "$status" = 77 ] ||
           ! grep -q "NATIVE_FORWARD_OK build=$profile" "$task_tmp/profile-$profile.log" ||
           ! grep -Fq 'unsupported standard Creator mode must forward without a birth claim' "$task_tmp/profile-$profile.log" ||
           grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/profile-$profile.log"; then
            failures=$((failures + 1)); continue
        fi
        echo "EXPECTED_RUNTIME_RED profile=$profile: unsupported standard mode wrongly qualifies a cache birth"
    elif [ "${EXPECT_ISOLATION_RED:-0}" = 1 ]; then
        if [ "$status" = 0 ] || [ "$status" = 77 ] ||
           ! grep -q "NATIVE_FORWARD_OK build=$profile" "$task_tmp/profile-$profile.log" ||
           ! grep -Fq 'unrelated active connection cannot permanently retire another source' "$task_tmp/profile-$profile.log" ||
           grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/profile-$profile.log"; then
            failures=$((failures + 1)); continue
        fi
        echo "EXPECTED_RUNTIME_RED profile=$profile: unrelated active operation permanently retires a still-live source"
    elif [ "${EXPECT_CONNECTION_RED:-0}" = 1 ]; then
        if [ "$status" = 0 ] || [ "$status" = 77 ] ||
           ! grep -q "NATIVE_FORWARD_OK build=$profile" "$task_tmp/profile-$profile.log" ||
           ! grep -Fq 'unobserved/retired SQLite connection cannot qualify cache birth' "$task_tmp/profile-$profile.log" ||
           grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/profile-$profile.log"; then
            failures=$((failures + 1)); continue
        fi
        echo "EXPECTED_RUNTIME_RED profile=$profile: native cache forwards but has no observed SQLite incarnation guard"
    elif [ "${EXPECT_RED:-0}" = 1 ]; then
        if [ "$status" = 0 ] || [ "$status" = 77 ]; then
            echo "FAIL: profile $profile did not produce the required old-production runtime RED" >&2
            failures=$((failures + 1)); continue
        fi
        first_failure=$(sed -n '/FAIL cache-birth:/ { p; q; }' "$task_tmp/profile-$profile.log")
        if ! grep -q "NATIVE_FORWARD_OK build=$profile owner16=16 rows24=24" "$task_tmp/profile-$profile.log" ||
           [[ "$first_failure" != *'missing observed cache birth and real ownership result'* ]] ||
           grep -Eq 'ERROR: (AddressSanitizer|ThreadSanitizer)|runtime error:|undefined symbol|Segmentation fault' "$task_tmp/profile-$profile.log"; then
            echo "FAIL: profile $profile failed before/other than the concrete missing ownership observation" >&2
            failures=$((failures + 1)); continue
        fi
        echo "EXPECTED_RUNTIME_RED profile=$profile: native creation/query forwarded; observed cache/source ownership missing"
    elif [ "$status" != 0 ]; then
        failures=$((failures + 1))
    fi
done
[ "$failures" = 0 ]
