#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CC:-cc}" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-function \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$root/src/azmod" "$root/tests/unit/sdk_data_callbacks_test.c" \
    -pthread -lm -o "$out/callbacks"
"$out/callbacks"
