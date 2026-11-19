#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-native-guards.XXXXXX")
trap 'rm -rf "$work"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" -DAZSDK_EXIT_TESTING tests/unit/sdkexit_guards_test.c src/azstem/sdk_native_commands.c tests/unit/native_unwind.c -lstdc++ -ldl -lm -o "$work/exit"
"$work/exit"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkloopranges_test.c src/azstem/sdk_native_commands.c tests/unit/native_unwind.c -lstdc++ -ldl -lm -o "$work/loop"
"$work/loop"
