#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-command-target.XXXXXX")
trap 'rm -rf "$work"' EXIT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Isrc/azmod)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" -c src/azmod/sdk_command_target.c -o "$work/target.o"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdk_command_target_test.c "$work/target.o" -lm -o "$work/test"
"$work/test"
