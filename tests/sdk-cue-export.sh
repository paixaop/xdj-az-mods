#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-cue-export.XXXXXX")
trap 'rm -rf "$work"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Isrc)
libraries=()
case $(uname -s) in Darwin)flags+=(-Wl,-export_dynamic);;*)flags+=(-rdynamic);libraries+=(-ldl);;esac
case ${SAN:-plain} in plain|'') ;; asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all);;tsan)flags+=(-fsanitize=thread);;*)exit 2;;esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkcueexport_test.c "${libraries[@]}" -o "$work/test"
status=0
(ulimit -c 0;"$work/test") >"$work/run.log" 2>&1 || status=$?
cat "$work/run.log"
if [ "${EXPECT_MASK_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q 'CUE_EXPORT_NATIVE_MASK_OK mask=1 before_receipts=1' "$work/run.log"
    grep -q 'missing actual cue observer support mask export' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: native registration support available before receipts; exported support mask missing'
elif [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q CUE_EXPORT_NATIVE_BOUNDARY_OK "$work/run.log"
    grep -q 'missing actual cue collection export' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: native typed sample available; versioned collection export missing'
else exit "$status";fi
