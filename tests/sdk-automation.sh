#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Unit qualification for automation. These fixtures use temporary files/fake SSH,
# never a live target. Real runtime invocations are explicit separate commands.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tests/unit/sdk_runtime_check_test.py
python3 tests/unit/sdk_native_run_test.py
python3 tests/unit/sdk_emulator_smoke_test.py
python3 tests/unit/sdk_emulator_jog_test.py
python3 tests/unit/sdk_emulator_slip_source_test.py
python3 tests/unit/sdk_emulator_sdk_test.py
python3 tests/unit/sdk_emulator_controls_test.py
python3 tests/unit/sdk_emulator_loaded_test.py
python3 tests/unit/sdk_emulator_public_test.py
python3 tests/unit/sdk_emulator_public_runtime_test.py
python3 tests/unit/sdk_emulator_loop_test.py
python3 tests/unit/sdk_emulator_loop_batch_test.py
python3 tests/unit/sdk_emulator_loop_runtime_test.py
python3 tests/unit/sdk_emulator_loop_continuation_test.py
python3 tests/unit/sdk_emulator_cue_runtime_test.py
python3 tests/unit/sdk_panel_transport_test.py
python3 tests/unit/sdk_position_probe_test.py
python3 tests/unit/sdk_emulator_resources_test.py
python3 tests/unit/sdk_emulator_diagnostics_test.py
python3 tests/unit/sdk_native_integration_test.py
python3 tests/unit/sdk_emulator_wave_layer_test.py
