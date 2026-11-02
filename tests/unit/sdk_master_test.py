#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile the actual confirmed-master hook and public deck-service projection."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
def fn(text, name):
    m = re.search(r'^static (?:int|uint32_t) ' + name + r'\([^;{]*\{', text, re.M)
    assert m, name
    return text[m.start():text.index('\n}', m.start()) + 2] + '\n'
with tempfile.TemporaryDirectory(prefix='az-sdk-master-') as tmp:
    out = Path(tmp)
    display = (root / 'src/azmod/stems_display.c').read_text()
    start = display.index('static long (*g_orig_master_xm)')
    end = display.index('static void install_master_hook', start)
    memory = (root / 'src/azmod/ep_mem.h').read_text()
    own = memory[memory.index('static inline int ep_addr_ok('):memory.index('/* own_q for n addresses')]
    install_start = display.index('static void install_master_hook', start)
    install_end = display.index('\n}', install_start) + 2
    (out / 'master.inc').write_text(own + display[start:install_end])
    deck = (root / 'src/azmod/sdk_deck.c').read_text()
    track = (root / 'src/azmod/sdk_track.c').read_text()
    (out / 'deck.inc').write_text(fn(deck, 'sd_native_load_read') + fn(deck, 'sd_native_cue_read') + fn(deck, 'sd_live_fields') + fn(deck, 'sd_deck_snapshot_v1') + fn(track, 'sdk_data_capabilities'))
    command = [os.environ.get('HOST_CC', os.environ.get('CC', 'cc')), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-variable', '-pthread',
               '-I' + str(root / 'src/azmod'),
               '-DMASTER_INC="' + str(out / 'master.inc') + '"',
               '-DDECK_DATA_INC="' + str(out / 'deck.inc') + '"',
               str(root / 'tests/unit/sdkmaster_test.c'), '-o', str(out / 'master')]
    san = os.environ.get('SAN', 'plain')
    if san == 'asan':
        command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    elif san == 'tsan':
        command += ['-fsanitize=thread', '-fno-omit-frame-pointer']
    elif san not in ('plain', ''):
        raise ValueError('SAN must be plain, asan or tsan')
    subprocess.run(command, check=True)
    subprocess.run([str(out / 'master')], check=True, timeout=5)
