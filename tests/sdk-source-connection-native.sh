#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
: "${OUT_DIR:?new retained packet directory required}"
mkdir -p "$OUT_DIR"
task_out=$(cd "$OUT_DIR" && pwd)
if [ "${MODE:-build}" = build ];then
 : "${ISLAND_OBJECT:?previous exact real installer object required}"
 [ ! -e "$task_out/source-connection-native" ]
 "${HOST_CXX:-g++}" -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread tests/unit/sdksourceconnectionnative_test.cpp src/azmod/sdk_connection_hooks.cpp "$ISLAND_OBJECT" -ldl -o "$task_out/source-connection-native"
 readelf -h -l -d -V "$task_out/source-connection-native" >"$task_out/probe-elf-requirements.txt"
 sha256sum tests/unit/sdksourceconnectionnative_test.cpp tests/sdk-source-connection-native.sh src/azmod/sdk_native_sources.cpp src/azmod/sdk_connection_hooks.cpp src/common/azsources.h src/common/azcachebirth.h src/common/azconnectionepoch.h src/common/azconnectionhooks.h "$ISLAND_OBJECT" "$task_out/source-connection-native" >"$task_out/build-inputs.txt"
 exit 0
fi
: "${MODULE_PATH:?explicit exact module required}" "${MODULE_BUILD:?1or2}" "${RUNTIME_LOADER:?coherent loader required}" "${RUNTIME_LIBRARY_PATH:?coherent group required}"
case $MODULE_BUILD in 1)expected=4b41d53a1b50d14e0d01cb08384546ba5f561a71cb4bd7a2784628095521f15d;;2)expected=9d0fd0efd8caf4053df104ef02ef45dc18f45092552bae98f62a52808a610b5b;;*)exit 2;;esac
actual=$(sha256sum "$MODULE_PATH");[ "${actual%% *}" = "$expected" ]
for late in early late;do
 task_run=$(mktemp -d "$task_out/run-$MODULE_BUILD-$late.XXXXXX")
 status=0
 (
  ulimit -c 0
  if [ "$late" = late ];then export AZSDK_TEST_LATE_PHYSICAL=1;else unset AZSDK_TEST_LATE_PHYSICAL;fi
  LD_PRELOAD= LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" OPENSSL_CONF=/dev/null "$RUNTIME_LOADER" --library-path "$RUNTIME_LIBRARY_PATH" "$task_out/source-connection-native" "$MODULE_PATH" "$MODULE_BUILD"
 ) >"$task_run/log.txt" 2>&1 || status=$?
 cat "$task_run/log.txt"
 if [ "${EXPECT_RED:-0}" = 1 ];then
  [ "$status" != 0 ] && [ "$status" != 77 ]
  grep -q NATIVE_SOURCE_STATE_BASELINE_OK "$task_run/log.txt"
  grep -q 'missing SourceState actual native connection installation bridge' "$task_run/log.txt"
  ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol|DLOPEN_FAILURE' "$task_run/log.txt"
  echo "EXPECTED_RUNTIME_RED build=$MODULE_BUILD/$late: actual shared proof/native API pass; SourceState startup bridge missing"
 else [ "$status" = 0 ];fi
done
