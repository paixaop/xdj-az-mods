#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-data-lifecycle.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work/lifecycle.inc" <<'PY'
from pathlib import Path
import sys
track=Path('src/azmod/sdk_track.c').read_text()
needle='static void sdk_data_retire_owner('
if needle in track:
    start=track.index(needle)
    retire=track[start:track.index('\n}',start)+2]
else:
    retire='static void sdk_data_retire_owner(int owner) { (void)owner; }'
source=Path('src/azmod/community.c').read_text()
start=source.index('static void cm_load_one(')
begin=source.index('    c->m = m;',start)
end=source.index('\n}',begin)
Path(sys.argv[1]).write_text(retire+'\nstatic void run_actual_init(cm_mod *c, const az_mod_v1 *m) {\n'+source[begin:end]+'\n}\n')
PY
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/azmod)
case ${SAN:-plain} in plain|'') ;; asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;tsan)flags+=(-fsanitize=thread);;*)exit 2;;esac
"${HOST_CC:-cc}" "${flags[@]}" "-DSDK_DATA_LIFECYCLE_INC=\"$work/lifecycle.inc\"" tests/unit/sdkdata_lifecycle_test.c -lm -o "$work/test"
status=0
python3 - "$work/test" >"$work/run.log" 2>&1 <<'PY' || status=$?
import resource, subprocess, sys
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
try:
    result=subprocess.run([sys.argv[1]],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                          universal_newlines=True,timeout=10)
except subprocess.TimeoutExpired:
    print('FAIL data lifecycle: bounded retirement test timed out')
    sys.exit(124)
print(result.stdout,end='')
sys.exit(result.returncode if result.returncode >= 0 else 128-result.returncode)
PY
cat "$work/run.log"
if [ "${EXPECT_RED:-0}" = 1 ]; then
    [ "$status" != 0 ] && [ "$status" != 77 ]
    grep -q FAILED_INIT_DATA_BASELINE_OK "$work/run.log"
    grep -q 'failed initialization must not strand unacquired data ticket quota' "$work/run.log"
    ! grep -Eq 'Segmentation fault|runtime error:|ERROR: AddressSanitizer' "$work/run.log"
    echo 'EXPECTED_RUNTIME_RED: failed native SDK init retains the global data ticket quota'
else exit "$status";fi
