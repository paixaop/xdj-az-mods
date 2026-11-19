#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-stem-startup.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
python3 tests/unit/azstem_startup_extract.py "$task_tmp/startup.inc"
"${HOST_CC:-cc}" "${flags[@]}" -DSTARTUP_INC="\"$task_tmp/startup.inc\"" tests/unit/azstemstartup_test.c -lm -o "$task_tmp/startup"
"$task_tmp/startup"
