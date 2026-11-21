#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread)
if [[ ${SAN:-plain} == asan ]]; then
    flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
fi
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/vocal_lines_draw_test.c src/azmod/vocal_lines_projection.c -lm -o "$dir/draw"
"$dir/draw"
