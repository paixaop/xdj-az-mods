#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/azmod)
case ${SAN:-plain} in
    plain) ;;
    asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
    tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
    *) exit 2 ;;
esac
"${CC:-cc}" "${flags[@]}" tests/analysis/test_events.c src/azmod/analysis_events.c -o "$dir/test"
"$dir/test"
