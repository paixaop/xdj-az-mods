#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-analysis-tests.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
    plain) ;;
    asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
    *) echo 'SAN must be plain or asan' >&2; exit 2 ;;
esac
${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod tests/unit/sdkanalysis_write_test.c -o "$task_tmp/write"
"$task_tmp/write"
if [ "$(uname -s)" = Linux ]; then
    ${HOST_CC:-cc} "${flags[@]}" -Wno-unused-function -Wno-missing-field-initializers -rdynamic \
        -DLIB_INC='"sdk_lib.c"' -Itests/unit -Isrc/azmod tests/unit/sdkanalysis_service_test.c \
        -lsqlite3 -ldl -lm -o "$task_tmp/service"
    "$task_tmp/service"
fi
