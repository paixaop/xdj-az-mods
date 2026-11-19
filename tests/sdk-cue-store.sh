#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-cue-store.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror)
if [[ ${CUE_DIAGNOSTIC:-0} == 1 ]]; then flags+=(-DAZSDK_CUE_DIAGNOSTIC); fi
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkcue_store_test.c src/azstem/sdk_cue_store.c tests/unit/sdkcue_store_return.c tests/unit/native_unwind.c -lstdc++ -ldl -lm -o "$task_tmp/test"
"$task_tmp/test"
