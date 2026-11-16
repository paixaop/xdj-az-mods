#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_output=$(mktemp -d "${TMPDIR:-/tmp}/sdk-beatport-test.XXXXXX")
trap 'rm -rf "$test_output"' EXIT HUP INT TERM
${CC:-clang} -std=c11 -Wall -Wextra -Werror -O2 -pthread \
    "$test_dir/sdk_beatport_test.c" -o "$test_output/plain"
"$test_output/plain"
${CC:-clang} -std=c11 -Wall -Wextra -Werror -O1 -g -pthread \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    "$test_dir/sdk_beatport_test.c" -o "$test_output/asan"
"$test_output/asan"
