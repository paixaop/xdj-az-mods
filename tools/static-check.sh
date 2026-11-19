#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# static-check.sh: clang-tidy and cppcheck over the mods sources; fails on any finding that is
# not suppressed with a written reason.
#
#   tools/static-check.sh                 every translation unit of the release build
#   tools/static-check.sh --as TU FILE    FILE (a copy of the unit TU, e.g. azhandoff/az-handoff-guard.c,
#                                         under guest/ or src/) with TU's compile flags: the controls' mode
#
# Scope: the release (AZMOD_RELEASE) translation units of azmod.so, azstem.so and the handoff guard,
# with everything they #include in guest/azmod, guest/azstem, guest/common, guest/azhandoff
# and guest/analysis. Vendored stb_vorbis and analysis dependencies are not analyzed. In the published source
# layout the same sources sit in src/ (with src/Makefile and src/.clang-tidy): the script uses
# src/ when there is no guest/.
#
# The compile flags are guest/Makefile's real lines: `make -n -B azmod azstem azhandoff CC=gcc`,
# turned into a compile_commands.json by tools/static-check/compdb.py. Builds run natively on
# arm64 (the azmod-test:18.04 image), so the target's headers are the image's own; no sysroot.
#
# What is checked, and what is suppressed and why:
#   clang-tidy 10   guest/.clang-tidy (bugprone-*, clang-analyzer-*, cert-* minus the listed checks)
#   cppcheck 1.82   --enable=warning,portability,performance, tools/static-check/cppcheck.supp
# Both come from docker/azmod-test.Dockerfile (pinned). Where they come from, first match wins:
#   1. clang-tidy-10 and cppcheck 1.82 on PATH (inside azmod-test:18.04, e.g. run-linux.sh)
#   2. the azmod-test:18.04 docker image: the script re-runs itself in it, the repository mounted
# Neither: exit 2. A missing analyzer never passes.
#
# Exit: 0 no findings, 1 findings (each printed as TIDY or CPPCHECK with file:line), 2 could not run.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=azmod-test:18.04
SC="$ROOT/tools/static-check"
S=guest
[ -d "$ROOT/guest" ] || [ ! -f "$ROOT/src/Makefile" ] || S=src
die() { echo "STATIC CHECK COULD NOT RUN: $*" >&2; exit 2; }

as_tu= as_file=
if [ "${1:-}" = "--as" ]; then
    [ $# -eq 3 ] || die "usage: $0 --as <tu, relative to $S/> <file>"
    as_tu=$2
    as_file="$(cd "$(dirname "$3")" && pwd)/$(basename "$3")"
    case "$as_file" in "$ROOT/$S"/*) ;; *) die "$3 is not under $ROOT/$S (the .clang-tidy and docker's mount)";; esac
elif [ $# -ne 0 ]; then
    die "usage: $0 [--as <tu> <file>]"
fi

tools_ok() {
    command -v clang-tidy-10 >/dev/null 2>&1 && clang-tidy-10 --version 2>/dev/null | grep -q 'LLVM version 10\.0\.0' &&
        command -v cppcheck >/dev/null 2>&1 && cppcheck --version 2>/dev/null | grep -q '^Cppcheck 1\.82$'
}
if ! tools_ok; then
    [ -z "${STATIC_CHECK_IN_DOCKER:-}" ] || die "clang-tidy-10 / cppcheck 1.82 missing inside $IMG (rebuild it: docker/azmod-test.Dockerfile)"
    command -v docker >/dev/null 2>&1 || die "no clang-tidy-10 + cppcheck 1.82 on PATH and no docker"
    docker image inspect "$IMG" >/dev/null 2>&1 ||
        die "no clang-tidy-10 + cppcheck 1.82 on PATH and no $IMG image; build it:
    docker build --platform=linux/arm64 -t $IMG -f docker/azmod-test.Dockerfile docker"
    exec docker run --rm --platform=linux/arm64 -v "$ROOT:$ROOT" -w "$ROOT" -e STATIC_CHECK_IN_DOCKER=1 "$IMG" \
        bash "$ROOT/tools/static-check.sh" ${as_tu:+--as "$as_tu" "$as_file"}
fi

cd "$ROOT/$S"
[ -f .clang-tidy ] || die "no $S/.clang-tidy"
[ -f "$SC/cppcheck.supp" ] || die "no tools/static-check/cppcheck.supp"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
LC_ALL=C make --print-directory -n -B azmod azstem azhandoff CC=gcc > "$W/make-n.txt" 2>"$W/make-n.err" || { cat "$W/make-n.err"; die "make -n failed"; }
python3 "$SC/compdb.py" "$PWD" < "$W/make-n.txt" > "$W/full.json" || die "compdb.py failed"

# the units to analyze: every entry, or (--as) the copy with its unit's flags and include path
python3 - "$W/full.json" "$W/compile_commands.json" "$as_tu" "$as_file" > "$W/units.txt" <<'EOF' || die "no such unit: $as_tu"
import json, os, sys
src, dst, tu, copy = sys.argv[1:5]
db = json.load(open(src))
if tu:
    e = [x for x in db if x["file"] == tu]
    if not e:
        sys.exit(1)
    e = dict(e[0])
    # the copy's quoted #includes resolve beside the unit first, as they would for the original
    a = e["arguments"][:-2] + ["-I" + os.path.dirname(tu), "-c", copy]
    db = [{"directory": e["directory"], "file": copy, "arguments": a}]
json.dump(db, open(dst, "w"), indent=1)
for x in db:
    print(x["file"])
EOF

echo "static-check: $(clang-tidy-10 --version | grep -o 'LLVM version [0-9.]*'), $(cppcheck --version)"

# ---- clang-tidy: every unit, two at a time. The atomic-compat header lets clang compile the
# __atomic_* builtins applied to _Atomic objects, which GCC accepts (see the header).
HF=".*/$S/(azmod|azstem|common|azhandoff|analysis)/.*"
export W HF SC
xargs -P2 -I{} sh -c 'f="$1"; clang-tidy-10 -p "$W" --quiet -header-filter="$HF" \
    -extra-arg=-include"$SC/clang-atomic-compat.h" "$f" > "$W/tidy.$(echo "$f" | tr / _).log" 2>&1; \
    echo $? > "$W/tidy.$(echo "$f" | tr / _).rc"' _ {} < "$W/units.txt"
: > "$W/tidy.all"
while IFS= read -r f; do
    k=$(echo "$f" | tr / _)
    [ -f "$W/tidy.$k.log" ] || die "clang-tidy did not run on $f"
    # a diagnostic line: path:line:col: warning|error: text [check]
    grep -E '^[^ ]+:[0-9]+:[0-9]+: (warning|error): ' "$W/tidy.$k.log" >> "$W/tidy.all" || true
    rc=$(cat "$W/tidy.$k.rc")
    if [ "$rc" != 0 ] && ! grep -qE ': (warning|error): ' "$W/tidy.$k.log"; then
        cat "$W/tidy.$k.log"; die "clang-tidy exited $rc on $f with no diagnostic"
    fi
done < "$W/units.txt"
# one finding is printed once, though every unit that includes its header reports it
sed -E "s|^$PWD/||" "$W/tidy.all" | sort -u | sed 's/^/TIDY  /' > "$W/tidy.out"

# ---- cppcheck: the same units, the same -D/-I (system include dirs left out: cppcheck reads
# the libc and POSIX APIs from its own --library files, not from headers), the compiler's
# predefined target macros, explicit suppressions.
python3 - "$W/compile_commands.json" > "$W/cpp.args" <<'EOF'
import json, sys
for e in json.load(open(sys.argv[1])):
    keep = [a for a in e["arguments"] if a.startswith("-D") or (a.startswith("-I") and not a.startswith("-I/usr/"))]
    # GCC defines this in C when unwind cleanup is enabled. Cppcheck does not
    # infer compiler predefined macros from code-generation flags.
    unwind = [a for a in e["arguments"] if a in ("-fexceptions", "-fno-exceptions")]
    if unwind and unwind[-1] == "-fexceptions":
        keep.append("-D__EXCEPTIONS=1")
    print("\t".join(keep + [e["file"]]))
EOF
: > "$W/cpp.all"
while IFS=$'\t' read -r -a a; do
    cppcheck -q --enable=warning,portability,performance --std=c11 --platform=unix64 \
        --library=posix --library=gnu -D__aarch64__=1 -D__linux__=1 -D__GNUC__=7 \
        --suppressions-list="$SC/cppcheck.supp" \
        --template='{file}:{line}: cppcheck-{id}: {severity}: {message}' "${a[@]}" >> "$W/cpp.all" 2>&1 ||
        die "cppcheck failed on ${a[-1]}"
done < "$W/cpp.args"
sed -E "s|^$PWD/||" "$W/cpp.all" | sort -u | sed 's/^/CPPCHECK  /' > "$W/cpp.out"

nt=$(wc -l < "$W/tidy.out" | tr -d ' ')
nc=$(wc -l < "$W/cpp.out" | tr -d ' ')
cat "$W/tidy.out" "$W/cpp.out"
nu=$(wc -l < "$W/units.txt" | tr -d ' ')
if [ "$nt" -ne 0 ] || [ "$nc" -ne 0 ]; then
    echo "STATIC CHECK: FAILED ($nt clang-tidy, $nc cppcheck findings in $nu units; fix each, or suppress it with its reason: $S/.clang-tidy, tools/static-check/cppcheck.supp)"
    exit 1
fi
echo "STATIC CHECK: OK ($nu units, clang-tidy and cppcheck clean)"
