#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) != Linux || $(uname -m) != aarch64 ]]; then
    echo 'SKIP: real entry execution requires ARM64 Linux'; exit 77
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/az-inline-entry.XXXXXX")
trap 'rm -rf "$work"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-gcc}" "${flags[@]}" tests/unit/inline_entry_test.c -o "$work/test"
"$work/test"
