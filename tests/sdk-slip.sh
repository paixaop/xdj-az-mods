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
${CXX:-g++} -std=c++11 -O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer ${CXXFLAGS:-} \
    "$@" src/azmod/sdk_native_slip.cpp tests/unit/sdkslip_test.cpp -pthread -o "$work/slip"
"$work/slip" raw
entry=
if [ "$(uname -s):$(uname -m)" = Linux:aarch64 ]; then
    ${CC:-gcc} -fexceptions -fasynchronous-unwind-tables -c src/azmod/sdk_slip_entry.S -o "$work/entry.o"
    entry="$work/entry.o"
    set -- "$@" -DAZSDK_SLIP_ARM_ENTRY
fi
${CXX:-g++} -std=c++11 -O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer ${CXXFLAGS:-} \
    "$@" src/azmod/sdk_native_slip.cpp src/azmod/sdk_slip_hooks.cpp tests/unit/sdkslip_hooks_test.cpp \
    $entry -pthread -o "$work/hooks"
for mode in success prefix body owner partial duplicate unaligned;do "$work/hooks" "$mode";done
getter_entry=
if [ "$(uname -s):$(uname -m)" = Linux:aarch64 ]; then
    ${CC:-gcc} -fPIC -fexceptions -fasynchronous-unwind-tables -c src/azmod/sdk_slip_getter_entry.S -o "$work/getter-entry.o"
    ${CC:-gcc} -fexceptions -fasynchronous-unwind-tables -c tests/unit/sdkslip_getter_native.S -o "$work/getter-native.o"
    ${CC:-gcc} -std=c11 -Wno-unused-function -c tests/unit/sdkslip_getter_patch.c -o "$work/getter-patch.o"
    getter_entry="$work/getter-entry.o $work/getter-native.o $work/getter-patch.o"
fi
${CXX:-g++} -std=c++11 -O1 -g -Wall -Wextra -Werror -fexceptions -fno-omit-frame-pointer ${CXXFLAGS:-} \
    "$@" src/azmod/sdk_slip_getters.cpp tests/unit/sdkslip_getter_test.cpp $getter_entry -pthread -o "$work/getters"
for mode in success body site partial;do "$work/getters" "$mode";done
if [ -n "$getter_entry" ];then "$work/getters" arm;fi
