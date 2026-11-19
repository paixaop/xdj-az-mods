#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-memory-ownership.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -Wno-unused-function -pthread -D_GNU_SOURCE -DAZ_STATUS_NATIVE_PRODUCER -DAZ_STATUS_MEMORY_PRODUCER -Isrc/common -Isrc/azmod)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" tests/unit/sdkstatus_memory_ownership_test.c tests/unit/sdkstatus_memory_return.S tests/unit/sdkstatus_memory_silent_return.S src/azstem/sdk_status_memory_entry.S -ldl -lm -o "$task_tmp/test"
"$task_tmp/test" 1
"$task_tmp/test" 2
