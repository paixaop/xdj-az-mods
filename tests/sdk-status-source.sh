#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -Isrc/azmod)
case ${SAN:-plain} in plain) ;; asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all);; tsan) flags+=(-fsanitize=thread);; *) exit 2;; esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkstatus_source_test.c -o "$work/source"
"$work/source"

"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkstatus_source_kind_test.c -o "$work/source-kind"
"$work/source-kind"
