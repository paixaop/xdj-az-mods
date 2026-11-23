#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Exact production entry points with observed native/UI boundary calls.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) != Linux || $(uname -m) != aarch64 ]]; then
    echo 'SKIP: native caller and device routing checks require ARM64 Linux'; exit 77
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/az-ui-routing.XXXXXX")
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
from pathlib import Path
import json,re,sys
out=Path(sys.argv[1])
groups={
    'refresh': ('waveform.c', ['ov_cue_deferred','ov_cue_defer','cue_marker_source','cue_markers_held',
        'overview_cue_update_hook','detail_cue_update_hook','cue_marker_entry','install_cue_marker_display',
        'ov_refresh_tile','ov_cue_rebuild_tile','ov_cue_refresh_tile','ov_refresh_if_needed']),
    'bus': ('bus_interpose.c', ['gpiodrv_slot','bus_fd_drop','bus_fd_forget','bus_note_panel',
        'bus_note_device','bus_fd_note','epoll_ctl','epoll_wait','bus_route_frame','bus_read_frame','read']),
    'overlay': ('mod_menu_overlay.c', ['ov_frame','overlay_thread']),
    'startup': ('azmod.c', ['sdk_source_join_patch','sdk_sources_configure_at_start',
        'sdk_source_capture_at_start','azmod_bind_build','azmod_bind_sources','azmod_init']),
    'waveinstall': ('azmod.c', ['install_wave_identity','install_wave_grid',
        'install_wave_renderers','install_cue_batch_presentation','install_wave_timers','install_wavestems_hook']),
    'statichooks': ('mod_menu_overlay.c', ['install_static_hooks']),
    'mode': ('stems_display.c', ['bpmctor_hook','modeget_from_deck_screen',
        'modeget_publish','modeget_stems','modeget_log','modeget_hook']),
}
for group,(filename,names) in groups.items():
    path=Path('src/azmod')/filename;source=path.read_text();parts=[]
    for name in names:
        match=re.search(r'^(?:__attribute__\(\(constructor\)\)\s+)?(?:static )?(?:void|bool|int|ssize_t) \*?'+name+r'\([^;{]*\{',source,re.M)
        assert match,name
        end=source.index('\n}',match.start())+2
        parts.append('#line {} {}\n'.format(source[:match.start()].count('\n')+1,json.dumps(str(path.resolve())))+source[match.start():end]+'\n')
    (out/(group+'.inc')).write_text(''.join(parts))
wave=(out/'waveinstall.inc').read_text()
hooks=re.findall(r'azm_install_hook\(\s*(\w+),\s*\(void \*\)\s*(\w+),\s*\(void \*\*\)\s*&(\w+)\)',wave)
assert len(hooks)==wave.count('azm_install_hook(')
targets=list(dict.fromkeys(h[0] for h in hooks))
declarations=['enum { '+','.join(targets)+', WAVE_HOOK_COUNT };']
declarations+=['static void '+name+'(void) {}' for name in sorted(set(h[1] for h in hooks))]
declarations+=['static void *'+name+';' for name in sorted(set(h[2] for h in hooks))]
(out/'wave-targets.inc').write_text('\n'.join(declarations)+'\n')
overlay=Path('src/azmod/mod_menu_overlay.c').read_text()
minimum=re.search(r'^#define OV_MIN_TICK_US .+$',overlay,re.M)
wakes=re.search(r'enum \{\s*OV_WAKE_FRAME = 1,\s*OV_WAKE_COMMAND = 2\s*\};',overlay)
assert minimum and wakes
(out/'overlay-constants.inc').write_text(minimum.group(0)+'\n'+wakes.group(0)+'\n')
PY
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -fwrapv -Isrc/azmod -Itests/unit -I"$work")
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
for fixture in status refresh bus overlay startup waveinstall mode; do
    "${HOST_CC:-gcc}" "${flags[@]}" -DROUTING_IMPL_INC="\"$work/$fixture.inc\"" "tests/unit/ui_${fixture}_test.c" -lm -o "$work/$fixture"
    "$work/$fixture"
done
