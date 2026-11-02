#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Deterministic actual-State race at the native cache binding scan; no mirrored registry.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || exit 77
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cache-reclamation.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -DAZSDK_CACHE_BIRTH_TESTING -DAZSDK_CACHE_RECLAIM_TESTING)
case ${SAN:-plain} in plain|'') ;;asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;tsan)flags+=(-fsanitize=thread -fno-omit-frame-pointer);;*)exit 2;;esac
python3 - src/azmod/sdk_native_sources.cpp "$task_tmp/source.cpp" <<'PY'
from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text()
before='        CacheBinding *slot = nullptr;'
after='        if (!slot || s.next_cache_birth == UINT64_MAX) return AZS_LIMIT;'
assert s.count(before)==s.count(after)==1
s=s.replace(before,before+'\n        if (cache_reclaim_test_boundary) cache_reclaim_test_boundary(0);')
s=s.replace(after,'        if (cache_reclaim_test_boundary) cache_reclaim_test_boundary(1);\n'+after)
Path(sys.argv[2]).write_text(s.replace('"../common/','"'))
PY
"${HOST_CXX:-g++}" "${flags[@]}" -DSOURCES_INC="\"$task_tmp/source.cpp\"" tests/unit/sdkcachebirth_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_tmp/test"
for profile in 1 2;do
 status=0
 (ulimit -c 0;"$task_tmp/test" "$profile") >"$task_tmp/$profile.log" 2>&1 || status=$?
 cat "$task_tmp/$profile.log"
 if [ "${EXPECT_RED:-0}" = 1 ];then
  [ "$status" != 0 ] && [ "$status" != 77 ]
  grep -q "NATIVE_FORWARD_OK build=$profile" "$task_tmp/$profile.log"
  grep -q 'receipt-only refusal cannot reclaim another live cache binding' "$task_tmp/$profile.log"
  ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol|ERROR:.*Sanitizer' "$task_tmp/$profile.log"
  echo "EXPECTED_RUNTIME_RED profile=$profile: real unrelated operation evicts retained native cache binding"
 else [ "$status" = 0 ];fi
done
