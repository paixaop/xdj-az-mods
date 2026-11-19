#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-playlist-message.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import sys
out = Path(sys.argv[1])
source = Path('src/azmod/playlist_editor.c').resolve()
text = source.read_text()
header = Path('src/azmod/azmod_internal.h').resolve()
h = header.read_text()
def fragment(path, text, start, end):
    return '#line %d "%s"\n' % (text.count('\n', 0, start) + 1, path) + text[start:end] + '\n'
def section(start, end):
    a = text.index(start)
    b = text.index(end, a)
    return fragment(source, text, a, b)
a = h.index('enum {\n    MV_OP_MOVE,')
b = h.index('\n};', a) + 3
declarations = fragment(header, h, a, b)
declarations += section('enum {\n    MV_IDLE,', 'static _Atomic int g_mv_kick;')
a = text.index('static struct {\n    int op;\n    unsigned pl, cid, from, to, media;')
b = text.index('} g_mv;', a) + len('} g_mv;')
declarations += fragment(source, text, a, b)
functions = section('static int mv_op_writes(', 'static void mv_pdb_sync(')
functions += section('struct pst_scope {', '/* onMessage returned normally:')
functions += section('static void mv_log_result(', 'static _Atomic unsigned long g_tracklist_widget;')
(out / 'playlist_declarations.inc').write_text(declarations)
(out / 'playlist_functions.inc').write_text(functions)
PY
flags=(-std=gnu11 -O1 -g -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -Wno-unused-function -Isrc/common -Itests/unit -I"$task_tmp")
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkplaylistmessage_test.c tests/unit/native_unwind.c -lstdc++ -ldl -o "$task_tmp/test"
"$task_tmp/test"
