#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Run in the normal Linux arm64 core build environment.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
src=$root/src/azmod
sdk_include=${AZ_SDK_INCLUDE:-$src}
stage=$work/root/az-usbload/community
mkdir -p "$stage/status-watch" "$work/root/azmod-community"
cc -O2 -no-pie -I"$src" "-DCM_ROOT=\"$work/root\"" "-DCM_LOGDIR=\"$work/log\"" \
  "$src/test/community/harness.c" -pthread -ldl -lm -rdynamic -o "$work/host"
cc -O2 -fPIC -shared -fvisibility=hidden -I"$sdk_include" "$src/test/community/status_mod.c" \
  -o "$stage/status-watch/status-watch.so"
printf '%s\n' '{"azmod":1,"id":"status-watch","name":"Status watch","version":"1.0.0","author":"t","summary":"status test","source":"https://example.com/x","api":1,"firmware":["any"],"hooks":false}' > "$stage/status-watch/azmod.json"
echo status-watch > "$stage/enabled"
touch "$work/root/azmod-community/.approved-init"
echo "status-watch $(sha256sum < "$stage/status-watch/status-watch.so" | cut -d' ' -f1)" > "$work/root/azmod-community/approved"
"$work/host" init status > "$work/trace"
grep 'MOD status-watch state=1 screen=0' "$work/trace"
grep '^LOOP ' "$work/trace" > "$work/events"
printf 'LOOP 1 -1 1\nLOOP 2 0 3\nLOOP 2 1 4\n' > "$work/want"
diff -u "$work/want" "$work/events"
cat "$work/events"
mkdir -p "$stage/status-motion"
cc -O2 -fPIC -shared -fvisibility=hidden -I"$sdk_include" "$src/test/community/status_motion_mod.c" \
  -o "$stage/status-motion/status-motion.so"
sed 's/status-watch/status-motion/g' "$stage/status-watch/azmod.json" > "$stage/status-motion/azmod.json"
echo status-motion > "$stage/enabled"
echo "status-motion $(sha256sum < "$stage/status-motion/status-motion.so" | cut -d' ' -f1)" > "$work/root/azmod-community/approved"
"$work/host" init cadence > "$work/cadence"
for hz in 1 30; do
  grep "^MOTION $hz " "$work/cadence" > "$work/events"
  python3 - "$root/tests/integration" "$work/events" "$hz" <<'PY'
import pathlib, sys
sys.path.insert(0, sys.argv[1])
from sdk_timestamps import EQUAL, compare_timestamps
rows = pathlib.Path(sys.argv[2]).read_text().splitlines()
expected = [(1, -1, 0), (2, 0, 0), (2, 1, 0), (2, 20, 1)]
assert len(rows) == len(expected), rows
for row, (kind, seconds, ordinal) in zip(rows, expected):
    tag, hz, actual_kind, actual_seconds, actual_ordinal = row.split()
    assert (tag, hz, int(actual_kind), int(actual_ordinal)) == ('MOTION', sys.argv[3], kind, ordinal), row
    # The controlled writer declares a 64-frame 44.1 kHz observation.
    assert compare_timestamps(float(actual_seconds), seconds, units_per_second=1,
                              sample_rate=44100, window_samples=64) == EQUAL, row
PY
  cat "$work/events"
done
