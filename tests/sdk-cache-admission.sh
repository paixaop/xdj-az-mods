#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-admission.XXXXXX")
trap 'rm -rf "$work"' EXIT
"${HOST_CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -c src/azmod/sdk_cache_entry.c -o "$work/entry.o"
"${HOST_CXX:-clang++}" -std=c++11 -O1 -g -Wall -Wextra -Werror tests/unit/sdkcacheadmission_test.cpp "$work/entry.o" -o "$work/test"
status=0
(ulimit -c 0; "$work/test") >"$work/run.log" 2>&1 || status=$?
cat "$work/run.log"
if [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q CACHE_CAPTURE_NATIVE_BASELINE_OK "$work/run.log"
    grep -q 'missing complete checked cache entry admission' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: genuine originals forward; complete cache admission missing'
else exit "$status"; fi
