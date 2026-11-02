#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Instrument only a temporary copy of the real Source.cpp. EXPECT_RED requires
# a concrete duplicate source epoch after genuine native forwarding/scheduling.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: source reuse requires Linux GNU ABI'; exit 77; }
case "$(uname -m)" in
aarch64|arm64) ;;
*) [ "${ALLOW_HOST_OWNER_TEST:-0}" = 1 ] || { echo 'SKIP: exact X8 baseline requires AArch64; host opt-in checks GNU ownership only'; exit 77; } ;;
esac
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-source-reuse.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
result_root=${SDK_SOURCE_REUSE_RESULTS:-$task_tmp/results}
mkdir -p "$task_tmp/src/azmod" "$task_tmp/src/common" "$result_root"
cp src/common/azsources.h src/common/azcachebirth.h src/common/azconnectionhooks.h src/common/azconnectionepoch.h "$task_tmp/src/common/"
python3 - src/azmod/sdk_native_sources.cpp "$task_tmp/src/azmod/sdk_native_sources.cpp" "$result_root/source-seams.json" <<'PY'
from pathlib import Path
import hashlib,json,sys
source=Path(sys.argv[1]); original=source.read_text()
start='    bool was_observed = false;\n    for (auto &e : source_state().entries) {'
end='    if (!observed_generation && was_observed) {'
if original.count(start)!=1 or original.count(end)!=1:
    raise SystemExit('FAIL: exact real source matching-key loop was not found once; no runtime RED possible')
instrumented=original.replace(start,'    bool was_observed = false;\n    source_reuse_fixture::lookup_boundary(0);\n    for (auto &e : source_state().entries) {',1)
instrumented=instrumented.replace(end,'    source_reuse_fixture::lookup_boundary(1);\n'+end,1)
Path(sys.argv[2]).write_text(instrumented)
Path(sys.argv[3]).write_text(json.dumps({'original':str(source),'original_sha256':hashlib.sha256(original.encode()).hexdigest(),'instrumented_sha256':hashlib.sha256(instrumented.encode()).hexdigest(),'seams':2,'scope':'callbacks schedule a genuine EpochState close only; actual lookup/key/receipts untouched'},indent=2)+'\n')
PY
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -rdynamic -DAZSDK_CACHE_BIRTH_TESTING)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
command=("${HOST_CXX:-g++}" "${flags[@]}" "-DSOURCES_INC=\"$task_tmp/src/azmod/sdk_native_sources.cpp\"" tests/unit/sdksourcereuse_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_tmp/source-reuse")
python3 - "$result_root/compile-command.json" "${command[@]}" <<'PY'
from pathlib import Path
import json,sys
Path(sys.argv[1]).write_text(json.dumps({'argv':sys.argv[2:]},indent=2)+'\n')
PY
if ! "${command[@]}" >"$result_root/compile.log" 2>&1; then
    cat "$result_root/compile.log"; echo 'FAIL: compilation cannot qualify as runtime RED' >&2; exit 1
fi
failures=0
for profile in 1 2; do
    status=0
    (ulimit -c 0; "$task_tmp/source-reuse" "$profile") >"$result_root/profile-$profile.log" 2>&1 || status=$?
    printf '%s\n' "$status" >"$result_root/profile-$profile.status"
    cat "$result_root/profile-$profile.log"
    if [ "${EXPECT_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/^FAIL / { p; q; }' "$result_root/profile-$profile.log")
        if [ "$status" != 134 ] ||
           ! grep -q "NATIVE_FORWARD_OK build=$profile owner16=16 rows24=24" "$result_root/profile-$profile.log" ||
           ! grep -q "SOURCE_REUSE_LOOKUP_OK build=$profile native_close=1 result=5" "$result_root/profile-$profile.log" ||
           [[ "$first_failure" != *'lookup-only unrelated close must reuse the exact existing source epoch'* ]] ||
           grep -Eq 'ERROR: (AddressSanitizer|ThreadSanitizer)|FATAL: ThreadSanitizer|runtime error:|undefined symbol|Segmentation fault|Illegal instruction|Bus error' "$result_root/profile-$profile.log"; then
            echo "FAIL: profile $profile did not reach the intended concrete duplicate-epoch runtime assertion" >&2
            failures=$((failures + 1))
        else
            echo "EXPECTED_RUNTIME_RED profile=$profile: lookup-only native close duplicated an unchanged source key"
        fi
    elif [ "$status" != 0 ] || ! grep -q "PASS source-reuse build=$profile" "$result_root/profile-$profile.log"; then
        failures=$((failures + 1))
    fi
done
[ "$failures" = 0 ]
