#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Small host-only startup-installer fixture. No native patch/install or target calls.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-hooks.XXXXXX")
trap 'rm -rf "$work"' EXIT
"${CC:-clang}" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-function \
    -Isrc/azmod tests/unit/sdkcachehooks_test.c -o "$work/hooks"
"$work/hooks"
"${CC:-clang}" -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-function \
    -DSDK_CACHE_HOOKS_MISSING -Isrc/azmod tests/unit/sdkcachehooks_test.c -o "$work/missing"
python3 - "$work/missing" <<'PY'
import subprocess, sys
result = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=5)
assert result.returncode != 0 and 'CACHE_NATIVE_BASELINE_OK build=1 installer=0' in result.stdout
assert 'missing complete startup batch installation' in result.stderr, result.stderr
print('CAUGHT missing startup batch installer after typed native baseline')
PY
