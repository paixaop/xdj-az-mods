#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Portable host regression and opposite-outcome controls; never contacts a deck.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-native-commands.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -Isrc/azmod -fvisibility=hidden -fvisibility-inlines-hidden)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
compiler=${HOST_CXX:-c++}
"${HOST_CC:-cc}" -std=c11 -Wall -Wextra -Werror -Isrc/common -x c -fsyntax-only src/common/aznativecommands.h
"$compiler" "${flags[@]}" src/azstem/sdk_native_commands.cpp tests/unit/sdknativecommands_test.cpp -o "$task_tmp/production"
"$task_tmp/production"
"$compiler" "${flags[@]}" -DAZSDK_NATIVE_COMMANDS_TESTING src/azstem/sdk_native_commands.cpp tests/unit/sdknativecommands_test.cpp -o "$task_tmp/real"
"$task_tmp/real"
# The real fixture above uses the requested sanitizer; controls use a plain build.
control_flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -DAZSDK_NATIVE_COMMANDS_TESTING -Isrc/common -Isrc/azmod -fvisibility=hidden -fvisibility-inlines-hidden)
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import sys
source = Path('src/azstem/sdk_native_commands.cpp').read_text()
source = source.replace('../common/aznativecommands.h', 'aznativecommands.h').replace('../azmod/sdk_command_engine.h', 'sdk_command_engine.h')
variants = [
    ('capability', 'azsdk_native_commands_capabilities(void) noexcept { return 0; }', 'azsdk_native_commands_capabilities(void) noexcept { return UINT64_C(1) << AZ_CMD_LOOP_EXIT; }'),
    ('owner-thread', 'if (!check_(owner_)) status = AZ_INVALID;', 'if (!check_(owner_) && false) status = AZ_INVALID;'),
    ('foreign-release', 'if (!gate_ || thread_ != OwnerGate::thread_cookie()) return AZ_INVALID;', 'if (!gate_) return AZ_INVALID;'),
    ('generation', 'if (generation() != expected_generation) return AZ_STALE;', 'if (generation() != expected_generation && false) return AZ_STALE;'),
    ('retirement-wait', 'while (state_.load(std::memory_order_acquire) & Reader) std::this_thread::yield();', 'while (false && (state_.load(std::memory_order_acquire) & Reader)) std::this_thread::yield();'),
    ('reentrant-close', 'state_.fetch_or(Retiring, std::memory_order_acq_rel);\n        return AZ_BUSY;', 'return AZ_BUSY;'),
    ('predicate-release', 'if (status != AZ_OK) out.end();', 'if (status != AZ_OK && false) out.end();'),
    ('exhaustion', 'if (last_generation_ == UINT64_MAX) return AZ_LIMIT;', 'if (last_generation_ == UINT64_MAX && false) return AZ_LIMIT;'),
    ('thread-reuse', 'cookie = expected + 1;', 'cookie = 1;'),
]
for name, old, new in variants:
    expected = 2 if name == 'generation' else 1
    assert source.count(old) == expected, (name, source.count(old))
    (Path(sys.argv[1]) / (name + '.cpp')).write_text(source.replace(old, new))
PY
for control in capability owner-thread foreign-release generation retirement-wait reentrant-close predicate-release exhaustion thread-reuse; do
    "$compiler" "${control_flags[@]}" "$task_tmp/$control.cpp" tests/unit/sdknativecommands_test.cpp -o "$task_tmp/$control"
    if ( ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: native command $control control was not caught" >&2; exit 1
    fi
    if ! grep -q 'FAIL nativecommands:' "$task_tmp/$control.log"; then
        echo "FAIL: native command $control lacked its required assertion" >&2
        cat "$task_tmp/$control.log" >&2
        exit 1
    fi
    echo "CONTROL caught: native command $control"
done
