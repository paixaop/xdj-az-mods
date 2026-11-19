#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Line, branch and function coverage of the host unit suites over the guest sources.

Run by `COVERAGE=1 tools/unit-tests/run.sh` after every suite has run; not useful on its own.

Method
  - Every test program was built with clang's source-based coverage (-fprofile-instr-generate
    -fcoverage-mapping) and wrote one raw profile per process. They are merged into one profile.
  - Most suites compile production functions EXTRACTED by text out of guest/ into files of their
    own (run.sh, extract), so the coverage mapping names those copies, not the guest file. Each
    copy is mapped only when the entire function definition matches production source,
    ignoring indentation. Altered control copies and partial fragments never contribute
    production hits. Ambiguous definitions remain unmeasured.
    Suites that #include a guest file whole are counted as that file directly.
  - The DENOMINATOR is the release build: guest/azmod/{azmod,menu,text,azconf}.c,
    guest/azstem/azstem.c and guest/azhandoff/az-handoff-guard.c compiled with -DAZMOD_RELEASE for
    aarch64 Linux with the same coverage flags (object files only, never run), so every line,
    branch and function of the shipped code is listed whether a suite reaches it or not. clang
    rejects gcc's __atomic builtins on _Atomic objects, so those sources are compiled from a copy
    with _Atomic spelled as volatile / __typeof__ (line numbers unchanged); it is used for its
    line map only. stb_vorbis.c (vendored) is left out. The headers come from azmod-build:18.04.
  - A guest line is covered when any test program executed it; a branch outcome when any took
    it; a function when any test called it. Lines that tests executed but the release build does
    not contain (development-only code) are reported apart.

Output (in --covdir): summary.txt (totals and per file), functions.tsv (per function), uncovered/
(per file, the uncovered lines of every function the suites call at least once), coverage.json,
and coverage.lcov (actual test-compiled mapped lines, with explicit zero-hit records).
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict

DIRS = ("azmod", "azstem", "common", "azhandoff")
UNIVERSE_TUS = [
    ("azmod/azmod.c", "azmod"), ("azmod/menu.c", "azmod"), ("azmod/text.c", "azmod"),
    ("azmod/azconf.c", "azmod"), ("azstem/azstem.c", "azstem"),
    ("azhandoff/az-handoff-guard.c", "azhandoff"),
]
EXCLUDE = {"azstem/stb_vorbis.c"}
SRC_EXT = (".c", ".h", ".def", ".inc")


def function_index(sources):
    path = os.path.join(os.path.dirname(__file__), "coverage-functions.py")
    spec = importlib.util.spec_from_file_location("coverage_functions", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.FunctionIndex(sources)


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, **kw).stdout


def is_instrumented(path):
    try:
        with open(path, "rb") as f:
            head = f.read(4)
            if head not in (b"\x7fELF", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"):
                return False
            return b"__llvm_covmap" in f.read() or False
    except OSError:
        return False


class Guest:
    """guest/{azmod,azstem,common,azhandoff} sources, indexed for aligning copies."""

    def __init__(self, root):
        self.root = os.path.join(root, "guest")
        self.lines = {}
        sources = {}
        for d in DIRS:
            for dp, _, fs in os.walk(os.path.join(self.root, d)):
                for fn in fs:
                    if not fn.endswith(SRC_EXT):
                        continue
                    rel = os.path.relpath(os.path.join(dp, fn), self.root)
                    with open(os.path.join(dp, fn), encoding="utf-8", errors="replace") as f:
                        text = f.read()
                        ls = text.split("\n")
                    self.lines[rel] = ls
                    sources[rel] = text
        self.index = function_index(sources)

    def rel(self, path):
        """guest-relative name of a path inside guest/, or None."""
        p = os.path.realpath(path)
        g = os.path.realpath(self.root)
        if p.startswith(g + os.sep):
            r = os.path.relpath(p, g)
            if r in self.lines:
                return r
        return None

    def align(self, text):
        """{copy line (1-based): (guest file, guest line 1-based)} for a copy's text."""
        return self.index.align(text)


def parse_lcov(text):
    """{source: {'DA': {line: count}, 'BR': {line: [count or None, ...]}, 'FN': {name: line}, 'FNDA': {name: count}}}"""
    files = {}
    cur = None
    for ln in text.splitlines():
        if ln.startswith("SF:"):
            cur = files.setdefault(ln[3:], {"DA": {}, "BR": defaultdict(list), "FN": {}, "FNDA": {}})
        elif cur is None:
            continue
        elif ln.startswith("DA:"):
            a, c = ln[3:].split(",")[:2]
            cur["DA"][int(a)] = max(cur["DA"].get(int(a), 0), int(c))
        elif ln.startswith("BRDA:"):
            a, _, _, c = ln[5:].split(",")
            cur["BR"][int(a)].append(None if c == "-" else int(c))
        elif ln.startswith("FN:"):
            a, name = ln[3:].split(",", 1)
            cur["FN"][name] = int(a)
        elif ln.startswith("FNDA:"):
            c, name = ln[5:].split(",", 1)
            cur["FNDA"][name] = max(cur["FNDA"].get(name, 0), int(c))
        elif ln == "end_of_record":
            cur = None
    return files



def write_mapped_lcov(path, hits):
    """Export actual test-compiled lines, retaining explicit unexecuted records.

    The unexecuted release-universe build supplies the legacy summary's denominator,
    not measured test coverage. Whole-function mapping already rejects partial and
    altered copies. Functions no test compiles remain absent (unmeasured).
    """
    files = defaultdict(dict)
    for (name, line), count in hits.items():
        files[name][line] = count
    with open(path, "w") as out:
        for name in sorted(files):
            out.write("SF:guest/%s\n" % name)
            for line, count in sorted(files[name].items()):
                out.write("DA:%d,%d\n" % (line, count))
            out.write("end_of_record\n")


def build_universe(args, guest, work):
    """Compiles the release translation units with coverage mapping; returns the object list
    and the copy's guest root, or ([], None) when the headers cannot be had."""
    sysroot = os.environ.get("AZ_COV_SYSROOT") or os.path.join(work, "sysroot")
    if not os.path.isdir(os.path.join(sysroot, "usr", "include")):
        os.makedirs(sysroot, exist_ok=True)
        try:
            tar = subprocess.run(
                ["docker", "run", "--rm", "--platform=linux/arm64", "azmod-build:18.04", "sh", "-c",
                 "tar -ch -C / usr/include usr/lib/gcc/aarch64-linux-gnu/7/include 2>/dev/null"],
                check=True, stdout=subprocess.PIPE).stdout
            subprocess.run(["tar", "-x", "-C", sysroot], input=tar, check=True)
        except (OSError, subprocess.CalledProcessError):
            print("coverage: azmod-build:18.04 headers not available: the release-build denominator is skipped")
            return [], None
    src = os.path.join(work, "src")
    shutil.rmtree(src, ignore_errors=True)
    for d in DIRS:
        shutil.copytree(os.path.join(guest.root, d), os.path.join(src, d),
                        ignore=shutil.ignore_patterns("out"))
    for dp, _, fs in os.walk(src):
        for fn in fs:
            if fn.endswith(SRC_EXT):
                p = os.path.join(dp, fn)
                with open(p, encoding="utf-8", errors="surrogateescape") as f:
                    t = f.read()
                t2 = re.sub(r"\b_Atomic\(", "__typeof__(", t)
                t2 = re.sub(r"\b_Atomic ", "volatile ", t2)
                if t2 != t:
                    with open(p, "w", encoding="utf-8", errors="surrogateescape") as f:
                        f.write(t2)
    gccinc = os.path.join(sysroot, "usr/lib/gcc/aarch64-linux-gnu/7/include")
    objs = []
    for tu, inc in UNIVERSE_TUS:
        o = os.path.join(work, tu.replace("/", "_") + ".o")
        cmd = [args.cc, "-target", "aarch64-unknown-linux-gnu", "--sysroot=" + sysroot,
               "-isystem", gccinc, "-isystem", os.path.join(sysroot, "usr/include/freetype2"),
               "-O0", "-w", "-fPIC", "-DAZMOD_RELEASE", "-fprofile-instr-generate", "-fcoverage-mapping",
               "-I" + inc, "-c", tu, "-o", o]
        r = subprocess.run(cmd, cwd=src, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            print("coverage: the release build of %s did not compile:" % tu)
            print(r.stdout.decode(errors="replace")[-2000:])
            return [], None
        objs.append(o)
    return objs, src


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True, help="run.sh's scratch directory (the suites' builds)")
    ap.add_argument("--prof", required=True, help="the directory of raw profiles")
    ap.add_argument("--covdir", required=True)
    ap.add_argument("--profdata", required=True)
    ap.add_argument("--cov", required=True)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--gcc-lcov", help="coverage.info of the gcc half (coverage-gcc.sh), merged by line")
    args = ap.parse_args()
    guest = Guest(args.root)
    covdir = args.covdir

    raws = [os.path.join(args.prof, f) for f in os.listdir(args.prof)
            if f.endswith(".profraw")]
    if not raws:
        print("coverage: no profiles were written")
        return 1
    merged = os.path.join(covdir, "merged.profdata")
    lst = os.path.join(covdir, "profraw.list")
    with open(lst, "w") as f:
        f.write("\n".join(raws) + "\n")
    run([args.profdata, "merge", "-sparse", "-f", lst, "-o", merged])

    bins = []
    for dp, _, fs in os.walk(args.out):
        for fn in fs:
            p = os.path.join(dp, fn)
            if not os.path.islink(p) and os.access(p, os.X_OK) and is_instrumented(p):
                bins.append(p)
    bins.sort()

    # per test program: lcov, aligned to the guest lines
    hits = defaultdict(int)                       # (file, line) -> count
    br = {}                                       # (file, line) -> [taken?]
    fncalled = defaultdict(int)                   # (file, line of definition) -> calls
    unmapped_missing = set()
    nprog = 0
    for b in bins:
        r = subprocess.run([args.cov, "export", "-format=lcov", "-instr-profile=" + merged, b],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            continue
        nprog += 1
        for sf, d in parse_lcov(r.stdout.decode(errors="replace")).items():
            g = guest.rel(sf)
            if g is not None:
                amap = None
            elif os.path.realpath(sf).startswith(os.path.realpath(args.out) + os.sep):
                try:
                    with open(sf, encoding="utf-8", errors="replace") as f:
                        amap = guest.align(f.read())
                except OSError:
                    unmapped_missing.add(sf)
                    continue
            else:
                continue          # system headers, the harness's own sources
            def where(line):
                if amap is None:
                    return (g, line)
                return amap.get(line)
            for line, c in d["DA"].items():
                w = where(line)
                if w:
                    hits[w] = max(hits.get(w, 0), 1 if c > 0 else 0)
            for line, outs in d["BR"].items():
                w = where(line)
                if not w:
                    continue
                prev = br.get(w)
                taken = [bool(c) for c in outs]
                if prev is None or len(prev) != len(taken):
                    if prev is None or sum(taken) > sum(prev):
                        br[w] = taken
                else:
                    br[w] = [x or y for x, y in zip(prev, taken)]
            for name, line in d["FN"].items():
                w = where(line)
                if w:
                    fncalled[w] += d["FNDA"].get(name, 0)

    # the gcc half: the suites clang cannot build, already mapped to guest/ (coverage-gcc-map.py).
    # Lines and calls are merged; a branch line clang has no outcomes for takes gcc's taken edges,
    # capped at the outcomes the release build has there.
    clang_hits, clang_br, clang_fn = dict(hits), dict(br), dict(fncalled)
    gbr = {}
    if args.gcc_lcov:
        for sf, d in parse_lcov(open(args.gcc_lcov, encoding="utf-8", errors="replace").read()).items():
            g = guest.rel(sf)
            if g is None:
                continue
            for line, c in d["DA"].items():
                hits[(g, line)] = max(hits.get((g, line), 0), 1 if c > 0 else 0)
            for line, outs in d["BR"].items():
                gbr[(g, line)] = sum(1 for c in outs if c)
            for name, line in d["FN"].items():
                fncalled[(g, line)] += d["FNDA"].get(name, 0)
        for k, n in gbr.items():
            if k not in br and n:
                br[k] = [True] * n

    write_mapped_lcov(os.path.join(covdir, "coverage.lcov"), hits)

    # the release build's lines, branches and functions
    work = os.path.join(covdir, "universe")
    os.makedirs(work, exist_ok=True)
    objs, usrc = build_universe(args, guest, work)
    uda = defaultdict(set)                        # file -> lines
    ubr = {}                                      # (file, line) -> number of outcomes
    ufn = []                                      # (file, name, first line, last line)
    if objs:
        for o in objs:
            r = run([args.cov, "export", "-format=lcov", "-instr-profile=" + merged, o],
                    stderr=subprocess.DEVNULL).decode(errors="replace")
            for sf, d in parse_lcov(r).items():
                rel = os.path.relpath(os.path.realpath(sf), os.path.realpath(usrc))
                if rel.startswith("..") or rel in EXCLUDE or rel not in guest.lines:
                    continue
                uda[rel].update(d["DA"].keys())
                for line, outs in d["BR"].items():
                    ubr[(rel, line)] = max(ubr.get((rel, line), 0), len(outs))
            js = json.loads(run([args.cov, "export", "-format=text", "-instr-profile=" + merged, o],
                                stderr=subprocess.DEVNULL))
            for fn in js["data"][0]["functions"]:
                if not fn["regions"]:
                    continue
                body = fn["regions"][0]  # the function's own body: [line, col, end line, end col, count, file, ...]
                rel = os.path.relpath(os.path.realpath(fn["filenames"][body[5]]), os.path.realpath(usrc))
                if rel.startswith("..") or rel in EXCLUDE or rel not in guest.lines:
                    continue
                name = fn["name"].split(":", 1)[-1]
                ufn.append((rel, name, body[0], body[2]))
    ufn = sorted(set(ufn))

    # totals
    def file_stats(rel, hits=hits, br=br, fncalled=fncalled):
        lines = uda.get(rel, set())
        lc = sum(1 for l in lines if hits.get((rel, l), 0) > 0)
        bt = sum(ubr.get((rel, l), 0) for l in lines)
        bc = 0
        for l in lines:
            n = ubr.get((rel, l), 0)
            t = br.get((rel, l))
            if n and t:
                bc += min(n, sum(t)) if len(t) != n else sum(t)
        fns = [f for f in ufn if f[0] == rel]
        fc = sum(1 for f in fns if fncalled.get((rel, f[2]), 0) > 0 or
                 any(hits.get((rel, l), 0) for l in range(f[2], f[2] + 1)))
        return len(lines), lc, bt, bc, len(fns), fc

    rows = []
    T = [0] * 6
    for rel in sorted(uda):
        s = file_stats(rel)
        rows.append((rel,) + s)
        T = [a + b for a, b in zip(T, s)]
    C = [0] * 6  # clang's suites alone
    for rel in sorted(uda):
        C = [a + b for a, b in zip(C, file_stats(rel, clang_hits, clang_br, defaultdict(int, clang_fn)))]
    by_dir = defaultdict(lambda: [0] * 6)
    for r in rows:
        d = r[0].split("/")[0]
        by_dir[d] = [a + b for a, b in zip(by_dir[d], r[1:])]

    # lines the suites run that the release build does not have
    outside = sorted({k for k, v in hits.items() if v > 0 and k[1] not in uda.get(k[0], ())})

    # the code the suites compile at all (the part a host test can reach today)
    compiled = defaultdict(set)
    for (f, l) in hits:
        if l in uda.get(f, ()):
            compiled[f].add(l)

    def pct(a, b):
        return "%5.1f%%" % (100.0 * a / b) if b else "    -"

    os.makedirs(os.path.join(covdir, "uncovered"), exist_ok=True)
    with open(os.path.join(covdir, "summary.txt"), "w") as out:
        def p(s=""):
            out.write(s + "\n")
        p("Host unit-suite coverage of the release build (%d test programs, %d profiles)" % (nprog, len(raws)))
        p()
        p("%-40s %15s %17s %15s" % ("", "lines", "branches", "functions"))
        p("%-40s %7d/%-6d %s %7d/%-6d %s %6d/%-5d %s" % ("TOTAL", T[1], T[0], pct(T[1], T[0]), T[3], T[2],
                                                      pct(T[3], T[2]), T[5], T[4], pct(T[5], T[4])))
        for d in DIRS:
            s = by_dir.get(d)
            if s:
                p("%-40s %7d/%-6d %s %7d/%-6d %s %6d/%-5d %s" % ("guest/" + d, s[1], s[0], pct(s[1], s[0]), s[3],
                                                              s[2], pct(s[3], s[2]), s[5], s[4], pct(s[5], s[4])))
        if args.gcc_lcov:
            p("%-40s %7d/%-6d %s %7d/%-6d %s %6d/%-5d %s" % ("  of which the clang suites alone", C[1], C[0], pct(C[1], C[0]),
                                                          C[3], C[2], pct(C[3], C[2]), C[5], C[4], pct(C[5], C[4])))
        else:
            p("  (clang suites only: the gcc half did not run)")
        cl = sum(len(v) for v in compiled.values())
        cc = sum(1 for f, v in compiled.items() for l in v if hits.get((f, l), 0) > 0)
        p()
        p("Of the release lines the suites compile at all: %d/%d %s" % (cc, cl, pct(cc, cl)))
        p("Lines the suites run that the release build compiles out (development-only): %d" % len(outside))
        if unmapped_missing:
            p("Copies whose source was gone before the report (not counted): %d" % len(unmapped_missing))
        p()
        p("Per file (release build):")
        for r in rows:
            p("  %-38s %7d/%-6d %s %7d/%-6d %s %6d/%-5d %s" % ("guest/" + r[0], r[2], r[1], pct(r[2], r[1]), r[4], r[3],
                                                            pct(r[4], r[3]), r[6], r[5], pct(r[6], r[5])))
    with open(os.path.join(covdir, "functions.tsv"), "w") as out:
        out.write("file\tfunction\tfirst\tlast\tlines\tcovered\tbranches\tbranches_covered\tcalled\n")
        for rel, name, a, z in ufn:
            ls = [l for l in uda.get(rel, ()) if a <= l <= z]
            lc = sum(1 for l in ls if hits.get((rel, l), 0) > 0)
            bt = sum(ubr.get((rel, l), 0) for l in ls)
            bc = sum(min(ubr.get((rel, l), 0), sum(br.get((rel, l)) or [])) for l in ls)
            called = 1 if (fncalled.get((rel, a), 0) > 0 or lc > 0) else 0
            out.write("guest/%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n" % (rel, name, a, z, len(ls), lc, bt, bc, called))
            if called and lc < len(ls):
                with open(os.path.join(covdir, "uncovered", rel.replace("/", "_") + ".txt"), "a") as u:
                    miss = sorted(l for l in ls if hits.get((rel, l), 0) == 0)
                    u.write("%s (%d/%d): %s\n" % (name, lc, len(ls), ",".join(map(str, miss))))
    with open(os.path.join(covdir, "coverage.json"), "w") as out:
        json.dump({"totals": {"lines": [T[1], T[0]], "branches": [T[3], T[2]], "functions": [T[5], T[4]]},
                   "files": {"guest/" + r[0]: {"lines": [r[2], r[1]], "branches": [r[4], r[3]],
                                               "functions": [r[6], r[5]]} for r in rows},
                   "compiled": [cc, cl]}, out, indent=1)
    with open(os.path.join(covdir, "summary.txt")) as f:
        sys.stdout.write("".join(f.readlines()[:10]))
    print("coverage: full report in %s (summary.txt, functions.tsv, coverage.lcov, uncovered/)" % covdir)
    if not objs:
        print("coverage: no release-build denominator (see above): totals cover nothing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
