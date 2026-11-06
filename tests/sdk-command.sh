#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Asset-independent command policy tests. No firmware/native calls or installation.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-command.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
compiler=${HOST_CC:-cc}
"$compiler" "${flags[@]}" -Isrc/azmod tests/unit/sdkcommand_test.c -lm -o "$task_tmp/real"
cpp=${HOST_CXX:-c++}
if command -v "$cpp" >/dev/null 2>&1; then
    printf '#include "sdk_command_engine.h"\n#include <type_traits>\nstatic_assert(std::is_standard_layout<scmd_adapter>::value, "native adapter must have C layout");\nint main() { return 0; }\n' > "$task_tmp/adapter.cpp"
    "$cpp" -std=c++11 -Wall -Wextra -Werror -Isrc/azmod -fsyntax-only "$task_tmp/adapter.cpp"
else
    echo 'SKIP: C++11 adapter-header check requires a C++ compiler'
fi
case ${SAN:-plain} in
asan)
    nm "$task_tmp/real" | grep '__asan_init' >/dev/null || { echo 'FAIL: ASan instrumentation missing'; exit 1; }
    nm "$task_tmp/real" | grep '__ubsan_handle' >/dev/null || { echo 'FAIL: UBSan instrumentation missing'; exit 1; } ;;
tsan)
    nm "$task_tmp/real" | grep '__tsan_init' >/dev/null || { echo 'FAIL: TSan instrumentation missing'; exit 1; } ;;
esac
"$task_tmp/real"
python3 - src/azmod/sdk_command_engine.h "$task_tmp" <<'PY'
from pathlib import Path
import re
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
controls = {
    'owner': ('e->records[i].owner == owner &&', '((void) owner, 1) &&'),
    'arguments': ('a->beats == b->beats &&', '1 &&'),
    'id-replay': ('r.request_id <= e->highest_id[owner]', '0'),
    'load': ('t->load_generation != r->expected_load_generation', '0'),
    'revision': ('t->revision != r->expected_revision', '0'),
    'persistence': ('(t->persistent_commands & bit)', '0'),
    'deadline': ('if (now && compare_timestamps(request.deadline_ns, now, 1000000000, 1000000000, 1) <= AZ_TIME_EQUAL)', 'if (0)'),
    'cancel-claim': ('else if (r->phase == SCMD_DISPATCHED || r->awaiting) rc = AZ_PENDING;',
                     'else if (r->phase == SCMD_DISPATCHED || r->awaiting) rc = AZ_OK;'),
    'deck-barrier': ('if (e->active[deck])', 'if (0)'),
    'timeout-barrier': ('SCMD_DETAIL_DEADLINE, now, dispatched);', 'SCMD_DETAIL_DEADLINE, now, 0);'),
    'completion-load': ('c->load_generation != r->request.expected_load_generation', '0'),
    'completion-revision': ('c->reason == AZ_OK && c->revision && c->load_generation && c->settled',
                             'c->reason == AZ_OK && c->load_generation && c->settled'),
    'completion-order': ('c->revision < r->execution_revision', '0'),
    'premature-completion': ('if (r->phase != SCMD_DISPATCHED) return AZ_STALE;', 'if (0) return AZ_STALE;'),
    'lease': ('r->phase != SCMD_FINAL || r->pinned || r->awaiting', 'r->phase != SCMD_FINAL || r->awaiting'),
    'terminal': ('return AZ_OK; /* The published terminal uncertainty remains immutable. */',
                 'r->result.outcome = c->outcome; return AZ_OK; /* Broken: rewrite a published terminal result. */'),
    'result-size': ('copy.size = scmd_size(out);\n    memcpy(out, &copy, sizeof copy);',
                    'copy.size = sizeof copy;\n    memcpy(out, &copy, sizeof copy);'),
}
for name, (old, new) in controls.items():
    assert text.count(old) == 1, (name, text.count(old))
    directory = output / name
    directory.mkdir()
    (directory / 'sdk_command_engine.h').write_text(text.replace(old, new))
grown = output / 'request-growth'
grown.mkdir()
(grown / 'sdk_command_engine.h').write_text(text)
header = (source.parent / 'azmod-mod.h').read_text()
old = 'char comment[128];\n} az_command_request;'
assert header.count(old) == 1
(grown / 'azmod-mod.h').write_text(header.replace(old, 'char comment[128];\n    uint64_t added_argument;\n} az_command_request;'))
track = (source.parent / 'sdk_track.c').read_text()
def function(name):
    match = re.search(r'^static int ' + name + r'\([^;]*?\)\s*\{', track, re.M)
    assert match, name
    end = track.index('\n}', match.start()) + 2
    return track[match.start():end] + '\n'
(output / 'owner.inc').write_text(function('st_mod_owner'))
submit = function('sdk_command_submit')
(output / 'submit.inc').write_text(submit)
module = (source.parent / 'sdk_command.c').read_text()
wrapper_controls = {
    'wrapper-submit-owner': ('submit', 'st_mod_owner(id, CM_RA(), "command_submit")', '((void) id, 0)'),
    'wrapper-poll-owner': ('module', 'st_mod_owner(id, CM_RA(), "command_poll")', '((void) id, 0)'),
    'wrapper-cancel-owner': ('module', 'st_mod_owner(id, CM_RA(), "command_cancel")', '((void) id, 0)'),
    'wrapper-release-owner': ('module', 'st_mod_owner(id, CM_RA(), "command_release")', '((void) id, 0)'),
    'wrapper-arguments': ('submit', '!scmd_request_valid(r)',
        '(!r || scmd_size(r) < sizeof *r || r->version != AZ_DATA_VERSION || r->deck > 3 || r->operation < AZ_CMD_LOOP_IN || r->operation > AZ_CMD_CUE_COLOR || !r->request_id || !r->expected_load_generation || !r->deadline_ns || r->quantization > AZ_QUANTIZE_ON)'),
    'wrapper-poll-state': ('module', 'scmd_poll(&g_sdk_commands, owner, request_id, out)', '((void) request_id, (void) out, AZ_STALE)'),
    'wrapper-cancel-state': ('module', 'sdk_exit_cancel_request(owner, request_id, out)', '((void) sdk_exit_cancel_request, (void) request_id, (void) out, AZ_STALE)'),
    'wrapper-release-state': ('module', 'scmd_release(&g_sdk_commands, owner, request_id)', '((void) request_id, AZ_STALE)'),
}
for name, (which, old, new) in wrapper_controls.items():
    original = submit if which == 'submit' else module
    assert original.count(old) == 1, name
    directory = output / name
    directory.mkdir()
    (directory / 'submit.inc').write_text(submit.replace(old, new) if which == 'submit' else submit)
    (directory / 'sdk_command.c').write_text(module.replace(old, new) if which == 'module' else module)
PY
expect_failure() {
    python3 - "$1" "$2" "$3" <<'PY'
from pathlib import Path
import re, resource, subprocess, sys
binary, pattern, label = sys.argv[1:]
def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
result = subprocess.run([binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, preexec_fn=no_core)
Path(binary + '.log').write_text(result.stdout)
if result.returncode == 0 or not re.search(pattern, result.stdout):
    print('FAIL: control', label, 'exit', result.returncode, '(required diagnostic absent)')
    print(result.stdout[-4000:])
    raise SystemExit(1)
print('CONTROL caught:', label)
PY
}
wrapper_flags=(-DSDK_COMMAND_OWNER_INC="\"$task_tmp/owner.inc\"")
"$compiler" "${flags[@]}" "${wrapper_flags[@]}" -DSDK_COMMAND_SUBMIT_INC="\"$task_tmp/submit.inc\"" -Isrc/azmod tests/unit/sdkcommand_wrapper_test.c -lm -o "$task_tmp/wrappers"
"$task_tmp/wrappers"
for control in wrapper-submit-owner wrapper-poll-owner wrapper-cancel-owner wrapper-release-owner wrapper-arguments wrapper-poll-state wrapper-cancel-state wrapper-release-state; do
    "$compiler" "${flags[@]}" "${wrapper_flags[@]}" -DSDK_COMMAND_SUBMIT_INC="\"$task_tmp/$control/submit.inc\"" -DSDK_COMMAND_IMPL="\"$task_tmp/$control/sdk_command.c\"" -Isrc/azmod tests/unit/sdkcommand_wrapper_test.c -lm -o "$task_tmp/$control-test"
    expect_failure "$task_tmp/$control-test" 'Assertion|assertion|ERROR: AddressSanitizer:|runtime error:' "$control"
done
if "$compiler" "${flags[@]}" -I"$task_tmp/request-growth" -Isrc/azmod tests/unit/sdkcommand_test.c -lm -o "$task_tmp/request-growth-test" >"$task_tmp/request-growth.log" 2>&1; then
    echo 'FAIL: appended command argument escaped validation/comparison review'; exit 1
fi
grep 'command request grew' "$task_tmp/request-growth.log" >/dev/null || { cat "$task_tmp/request-growth.log"; exit 1; }
echo 'CONTROL caught: request growth requires explicit validation/comparison update'
for control in owner arguments id-replay load revision persistence deadline cancel-claim deck-barrier timeout-barrier completion-load completion-revision completion-order premature-completion lease terminal result-size; do
    "$compiler" "${flags[@]}" -I"$task_tmp/$control" -Isrc/azmod tests/unit/sdkcommand_test.c -lm -o "$task_tmp/$control-test"
    expect_failure "$task_tmp/$control-test" 'Assertion|assertion|ERROR: AddressSanitizer:|runtime error:' "$control"
done
case ${SAN:-plain} in
asan)
    # Check each sanitizer separately so the planted heap fault cannot be consumed
    # by UBSan before AddressSanitizer itself demonstrates its instrumentation.
    "$compiler" "${flags[@]}" -fno-sanitize=undefined -DSCMD_TEST_HEAP -Isrc/azmod tests/unit/sdkcommand_test.c -lm -o "$task_tmp/heap"
    expect_failure "$task_tmp/heap" 'AddressSanitizer: heap-buffer-overflow' 'ASan heap fault'
    "$compiler" "${flags[@]}" -fno-sanitize=address -DSCMD_TEST_SIGNED -Isrc/azmod tests/unit/sdkcommand_test.c -lm -o "$task_tmp/signed"
    expect_failure "$task_tmp/signed" 'runtime error: signed integer overflow' 'UBSan signed overflow' ;;
tsan)
    "$compiler" "${flags[@]}" -DSCMD_TEST_RACE -Isrc/azmod tests/unit/sdkcommand_test.c -lm -o "$task_tmp/race"
    expect_failure "$task_tmp/race" 'ThreadSanitizer: data race' 'TSan race' ;;
esac
echo "sdk commands: ${SAN:-plain} tests, 17 policy controls, 8 wrapper controls and request-growth control passed"
