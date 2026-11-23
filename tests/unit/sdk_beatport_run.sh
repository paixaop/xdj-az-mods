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

for mode in plain asan; do
    san=""
    [ "$mode" = plain ] || san="-fsanitize=address,undefined -fno-sanitize-recover=all"
    ${CC:-clang} -std=gnu11 -Wall -Wextra -Werror -O1 -g -pthread $san \
        -I"$test_dir/../../src/azmod" "$test_dir/beatport_module_test.c" \
        "$test_dir/../../src/azmod/beatport_service.c" -o "$test_output/module-$mode"
    "$test_output/module-$mode" "$test_output/login-$mode.dat"
done
