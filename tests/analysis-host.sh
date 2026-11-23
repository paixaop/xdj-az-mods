#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
flags=(-std=gnu11 -Wall -Wextra -Werror -O1 -g -pthread -I"$root/src/azmod")
case "${SAN:-asan}" in
  plain) ;;
  asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all) ;;
  *) echo "unsupported SAN" >&2; exit 2 ;;
esac
"${CC:-cc}" "${flags[@]}" "$root/tests/analysis/test_host.c" \
  "$root/src/azmod/analysis_host.c" "$root/src/azmod/analysis_owner.c" -o "$work/host"
"$work/host"
wrap=()
if [ "$(uname -s)" = Linux ]; then wrap+=(-Wl,--wrap=azconf_set); fi
"${CC:-cc}" "${flags[@]}" "$root/tests/analysis/test_settings.c" \
  "$root/src/azmod/analysis_settings.c" "$root/src/azmod/azconf.c" "${wrap[@]}" -o "$work/settings"
"$work/settings" "$work/settings.conf"
