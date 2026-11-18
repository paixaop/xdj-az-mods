#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc=${HOST_CC:-cc}
flags=(-std=gnu11 -Wall -Wextra -Werror -O2 -g -pthread -fexceptions -fasynchronous-unwind-tables)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
runtime=(-lstdc++ -ldl)
if [[ $(uname -s) == Darwin ]]; then runtime=(-lc++); fi
if [[ $(uname -s) == Linux ]]; then
    "$cc" "${flags[@]}" -fPIC -fvisibility=hidden -shared -shared-libgcc -Wl,--no-undefined \
        tests/unit/native_propagation.c src/azstem/sdk_native_commands.c -ldl -lm -o "$work/core.so"
    if readelf -d "$work/core.so" | grep -q 'libstdc++'; then
        echo 'FAIL: production C boundary links the exception runtime' >&2; exit 1
    fi
    export CORE_UNWIND_LIBRARY="$work/core.so"
fi
"$cc" "${flags[@]}" tests/unit/native_errors_test.c tests/unit/native_unwind.c "${runtime[@]}" -o "$work/errors"
"$work/errors"
# The deck toolchain provides an independent small indirect-result oracle.
if [[ $(uname -s) == Linux ]]; then
    # Match the production shared-library link and local loading.
    "$cc" "${flags[@]}" -fPIC -shared -shared-libgcc -Wl,--no-undefined \
        tests/unit/native_errors_test.c tests/unit/native_unwind.c "${runtime[@]}" -o "$work/errors.so"
    "$cc" "${flags[@]}" tests/unit/native_errors_loader_test.c -ldl -o "$work/loader"
    "$work/loader" "$work/errors.so"
    # Opposite-outcome control: unwind tables alone do not run C cleanup.
    "$cc" "${flags[@]}" -fno-exceptions -fPIC -fvisibility=hidden -shared -shared-libgcc -Wl,--no-undefined \
        tests/unit/native_propagation.c src/azstem/sdk_native_commands.c -ldl -lm -o "$work/no-cleanup.so"
    if (ulimit -c 0; CORE_UNWIND_LIBRARY="$work/no-cleanup.so" "$work/errors" >"$work/control.log" 2>&1) 2>/dev/null; then
        echo 'FAIL: missing production C cleanup was not detected' >&2; exit 1
    fi
    grep -q 'core_cleaned == before + 2' "$work/control.log" || { cat "$work/control.log" >&2; exit 1; }
    echo 'CONTROL caught: missing production C unwind cleanup'
    "${NATIVE_ABI_CC:-gcc}" -std=gnu11 -O2 -Wall -Wextra -Werror -fpcc-struct-return -c tests/unit/native_result_fixture.c -o "$work/result.o"
    "$cc" "${flags[@]}" tests/unit/native_result_test.c tests/unit/native_unwind.c "$work/result.o" "${runtime[@]}" -o "$work/results"
    "$work/results"
fi
