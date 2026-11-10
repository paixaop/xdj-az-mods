#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-load.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
c_flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common)
case ${SAN:-plain} in
plain|'') ;;
asan) c_flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) c_flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
"${HOST_CC:-cc}" "${c_flags[@]}" src/azstem/sdk_load_export.c tests/unit/sdkloadexport_test.c -o "$task_tmp/export"
"$task_tmp/export"
python3 - "$task_tmp" <<'PYEXPORT'
from pathlib import Path
import sys
source = Path('src/azstem/sdk_load_export.c').read_text().replace('../common/azloadexport.h', 'azloadexport.h')
old = 'if (size < sizeof(struct azstem_load_observation_v1)) return -1;'
assert source.count(old) == 1
(Path(sys.argv[1]) / 'load-export-size.c').write_text(source.replace(old, 'if (0) return -1;'))
PYEXPORT
"${HOST_CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer "$task_tmp/load-export-size.c" tests/unit/sdkloadexport_test.c -o "$task_tmp/export-size"
if ( ulimit -c 0; ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$task_tmp/export-size" >"$task_tmp/export-size.log" 2>&1 ) 2>/dev/null; then
    echo 'FAIL: load export-size control was not caught' >&2; exit 1
fi
grep -q 'AddressSanitizer: heap-buffer-overflow' "$task_tmp/export-size.log" || { cat "$task_tmp/export-size.log" >&2; exit 1; }
echo 'CONTROL caught: load export-size'
if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: load GNU virtual-dispatch fixture requires Linux ELF main-image guards' >&2
    exit 77
fi
python3 tests/unit/sdk_load_public_test.py --native
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -DAZSDK_LOAD_TESTING -Isrc/common -fvisibility=hidden -fvisibility-inlines-hidden -fno-devirtualize -fno-devirtualize-speculatively)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
compiler=${HOST_CXX:-g++}
"${HOST_CC:-cc}" -std=c11 -Wall -Wextra -Werror -Isrc/common -x c -fsyntax-only src/common/azloadobserver.h
"$compiler" "${flags[@]}" -Wl,--version-script=src/azstem/sdk_native_cpp.map src/azstem/sdk_load_observer.cpp tests/unit/sdkloadobserver_test.cpp -ldl -o "$task_tmp/real"
for build in 1 2; do
    for mode in happy decline1 decline2 decline3 decline4 partial1 partial2 partial3 partial4 mismatch1 mismatch2 mismatch3 mismatch4 shared shared-concurrent unknown unknown-unload configure capacity wrong1 wrong2 wrong3 wrong4; do "$task_tmp/real" "$build" "$mode"; done
done
control_flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -DAZSDK_LOAD_TESTING -Isrc/common -fvisibility=hidden -fvisibility-inlines-hidden -fno-devirtualize -fno-devirtualize-speculatively)
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import sys
source = Path('src/azstem/sdk_load_observer.cpp').read_text().replace('../common/azloadobserver.h', 'azloadobserver.h')
variants = [
    ('listener-offset', 'kCompleteThis = -0x150', 'kCompleteThis = -0x120'),
    ('request-id', 'd.pending_id.load(std::memory_order_acquire) != std::uint64_t(id) + 1', 'false'),
    ('failure', 'result == 0 ? load_generation() : 0', '(static_cast<void>(result), load_generation())'),
    ('monotonic', 'if (value <= previous) return false;', 'if (value < previous) return false;'),
    ('late-consume', 'd.pending_id.store(0, std::memory_order_release);\n            std::uint64_t generation', 'std::uint64_t generation'),
    ('unload-epoch', 'const auto epoch = invalidate(d);\n#ifdef AZ_STATUS_NATIVE_PRODUCER\n    d.awaiting_empty', 'const auto epoch = d.epoch.load();\n#ifdef AZ_STATUS_NATIVE_PRODUCER\n    d.awaiting_empty'),
    ('retire-epoch', 'const auto epoch = invalidate(d);\n#ifdef AZ_STATUS_NATIVE_PRODUCER\n    status_load(deck', 'const auto epoch = d.epoch.load();\n#ifdef AZ_STATUS_NATIVE_PRODUCER\n    status_load(deck'),
    ('partial-ack', 'return acknowledged.load(std::memory_order_acquire) && expected &&', 'return expected &&'),
    ('original-order', 'if (!expected) {\n        expected = current;\n        __atomic_store_n(original, expected, __ATOMIC_RELEASE);\n    }', 'if (!expected) expected = current;'),
    ('native-entry', 'if ((current != hook && current != verified) || (expected && expected != verified)) return false;', 'static_cast<void>(verified);'),
    ('configure-order', 'std::lock_guard<std::mutex> lock(management);\n        enabled.store(0, std::memory_order_release);', 'std::lock_guard<std::mutex> lock(management);'),
    ('unknown-load', '        d.poison.store(1, std::memory_order_release);\n        return;\n    }\n#ifdef AZ_STATUS_NATIVE_PRODUCER\n    status_load', '        return;\n    }\n#ifdef AZ_STATUS_NATIVE_PRODUCER\n    status_load'),
    ('unknown-unload', 'else d.poison.store(1, std::memory_order_release);', 'else {}'),
    ('shared-listener', 'if (owner < kDecks) {\n            decks[owner].poison.store(1, std::memory_order_release);\n            invalidate(decks[owner]);\n        }', 'static_cast<void>(owner);'),
    ('short-name', 'for (std::size_t i = 0; i < bytes; ++i)\n        if (actual[i] != expected[i]) return 0;', 'if (std::memcmp(const_cast<const char *>(actual), expected, bytes)) return 0;'),
]
for name, old, new in variants:
    assert source.count(old) == 1, (name, source.count(old))
    changed = source.replace(old, new)
    if name == 'retire-epoch':
        begin = changed.index('extern "C" void azsdk_load_retire')
        changed = changed[:begin] + changed[begin:].replace('d.unit_generation.store(0, std::memory_order_release);', '/* retired unit deliberately kept live */', 1)
    if name == 'request-id':
        inner = 'd.pending_id.load(std::memory_order_acquire) == std::uint64_t(id) + 1'
        assert changed.count(inner) == 1
        changed = changed.replace(inner, 'true')
    (Path(sys.argv[1]) / (name + '.cpp')).write_text(changed)
PY
for control in listener-offset request-id failure monotonic late-consume unload-epoch retire-epoch partial-ack original-order native-entry configure-order unknown-load unknown-unload shared-listener; do
    "$compiler" "${control_flags[@]}" "$task_tmp/$control.cpp" tests/unit/sdkloadobserver_test.cpp -ldl -o "$task_tmp/$control"
    mode=happy
    [ "$control" != partial-ack ] || mode=partial2
    [ "$control" != native-entry ] || mode=wrong1
    [ "$control" != configure-order ] || mode=configure
    [ "$control" != unknown-load ] || mode=unknown
    [ "$control" != unknown-unload ] || mode=unknown-unload
    [ "$control" != shared-listener ] || mode=shared
    if ( ulimit -c 0; "$task_tmp/$control" 2 "$mode" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: load $control control was not caught" >&2; exit 1
    fi
    grep -q 'FAIL load:' "$task_tmp/$control.log" || { cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: load $control"
done
# A raw memcmp crosses the short RTTI object's ASan global redzone; use ASan to
# distinguish that error from a normal bounded nominal-name rejection.
"$compiler" "${control_flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer "$task_tmp/short-name.cpp" tests/unit/sdkloadobserver_test.cpp -ldl -o "$task_tmp/short-name"
if ( ulimit -c 0; ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$task_tmp/short-name" 2 happy >"$task_tmp/short-name.log" 2>&1 ) 2>/dev/null; then
    echo 'FAIL: load short-name control was not caught' >&2; exit 1
fi
grep -q 'AddressSanitizer: global-buffer-overflow' "$task_tmp/short-name.log" || { cat "$task_tmp/short-name.log" >&2; exit 1; }
echo 'CONTROL caught: load short-name'
