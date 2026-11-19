#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -pthread -Wall -Wextra -Werror -DAZ_STATUS_MAPS_PRODUCER -DAZ_STATUS_NATIVE_PRODUCER)
if [[ ${SAN:-plain} = asan ]]; then flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer); fi
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkstatus_maps_test.c tests/unit/sdkstatus_map_return.c tests/unit/native_unwind.c -lstdc++ -ldl -lm -o "$dir/maps"
"$dir/maps" 1
"$dir/maps" 2
"$dir/maps" 1 browse-retain
"$dir/maps" 2 browse-retain
"$dir/maps" 2 analysis
