#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != aarch64 ]; then
    echo 'SKIP: cache island entry execution requires AArch64 Linux'; exit 77
fi
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-island.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread)
if grep -q 'static int azm_install_island_hook_at(' src/azmod/inline_hooks.c; then flags+=(-DHAVE_ISLAND_ONLY_ENTRY); fi
"${HOST_CC:-gcc}" "${flags[@]}" tests/unit/sdkcacheisland_test.c -o "$task_tmp/test"
status=0
(ulimit -c 0; "$task_tmp/test") >"$task_tmp/run.log" 2>&1 || status=$?
cat "$task_tmp/run.log"
if [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q NATIVE_ISLAND_BASELINE_OK "$task_tmp/run.log"
    grep -q 'missing dedicated single-instruction island installer' "$task_tmp/run.log"
    ! grep -Eq 'undefined symbol|Segmentation fault|runtime error:' "$task_tmp/run.log"
    echo 'EXPECTED_RUNTIME_RED: original native branches work; dedicated island-only path absent'
else exit "$status"; fi
