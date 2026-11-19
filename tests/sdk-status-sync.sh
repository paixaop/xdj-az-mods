#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-status-sync.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -Wno-unused-function -pthread -D_GNU_SOURCE -DAZ_STATUS_NATIVE_PRODUCER -DAZSDK_STATUS_LANE_TEST -Isrc/common -Isrc/azmod)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -DAZSDK_EXIT_TESTING tests/unit/sdkstatus_sync_test.c src/azstem/sdk_status_capture.c src/azstem/sdk_native_commands.c tests/unit/native_unwind.c -lstdc++ -ldl -o "$task_tmp/sync"
for build in 1 2; do "$task_tmp/sync" "$build"; done
