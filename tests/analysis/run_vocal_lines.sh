#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
build=${OUT:-"$root/../temp/vocal-lines/core-tests"}
mkdir -p "$build"
for pass in ${SAN:-plain asan tsan}; do
    case "$pass" in
        plain) flags='-O2' ;;
        asan) flags='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all' ;;
        tsan) flags='-O1 -g -fsanitize=thread' ;;
        *) exit 2 ;;
    esac
    sources="$root/src/azmod"
    tests="$root/tests/analysis"
    ${CC:-clang} -std=c11 -Wall -Wextra -Werror $flags -I"$sources" \
        "$tests/test_vocal_lines_intervals.c" "$sources/vocal_lines_intervals.c" \
        -lm -o "$build/intervals-$pass"
    "$build/intervals-$pass"
    ${CC:-clang} -std=c11 -Wall -Wextra -Werror $flags -I"$sources" \
        "$tests/test_vocal_lines_projection.c" "$sources/vocal_lines_projection.c" \
        -lm -o "$build/projection-$pass"
    "$build/projection-$pass"
    ${CC:-clang} -std=c11 -Wall -Wextra -Werror $flags -I"$sources" \
        "$tests/test_vocal_lines_cache.c" "$sources/vocal_lines_intervals.c" \
        "$sources/vocal_lines_projection.c" "$sources/analysis_owner.c" "$tests/thread_fixture.c" \
        -lm -lpthread -o "$build/cache-$pass"
    "$build/cache-$pass"
done
