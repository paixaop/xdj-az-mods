#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)/$(uname -m)" = Linux/aarch64 ] || exit 77
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-overview-native.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
cat > "$task_tmp/client.c" <<'C'
#include "azoverviewlease.h"
int overview_client(int (*service)(uint32_t, struct azov_lease_api *, size_t), struct azov_lease_api *api) {
    int result = service(AZOV_LEASE_ABI, api, sizeof *api);
    return result && api->struct_size == sizeof *api;
}
C
mkdir "$task_tmp/other"
"${HOST_CC:-cc}" "${flags[@]}" -fPIC -shared -Isrc/common "$task_tmp/client.c" -o "$task_tmp/azmod.so"
cp "$task_tmp/azmod.so" "$task_tmp/wrong.so"
cp "$task_tmp/azmod.so" "$task_tmp/other/azmod.so"
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/overviewnative_test.c -ldl -o "$task_tmp/test"
"$task_tmp/test" "$task_tmp/azmod.so" "$task_tmp/wrong.so" "$task_tmp/other/azmod.so"
