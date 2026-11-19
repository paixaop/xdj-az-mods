#!/usr/bin/env python3
"""Extract unchanged production spool functions for the Linux source fixture."""
from pathlib import Path
import re

def definition(path, pattern):
    source = path.read_text()
    match = re.search(pattern, source, re.M)
    if not match:
        raise RuntimeError(pattern)
    end = source.index('\n}', match.start()) + 2
    emit(path, source, match.start(), end)

def emit(path, source, start, end):
    line = source.count('\n', 0, start) + 1
    print(f'#line {line} "{path.resolve()}"')
    print(source[start:end])

root = Path(__file__).resolve().parents[2]
source = root / 'src/azmod/library_edits.c'
text = source.read_text()
definition(root / 'src/azmod/library_db.c', r'^static int lp_exec\(')
a = text.index('static const char *const LS_TABLES[]')
b = text.index('static const char *(*g_sq_db_filename)', a)
emit(source, text, a, b)
definition(source, r'^static void ls_source_hint\(')
a = text.index('#define LS_SPOOL_CAP')
b = text.index('static int ls_apply_file', a)
emit(source, text, a, b)
