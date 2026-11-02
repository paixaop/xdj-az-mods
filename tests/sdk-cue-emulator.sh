#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Host tests only. An owned firmware run is a separate explicit operation.
set -euo pipefail
cd "$(dirname "$0")/.."
sdk_root=${SDK_ROOT:-../sdk}
if [[ $# != 0 ]]; then
    [[ $# == 2 && $1 == --sdk-root ]] || { echo 'usage: sdk-cue-emulator.sh [--sdk-root SDK_CHECKOUT]' >&2; exit 2; }
    sdk_root=$2
fi
sdk_root=$(cd "$sdk_root" && pwd)
python3 tests/unit/sdk_emulator_cue_runtime_test.py
cue_test_dir=$(mktemp -d)
trap 'rm -rf "$cue_test_dir"' EXIT
for cue_san in plain asan; do
    cue_args=()
    [[ $cue_san == plain ]] || cue_args=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror "${cue_args[@]}" -I "$sdk_root/include" \
        "-DCUE_PROBE_SOURCE=\"$sdk_root/examples/cue-command-probe/probe.c\"" \
        tests/unit/sdk_emulator_cue_probe_test.c -lm -o "$cue_test_dir/cue-$cue_san"
    "$cue_test_dir/cue-$cue_san"
done
echo 'PASS cue-command host tests'
