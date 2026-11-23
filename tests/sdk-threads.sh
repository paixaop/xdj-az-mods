#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-threads.XXXXXX")
trap 'rm -rf "$work"' EXIT
awk '/^static _Atomic int g_sd_threads/ {copy=1} copy {print}' src/azmod/sdk_ui.c > "$work/real.inc"
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -Isrc/azmod)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
tsan) flags+=(-fsanitize=thread);;
*) exit 2;;
esac
"${HOST_CC:-cc}" "${flags[@]}" "-DTHREAD_INC=\"$work/real.inc\"" tests/unit/sdk_thread_start_test.c src/azmod/analysis_owner.c -lm -o "$work/test"
"$work/test"
