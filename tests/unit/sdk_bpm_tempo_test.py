#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Lightweight host checks of the actual BPM reader/publication/public projection."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
def fn(text, name):
    m = re.search(r'^static (?:int|void|uint32_t|double) ' + name + r'\([^;{]*\{', text, re.M)
    assert m, name
    return text[m.start():text.index('\n}', m.start()) + 2] + '\n'

with tempfile.TemporaryDirectory(prefix='az-bpm-tempo-') as tmp:
    out = Path(tmp)
    display = (root / 'src/azmod/stems_display.c').read_text()
    declarations = [line for line in display.splitlines() if
                    re.match(r'^#define (BPMSTATE_(DECK|TRACK|PIN_MAX)|BPMCTRL_(OFF|USE|WEAK)) ', line) or
                    re.match(r'^static (unsigned long|int|pthread_mutex_t) g_bpmstate', line)]
    # The extracted reader does not include the ctor's logging-only counter.
    declarations = [line.replace('g_bpmstate_n = 0, ', '') for line in declarations]
    bpm = '\n'.join(declarations) + '\n' + ''.join(fn(display, n) for n in
          ('bpmstate_capture', 'bpmstate_hold', 'bpmstate_release', 'bpmstate_sample', 'bpmstate_read', 'bpmstate_current'))
    (out / 'bpm.inc').write_text(bpm)
    community = (root / 'src/azmod/community.c').read_text()
    (out / 'tempo.inc').write_text(community[community.index('static _Atomic double g_cm_bpm['):
                                           community.index('static int (*g_cm_track_fn)')] + fn(community, 'cm_deck_bpm'))
    deck = (root / 'src/azmod/sdk_deck.c').read_text()
    (out / 'deck.inc').write_text(fn(deck, 'sd_native_load_read') + fn(deck, 'sd_native_cue_read') + fn(deck, 'sd_live_fields') + fn(deck, 'sd_deck_snapshot_v1'))
    flags = [os.environ.get('HOST_CC', os.environ.get('CC', 'cc')), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-pthread', '-I' + str(root / 'src/azmod'), '-I' + str(root / 'tests/unit')]
    san = os.environ.get('SAN', 'plain')
    if san == 'asan':
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    elif san == 'tsan':
        flags += ['-fsanitize=thread', '-fno-omit-frame-pointer']
    elif san not in ('plain', ''):
        raise ValueError('SAN must be plain, asan or tsan')
    for name, source in [('lease', 'bpmstate_test.c'), ('public', 'sdkdeckdata_test.c'), ('feed', 'sdkbpmtempo_test.c')]:
        if name == 'feed':
            (out / 'feed-deck.inc').write_text((out / 'deck.inc').read_text() + fn(deck, 'sd_snapshot') + fn(deck, 'sd_deck_info'))
        command = flags + ['-DBPM_INC="' + str(out / 'bpm.inc') + '"',
                           '-DTEMPO_INC="' + str(out / 'tempo.inc') + '"',
                           '-DDECK_DATA_INC="' + str(out / ('feed-deck.inc' if name == 'feed' else 'deck.inc')) + '"',
                           str(root / 'tests/unit' / source), '-o', str(out / name)]
        subprocess.run(command, check=True)
        subprocess.run([str(out / name)], check=True, timeout=5)
