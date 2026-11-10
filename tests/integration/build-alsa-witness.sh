#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Compile only. Execute the resulting regression programs in the owned emulator.
set -euo pipefail
out=${1:?usage: build-alsa-witness.sh OUTPUT_DIRECTORY}
src=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
flags=(-std=gnu11 -O2 -g -Wall -Wextra -Werror -I"$src")
"${CC:-cc}" "${flags[@]}" -fPIC -fvisibility=hidden -shared -Wl,-z,now -Wl,-z,defs \
    "$src/alsa-witness.c" -ldl -o "$out/alsa-witness.so"
"${CC:-cc}" "${flags[@]}" -fPIC -shared -Wl,-z,now -Wl,-z,defs \
    "$src/alsa-witness-stub.c" -o "$out/alsa-witness-stub.so"
"${CC:-cc}" "${flags[@]}" -fPIC -shared -Wl,-z,now -Wl,-z,defs -DAZ_ALSA_PARTIAL_STUB \
    "$src/alsa-witness-stub.c" -o "$out/alsa-witness-partial.so"
"${CC:-cc}" "${flags[@]}" "$src/alsa-witness-test.c" -ldl -o "$out/alsa-witness-test"
