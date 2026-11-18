#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Private GNU owner handoff only; no installed native hook/transport/source join.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-reply.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2;; esac
if [ -n "${REPLY_HEADER:-}" ]; then flags+=("-DREPLY_HEADER=\"${REPLY_HEADER}\""); fi
"${HOST_CC:-gcc}" "${flags[@]}" tests/unit/sdkcachereply_test.c src/common/aznative_guard.S src/common/azcache_scopes.c -lstdc++ -o "$task_tmp/test"
for profile in 1 2; do
    status=0
    (ulimit -c 0; "$task_tmp/test" "$profile") >"$task_tmp/run.log" 2>&1 || status=$?
    cat "$task_tmp/run.log"
    if [ "${EXPECT_RED:-0}" = 1 ]; then
        [ "$status" != 0 ] && [ "$status" != 77 ]
        grep -q NATIVE_GNU_REPLY_BASELINE_OK "$task_tmp/run.log"
        first_failure=$(sed -n '/FAIL cache-reply:/ { p; q; }' "$task_tmp/run.log")
        [[ "$first_failure" == *'missing selected Ticket to native-owned Reception receipt'* ]]
        ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/run.log"
        echo "EXPECTED_RUNTIME_RED: genuine native weak receiver forwards; owning reply receipt missing (profile $profile)"
    else [ "$status" = 0 ]; fi
done
