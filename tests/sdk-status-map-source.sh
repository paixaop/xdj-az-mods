#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -Isrc/azmod)
libs=()
if [[ $(uname) == Linux ]]; then flags+=(-D_GNU_SOURCE); libs+=(-ldl); fi
case ${SAN:-plain} in plain) ;; asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all);; *) exit 2;; esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkstatus_map_source_test.c "${libs[@]}" -o "$work/test"
"$work/test"
