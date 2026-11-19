#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Real folder discovery and native status qualification in a private filesystem.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-stem-folder.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
python3 - "$PWD/src/azstem/stem_playback.c" "$task_tmp" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]);s=source.read_text();out=Path(sys.argv[2])
parts=[]
for start,end in [('static DIR *stem_opendir(', 'static int child_named('),
                  ('static int slot_for_filename(', '/* Decode one .ogg'),
                  ('static int stem_folder_rank(', '\n#endif')]:
    a=s.index(start);b=s.index(end,a)
    # The exact production text retains its original source location. Only the
    # filesystem boundary is substituted in the fixture, never function bodies.
    parts.append('#line %d "%s"\n%s\n' % (s.count('\n',0,a)+1,source,s[a:b]))
parts[-1]+='#endif\n'
real=''.join(parts);(out/'real.inc').write_text(real)
for name,old,new in [('identity','if (found && !ident) return AZ_AVAIL_LOADING;','if (0) return AZ_AVAIL_LOADING;'),
                     ('changed','!status_stem_source_unchanged(src, &start)','0'),
                     ('incomplete','!complete || !status_stem_source_unchanged','!status_stem_source_unchanged')]:
    assert real.count(old)==1,name
    # A deliberately changed control must never contribute production coverage.
    broken=real.replace(old,new)
    broken='\n'.join(line for line in broken.splitlines() if not line.startswith('#line '))+'\n'
    (out/(name+'.inc')).write_text(broken)
PY
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -Isrc/azmod -Isrc/azstem)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
for name in real identity changed incomplete; do
    "${HOST_CC:-cc}" "${flags[@]}" -DFOLDER_INC="\"$task_tmp/$name.inc\"" tests/unit/stem_folder_test.c -o "$task_tmp/$name"
    mkdir "$task_tmp/fs-$name"
    if [ "$name" = real ]; then
        "$task_tmp/$name" "$task_tmp/fs-$name"
    elif (ulimit -c 0; "$task_tmp/$name" "$task_tmp/fs-$name" >"$task_tmp/$name.log" 2>&1) 2>/dev/null; then
        echo "FAIL: $name regression was not detected" >&2; exit 1
    else
        grep -Eq 'Assertion.*failed|Assertion failed' "$task_tmp/$name.log" || { cat "$task_tmp/$name.log"; exit 1; }
        echo "CONTROL caught: stem folder $name"
    fi
done
