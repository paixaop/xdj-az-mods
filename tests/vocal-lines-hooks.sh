#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -n ${VOCAL_HOOKS_DIR:-} ]]; then
    dir=$VOCAL_HOOKS_DIR
    mkdir -p "$dir"
else
    dir=$(mktemp -d)
    trap 'rm -rf "$dir"' EXIT
fi
python3 - "$dir" <<'PY'
from pathlib import Path
import json, sys
out = Path(sys.argv[1])
wave = Path('src/azmod/waveform.c').resolve()
stem = Path('src/azmod/stem_marks.c').resolve()
def section(path, start, stop):
    text = path.read_text()
    assert text.count(start) == 1, start
    first = text.index(start)
    last = text.index(stop, first)
    return '#line {} {}\n'.format(text[:first].count('\n') + 1, json.dumps(str(path))) + text[first:last]
parts = [section(wave, '/* Update profile:', '/* ---- Bright Grid'),
         section(wave, 'void azm_upd_exit(int prev) {', '#include "brightgrid.h"'),
         section(wave, 'void azm_rw_exit(long prov, long *save) {', '/* renderWaveform_new takes'),
         section(stem, 'struct sm_world_watch {', '/* Liveness of the browse list widgets')]
(out / 'hooks.inc').write_text('\n'.join(parts))
PY
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -fno-omit-frame-pointer -fno-optimize-sibling-calls)
if [[ ${SAN:-plain} == asan ]]; then
    flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi
"${HOST_CC:-cc}" "${flags[@]}" -DVOCAL_HOOKS_INC="\"$dir/hooks.inc\"" \
    tests/unit/vocal_lines_hooks_test.c -o "$dir/hooks"
"$dir/hooks"
