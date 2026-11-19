#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-file-install.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -Wno-unused-function -pthread -D_GNU_SOURCE -DAZ_STATUS_NATIVE_PRODUCER -DAZ_STATUS_MEMORY_PRODUCER -Isrc/common -Isrc/azmod)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" tests/unit/sdkstatus_file_install_test.c tests/unit/sdkstatus_memory_return.S src/azstem/sdk_status_memory_entry.S -ldl -lm -o "$task_tmp/test"
for build in 1 2; do
    for family in cues maps; do
        count=13
        [[ $family != maps ]] || count=10
        "$task_tmp/test" "$build" "$family" -1 0
        for ((index=0; index<count; ++index)); do
            for mode in 0 1 2; do "$task_tmp/test" "$build" "$family" "$index" "$mode"; done
        done
    done
done
echo 'PASS FILE installers: both profiles, every signature/patch/missing-original failure, callback admission, retained partial retry and mutation idempotence'
