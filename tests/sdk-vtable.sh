#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-vtable.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
${HOST_CC:-cc} "${flags[@]}" -Isrc/azstem tests/unit/sdkvtablepatch_test.c -o "$task_tmp/real"
"$task_tmp/real"
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import sys
source=Path('src/azstem/sdk_vtable_patch.h').read_text()
for name, old, new in [
    ('expected', '__atomic_load_n(slot, __ATOMIC_ACQUIRE) != expected', '0'),
    ('permissions', 'rollback.prot = protection.prot;', ';'),
    ('foreign', '(void) __atomic_compare_exchange_n(slot, &ours, expected, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);', '(void) ours; __atomic_store_n(slot, expected, __ATOMIC_RELEASE);'),
]:
    assert source.count(old)==1,(name,old)
    target=Path(sys.argv[1])/name
    target.mkdir()
    (target/'sdk_vtable_patch.h').write_text(source.replace(old,new))
PY
for control in expected permissions foreign; do
    ${HOST_CC:-cc} "${flags[@]}" -I"$task_tmp/$control" tests/unit/sdkvtablepatch_test.c -o "$task_tmp/$control-test"
    if ( ulimit -c 0; "$task_tmp/$control-test" >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: $control vtable control was not caught" >&2; exit 1
    fi
    grep -qi 'assert' "$task_tmp/$control.log" || { echo "FAIL: $control lacked its assertion" >&2; exit 1; }
    echo "CONTROL caught: vtable-$control"
done
# Header and C++ redirects must remain visible to the same linter as C hooks.
mkdir -p "$task_tmp/lint/azmod" "$task_tmp/lint/azstem"
cp src/azstem/sdk_vtable_patch.h "$task_tmp/lint/azstem/"
bash tests/unit/redirect-lint.sh "$task_tmp/lint" >"$task_tmp/lint-real.log"
for extension in h cpp; do
    printf 'static void unreviewed(void) {\n    azprot_unlock(0, 0, 0);\n}\n' >"$task_tmp/lint/azstem/unreviewed.$extension"
    if bash tests/unit/redirect-lint.sh "$task_tmp/lint" >"$task_tmp/lint-$extension.log" 2>&1; then
        echo "FAIL: redirect lint ignored $extension" >&2; exit 1
    fi
    grep -q "unreviewed.$extension:" "$task_tmp/lint-$extension.log" || exit 1
    rm "$task_tmp/lint/azstem/unreviewed.$extension"
    echo "CONTROL caught: unreviewed $extension redirect"
done
