#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""coverage-gcc-map.py <guest-dir> <gcov-root> <out-dir> [--exclude SUITE ...]

Reads gcov text reports (gcov -b -c -p) laid out as <gcov-root>/<suite>/<object>/*.gcov and
attributes every line to an original file under <guest-dir>:
  - a report whose Source path is a guest file (or /g/<path>, the containers' mount) maps 1:1;
  - an extracted copy maps only complete, uniquely matching production definitions;
    altered functions (including FDT_DEV path rewrites) never contribute production hits;
  - test sources (tools/unit-tests, guest/**/test/, *_test.c) and system headers are dropped.
Writes summary.txt, files.tsv, functions.tsv, unmapped.txt and coverage.info (lcov).
"""
import importlib.util
import os
import re
import sys
from collections import defaultdict

guest, root, outd = sys.argv[1:4]
excl = set()
a = sys.argv[4:]
while a:
    if a[0] == '--exclude':
        excl.add(a[1]); a = a[2:]
    else:
        sys.exit('bad arg ' + a[0])
guest = os.path.abspath(guest)


def is_test(rel):
    return '/test/' in '/' + rel or rel.endswith('_test.c') or rel.startswith('tools/')


# guest index
gfiles = {}
for dp, dn, fn in os.walk(guest):
    dn[:] = [d for d in dn if d not in ('out', '.git')]
    for f in fn:
        if f.endswith(('.c', '.h', '.inc')):
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, guest)
            if is_test(rel):
                continue
            with open(p, encoding='utf-8', errors='replace') as fh:
                gfiles[rel] = fh.read().split('\n')
spec = importlib.util.spec_from_file_location("coverage_functions", os.path.join(os.path.dirname(__file__), "coverage-functions.py"))
coverage_functions = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage_functions)
function_index = coverage_functions.FunctionIndex({rel: "\n".join(lines) for rel, lines in gfiles.items()})


def guest_rel(path):
    p = os.path.normpath(path)
    if p.startswith('guest/'):
        return p[6:]
    if p.startswith('/g/'):
        return p[3:]
    k = p.rfind('/guest/')
    if k >= 0:
        return p[k + 7:]
    return None


align_cache = {}


def align(path, text):
    """inc line number (1-based) -> (rel, guest line 1-based)"""
    key = (path, tuple(text))
    if key in align_cache:
        return align_cache[key]
    m = function_index.align("\n".join(text))
    align_cache[key] = m
    return m


def parse(fn):
    src = None
    rows = []  # (lineno, count or None, text, [branch taken bools], [funcs])
    pend_funcs = []
    cur = None
    with open(fn, encoding='utf-8', errors='replace') as fh:
        for raw in fh:
            raw = raw.rstrip('\n')
            if raw.startswith('function '):
                mm = re.match(r'function (\S+) called (\d+)', raw)
                if mm:
                    pend_funcs.append((mm.group(1), int(mm.group(2))))
                continue
            if raw.startswith('branch '):
                if cur is not None:
                    mm = re.match(r'branch\s+\d+\s+(taken (\d+)|never executed)', raw)
                    if mm:
                        cur[3].append(bool(mm.group(2)) and int(mm.group(2)) > 0)
                continue
            if raw.startswith(('call ', 'unconditional ')):
                continue
            parts = raw.split(':', 2)
            if len(parts) < 3:
                continue
            c, ln = parts[0].strip(), parts[1].strip()
            if not ln.isdigit():
                continue
            ln = int(ln)
            if ln == 0:
                if parts[2].startswith('Source:'):
                    src = parts[2][7:]
                continue
            c = c.rstrip('*')
            cnt = None if c == '-' else (0 if c in ('#####', '=====') else int(c))
            cur = [ln, cnt, parts[2], [], pend_funcs]
            pend_funcs = []
            rows.append(cur)
    return src, rows


# accumulation
LINE = {}            # (rel, gl) -> [count, set(suites)]
BR = {}              # (rel, gl) -> list of taken bools (union over objects)
BRS = defaultdict(set)  # (rel, gl) -> suites that took any branch there
FUNC = {}            # (rel, gl) -> [name, calls, set(suites)]
unmapped = defaultdict(int)
suites_seen = set()

for suite in sorted(os.listdir(root)):
    if suite in excl:
        continue
    sd = os.path.join(root, suite)
    if not os.path.isdir(sd):
        continue
    suites_seen.add(suite)
    for dp, dn, fn in os.walk(sd):
        for f in fn:
            if not f.endswith('.gcov'):
                continue
            src, rows = parse(os.path.join(dp, f))
            if not src or src.startswith('/usr/'):
                continue
            rel = guest_rel(src)
            if rel is not None and rel.startswith('out/'):
                rel = None  # a copy the suites made under guest/out: aligned like any other copy
            mapping = None
            text = [""] * max((r[0] for r in rows), default=0)
            for row in rows:
                text[row[0] - 1] = row[2]
            if rel is not None:
                if is_test(rel) or rel not in gfiles:
                    continue
                # A source-looking path is not proof that the compiled text
                # was production. gcov retains that text; verify it as well.
                actual = "\n".join(text).rstrip("\n")
                expected = "\n".join(gfiles[rel]).rstrip("\n")
                if actual != expected:
                    mapping = coverage_functions.FunctionIndex({rel: expected}).align(actual)
            else:
                if (src.startswith('tools/') or '/tools/unit-tests/' in src) and not src.endswith('.inc') \
                        and '/az-unit-tests.' not in src:
                    continue
                mapping = align(src, text)
            for ln, cnt, txt, brs, funcs in rows:
                if mapping is None:
                    key = (rel, ln)
                else:
                    if ln not in mapping:
                        if cnt is not None:
                            unmapped[src] += 1
                        continue
                    key = mapping[ln]
                for name, calls in funcs:
                    e = FUNC.setdefault(key, [name, 0, set()])
                    e[1] += calls
                    if calls:
                        e[2].add(suite)
                if cnt is None:
                    continue
                e = LINE.setdefault(key, [0, set()])
                e[0] += cnt
                if cnt:
                    e[1].add(suite)
                if brs:
                    old = BR.get(key, [])
                    n = max(len(old), len(brs))
                    BR[key] = [(old[i] if i < len(old) else False) or (brs[i] if i < len(brs) else False) for i in range(n)]
                    if any(brs):
                        BRS[key].add(suite)

os.makedirs(outd, exist_ok=True)
per = defaultdict(lambda: [0, 0, 0, 0, defaultdict(int)])
for (rel, gl), (cnt, ss) in LINE.items():
    p = per[rel]
    p[1] += 1
    if cnt:
        p[0] += 1
        for s in ss:
            p[4][s] += 1
for (rel, gl), b in BR.items():
    per[rel][3] += len(b)
    per[rel][2] += sum(b)

tl = sum(p[1] for p in per.values()); cl = sum(p[0] for p in per.values())
tb = sum(p[3] for p in per.values()); cb = sum(p[2] for p in per.values())
with open(os.path.join(outd, 'files.tsv'), 'w') as fh:
    fh.write('file\tlines_cov\tlines_tot\tbr_cov\tbr_tot\tfile_lines\tsuites(lines covered)\n')
    for rel in sorted(per):
        p = per[rel]
        fh.write('%s\t%d\t%d\t%d\t%d\t%d\t%s\n' % (rel, p[0], p[1], p[2], p[3], len(gfiles[rel]),
                 ' '.join('%s=%d' % kv for kv in sorted(p[4].items()))))
with open(os.path.join(outd, 'functions.tsv'), 'w') as fh:
    fh.write('file:line\tfunction\tcalls\tsuites\n')
    for (rel, gl) in sorted(FUNC):
        n, c, ss = FUNC[(rel, gl)]
        fh.write('%s:%d\t%s\t%d\t%s\n' % (rel, gl, n, c, ','.join(sorted(ss))))
with open(os.path.join(outd, 'unmapped.txt'), 'w') as fh:
    for s, n in sorted(unmapped.items()):
        fh.write('%6d executable lines not mapped  %s\n' % (n, s))
with open(os.path.join(outd, 'summary.txt'), 'w') as fh:
    fh.write('suites: %s%s\n' % (' '.join(sorted(suites_seen)), ('  (excluded: %s)' % ' '.join(sorted(excl))) if excl else ''))
    fh.write('TOTAL lines %d/%d (%.1f%%)  branches %d/%d (%.1f%%)  over %d guest files\n' % (
        cl, tl, 100.0 * cl / max(tl, 1), cb, tb, 100.0 * cb / max(tb, 1), len(per)))
    fh.write('unmapped executable lines: %d (unmapped.txt)\n\n' % sum(unmapped.values()))
    fh.write('%-40s %13s %13s  %s\n' % ('guest file', 'lines', 'branches', 'suites (lines covered)'))
    for rel in sorted(per):
        p = per[rel]
        fh.write('%-40s %6d/%-6d %6d/%-6d  %s\n' % (rel, p[0], p[1], p[2], p[3],
                 ' '.join('%s=%d' % kv for kv in sorted(p[4].items()))))
with open(os.path.join(outd, 'coverage.info'), 'w') as fh:
    for rel in sorted(per):
        fh.write('TN:\nSF:%s\n' % os.path.join(guest, rel))
        for (r, gl) in sorted(k for k in FUNC if k[0] == rel):
            fh.write('FN:%d,%s\n' % (gl, FUNC[(r, gl)][0]))
        for (r, gl) in sorted(k for k in FUNC if k[0] == rel):
            fh.write('FNDA:%d,%s\n' % (FUNC[(r, gl)][1], FUNC[(r, gl)][0]))
        for (r, gl) in sorted(k for k in BR if k[0] == rel):
            for i, t in enumerate(BR[(r, gl)]):
                fh.write('BRDA:%d,0,%d,%s\n' % (gl, i, '1' if t else '0'))
        for (r, gl) in sorted(k for k in LINE if k[0] == rel):
            fh.write('DA:%d,%d\n' % (gl, LINE[(r, gl)][0]))
        fh.write('end_of_record\n')
print(open(os.path.join(outd, 'summary.txt')).read())
