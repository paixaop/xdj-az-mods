#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$root/src/azmod/mod_menu_overlay.c" "$work/status-wait.inc" <<'PY'
import pathlib, sys
text = pathlib.Path(sys.argv[1]).read_text()
start = text.index('static void ov_wait(')
end = text.index("/* The Mod Menu's thread:", start)
pathlib.Path(sys.argv[2]).write_text(text[start:end])
PY
set --
if [ "${SAN:-}" = asan ]; then set -- -fsanitize=address,undefined -fno-sanitize-recover=all; fi
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -O1 -g "$@" -I"$work" \
    "$root/tests/unit/sdk_status_wait_test.c" -o "$work/wait"
"$work/wait"
