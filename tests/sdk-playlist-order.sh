#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-playlist-order.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkplaylistorder_test.c -lsqlite3 -ldl -o "$task_tmp/test"
"$task_tmp/test" "$task_tmp/library.db"
