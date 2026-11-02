#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != aarch64 ]; then echo 'SKIP: AArch64 Linux required';exit 77;fi
work=$(mktemp -d "${TMPDIR:-/tmp}/az-entry-caller.XXXXXX")
trap 'rm -rf "$work"' EXIT
"${HOST_CXX:-g++}" -std=c++11 -O1 -g -Wall -Wextra -Werror -Isrc/common tests/unit/sdkcacheentrycaller_test.cpp src/azmod/sdk_cache_entry.c src/azmod/sdk_cache_entry.S -o "$work/test"
status=0
(ulimit -c 0;"$work/test") >"$work/run.log" 2>&1 || status=$?
cat "$work/run.log"
if [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q NATIVE_X21_CALLER_BASELINE_OK "$work/run.log"
    grep -q 'missing original caller X21 capture before C++ dispatch' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: original native call works; observer cannot capture caller promotion witness'
else exit "$status";fi
