#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Unit gates for the firmware loop-command experiment. No device/VM is started.
set -euo pipefail
cd "$(dirname "$0")/.."
sdk_root=${SDK_ROOT:-../sdk}
if [[ $# != 0 ]]; then
    [[ $# == 2 && $1 == --sdk-root ]] || { echo 'usage: sdk-loop-emulator.sh [--sdk-root SDK_CHECKOUT]' >&2; exit 2; }
    sdk_root=$2
fi
test -f "$sdk_root/tests/test_loop_command_probe.py"
bash tests/sdk-exit.sh
python3 -m unittest discover -s tests/unit -p sdk_emulator_loop_test.py
python3 -m unittest discover -s tests/unit -p sdk_emulator_loop_batch_test.py
python3 -m unittest discover -s tests/unit -p sdk_emulator_loop_runtime_test.py
python3 -m unittest discover -s tests/unit -p sdk_emulator_loop_continuation_test.py
python3 -m unittest discover -s tests/unit -p sdk_emulator_public_runtime_test.py
python3 -m unittest discover -s "$sdk_root/tests" -p test_loop_command_probe.py
bash tests/sdk-cue-emulator.sh --sdk-root "$sdk_root"
echo 'PASS loop-command unit gates; firmware execution is a separate owned-emulator run'
