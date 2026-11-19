#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) != Linux ]]; then
    echo 'SKIP: surface placement checks require the Linux X11 build headers'; exit 77
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/az-surface-placement.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
from pathlib import Path
import json,re,sys
out=Path(sys.argv[1]);surface=Path('src/azmod/sdk_surface.c').read_text();overlay=Path('src/azmod/mod_menu_overlay.c').read_text()
def structure(source,name):
    match=re.search(r'^struct '+name+r' \{.*?^\};',source,re.M|re.S);assert match,name
    return match.group(0)+'\n'
(out/'types.inc').write_text(structure(overlay,'ov_window')+structure(surface,'az_surface')+'#define SDK_SURF_ALL (SDK_SURF_MAX+1)\n')
def function(name):
    match=re.search(r'^static (?:int|void) '+name+r'\([^;{]*\{',surface,re.M);assert match,name
    end=surface.index('\n}',match.start())+2
    return '#line {} {}\n'.format(surface[:match.start()].count('\n')+1,json.dumps(str(Path('src/azmod/sdk_surface.c').resolve())))+surface[match.start():end]+'\n'
(out/'surface.inc').write_text(''.join(function(name) for name in ['sdk_xtest_load','sdk_surf_create_window','sdk_surf_place_window','sdk_overlay_place']))
PY
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -fwrapv -Isrc/azmod -Itests/unit)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-gcc}" "${flags[@]}" -DSURFACE_TYPES_INC="\"$work/types.inc\"" -DSURFACE_IMPL_INC="\"$work/surface.inc\"" tests/unit/sdksurface_place_test.c -o "$work/placement"
"$work/placement" available
"$work/placement" missing
"${HOST_CC:-gcc}" "${flags[@]}" tests/unit/sdkdraw_test.c -o "$work/drawing"
"$work/drawing"
