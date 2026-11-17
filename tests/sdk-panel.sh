#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-panel.XXXXXX")
trap 'rm -rf "$work"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Isrc -pthread)
case ${SAN:-plain} in
    plain|'') ;;
    asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
    tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer);;
    *) exit 2;;
esac
"${HOST_CC:-gcc}" "${flags[@]}" tests/unit/sdkpanel_test.c -o "$work/test"
"$work/test"
