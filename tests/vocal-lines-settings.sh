#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
flags=(-std=gnu11 -O1 -g -I"$root/src/azmod" -I"$root/tests/unit")
case "${SAN:-plain}" in
    plain|'') ;;
    asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all) ;;
    tsan) flags+=(-fsanitize=thread) ;;
    *) echo 'unsupported SAN' >&2; exit 2 ;;
esac
"${CC:-cc}" "${flags[@]}" -Dmain=lanetest_main \
    -Dazmod_ttf_menu=lanetest_ttf_menu -Dazmod_ttf_menu2=lanetest_ttf_menu2 \
    -c "$root/src/azmod/test/lanetest.c" -o "$work/stubs.o"
"${CC:-cc}" "${flags[@]}" -DMENU_C="\"$root/src/azmod/menu.c\"" \
    -DCM_CATALOG="\"$work/no-catalog\"" "$root/tests/unit/rows_test.c" "$work/stubs.o" \
    -lm -o "$work/rows"
"$work/rows"
"${CC:-cc}" "${flags[@]}" -DAZMOD_RELEASE -DMENU_C="\"$root/src/azmod/menu.c\"" \
    -DCM_CATALOG="\"$work/no-catalog\"" "$root/tests/unit/keys_test.c" "$work/stubs.o" \
    -lm -o "$work/keys"
"$work/keys" "$root/docs/MOD-MENU-KEYS.md"
