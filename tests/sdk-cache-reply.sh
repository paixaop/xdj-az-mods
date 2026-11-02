#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Private GNU owner handoff only; no installed native hook/transport/source join.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-reply.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
compiler=${HOST_CXX:-g++}
if [ "$(uname -s)" = Darwin ] && [ -z "${HOST_CXX:-}" ]; then compiler=/usr/local/bin/g++-15; fi
"$compiler" -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread tests/unit/sdkcachereply_test.cpp -o "$task_tmp/test"
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
