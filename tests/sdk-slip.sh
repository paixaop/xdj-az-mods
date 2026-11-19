#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
case ${SAN:-plain} in
    plain|'') set -- ;;
    asan) set -- -fsanitize=address,undefined -fno-sanitize-recover=all ;;
    tsan) set -- -fsanitize=thread ;;
    *) echo 'unknown SAN' >&2;exit 2 ;;
esac
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fno-omit-frame-pointer ${CFLAGS:-} \
    "$@" tests/unit/sdkslip_startup_test.c -o "$work/startup"
"$work/startup"
for name in sdk_native_slip sdk_slip_hooks sdk_slip_getters; do
    ${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fexceptions -fasynchronous-unwind-tables -fno-omit-frame-pointer ${CFLAGS:-} \
        "$@" -c "src/azmod/$name.c" -o "$work/$name.o"
done
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fexceptions -fasynchronous-unwind-tables -fno-omit-frame-pointer ${CFLAGS:-} "$@" -c tests/unit/native_unwind.c -o "$work/guard.o"
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer ${CFLAGS:-} \
    "$@" "$work/sdk_native_slip.o" "$work/guard.o" tests/unit/sdkslip_test.c -pthread -lstdc++ -ldl -o "$work/slip"
"$work/slip" raw
entry=
if [ "$(uname -s):$(uname -m)" = Linux:aarch64 ]; then
    ${CC:-gcc} -fexceptions -fasynchronous-unwind-tables -c src/azmod/sdk_slip_entry.S -o "$work/entry.o"
    entry="$work/entry.o"
    set -- "$@" -DAZSDK_SLIP_ARM_ENTRY
fi
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer ${CFLAGS:-} \
    "$@" "$work/sdk_native_slip.o" "$work/guard.o" "$work/sdk_slip_hooks.o" tests/unit/sdkslip_hooks_test.c \
    $entry -pthread -lstdc++ -ldl -o "$work/hooks"
for mode in success prefix body owner partial duplicate unaligned;do "$work/hooks" "$mode";done
getter_entry=
if [ "$(uname -s):$(uname -m)" = Linux:aarch64 ]; then
    ${CC:-gcc} -fPIC -fexceptions -fasynchronous-unwind-tables -c src/azmod/sdk_slip_getter_entry.S -o "$work/getter-entry.o"
    ${CC:-gcc} -fexceptions -fasynchronous-unwind-tables -c tests/unit/sdkslip_getter_native.S -o "$work/getter-native.o"
    ${CC:-gcc} -std=c11 -Wno-unused-function -c tests/unit/sdkslip_getter_patch.c -o "$work/getter-patch.o"
    getter_entry="$work/getter-entry.o $work/getter-native.o $work/getter-patch.o"
fi
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer ${CFLAGS:-} \
    "$@" "$work/sdk_slip_getters.o" tests/unit/sdkslip_getter_test.c "$work/guard.o" $getter_entry -pthread -lstdc++ -ldl -o "$work/getters"
for mode in success body site partial;do "$work/getters" "$mode";done
if [ -n "$getter_entry" ];then "$work/getters" arm;fi
