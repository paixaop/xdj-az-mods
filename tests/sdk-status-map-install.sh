#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-map-install.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -O1 -g -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -Wno-unused-function -pthread -D_GNU_SOURCE -DAZ_STATUS_NATIVE_PRODUCER -Isrc/common -Isrc/azmod)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkstatus_map_install_test.c tests/unit/sdkstatus_map_return.c tests/unit/native_unwind.c -lstdc++ -ldl -lm -o "$task_tmp/test"
for build in 1 2; do
    "$task_tmp/test" "$build" writers -1 0
    for index in 0 1 2 3; do
        for mode in 0 1 2; do "$task_tmp/test" "$build" writers "$index" "$mode"; done
    done
done
for index in 0 1 2 3 4 5; do "$task_tmp/test" 2 analysis "$index" 0; done
echo 'PASS map installers: both profiles, every writer patch/signature/original failure, six analysis admission signatures, retry and idempotence'
