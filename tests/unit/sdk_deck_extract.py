#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Extract exact deck projection definitions with their private dependencies."""
from pathlib import Path
import re
import json
import sys


def mapped(source, path, start, end):
    return '#line {} {}\n'.format(source[:start].count('\n') + 1, json.dumps(str(path.resolve()))) + source[start:end] + '\n#line 1 "sdk-deck-extraction-fixture.inc"\n'


def function(source, name, path):
    match = re.search(r'^static (?:int|uint32_t|void) ' + name + r'\([^;{]*\{', source, re.M)
    assert match, name
    return mapped(source, path, match.start(), source.index('\n}', match.start()) + 2)


def projection(source, include_bind=False, path=None):
    path = path or Path(__file__).resolve().parents[2] / 'src/azmod/sdk_deck.c'
    if source != path.read_text():
        raise ValueError('deck provenance requires the exact production source')
    parts = []
    if include_bind:
        start = source.index('static void sd_bind_cue_mask(')
        parts.append(mapped(source, path, start, source.index('static int sd_snapshot(', start)))
    parts.append(function(source, 'sd_cue_receipt_valid', path))
    start = source.index('static int sd_load_receipt_valid(')
    parts.append(mapped(source, path, start, len(source)))
    return '\n'.join(parts)


if __name__ == '__main__':
    source, output = map(Path, sys.argv[1:3])
    output.write_text(projection(source.read_text(), '--bind' in sys.argv[3:], source))
