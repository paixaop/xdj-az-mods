#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-cue-lifecycle.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -pthread -O1 -g -Wall -Wextra -Werror)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" -c tests/unit/sdkcuelifecycle_test.c -o "$task_tmp/test.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -fexceptions -fasynchronous-unwind-tables -c tests/unit/native_unwind.c -o "$task_tmp/guard.o"
"${HOST_CC:-cc}" "$task_tmp/test.o" "$task_tmp/guard.o" "${flags[@]:1}" -lstdc++ -ldl -o "$task_tmp/test"
"$task_tmp/test"
