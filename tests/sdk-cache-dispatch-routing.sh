#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Portable routing spies only; sdk-cache-dispatch.sh validates real cache state.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-routing.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -fexceptions -fasynchronous-unwind-tables)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2;; esac
"${HOST_CC:-gcc}" "${flags[@]}" tests/unit/sdkcachedispatchrouting_test.c src/azmod/sdk_cache_message.c \
    src/azmod/sdk_cache_entry.c src/common/aznative_guard.c src/common/azcache_scopes.c -lstdc++ -ldl -o "$task_tmp/test"

status=0
(ulimit -c 0; "$task_tmp/test") >"$task_tmp/run.log" 2>&1 || status=$?
cat "$task_tmp/run.log"
if [ "${EXPECT_WRITE_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q ROUTING_FORWARD_BASELINE_OK "$task_tmp/run.log"
    grep -q 'WRITE_NATIVE_FORWARD_OK kind=6 status=00000000' "$task_tmp/run.log"
    first_failure=$(sed -n '/FAIL cache-dispatch-routing:/ { p; q; }' "$task_tmp/run.log")
    [[ "$first_failure" == *'missing native construction write entry adaptation'* ]]
    ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/run.log"
    echo 'EXPECTED_RUNTIME_RED: setup original and full results forward; construction write adaptation missing'
elif [ "${EXPECT_FACTORY_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q ROUTING_FORWARD_BASELINE_OK "$task_tmp/run.log"
    grep -q 'ORIGINAL_FRAME_FORWARD_OK kind=11 depth=0' "$task_tmp/run.log"
    first_failure=$(sed -n '/FAIL cache-dispatch-routing:/ { p; q; }' "$task_tmp/run.log")
    [[ "$first_failure" == *'missing native factory entry adaptation'* ]]
    ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/run.log"
    echo 'EXPECTED_RUNTIME_RED: genuine factory original/results forward; factory adaptation missing'
elif [ "${EXPECT_APPEND_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q ROUTING_FORWARD_BASELINE_OK "$task_tmp/run.log"
    grep -q LEGACY_ENTRY_TABLE_OK "$task_tmp/run.log"
    first_failure=$(sed -n '/FAIL cache-dispatch-routing:/ { p; q; }' "$task_tmp/run.log")
    [[ "$first_failure" == *'missing distinct appended cache entries and original slots'* ]]
    ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/run.log"
    echo 'EXPECTED_RUNTIME_RED: existing routes forward; appended distinct entries/original slots unavailable'
elif [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q ROUTING_FORWARD_BASELINE_OK "$task_tmp/run.log"
    grep -q 'ORIGINAL_FRAME_FORWARD_OK kind=8 depth=0' "$task_tmp/run.log"
    first_failure=$(sed -n '/FAIL cache-dispatch-routing:/ { p; q; }' "$task_tmp/run.log")
    [[ "$first_failure" == *'missing mutation retirement routing before original'* ]]
    ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/run.log"
    echo 'EXPECTED_RUNTIME_RED: known mutation original reached before retirement forwarding'
else exit "$status"; fi
