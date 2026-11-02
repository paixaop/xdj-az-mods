#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Tests only: missing-header fallback cannot copy or inspect the carrier.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: row schema fixture requires Linux GNU ABI'; exit 77; }
case "$(uname -m)" in
aarch64|arm64) ;;
*) [ "${ALLOW_HOST_OWNER_TEST:-0}" = 1 ] || { echo 'SKIP: ARM ABI baseline requires AArch64; host opt-in covers GNU schema/ownership only'; exit 77; } ;;
esac
task_parent=${EVIDENCE_DIR:-${TMPDIR:-/tmp}}
mkdir -p "$task_parent"
task_tmp=$(mktemp -d "$task_parent/az-sdk-cache-row.XXXXXX")
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$task_tmp"' EXIT; fi
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
if [ -n "${ROW_HEADER:-}" ]; then flags+=("-DAZCACHE_ROW_HEADER=\"${ROW_HEADER}\""); fi
command=("${HOST_CXX:-g++}" "${flags[@]}" tests/unit/sdkcacherow_test.cpp -o "$task_tmp/cache-row")
python3 - "$task_tmp/command.json" "${command[@]}" <<'PY'
from pathlib import Path
import hashlib,json,sys
paths=['tests/unit/sdkcacherow_test.cpp','tests/sdk-cache-row.sh']
p=Path('src/common/azcacherow.h')
if p.exists(): paths.append(str(p))
Path(sys.argv[1]).write_text(json.dumps({'argv':sys.argv[2:],'sha256':{x:hashlib.sha256(Path(x).read_bytes()).hexdigest()for x in paths},'production_header_present':p.exists()},indent=2)+'\n')
PY
if ! "${command[@]}" >"$task_tmp/compile.log" 2>&1; then
    cat "$task_tmp/compile.log"; echo 'FAIL: compilation cannot qualify runtime RED' >&2; exit 1
fi
failures=0
for profile in 1 2; do
    status=0
    (ulimit -c 0; "$task_tmp/cache-row" "$profile") >"$task_tmp/profile-$profile.log" 2>&1 || status=$?
    printf '%s\n' "$status" >"$task_tmp/profile-$profile.status"
    cat "$task_tmp/profile-$profile.log"
    if [ "${EXPECT_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/^FAIL / { p; q; }' "$task_tmp/profile-$profile.log")
        if [ "$status" != 134 ] ||
           ! grep -q "NATIVE_ROW_FORWARD_OK build=$profile context=0 carrier24=24 slot8=8 row98=152 forwarded=1 cleaned=3" "$task_tmp/profile-$profile.log" ||
           [[ "$first_failure" != *'missing owned tag0x23 scalar copy'* ]] ||
           grep -Eq 'ERROR: (AddressSanitizer|ThreadSanitizer)|FATAL: ThreadSanitizer|runtime error:|undefined symbol|Segmentation fault|Illegal instruction|Bus error' "$task_tmp/profile-$profile.log"; then
            echo "FAIL: profile $profile did not reach intended missing scalar runtime assertion" >&2
            failures=$((failures + 1))
        else echo "EXPECTED_RUNTIME_RED profile=$profile: genuine native-shaped rows forward/clean; owned scalar copy missing"; fi
    elif [ "$status" != 0 ] || ! grep -q "PASS cache-row build=$profile" "$task_tmp/profile-$profile.log"; then
        failures=$((failures + 1))
    fi
done
if [ -n "${EVIDENCE_DIR:-}" ]; then echo "EVIDENCE_RETAINED=$task_tmp"; fi
[ "$failures" = 0 ]
