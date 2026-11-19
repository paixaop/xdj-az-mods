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
${HOST_CC:-cc} "${flags[@]}" -Isrc/azmod tests/unit/sdkvocal_test.c -lm -o "$task_tmp/vocals"
"$task_tmp/vocals"
if [ "$(uname -s)" = Linux ]; then
    python3 tests/unit/extract_spool.py > "$task_tmp/spool.inc"
    ${HOST_CC:-cc} "${flags[@]}" -Wno-unused-function -Wno-missing-field-initializers -rdynamic \
        -DLIB_INC='"sdk_lib.c"' -DLSAPPLY_INC="\"$task_tmp/spool.inc\"" -Itests/unit -Isrc/azmod tests/unit/sdkanalysis_service_test.c src/azmod/sdk_command_engine.c src/azmod/analysis_owner.c \
        -lsqlite3 -ldl -lm -o "$task_tmp/service"
    "$task_tmp/service"
fi
