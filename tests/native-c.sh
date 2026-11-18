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
"$cc" "${flags[@]}" tests/unit/native_errors_test.c src/common/aznative_guard.c "${runtime[@]}" -o "$work/errors"
"$work/errors"
# The deck toolchain provides an independent small indirect-result oracle.
if [[ $(uname -s) == Linux ]]; then
    # Match the production shared-library/static-libgcc link and local loading.
    "$cc" "${flags[@]}" -fPIC -shared -static-libgcc -Wl,--no-undefined \
        tests/unit/native_errors_test.c src/common/aznative_guard.c "${runtime[@]}" -o "$work/errors.so"
    "$cc" "${flags[@]}" tests/unit/native_errors_loader_test.c -ldl -o "$work/loader"
    "$work/loader" "$work/errors.so"
    "${NATIVE_ABI_CC:-gcc}" -std=gnu11 -O2 -Wall -Wextra -Werror -fpcc-struct-return -c tests/unit/native_result_fixture.c -o "$work/result.o"
    "$cc" "${flags[@]}" tests/unit/native_result_test.c src/common/aznative_guard.c "$work/result.o" "${runtime[@]}" -o "$work/results"
    "$work/results"
fi
