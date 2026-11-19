#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-status-install.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -Wno-unused-function -pthread -DAZ_STATUS_NATIVE_PRODUCER -DAZSDK_STATUS_LANE_TEST -Isrc/common -Isrc/azmod)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -c src/azstem/sdk_status_capture.c -o "$task_tmp/lane.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -D_GNU_SOURCE -c tests/unit/sdkstatus_map_alias_bridge.c -o "$task_tmp/map-alias.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -c src/azstem/sdk_native_commands.c -o "$task_tmp/commands.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -c tests/unit/native_unwind.c -o "$task_tmp/unwind.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -DAZSDK_EXIT_TESTING -DAZSDK_MAP_ALIAS_TESTING tests/unit/sdkstatus_install_test.c "$task_tmp/lane.o" "$task_tmp/map-alias.o" "$task_tmp/commands.o" "$task_tmp/unwind.o" -lstdc++ -ldl -o "$task_tmp/install"
"$task_tmp/install" callbacks
"$task_tmp/install" edges
"$task_tmp/install" map-clock
for build in 1 2; do
    for scenario in ${1:-settings terminal motion}; do
        case $scenario in settings) count=27;; terminal) count=7;; motion) count=13;; esac
        "$task_tmp/install" "$scenario" "$build" -1 0
        for ((index=0; index<count; ++index)); do
            "$task_tmp/install" "$scenario" "$build" "$index" 0
            "$task_tmp/install" "$scenario" "$build" "$index" 1
        done
    done
done
echo 'PASS native installers: both profiles, every patch/signature failure, capability admission and partial retry'
