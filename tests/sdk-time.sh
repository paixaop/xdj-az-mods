#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Source-clock quantization, grid interpolation and receipt-clock arithmetic.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-time.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
cflags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
cppflags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -Isrc/common)
san=()
case ${SAN:-plain} in
plain|'') ;;
asan) san=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) san=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
${HOST_CC:-cc} "${cflags[@]}" "${san[@]}" -Isrc/common tests/unit/timestamp_test.c -lm -o "$task_tmp/timestamps-c"
"$task_tmp/timestamps-c"
${HOST_CXX:-c++} "${cppflags[@]}" "${san[@]}" -x c++ -Isrc/common tests/unit/timestamp_test.c -lm -o "$task_tmp/timestamps-cpp"
"$task_tmp/timestamps-cpp"
python3 tests/unit/sdk_timestamps_test.py
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import sys
root=Path(sys.argv[1])
(root/'common').mkdir()
(root/'common/aztimestamp.h').write_bytes(Path('src/common/aztimestamp.h').read_bytes())
s=Path('src/azstem/sdk_loop_observer.cpp').read_text()
a=s.index('bool source_time_consistent_us(')
b=s.index('\n}',a)+2
helper=s[a:b]+'\n'
(root/'sample.inc').write_text(helper)
variants=[
 ('strict', 'return compare_timestamps(2u * source, 2u * cached, 1, 1, UINT64_C(2000000) + rate + 1) == AZ_TIME_EQUAL;', 'return source == cached;'),
 ('fixed', 'UINT64_C(2000000) + rate + 1', 'UINT64_C(10) * rate + 1'),
 ('boundary', 'UINT64_C(2000000) + rate + 1', 'UINT64_C(2000000) + rate'),
 ('narrow', 'unsigned __int128', 'std::uint64_t'),
]
for name,old,new in variants:
 assert helper.count(old)==1,(name,old)
 (root/(name+'.inc')).write_text(helper.replace(old,new))
source=Path('src/azmod/sdk_input.c').read_text()
input_time='#define SDK_FILTER_BUDGET_US 200\n'
for name in ['sdk_monotonic_ticks','sdk_budget_for_frequency','sdk_elapsed_ticks']:
 a=source.index('static inline uint64_t '+name+'(')
 b=source.index('\n}',a)+2
 input_time+=source[a:b]+'\n'
assert '#define SDK_FILTER_BUDGET_US 200 ' in source
(root/'input-time.inc').write_text(input_time)
for name,old,new in [
 ('input-clock','if (clock_gettime(CLOCK_MONOTONIC, &ts) ||','if ((clock_gettime(CLOCK_MONOTONIC, &ts), 0) ||'),
 ('input-budget','return (frequency / UINT64_C(1000000)) * SDK_FILTER_BUDGET_US +\n           (frequency % UINT64_C(1000000)) * SDK_FILTER_BUDGET_US / UINT64_C(1000000);','return frequency / UINT64_C(1000000) * SDK_FILTER_BUDGET_US;'),
 ('input-elapsed','return start && compare_timestamps(end, start, 1, 1, 1) >= AZ_TIME_EQUAL ? end - start : 0;','return end - start;'),
]:
 assert input_time.count(old)==1,(name,old)
 (root/(name+'.inc')).write_text(input_time.replace(old,new))
for name,filename,old,new in [
 ('clock', 'sdk_data_engine.h', 'if (clock_gettime(CLOCK_MONOTONIC, &ts) ||', 'if ((clock_gettime(CLOCK_MONOTONIC, &ts), 0) ||'),
 ('grid', 'sdk_analysis.h', 'if (!isfinite(first) || !isfinite(last) || !isfinite(span) || !(compare_timestamps(span, 0., 1, 1000, 1) == AZ_TIME_AFTER))', 'if (0)'),
 ('interval', 'sdk_analysis.h', '(source_order == AZ_TIME_EQUAL || fpclassify(playback) != FP_ZERO)', '(fpclassify(playback) != FP_ZERO)'),
]:
 target=root/name;target.mkdir()
 for dep in ['sdk_data_engine.h','sdk_analysis.h','azmod-mod.h','hc_anlz.h']:
  (target/dep).write_bytes((Path('src/azmod')/dep).read_bytes())
 p=target/filename;s=p.read_text();assert s.count(old)==1,(name,old)
 p.write_text(s.replace(old,new))
PY
${HOST_CXX:-c++} "${cppflags[@]}" "${san[@]}" -DSAMPLE_TIME_IMPL="\"$task_tmp/sample.inc\"" tests/unit/sdksampletime_test.cpp -lm -o "$task_tmp/sample"
"$task_tmp/sample"
${HOST_CC:-cc} "${cflags[@]}" "${san[@]}" -DINPUT_TIME_IMPL="\"$task_tmp/input-time.inc\"" -Isrc/azmod tests/unit/sdktime_test.c -lm -o "$task_tmp/time"
for mode in clock budget elapsed grid interval; do "$task_tmp/time" "$mode"; done
${HOST_CC:-cc} "${cflags[@]}" "${san[@]}" -Wno-unused-function -Wno-missing-field-initializers -Isrc/azmod -Itests/unit -DSDK_INPUT_C="\"$PWD/src/azmod/sdk_input.c\"" tests/unit/sdkinput_test.c -lm -o "$task_tmp/input"
"$task_tmp/input"
for control in strict fixed boundary narrow; do
    ${HOST_CXX:-c++} "${cppflags[@]}" -DSAMPLE_TIME_IMPL="\"$task_tmp/$control.inc\"" tests/unit/sdksampletime_test.cpp -lm -o "$task_tmp/$control"
    if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: sample-time $control control was not caught" >&2; exit 1
    fi
    grep -qi 'assert' "$task_tmp/$control.log" || { cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: sample-time $control"
done
for control in input-clock input-budget input-elapsed; do
    mode=${control#input-}
    ${HOST_CC:-cc} "${cflags[@]}" -DINPUT_TIME_IMPL="\"$task_tmp/$control.inc\"" -Isrc/azmod tests/unit/sdktime_test.c -lm -o "$task_tmp/$control"
    if ( ulimit -c 0; "$task_tmp/$control" "$mode" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: time $control control was not caught" >&2; exit 1
    fi
    grep -qi 'assert' "$task_tmp/$control.log" || { cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: time $control"
done
for control in clock grid interval; do
    ${HOST_CC:-cc} "${cflags[@]}" -I"$task_tmp/$control" tests/unit/sdktime_test.c -lm -o "$task_tmp/time-$control"
    if ( ulimit -c 0; "$task_tmp/time-$control" "$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: time $control control was not caught" >&2; exit 1
    fi
    grep -qi 'assert' "$task_tmp/$control.log" || { cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: time $control"
done
