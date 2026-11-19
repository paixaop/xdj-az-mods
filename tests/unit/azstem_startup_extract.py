# SPDX-License-Identifier: GPL-3.0-only
"""Extract complete startup/refcount functions with production locations."""
from pathlib import Path
import re
import sys
root = Path(__file__).resolve().parents[2]
names = 'azstem_process_matches azstem_identify_build azstem_initial_state azstem_hook_address azstem_lti_hooks azstem_configure_lti azstem_vtable_override azstem_install_named azstem_audio_hooks azstem_start_pollers azstem_init azn_ref_pin azn_ref_release'.split()
def function(path, name):
    text = (root / path).read_text()
    start = re.search(r'^.*\b' + name + r'\([^;\n]*\) \{', text, re.M)
    assert start, name
    end = text.index('\n', start.start())
    if not text[start.start():end].endswith('}'):
        end = text.index('\n}', start.start()) + 2
    return '#line {} "{}"\n{}\n'.format(text.count('\n', 0, start.start()) + 1, root / path, text[start.start():end])
macros = set('DJPU_VTABLE DJPUVT_PATH DJPUCTOR_DEFAULT DJPUCTOR_PATH LSDTOR_DEFAULT LTI_UPDATE_DEFAULT LSCTOR_DEFAULT LSCTOR_PATH LTIPROBE_PATH PBREAD_PATH PBREAD_DEFAULT FRADDR_PATH FREAD_DEFAULT OPADDR_PATH OPERATE_DEFAULT KFADDR_PATH KEYFAC_DEFAULT FGPRADDR_PATH FGPR_SETSRC_DEFAULT FGPRREADADDR_PATH FGPRREAD_DEFAULT RINGFILLADDR_PATH RINGFILL_DEFAULT FGPROP_DEFAULT MEOW_DESTROY BG_XOCR MEOW_RC_OFF MEOW_GUARD_OFF MEOW_OWNER_OFF'.split())
prefix = []
for path in ('azstem.c', 'deck_track_map.c', 'key_shift.c', 'drumroll.c'):
    for line in (root / 'src/azstem' / path).read_text().splitlines():
        match = re.match(r'#define (\w+)\s', line)
        if match and match[1] in macros:
            prefix.append(line)
            macros.remove(match[1])
assert not macros, macros
text = '\n'.join(prefix) + '\n'
text += function('src/azstem/drumroll.c', 'bg_ptr_ok') + function('src/azstem/drumroll.c', 'meow_release')
text += ''.join(function('src/azstem/azstem.c', name) for name in names)
Path(sys.argv[1]).write_text(text)
