#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-community-screen.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -O1 -g -w -fwrapv -Isrc/azmod -Itests/unit)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
for fixture in ${COMMUNITY_FIXTURES:-cmcat cmmod cmupdtab cmnotloaded cmtouch screenrow cmflow}; do
    mapfile -t renamed < <(python3 - "tests/unit/${fixture}_test.c" <<'PY'
import re, sys
from pathlib import Path
source = Path(sys.argv[1]).read_text()
for name in re.findall(r'^(?:int|void|bool|long|double|unsigned)\s+(az\w+)\s*\(', source, re.M):
    print('-D%s=lanetest_%s' % (name, name))
PY
    )
    "${HOST_CC:-cc}" "${flags[@]}" -Dmain=lanetest_main "${renamed[@]}" -c src/azmod/test/lanetest.c -o "$task_tmp/stubs.o"
    "${HOST_CC:-cc}" "${flags[@]}" -DMENU_C="\"$(pwd)/src/azmod/menu.c\"" \
        -DCM_CATALOG="\"$task_tmp/$fixture-catalog\"" -DCM_STATUS="\"$task_tmp/$fixture-status\"" -DCM_REQ="\"$task_tmp/$fixture-req\"" \
        "tests/unit/${fixture}_test.c" "$task_tmp/stubs.o" -o "$task_tmp/$fixture"
    "$task_tmp/$fixture"
done
