#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-status-lti.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
python3 - "$PWD/src/azstem/deck_track_map.c" "$task_tmp" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]);s=source.read_text();out=Path(sys.argv[2])
a=s.index('#define LTI_STR ');b=s.index('static int g_lti_probe =',a)
body=s[a:b]
(out/'real.inc').write_text('#line %d "%s"\n%s' % (s.count('\n',0,a)+1,source,body))
for name,old,new in [('lifecycle','capture->fence == azn_lifecycle_fence(d)','1'),
                     ('retry','lti_status_changed(sn, &r, seed) && azn_offer(&r)','lti_status_changed(sn, &r, seed) && (azn_offer(&r), 1)'),
                     ('generation','r->unit == atomic_load_explicit(&g_ls_gen[d], memory_order_acquire)','1')]:
    assert body.count(old)==1,name
    (out/(name+'.inc')).write_text(body.replace(old,new))
PY
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -Isrc/common)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
for name in real lifecycle retry generation; do
    "${HOST_CC:-cc}" "${flags[@]}" -DLTI_INC="\"$task_tmp/$name.inc\"" tests/unit/sdkstatus_lti_test.c -o "$task_tmp/$name"
    if [ "$name" = real ]; then "$task_tmp/$name";
    elif (ulimit -c 0; "$task_tmp/$name" >"$task_tmp/$name.log" 2>&1) 2>/dev/null; then
        echo "FAIL: LTI $name regression was not caught" >&2; exit 1
    else
        grep -Eq 'Assertion.*failed|Assertion failed' "$task_tmp/$name.log" || { cat "$task_tmp/$name.log"; exit 1; }
        echo "CONTROL caught: LTI $name"
    fi
done
