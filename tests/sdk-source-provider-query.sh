#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Isrc/azmod)
case ${SAN:-plain} in
    plain) ;;
    asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all) ;;
    *) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/source_provider_query_test.c src/azmod/source_provider_query.c \
    src/azmod/source_provider_catalog.c -lsqlite3 -o "$work/test"
"$work/test" "$work"
