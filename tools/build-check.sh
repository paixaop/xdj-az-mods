#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# build-check.sh [GUEST_DIR]: release build of azmod.so + azstem.so that cannot report a
# stale result: the outputs are deleted first, the build's exit status is checked, and
# the md5s are printed only for files this run produced. (A failed build leaves the old
# .so in place, so an md5 read without these two steps proves nothing.)
# GUEST_DIR defaults to guest/ two levels up (the development layout, and the tree tests/run.sh
# assembles); in the published source, run it as tools/build-check.sh src.
set -euo pipefail
g=${1:-$(dirname "$0")/../../guest}
[ $# -le 1 ] && [ -f "$g/Makefile" ] || {
    echo "usage: $0 [GUEST_DIR]   (GUEST_DIR holds the Makefile; in the published source: src)" >&2
    exit 2
}
g=$(cd "$g" && pwd)
project=$(cd "$g/.." && pwd)
source_dir=$(basename "$g")
[ -f "$project/src/analysis/build.mk" ] && [ -d "$project/vendor/analysis" ] || {
    echo "build-check needs the source and vendor directories from one core checkout" >&2; exit 2;
}
mkdir -p "$g/out"; rm -f "$g/out/azmod.so" "$g/out/azstem.so"
if ! docker run --rm --platform=linux/arm64 -v "$project":/work azmod-build:18.04 make -B -C "/work/$source_dir" CC=gcc azmod azstem >"$g/out/build-check.log" 2>&1; then
    grep -m5 "error" "$g/out/build-check.log" >&2 || true
    echo "BUILD FAILED (log: $g/out/build-check.log)" >&2; exit 1
fi
for f in azmod.so azstem.so; do
    [ -s "$g/out/$f" ] || { echo "BUILD PRODUCED NO $f (make exited 0: missing Makefile or a stale mount?)" >&2; exit 1; }
done
grep -q "Nothing to be done" "$g/out/build-check.log" && { echo "make did nothing: not a build" >&2; exit 1; }
md5 -q "$g/out/azmod.so" 2>/dev/null | sed 's/^/azmod.so  /' || md5sum "$g/out/azmod.so"
md5 -q "$g/out/azstem.so" 2>/dev/null | sed 's/^/azstem.so /' || md5sum "$g/out/azstem.so"
