#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || exit 77
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-message-hook.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common)
case ${SAN:-plain} in plain|'') ;;asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;tsan)flags+=(-fsanitize=thread -fno-omit-frame-pointer);;*)exit 2;;esac
python3 - src/azmod/playlist_editor.c tests/unit/sdkcachemessagehook_fixture.cpp "$task_tmp/test.cpp" <<'PY'
from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text();a=s.index('static void mlserver_onmsg_hook(');b=s.index('\n}',a)+2;body=s[a:b]
marker='    /* Original message forwarding boundary. */'
a=body.index(marker) if marker in body else body.index('    g_orig_mlserver_onmsg(server, msg, param);')
tail=body[a:]
fixture=Path(sys.argv[2]).read_text();assert fixture.count('/* ACTUAL_NATIVE_HOOK_TAIL */')==1
Path(sys.argv[3]).write_text(fixture.replace('/* ACTUAL_NATIVE_HOOK_TAIL */',tail))
PY
"${HOST_CXX:-g++}" "${flags[@]}" "$task_tmp/test.cpp" src/azmod/sdk_cache_message.cpp -o "$task_tmp/test"
status=0
(ulimit -c 0;"$task_tmp/test") >"$task_tmp/run.log" 2>&1 || status=$?
cat "$task_tmp/run.log"
if [ "${EXPECT_RED:-0}" = 1 ];then
 [ "$status" != 0 ] && [ "$status" != 77 ]
 grep -q NATIVE_HOOK_FORWARD_OK "$task_tmp/run.log"
 grep -q 'native hook must enter copied message bridge before original' "$task_tmp/run.log"
 ! grep -Eq 'Segmentation fault|runtime error:|undefined symbol|ERROR:.*Sanitizer' "$task_tmp/run.log"
 echo 'EXPECTED_RUNTIME_RED: actual native hook tail bypasses the owned forwarding bridge'
else exit "$status";fi
