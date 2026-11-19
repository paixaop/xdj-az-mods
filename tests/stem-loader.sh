#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-stem-loader.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
python3 - "$task_tmp/loader.inc" <<'PY'
from pathlib import Path
import re,sys
path=Path('src/azstem/stem_playback.c').resolve();s=path.read_text()
def mapped(a,b): return '#line {} "{}"\n{}\n'.format(s.count('\n',0,a)+1,path,s[a:b])
a=s.index('typedef struct {',s.index("/* Decode a deck's newly-matched song"));b=s.index('static void *loader_thread(',a)
out=mapped(a,b)
for name in ['loader_thread','loaded_track_path','stem_load_source','start_load']:
 m=re.search(r'^static [^\n]*\b'+name+r'\([^\n;]*\) \{',s,re.M);assert m,name
 out+=mapped(m.start(),s.index('\n}',m.end())+2)
Path(sys.argv[1]).write_text(out)
PY
"${HOST_CC:-cc}" "${flags[@]}" -DSTEM_LOADER_INC="\"$task_tmp/loader.inc\"" tests/unit/stem_loader_test.c -o "$task_tmp/test"
"$task_tmp/test"
