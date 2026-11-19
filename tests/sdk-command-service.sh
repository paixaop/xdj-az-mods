#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
"${HOST_CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/azmod \
    tests/unit/sdk_command_service_test.c src/azmod/sdk_command_service.c src/azmod/sdk_command_engine.c -lm -o "$work/commands"
"$work/commands"
