#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# GNU native-listener model and opposite-outcome controls. No device or firmware needed.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-live.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -fvisibility=hidden -fvisibility-inlines-hidden)
cflags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/azmod -Itests/unit -Wno-unused-function -Wno-unused-variable)
csan=()
case ${SAN:-plain} in
plain|'') ;;
asan) csan=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer); flags+=("${csan[@]}") ;;
tsan) csan=(-fsanitize=thread -fno-omit-frame-pointer); flags+=("${csan[@]}") ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
python3 tests/unit/sdk_master_test.py
python3 -B tests/unit/sdk_bpm_tempo_test.py
python3 tests/unit/sdk_live_extract.py "$task_tmp"
for service in export sync-export deck lifetime probe flags; do
    fixture=sdkliveexport_test.c; macro=LIVE_EXPORT_INC; inc=live-export
    case "$service" in
        sync-export) fixture=sdksyncexport_test.c; macro=SYNC_EXPORT_INC; inc=sync-export ;;
        deck) fixture=sdkdeckdata_test.c; macro=DECK_DATA_INC; inc=live-deck ;;
        lifetime) fixture=lifetime_test.c; macro=LIFE_INC; inc=live-lifetime ;;
        probe) fixture=sdkliveprobe_test.c; macro=LIVE_PROBE_INC; inc=live-probe ;;
        flags) fixture=sdkobserverflags_test.c; macro=OBSERVER_FLAGS_INC; inc=observer-flags ;;
    esac
    ${HOST_CC:-cc} "${cflags[@]}" "${csan[@]}" -DLIVE_CONFIG_INC="\"$task_tmp/live-config.inc\"" -D$macro="\"$task_tmp/$inc.inc\"" "tests/unit/$fixture" -lm -o "$task_tmp/c-$service"
    "$task_tmp/c-$service"
done
for control in live-export-size live-export-version sync-export-size sync-export-version live-deck-prefix live-deck-clock live-deck-valid live-deck-normalized live-deck-master-tempo-valid live-deck-master-tempo-source live-deck-sync-valid live-deck-sync-source live-lifetime-register live-lifetime-retire live-config-required live-probe-clock live-probe-age live-probe-bound; do
    fixture=sdkliveexport_test.c; macro=LIVE_EXPORT_INC
    extra=(-DLIVE_CONFIG_INC="\"$task_tmp/live-config.inc\"")
    case "$control" in
        sync-export-*) fixture=sdksyncexport_test.c; macro=SYNC_EXPORT_INC ;;
        live-deck-*) fixture=sdkdeckdata_test.c; macro=DECK_DATA_INC ;;
        live-lifetime-*) fixture=lifetime_test.c; macro=LIFE_INC ;;
        live-probe-*) fixture=sdkliveprobe_test.c; macro=LIVE_PROBE_INC ;;
        live-config-*) macro=LIVE_CONFIG_INC; extra=(-DLIVE_EXPORT_INC="\"$task_tmp/live-export.inc\"") ;;
    esac
    ${HOST_CC:-cc} "${cflags[@]}" "${extra[@]}" -D$macro="\"$task_tmp/$control.inc\"" "tests/unit/$fixture" -lm -o "$task_tmp/$control"
    if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: $control control was not caught" >&2; exit 1
    fi
    grep -Eq 'Assertion .* failed|Assertion failed|FAIL  lifetime' "$task_tmp/$control.log" || { echo "FAIL: $control lacked its required assertion" >&2; cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: $control"
done
if [ "${SAN:-plain}" = tsan ]; then
    for control in observer-loop-race observer-live-race; do
        ${HOST_CC:-cc} "${cflags[@]}" "${csan[@]}" -DOBSERVER_FLAGS_INC="\"$task_tmp/$control.inc\"" tests/unit/sdkobserverflags_test.c -lm -o "$task_tmp/$control"
        if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
            echo "FAIL: $control control was not caught" >&2; exit 1
        fi
        grep -q 'WARNING: ThreadSanitizer: data race' "$task_tmp/$control.log" || { echo "FAIL: $control lacked its TSan race report" >&2; cat "$task_tmp/$control.log" >&2; exit 1; }
        echo "CONTROL caught: $control (TSan)"
    done
fi
if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: live GNU observer fixture requires Linux ELF main-image guards' >&2
    exit 77
fi
map=-Wl,--version-script=src/azstem/sdk_native_cpp.map
${HOST_CXX:-g++} "${flags[@]}" "$map" src/azstem/sdk_live_observer.cpp tests/unit/sdkliveobserver_test.cpp -ldl -o "$task_tmp/real"
"$task_tmp/real"
${HOST_CXX:-g++} "${flags[@]}" "$map" src/azstem/sdk_loop_observer.cpp tests/unit/sdkloopobserver_test.cpp -ldl -o "$task_tmp/loop"
"$task_tmp/loop"
# Controls use a plain build; the real fixture above always uses the requested sanitizer.
control_flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -fvisibility=hidden -fvisibility-inlines-hidden)
python3 tests/unit/sdk_loop_controls.py "$task_tmp"
for deck in 0 1 2 3; do
    control=loop-strict-slot-$deck
    ${HOST_CXX:-g++} "${control_flags[@]}" "$map" "$task_tmp/$control.cpp" tests/unit/sdkloopobserver_test.cpp -ldl -o "$task_tmp/$control"
    if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: $control was not caught" >&2; exit 1
    fi
    grep -q 'after.engaged == engaged && after.bounds_valid == valid' "$task_tmp/$control.log" || { cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: $control"
done
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import re, sys
source = Path('src/azstem/sdk_live_observer.cpp').read_text().replace('../common/azliveobserver.h', 'azliveobserver.h')
variants = [
    ('player', r'player != deck', 'player != deck && false', 1),
    ('generation', r'p\.active_generation\.load\(std::memory_order_acquire\) != generation', 'false', 2),
    ('slot', r'\{15, 19, 21, 2, 5\}', '{14, 19, 21, 2, 5}', 1),
    ('boolean', r'a <= 1 && b <= 1 && c <= 1', 'true', 1),
    ('invalid-retention', r'if \(stream == kSync\) valid = a == 0 \|\| a == 2;', 'if (stream == kSync) valid = a == 0 || a == 2;\n    if (!valid) return;', 1),
    ('allocation-rollback', r'pending\[s\] = std::make_shared<CaptureListener>', 'owners[deck][s] = std::make_shared<CaptureListener>', 1),
    ('sync-slot', r'\{15, 19, 21, 2, 5\}', '{15, 19, 21, 3, 5}', 1),
    ('sync-mode', r'if \(stream == kSync\) valid = a == 0 \|\| a == 2;', 'if (stream == kSync) valid = a <= 2;', 1),
    ('sync-legacy-leak', r'    if \(!value.observed_fields \|\|', '    if (p.components[kSync].generation.load(std::memory_order_relaxed) == active && p.components[kSync].revision.load(std::memory_order_relaxed)) value.observed_fields |= 8u;\n    if (!value.observed_fields ||', 1),
]
for name, pattern, replacement, expected in variants:
    s, count = re.subn(pattern, replacement, source)
    assert count == expected, (name, count)
    if name == 'allocation-rollback':
        old = 'for (unsigned s = 0; s < kStreams; ++s) owners[deck][s] = pending[s];'
        assert s.count(old) == 1
        s = s.replace(old, 'for (unsigned s = 0; s < kStreams; ++s) pending[s] = owners[deck][s];')
    (Path(sys.argv[1]) / (name + '.cpp')).write_text(s)
PY
for control in player generation slot boolean invalid-retention allocation-rollback sync-slot sync-mode sync-legacy-leak; do
    ${HOST_CXX:-g++} "${control_flags[@]}" "$map" "$task_tmp/$control.cpp" tests/unit/sdkliveobserver_test.cpp -ldl -o "$task_tmp/$control"
    status=0
    ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null || status=$?
    if [ "$status" -eq 0 ]; then
        echo "FAIL: live $control control was not caught" >&2; exit 1
    fi
    required='FAIL live:'
    if [ "$control" = slot ]; then
        required='FAIL live: native registration slot 14'
        [ "$status" -eq 134 ] || { echo "FAIL: live slot expected tagged SIGABRT, got $status" >&2; cat "$task_tmp/$control.log" >&2; exit 1; }
    fi
    if [ "$control" = sync-slot ]; then
        required='FAIL live: native Sync registration slot 3'
        [ "$status" -eq 134 ] || { echo "FAIL: live Sync slot expected tagged SIGABRT, got $status" >&2; cat "$task_tmp/$control.log" >&2; exit 1; }
    fi
    grep -Fq "$required" "$task_tmp/$control.log" || { echo "FAIL: live $control lacked its required assertion" >&2; cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: live $control"
done
# All replaceable C++11 allocation forms must honor the fixture failure budget,
# including the nothrow form needed by statically linked GNU sanitizer runtimes.
python3 - "$task_tmp" <<'PYA'
from pathlib import Path
import sys
s = Path('tests/unit/sdkliveobserver_test.cpp').read_text()
a = s.index('void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept {')
i = s.index('return ::operator new(bytes);', a)
s = s[:i] + s[i:].replace('return ::operator new(bytes);', 'return std::malloc(bytes ? bytes : 1);', 1)
(Path(sys.argv[1]) / 'allocator-nothrow-bypass.cpp').write_text(s)
PYA
${HOST_CXX:-g++} "${control_flags[@]}" "$map" src/azstem/sdk_live_observer.cpp "$task_tmp/allocator-nothrow-bypass.cpp" -ldl -o "$task_tmp/allocator-nothrow-bypass"
if ( ulimit -c 0; "$task_tmp/allocator-nothrow-bypass" >"$task_tmp/allocator-nothrow-bypass.log" 2>&1 ) 2>/dev/null; then
    echo 'FAIL: live allocator-nothrow-bypass control was not caught' >&2; exit 1
fi
grep -q 'FAIL live:' "$task_tmp/allocator-nothrow-bypass.log" || { echo 'FAIL: live allocator control lacked its required assertion' >&2; cat "$task_tmp/allocator-nothrow-bypass.log" >&2; exit 1; }
echo 'CONTROL caught: live allocator-nothrow-bypass'
if [ "${SAN:-plain}" = asan ]; then
    # Native Sync borrows exactly four bytes. Three supplied bytes must fail at byte four.
    if ( ulimit -c 0; "$task_tmp/real" --sync-payload-short >"$task_tmp/sync-payload-short.log" 2>&1 ) 2>/dev/null; then
        echo 'FAIL: three-byte Sync payload was not diagnosed' >&2; exit 1
    fi
    grep -q 'AddressSanitizer: heap-buffer-overflow' "$task_tmp/sync-payload-short.log" || { cat "$task_tmp/sync-payload-short.log" >&2; exit 1; }
    echo 'CONTROL caught: live sync-payload-short (ASan)'
    # Overreads must fail in the sanitizer, independently of scalar assertions.
    python3 - "$task_tmp" <<'PYX'
from pathlib import Path
import sys
s = Path('src/azstem/sdk_live_observer.cpp').read_text().replace('../common/azliveobserver.h', 'azliveobserver.h')
old = 'unsigned char bytes[3];'
assert s.count(old) == 1
root = Path(sys.argv[1])
(root / 'payload-overread.cpp').write_text(s.replace(old, 'unsigned char bytes[4];'))
a = s.index('    const volatile char *actual =')
b = s.index('    return table;', a) + len('    return table;')
(root / 'rtti-overread.cpp').write_text(s[:a] + '    return !std::memcmp(reinterpret_cast<const void *>(name), expected, bytes) ? table : 0;' + s[b:])
s = Path('src/azstem/sdk_loop_observer.cpp').read_text().replace('../common/azloopobserver.h', 'azloopobserver.h')
a = s.index('    const volatile char *actual =')
b = s.index('    return table;', a) + len('    return table;')
(root / 'loop-rtti-overread.cpp').write_text(s[:a] + '    return !std::memcmp(reinterpret_cast<const void *>(name), expected, length) ? table : 0;' + s[b:])
PYX
    for control in payload-overread rtti-overread loop-rtti-overread; do
        fixture=sdkliveobserver_test.cpp
        [ "$control" != loop-rtti-overread ] || fixture=sdkloopobserver_test.cpp
        ${HOST_CXX:-g++} "${flags[@]}" "$map" "$task_tmp/$control.cpp" "tests/unit/$fixture" -ldl -o "$task_tmp/$control"
        if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
            echo "FAIL: live $control control was not caught" >&2; exit 1
        fi
        expected='AddressSanitizer: heap-buffer-overflow'
        [ "$control" = payload-overread ] || expected='AddressSanitizer: global-buffer-overflow'
        grep -q "$expected" "$task_tmp/$control.log" || { echo "FAIL: live $control lacked its ASan report" >&2; cat "$task_tmp/$control.log" >&2; exit 1; }
        echo "CONTROL caught: live $control (ASan)"
    done
fi
