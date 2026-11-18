#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != aarch64 ]; then echo 'SKIP: AArch64 Linux required';exit 77;fi
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-entry-fp.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
"${HOST_CC:-gcc}" "${flags[@]}" -Isrc/common tests/unit/sdkcacheentryfp_test.c src/azmod/sdk_cache_entry.c src/azmod/sdk_cache_entry.S src/common/aznative_guard.S -lstdc++ -o "$task_tmp/test"
status=0
(ulimit -c 0; "$task_tmp/test") >"$task_tmp/run.log" 2>&1 || status=$?
cat "$task_tmp/run.log"
if [ "${EXPECT_RED:-0}" = 1 ]; then
 [ "$status" != 0 ] && [ "$status" != 77 ]
 grep -q NATIVE_FP_BASELINE_OK "$task_tmp/run.log"
 grep -q 'floating/vector arguments changed by observer' "$task_tmp/run.log"
 ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/run.log"
 echo 'EXPECTED_RUNTIME_RED: genuine native FP baseline passes; entry observer clobbers FP inputs'
else exit "$status";fi
