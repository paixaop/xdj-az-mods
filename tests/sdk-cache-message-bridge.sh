#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: Linux required';exit 77; }
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-message-bridge.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common)
case ${SAN:-plain} in plain|'') ;;asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;tsan)flags+=(-fsanitize=thread -fno-omit-frame-pointer);;*)exit 2;;esac
sources=(tests/unit/sdkcachemessagebridge_test.cpp)
[ ! -f src/azmod/sdk_cache_message.cpp ] || sources+=(src/azmod/sdk_cache_message.cpp)
"${HOST_CXX:-g++}" "${flags[@]}" "${sources[@]}" -o "$task_tmp/test"
status=0
(ulimit -c 0;"$task_tmp/test") >"$task_tmp/run.log" 2>&1 || status=$?
cat "$task_tmp/run.log"
if [ "${EXPECT_RED:-0}" = 1 ];then
 [ "$status" != 0 ] && [ "$status" != 77 ]
 grep -q NATIVE_C_MESSAGE_FORWARD_OK "$task_tmp/run.log"
 grep -q 'missing actual C bridge owned message frame' "$task_tmp/run.log"
 ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/run.log"
 echo 'EXPECTED_RUNTIME_RED: actual native C callback forwarded, copied frame bridge missing'
else exit "$status";fi
