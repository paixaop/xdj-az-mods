#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-status-store.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
# Reuse the physical port, SQLite, SQLCipher-key stand-in and failing-open fixtures.
sed 's/^int main(void) {/int sdktrack_fixture_main(void) {/' tests/unit/sdktrack_test.c > "$task_tmp/sdktrack_fixture.h"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers
    -pthread -DLIB_INC='"sdk_lib.c"' -Isrc/azmod -Itests/unit -I"$task_tmp")
libs=(-lsqlite3 -lm)
if [ "$(uname -s)" = Linux ]; then flags+=(-rdynamic); libs+=(-ldl)
    if "${HOST_CC:-cc}" --version | grep -qi "gcc\|Free Software Foundation"; then flags+=(-Wno-format-truncation); fi else flags+=(-Wl,-export_dynamic); fi
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer);;
*) exit 2;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdkstatus_store_test.c "${libs[@]}" -o "$task_tmp/test"
"$task_tmp/test" "$task_tmp"
