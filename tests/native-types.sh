#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-native-types.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -Wall -Wextra -Werror -fsyntax-only)
task_source="$PWD/tests/unit/native_types_test.c"
# Instrumented compilers can emit notes even for syntax-only checks.
cd "$task_tmp"
"${HOST_CC:-cc}" "${flags[@]}" -DTYPE_ERROR=0 "$task_source"
for mismatch in {1..9}; do
    if "${HOST_CC:-cc}" "${flags[@]}" -DTYPE_ERROR="$mismatch" "$task_source" >"$task_tmp/$mismatch.log" 2>&1; then
        echo "FAIL: native type mismatch $mismatch compiled" >&2
        exit 1
    fi
    if ! grep -Eq 'incompatible|too many arguments' "$task_tmp/$mismatch.log"; then
        cat "$task_tmp/$mismatch.log" >&2
        exit 1
    fi
done
echo 'PASS native types: valid bindings compile; nine signature/object mismatches are rejected'
