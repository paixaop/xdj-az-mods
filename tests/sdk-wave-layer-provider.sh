#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Actual file provider/public wrappers with disposable SQLite/ANLZ fixtures.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: Linux SQLite file-provider fixture required'; exit 77; }
task_parent=${EVIDENCE_DIR:-${TMPDIR:-/tmp}}
mkdir -p "$task_parent"
task_tmp=$(mktemp -d "$task_parent/az-wave-layer-provider.XXXXXX")
if [ -z "${EVIDENCE_DIR:-}" ];then trap 'rm -rf "$task_tmp"' EXIT;fi
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread
       -Wno-unused-function -Wno-missing-field-initializers -rdynamic)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer);;
*) echo 'SAN must be plain, asan or tsan' >&2;exit 2;;
esac
command=("${HOST_CC:-gcc}" "${flags[@]}" '-DLIB_INC="sdk_lib.c"'
         -Itests/unit -Isrc/azmod tests/unit/sdktrack_test.c -lsqlite3 -ldl -lm -o "$task_tmp/provider")
python3 - "$task_tmp/command.json" "${command[@]}" <<'PY'
from pathlib import Path
import hashlib,json,sys
files=['tests/unit/sdktrack_test.c','tests/sdk-wave-layer-provider.sh','src/azmod/sdk_track.c',
       'src/azmod/sdk_wave_provider.h','src/azmod/sdk_waveform.h','src/azmod/sdk_data_engine.h',
       'src/azmod/azmod-mod.h']
Path(sys.argv[1]).write_text(json.dumps({'argv':sys.argv[2:],'sha256':{f:hashlib.sha256(Path(f).read_bytes()).hexdigest()for f in files},'scope':'real file-provider functions; disposable SQLite fixtures, no firmware/source-load/native controls'},indent=2)+'\n')
PY
if ! "${command[@]}" >"$task_tmp/compile.log" 2>&1;then
    cat "$task_tmp/compile.log";echo 'FAIL: compilation cannot qualify runtime RED' >&2;exit 1
fi
status=0
(ulimit -c 0;"$task_tmp/provider") >"$task_tmp/runtime.log" 2>&1 || status=$?
printf '%s\n' "$status" >"$task_tmp/runtime.status"
cat "$task_tmp/runtime.log"
if [ "${EXPECT_RED:-0}" = 1 ];then
    [ "$status" = 134 ] &&
        grep -q '^LAYERED_PROVIDER_BASELINE_OK existing_file_windows=1 old_encodings=1' "$task_tmp/runtime.log" &&
        grep -q '^FAIL layered-provider: missing derived layered waveform request (owner=0 rc=-2)' "$task_tmp/runtime.log" &&
        ! grep -Eq '^FAIL  |ERROR: (AddressSanitizer|ThreadSanitizer)|FATAL: ThreadSanitizer|runtime error:|Segmentation fault|Illegal instruction|Bus error' "$task_tmp/runtime.log" || {
            echo 'FAIL: required missing-format assertion after old-provider baseline was not reached' >&2;exit 1;
        }
    echo 'EXPECTED_RUNTIME_RED: actual public waveform request rejects numeric8 after old formats pass'
else
    [ "$status" = 0 ] && grep -q '^sdk disk provider: exact sources,' "$task_tmp/runtime.log" || exit 1
fi
if [ -n "${EVIDENCE_DIR:-}" ];then echo "EVIDENCE_RETAINED=$task_tmp";fi
