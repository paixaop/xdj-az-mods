#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Small host-only actual startup caller fixture. No native patching/runtime.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-startup.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
build() {
    local output=$1
    shift
    "${HOST_CC:-clang}" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-function "$@" \
        -Isrc/azmod tests/unit/sdkcachestartup_test.c src/azmod/sdk_cache_entry.c -o "$output"
}
build "$task_tmp/startup"
ulimit -c 0
missing_caller() {
    python3 - "$1" <<'PY'
import subprocess, sys
r = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
assert r.returncode != 0 and 'CACHE_STARTUP_NATIVE_BASELINE_OK build=1 caller=0' in r.stdout, (r.stdout, r.stderr)
assert 'missing actual full cache startup caller' in r.stderr, r.stderr
print('EXPECTED_RUNTIME_RED: typed native original baseline passed; full startup caller absent')
PY
}
if [ "${EXPECT_RED:-0}" = 1 ]; then
    missing_caller "$task_tmp/startup"
else
    "$task_tmp/startup"
    build "$task_tmp/missing" -DSDK_CACHE_STARTUP_MISSING
    missing_caller "$task_tmp/missing"
fi
