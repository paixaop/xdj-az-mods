#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Exact native SQLCipher metadata on disposable local databases only.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${MODULE_PATH:?explicit native SQLCipher library required}" "${MODULE_BUILD:?explicit supported build required}"
case $MODULE_BUILD in 1|2) ;; *) exit 2 ;; esac
[ "$(uname -s)/$(uname -m)" = Linux/aarch64 ] || exit 77
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sources-native.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror -pthread -Isrc/common)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-gcc}" "${flags[@]}" tests/unit/sdknativesources_test.c src/azmod/sdk_connection_hooks.c src/common/azcache_scopes.c tests/unit/native_unwind.c -lstdc++ -ldl -o "$task_tmp/test"
runner=("$task_tmp/test")
if [ -n "${RUNTIME_LOADER:-}" ]; then
    : "${RUNTIME_LIBRARY_PATH:?explicit coherent native runtime required}"
    runner=("$RUNTIME_LOADER" --library-path "$RUNTIME_LIBRARY_PATH" "$task_tmp/test")
fi
LD_PRELOAD= LD_LIBRARY_PATH="${RUNTIME_LIBRARY_PATH:-${LD_LIBRARY_PATH:-}}" OPENSSL_CONF=/dev/null \
    "${runner[@]}" --native-sqlcipher "$MODULE_PATH" "$MODULE_BUILD"
