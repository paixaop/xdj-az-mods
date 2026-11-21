#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -Isrc/azmod)
case ${SAN:-plain} in
    plain) ;;
    asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
    tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
    *) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/vocal_workflow_test.c src/azmod/analysis_events.c -o "$dir/test"
"$dir/test"
