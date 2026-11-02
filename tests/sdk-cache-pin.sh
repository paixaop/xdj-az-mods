#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Header-only GNU owner semantics; no firmware/module/entry execution.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-pin.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
compiler=${HOST_CXX:-g++}
if [ "$(uname -s)" = Darwin ] && [ -z "${HOST_CXX:-}" ]; then compiler=/usr/local/bin/g++-15; fi
"$compiler" -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread tests/unit/sdkcachepin_test.cpp -o "$task_tmp/test"
status=0
(ulimit -c 0; "$task_tmp/test") >"$task_tmp/run.log" 2>&1 || status=$?
cat "$task_tmp/run.log"
if [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q NATIVE_GNU_PIN_BASELINE_OK "$task_tmp/run.log"
    first_failure=$(sed -n '/FAIL cache-pin:/ { p; q; }' "$task_tmp/run.log")
    [[ "$first_failure" == *'missing observed stable cache/source ownership pins'* ]]
    ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/run.log"
    echo 'EXPECTED_RUNTIME_RED: native GNU result forwards; stable local ownership observation missing'
else exit "$status"; fi
