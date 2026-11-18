#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-cue-store.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-DAZSDK_CUE_STORE_TESTING -std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkcue_store_test.c src/azstem/sdk_cue_store.c tests/unit/sdkcue_store_return.S src/common/aznative_guard.S -lstdc++ -lm -o "$task_tmp/test"
"$task_tmp/test"
