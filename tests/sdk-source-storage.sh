#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: SourceState shutdown fixture requires Linux GNU ABI';exit 77; }
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-source-storage.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2;; esac
"${HOST_CXX:-g++}" "${flags[@]}" tests/unit/sdksourcestorage_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_tmp/test"
failures=0
for profile in 1 2;do
 status=0
 (ulimit -c 0;"$task_tmp/test" "$profile") >"$task_tmp/$profile.log" 2>&1 || status=$?
 cat "$task_tmp/$profile.log"
 if [ "${EXPECT_RED:-0}" = 1 ];then
  if [ "$status" = 0 ] || ! grep -q "NATIVE_STATE_STORAGE_OK build=$profile" "$task_tmp/$profile.log" ||
     ! grep -q 'SourceState destroyed before later native shutdown hook' "$task_tmp/$profile.log" ||
     grep -Eq 'Segmentation fault|runtime error:|undefined symbol' "$task_tmp/$profile.log";then failures=$((failures+1));
  else echo "EXPECTED_RUNTIME_RED profile=$profile: later native hook outlives ordinary SourceState destructor";fi
 elif [ "$status" != 0 ];then failures=$((failures+1));fi
done
[ "$failures" = 0 ]
