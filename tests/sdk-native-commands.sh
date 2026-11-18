#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Portable host regression and opposite-outcome controls; never contacts a deck.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-native-commands.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -Isrc/azmod -fvisibility=hidden)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
compiler=${HOST_CC:-cc}
"${HOST_CC:-cc}" -std=c11 -Wall -Wextra -Werror -Isrc/common -x c -fsyntax-only src/common/aznativecommands.h
"$compiler" "${flags[@]}" src/azstem/sdk_native_commands.c tests/unit/sdknativecommands_test.c tests/unit/native_unwind.c -lstdc++ -ldl -lm -o "$task_tmp/production"
"$task_tmp/production"
"$compiler" "${flags[@]}" -DAZSDK_NATIVE_COMMANDS_TESTING src/azstem/sdk_native_commands.c tests/unit/sdknativecommands_test.c tests/unit/native_unwind.c -lstdc++ -ldl -lm -o "$task_tmp/real"
"$task_tmp/real"
# The real fixture above uses the requested sanitizer; controls use a plain build.
control_flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror -pthread -DAZSDK_NATIVE_COMMANDS_TESTING -Isrc/common -Isrc/azmod -fvisibility=hidden)
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import sys
source = Path('src/azstem/sdk_native_commands.c').read_text()
source = source.replace('../common/', '').replace('../azmod/', '')
variants = [
    ('capability', 'azsdk_native_commands_capabilities(void) { return 0; }', 'azsdk_native_commands_capabilities(void) { return UINT64_C(1) << AZ_CMD_LOOP_EXIT; }'),
    ('owner-thread', 'if (!gate->check(gate->owner)) return AZ_INVALID;', 'if (!gate->check(gate->owner) && false) return AZ_INVALID;'),
    ('foreign-release', 'if (!lease->gate || lease->thread != thread_cookie()) return AZ_INVALID;', 'if (!lease->gate) return AZ_INVALID;'),
    ('generation', 'if (az_owner_generation(gate) != expected_generation) return AZ_STALE;', 'if (az_owner_generation(gate) != expected_generation && false) return AZ_STALE;'),
    ('retirement-wait', 'while (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & Reader) sched_yield();', 'while (false && (__atomic_load_n(&gate->state, __ATOMIC_ACQUIRE) & Reader)) sched_yield();'),
    ('reentrant-close', '__atomic_fetch_or(&gate->state, Retiring, __ATOMIC_ACQ_REL);\n        return AZ_BUSY;', 'return AZ_BUSY;'),
    ('predicate-release', 'if (!gate->check(gate->owner)) return AZ_INVALID;', 'if (!gate->check(gate->owner)) { lease = (struct az_owner_lease) {0}; return AZ_INVALID; }'),
    ('exhaustion', 'if (gate->last_generation == UINT64_MAX) return AZ_LIMIT;', 'if (gate->last_generation == UINT64_MAX && false) return AZ_LIMIT;'),
    ('thread-reuse', 'cookie = expected + 1;', 'cookie = 1;'),
]
for name, old, new in variants:
    expected = 2 if name == 'generation' else 1
    assert source.count(old) == expected, (name, source.count(old))
    (Path(sys.argv[1]) / (name + '.c')).write_text(source.replace(old, new))
PY
for control in capability owner-thread foreign-release generation retirement-wait reentrant-close predicate-release exhaustion thread-reuse; do
    "$compiler" "${control_flags[@]}" "$task_tmp/$control.c" tests/unit/sdknativecommands_test.c tests/unit/native_unwind.c -lstdc++ -ldl -lm -o "$task_tmp/$control"
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
