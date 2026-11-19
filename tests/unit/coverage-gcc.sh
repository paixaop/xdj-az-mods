#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# coverage-gcc.sh <work-dir> : the coverage pass's gcc half (COVERAGE=1 tools/unit-tests/run.sh).
#
# The suites clang cannot build (gcc's __atomic builtins on _Atomic objects, aarch64 page tables,
# real mod libraries) are measured with gcc 7.5's --coverage in the repository's own containers
# (linux/arm64), from this tree:
#   linux     readers (tc, mixch, vb), f4ident, fd, sdkproc, sdkvslot: the same run.sh sections,
#             cut out of tools/unit-tests/run.sh by their headers, in azmod-test:18.04
#   hooks     guest/azmod/test/hooks/run.sh (hooktest + crashchain), in azmod-build:18.04
#   altstack  guest/common/azaltstack_test.c, as run.sh's alttest builds it
#   community guest/azmod/test/community/run.sh (the host and community.c; the mods plain)
# The compiler is coverage-gcc-cc.sh: one .gcno/.gcda per output binary, control variants and
# shared objects built plain, a fatal signal flushing the counters first. gcov -b -c -p per
# object, then coverage-gcc-map.py attributes every line to its guest/ file (extracted copies
# aligned back by text) and writes coverage.info (lcov), which coverage-report.py merges with
# the clang numbers.
# Controls, in the same round: the hook suites rebuilt without the counter flush must read
# crash_guards.c's crash path as never run, and the mapping without the sdkvslot suite must lose
# exactly sdk_vslot.c's lines. <work-dir> must be a fresh absolute directory; it is
# mounted separately so evidence can live outside the repository. Development-only
# hook/crash controls are explicitly skipped when absent from a published tree.
# Exit 0 = every available suite and applicable control behaved.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
[ $# -eq 1 ] || { echo "usage: $0 <work-dir>"; exit 2; }
W=$1
case $W in /*) ;; *) echo "coverage-gcc: use an absolute fresh work directory"; exit 2 ;; esac
[ ! -e "$W" ] || { echo "coverage-gcc: preserve existing evidence; choose a new work directory"; exit 2; }
mkdir -p "$W/bin" "$W/tmp" "$W/gcov" "$W/nof" "$W/logs"
T=tools/unit-tests
cp $T/coverage-gcc-cc.sh "$W/bin/covcc"; chmod +x "$W/bin/covcc"

# gcov-all.sh <dir> <gcov-out-root> <label>|auto : one gcov run per .gcda, each in its own dir
cat > "$W/bin/gcov-all.sh" <<'GC_EOF'
dir=$1; root=$2; label=$3
find "$dir" -name '*.cov*.gcda' | sort | while read -r g; do
    o=${g%.gcda}.o; b=$(basename "${g%.gcda}")
    if [ "$label" = auto ]; then
        s=$(basename "$(dirname "$g")"); [ "$s" = readers ] && s=readers-$(echo "$b" | cut -d. -f1)
    else s=$label; fi
    d="$root/$s/$b"; mkdir -p "$d"
    (cd "$d" && gcov -b -c -p -o "$o" "$o" > gcov.log 2>&1) || echo "gcov failed: $g"
done
GC_EOF

# ---- 1. the Linux-only host suites, cut from run.sh by section header
RS=$T/run.sh; CR=$W/cov-run.sh
{
    awk '/^# -+ the sanitizer.s own controls/ {exit} {print}' "$RS" |
        sed "s|^trap 'rm -rf \"\$OUT\".*EXIT\$|trap - EXIT|; s|^cd \"\$(dirname \"\$0\")/\.\./\.\.\"\$|cd \"$ROOT\"|"
    awk '/^region\(\) \{/ {p=1} p {print} p && /^\}$/ {exit}' "$RS"
    awk '/^# -+ fd tracking$/ {p=1; print; next} p && /^# -+ / {exit} p {print}' "$RS"
    awk '/^# -+ reader count/ {p=1} p && /^# -+ deck identity/ {exit} p {print}' "$RS"
    awk '/^# sdk_linux <dir>/ {p=1} p {print} p && /^suite "sdk: claim_vslot"/ {exit}' "$RS"
    echo 'echo "OUT=$OUT"; echo "COVERAGE SUITES: failures=$failures skipped=$skipped"; exit $failures'
} > "$CR"
for want in 'trap - EXIT' "cd \"$ROOT\"" 'suite "fd tracking"' 'suite "readers: TaglistEditor"' 'suite "readers: mixer Channel"' \
            'suite "readers: VinylSpeedAdjustor"' 'suite "f4 identity"' 'suite "sdk: other programs"' 'suite "sdk: claim_vslot"' '^region()'; do
    grep -q "$want" "$CR" || { echo "coverage-gcc: section cut failed: '$want' not in $CR"; exit 2; }
done
rc=0
echo "coverage-gcc: the Linux-only suites (azmod-test:18.04)"
docker run --rm --platform=linux/arm64 --security-opt seccomp=unconfined -v "$ROOT:$ROOT" -v "$W:$W" -w "$ROOT" \
    -e HOST_CC="$W/bin/covcc" -e TMPDIR="$W/tmp" -e SDK_PROC_TMPDIR=/tmp -e COVERAGE= -e SAN= azmod-test:18.04 bash -c '
        bash "'"$CR"'"; r=$?
        out=$(ls -d "'"$W"'"/tmp/az-unit-tests.* | head -1)
        bash "'"$W"'/bin/gcov-all.sh" "$out" "'"$W"'/gcov" auto; exit $r' > "$W/logs/linux.log" 2>&1 || rc=1
grep -E '^(FAIL|COVERAGE SUITES|gcov failed)' "$W/logs/linux.log" | tail -5 || true

# ---- 2. hooks + altstack; the hook suites again without the flush (the control)
hooks() { # hooks <gcov-root> <noflush 0|1>
    docker run --rm --platform=linux/arm64 -v "$ROOT/guest":/g:ro -v "$W:$W" -e CC="$W/bin/covcc" \
        -e COVCC_NOFLUSH="$2" azmod-build:18.04 bash -c '
        cd /g
        if [ -f azmod/test/hooks/run.sh ]; then
            bash azmod/test/hooks/run.sh; r=$?
        else
            r=77
            echo "SKIP: development hook/crash-chain fixture is absent from the published source tree"
        fi
        "'"$W"'/bin/covcc" -O0 -pthread -I/g/common -o /tmp/alt /g/common/azaltstack_test.c && /tmp/alt; a=$?
        mkdir -p /tmp/hk /tmp/al
        if [ "$r" != 77 ]; then mv /tmp/hooktest.cov* /tmp/crashchain.cov* /tmp/hk/; fi
        mv /tmp/alt.cov* /tmp/al/
        bash "'"$W"'/bin/gcov-all.sh" /tmp/hk "'"$1"'" hooks
        bash "'"$W"'/bin/gcov-all.sh" /tmp/al "'"$1"'" altstack
        echo "HOOKS rc=$r ALTSTACK rc=$a"; { [ "$r" = 0 ] || [ "$r" = 77 ]; } && [ "$a" = 0 ]'
}
echo "coverage-gcc: hooks + altstack (azmod-build:18.04)"
hooks "$W/gcov" 0 > "$W/logs/hooks.log" 2>&1 || rc=1
grep -E '^SKIP:|HOOKS rc|gcov failed' "$W/logs/hooks.log" | tail -3 || true
hooks "$W/nof" 1 > "$W/logs/hooks-noflush.log" 2>&1 || true

# ---- 3. the Community Mods host
echo "coverage-gcc: community host (azmod-build:18.04)"
docker run --rm --platform=linux/arm64 -v "$ROOT/guest":/g:ro -v "$W:$W" -e CC="$W/bin/covcc" -e HARNESS_CC="$W/bin/covcc" \
    -e COVCC_SKIP='ctl|harness-' azmod-build:18.04 bash -c '
    cd /g && bash azmod/test/community/run.sh; r=$?
    bash "'"$W"'/bin/gcov-all.sh" /tmp/cmtest "'"$W"'/gcov" community
    echo "COMMUNITY rc=$r"; exit $r' > "$W/logs/community.log" 2>&1 || rc=1
grep -E '^FAIL|^passed|COMMUNITY rc|gcov failed' "$W/logs/community.log" | tail -3 || true

# ---- 4. mapped to guest/, and the two controls
docker run --rm --platform=linux/arm64 -v "$ROOT:$ROOT" -v "$W:$W" azmod-test:18.04 bash -c "
    python3 $ROOT/$T/coverage-gcc-map.py $ROOT/guest $W/gcov $W/report > /dev/null &&
    python3 $ROOT/$T/coverage-gcc-map.py $ROOT/guest $W/gcov $W/ctl-novslot --exclude sdkvslot > /dev/null &&
    python3 $ROOT/$T/coverage-gcc-map.py $ROOT/guest $W/nof $W/ctl-noflush > /dev/null" > "$W/logs/map.log" 2>&1 ||
    { echo "coverage-gcc: mapping failed (logs/map.log)"; exit 1; }
sed -n 2,3p "$W/report/summary.txt"
# lines covered in one file of a report (files.tsv: file, covered, total, ...)
cov_of() { awk -F'\t' -v f="$2" '$1 == f {print $2; found=1} END {if (!found) print 0}' "$1/files.tsv"; }
cg=$(cov_of "$W/report" azmod/crash_guards.c); cn=$(cov_of "$W/ctl-noflush" azmod/crash_guards.c)
if [ ! -f "$ROOT/guest/azmod/test/hooks/run.sh" ]; then
    echo "SKIP: crash counter-flush control requires the absent development hook/crash-chain fixture"
elif [ "$cg" -gt 0 ] && [ "$cn" -lt "$cg" ]; then
    echo "CONTROL FAILED AS REQUIRED  coverage-gcc: without the counter flush, crash_guards.c reads $cn of $cg lines"
else
    echo "BROKEN TEST: control PASSED  coverage-gcc: crash_guards.c reads $cn lines without the flush, $cg with it"; rc=1
fi
vs=$(cov_of "$W/report" azmod/sdk_vslot.c); vn=$(cov_of "$W/ctl-novslot" azmod/sdk_vslot.c)
tot=$(awk -F'\t' 'NR > 1 {s += $2} END {print s + 0}' "$W/report/files.tsv")
tnv=$(awk -F'\t' 'NR > 1 {s += $2} END {print s + 0}' "$W/ctl-novslot/files.tsv")
if [ "$vs" -gt 0 ] && [ "$vn" = 0 ] && [ $((tot - tnv)) = "$vs" ]; then
    echo "CONTROL FAILED AS REQUIRED  coverage-gcc: without the sdkvslot suite exactly sdk_vslot.c's $vs lines go"
else
    echo "BROKEN TEST: control PASSED  coverage-gcc: without sdkvslot $((tot - tnv)) lines went, sdk_vslot.c had $vs"; rc=1
fi
exit $rc
