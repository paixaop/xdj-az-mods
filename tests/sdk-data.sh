#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Asset-independent tests for the stored-track API. Same production headers as the full suite.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-data.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
cp src/azmod/{sdk_data_engine.h,sdk_analysis.h,azmod-mod.h,hc_anlz.h} "$task_tmp/"
${HOST_CC:-cc} "${flags[@]}" -I"$task_tmp" -Isrc/azmod tests/unit/sdkdata_test.c -lm -o "$task_tmp/real"
${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod tests/unit/sdkwaveform_test.c -o "$task_tmp/waveform"
"$task_tmp/waveform"
"$task_tmp/real"
bash tests/sdk-data-lifecycle.sh
# Compile the actual source identity publication on every host. Deterministic
# stat/mount fixtures exercise exact tuple equality and terminal exhaustion.
python3 tests/unit/sdk_source_identity_extract.py "$task_tmp"
${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod -DSOURCE_IDENTITY_INC="\"$task_tmp/source-identity.inc\"" tests/unit/sdksourceidentity_test.c -o "$task_tmp/source-identity"
"$task_tmp/source-identity"
for control in source-tuple source-wrap; do
    ${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod -DSOURCE_IDENTITY_INC="\"$task_tmp/$control.inc\"" tests/unit/sdksourceidentity_test.c -o "$task_tmp/$control"
    if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: $control control was not caught" >&2; exit 1
    fi
    grep -qi 'assert' "$task_tmp/$control.log" || { cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: $control"
done
python3 tests/unit/sdk_review_controls.py src/azmod "$task_tmp/review"
for control in timing-size timing-version; do
    ${HOST_CC:-cc} "${flags[@]}" -I"$task_tmp/review/$control" -Isrc/azmod tests/unit/sdkdata_test.c -lm -o "$task_tmp/$control"
    if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: $control control was not caught" >&2; exit 1
    fi
    echo "CONTROL caught: $control"
done
if [ "${SAN:-plain}" = asan ] && "${HOST_CC:-cc}" --version | grep -qi clang; then
    ${HOST_CC:-cc} "${flags[@]}" -I"$task_tmp/review/timing-prefix-read" -Isrc/azmod tests/unit/sdkdata_test.c -lm -o "$task_tmp/timing-prefix-read"
    if ( ulimit -c 0; "$task_tmp/timing-prefix-read" >"$task_tmp/timing-prefix-read.log" 2>&1 ) 2>/dev/null; then
        echo 'FAIL: short-prefix object access control was not caught' >&2; exit 1
    fi
    grep -q 'runtime error:' "$task_tmp/timing-prefix-read.log" || { echo 'FAIL: short-prefix control lacked its UBSan diagnostic' >&2; exit 1; }
    echo 'CONTROL caught: timing-prefix-read'
fi
if [ "$(uname -s)" = Linux ]; then
    provider_flags=("${flags[@]}" -Wno-unused-function -Wno-missing-field-initializers -rdynamic -DLIB_INC='"sdk_lib.c"' -Itests/unit)
    ${HOST_CC:-cc} "${provider_flags[@]}" -Isrc/azmod tests/unit/sdktrack_test.c -lsqlite3 -ldl -lm -o "$task_tmp/provider"
    "$task_tmp/provider"
    for control in database-alias database-uri missing-parent wave-errors analysis-binding track-fifo wave-fifo analysis-path-limit preview-extension; do
        ${HOST_CC:-cc} "${provider_flags[@]}" -I"$task_tmp/review/$control" -Isrc/azmod tests/unit/sdktrack_test.c -lsqlite3 -ldl -lm -o "$task_tmp/$control"
        if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
            echo "FAIL: $control control was not caught" >&2; exit 1
        fi
        echo "CONTROL caught: $control"
    done
else
    echo 'SKIP: SQLite disk provider and its controls require Linux (run tests/run-linux.sh)'
fi
# Compile the actual public deck snapshot service, keeping its independent stream clocks
# and earlier development output prefix under the same sanitizer mode.
awk '/^static int sd_native_load_read\(/,/^}/; /^static int sd_native_cue_read\(/,/^}/; /^static uint32_t sd_live_fields\(/,/^}/; /^static int sd_deck_snapshot_v1\(/,/^}/' src/azmod/sdk_deck.c > "$task_tmp/deck.inc"
${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod -DDECK_DATA_INC="\"$task_tmp/deck.inc\"" tests/unit/sdkdeckdata_test.c -o "$task_tmp/deck"
"$task_tmp/deck"
python3 - "$task_tmp/deck.inc" "$task_tmp/deck-control.inc" <<'PY'
import sys
s = open(sys.argv[1]).read()
old = 'memcpy(out, &v, bytes);'
assert s.count(old) == 1
open(sys.argv[2], 'w').write(s.replace(old, 'memcpy(out, &v, bytes + (sizeof v - bytes));'))
PY
${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod -DDECK_DATA_INC="\"$task_tmp/deck-control.inc\"" tests/unit/sdkdeckdata_test.c -o "$task_tmp/deck-control"
if ( "$task_tmp/deck-control" >"$task_tmp/deck-control.log" 2>&1 ) 2>/dev/null; then
    echo 'FAIL: deck output prefix control was not caught' >&2; exit 1
fi
echo 'CONTROL caught: overwriting an earlier deck snapshot prefix'
# Private native/public boundary: use the real versioned exports, not a mock ABI.
python3 tests/unit/sdk_observation_extract.py "$task_tmp"
for test in observation loop-export; do
    fixture=sdkobservation_test.c; macro=OBS_INC
    [ "$test" != loop-export ] || { fixture=sdkloopexport_test.c; macro=LOOP_EXPORT_INC; }
    ${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod -D$macro="\"$task_tmp/$test.inc\"" "tests/unit/$fixture" -o "$task_tmp/$test"
    "$task_tmp/$test"
    controls=(observation-size observation-padding)
    [ "$test" != loop-export ] || controls=(loop-export-v1-size loop-export-v2-size)
    for control in "${controls[@]}"; do
        ${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod -D$macro="\"$task_tmp/$control.inc\"" "tests/unit/$fixture" -o "$task_tmp/$control"
        if ( "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
            echo "FAIL: private boundary control $control was not caught" >&2; exit 1
        fi
        echo "CONTROL caught: $control"
    done
done
# A deliberately broken source-qualified identity must make the same test fail.
python3 - "$task_tmp/sdk_data_engine.h" <<'PY'
import sys
p=sys.argv[1]
s=open(p).read()
a=s.index('    return a->source_id == b->source_id')
b=s.index('\n}',a)
s=s[:a]+'    return a->track_id==b->track_id;'+s[b:]
open(p,'w').write(s)
PY
${HOST_CC:-cc} "${flags[@]}" -I"$task_tmp" -Isrc/azmod tests/unit/sdkdata_test.c -lm -o "$task_tmp/control"
if ( "$task_tmp/control" >"$task_tmp/control.log" 2>&1 ) 2>/dev/null; then
    echo 'FAIL: source identity control was not caught' >&2; exit 1
fi
echo 'CONTROL caught: merging duplicate track IDs across sources'
# Optional user-owned fixtures stay outside the contribution. No bytes are copied here.
if [ -n "${ANLZ_SAMPLE_DIR:-}" ]; then
    ${HOST_CC:-cc} "${flags[@]}" -I"$task_tmp" -Isrc/azmod tests/unit/sdkrealanlz_probe.c -lm -o "$task_tmp/real-files"
    sample_args=("$ANLZ_SAMPLE_DIR/ANLZ0000.DAT" "$ANLZ_SAMPLE_DIR/ANLZ0000.EXT")
    [ ! -f "$ANLZ_SAMPLE_DIR/ANLZ0000.2EX" ] || sample_args+=("$ANLZ_SAMPLE_DIR/ANLZ0000.2EX")
    "$task_tmp/real-files" "${sample_args[@]}"
    ${HOST_CC:-cc} "${flags[@]}" tests/unit/sdkwaveprovider_probe.c -lm -o "$task_tmp/real-windows"
    window_args=("$ANLZ_SAMPLE_DIR/ANLZ0000.EXT")
    [ ! -f "$ANLZ_SAMPLE_DIR/ANLZ0000.2EX" ] || window_args+=("$ANLZ_SAMPLE_DIR/ANLZ0000.2EX")
    "$task_tmp/real-windows" "${window_args[@]}"
fi
