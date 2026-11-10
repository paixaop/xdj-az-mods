#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
  -pthread -fsanitize=address,undefined -fno-sanitize-recover=all -I"$root/src/azmod" \
  "$root/tests/unit/sdkstatus_gui_test.c" -lm -o "$work/status"
"$work/status"
"$work/status" pad-race
"$work/status" pad-certificate
"$work/status" pad-selection
"$work/status" pad-refresh
"$work/status" pad-inactive
"$work/status" pad-reselect
