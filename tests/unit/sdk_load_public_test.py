#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Public load receipt regression; --native also uses real native-entry dispatch."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[2]
def fn(text, name):
    m = re.search(r'^static (?:int|uint32_t|void) ' + name + r'\([^;{]*\{', text, re.M)
    assert m, name
    return text[m.start():text.index('\n}', m.start()) + 2] + '\n'
with tempfile.TemporaryDirectory(prefix='az-sdk-public-load-') as tmp:
    out = Path(tmp)
    deck = (root / 'src/azmod/sdk_deck.c').read_text()
    (out / 'deck.inc').write_text(fn(deck, 'sd_bind') + fn(deck, 'sd_native_load_read') + fn(deck, 'sd_native_cue_read') + fn(deck, 'sd_live_fields') + fn(deck, 'sd_deck_snapshot_v1'))
    common = ['-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
              '-Wno-unused-function', '-Wno-unused-variable', '-I' + str(root / 'src/azmod'),
              '-I' + str(root / 'src/common')]
    san = os.environ.get('SAN', 'plain')
    if san == 'asan': common += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    elif san == 'tsan': common += ['-fsanitize=thread', '-fno-omit-frame-pointer']
    elif san not in ('plain', ''): raise ValueError('SAN must be plain, asan or tsan')
    cc = os.environ.get('HOST_CC', os.environ.get('CC', 'cc'))
    fixture = [cc, '-std=c11'] + common + ['-DDECK_DATA_INC="' + str(out / 'deck.inc') + '"',
               str(root / 'tests/unit/sdkloadpublic_test.c')]
    subprocess.run(fixture + ['-DSDK_LOAD_PROJECTION_STANDALONE', str(root / 'src/azstem/sdk_load_export.c'), '-o', str(out / 'projection')], check=True)
    subprocess.run([str(out / 'projection')], check=True, timeout=5)
    if '--native' in sys.argv:
        if not sys.platform.startswith('linux'):
            raise RuntimeError('--native requires Linux ELF guards and GNU C++ ABI')
        subprocess.run(fixture + ['-c', '-o', str(out / 'public.o')], check=True)
        subprocess.run([cc, '-std=c11'] + common + ['-c', str(root / 'src/azstem/sdk_load_export.c'), '-o', str(out / 'export.o')], check=True)
        command = [os.environ.get('HOST_CXX', 'g++'), '-std=c++11'] + common + [
            '-DAZSDK_LOAD_TESTING', '-DAZSDK_LOAD_PUBLIC_TEST', '-fno-devirtualize', '-fno-devirtualize-speculatively',
            str(root / 'src/azstem/sdk_load_observer.cpp'), str(root / 'tests/unit/sdkloadobserver_test.cpp'),
            str(out / 'public.o'), str(out / 'export.o'), '-ldl', '-o', str(out / 'native')]
        subprocess.run(command, check=True)
        for build in (1, 2):
            subprocess.run([str(out / 'native'), str(build), 'public'], check=True, timeout=15)
