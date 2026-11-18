#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# busread-trace.sh [GUEST_DIR] [OUT_DIR]: replay the panel-bus read path and compare the
# trace's MD5 with busread-trace.md5 (see busread_trace_test.c). Exit 0 = the same behavior.
# BUSTRACE_HARNESS names another harness file (the suite's controls use it).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
G=$(cd "${1:-$here/../../guest}" && pwd)
O=${2:-$(mktemp -d)}
mkdir -p "$O"
f="$G/azmod/bus_interpose.c"
awk '/^\/\* ---- read\(\): the panel input buses/ {p=1} p' "$f" > "$O/read_path.inc"
awk '/^static uint16_t crc16_x25\(/ {p=1} p {print} p && /^}/ {exit}' "$f" > "$O/crc.inc"
[ -s "$O/read_path.inc" ] && [ -s "$O/crc.inc" ] || { echo "busread-trace: regions not found in $f"; exit 2; }
${CC:-cc} -O1 -w -I"$G/azmod" -I"$O" -DBI_INC='"read_path.inc"' -DCRC_INC='"crc.inc"' \
    -o "$O/busread_trace" "${BUSTRACE_HARNESS:-$here/busread_trace_test.c}"
if [ "${AZ_COMPILE_ONLY:-0}" = 1 ]; then # built, not replayed (run.sh's compile-only pass)
    exit 0
fi
got=$("$O/busread_trace" 400000 | { md5sum 2>/dev/null || md5 -r; } | cut -c1-32)
want=$(cut -c1-32 "$here/busread-trace.md5")
if [ "$got" = "$want" ]; then
    echo "PASS  busread-trace: 400000 frames, the read path behaves as recorded ($got)"
else
    echo "FAIL  busread-trace: trace $got, recorded $want (a behavior change in the read path)"
    exit 1
fi
