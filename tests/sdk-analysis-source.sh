#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${1:-} == --docker ]]; then
    command -v docker >/dev/null || { echo 'SKIP: Docker unavailable'; exit 77; }
    exec docker run --rm --platform linux/arm64 -v "$PWD:/work" -w /work \
        "${AZ_TEST_IMAGE:-azmod-build:18.04}" bash tests/sdk-analysis-source.sh
fi
if [[ $(uname -s) != Linux || $(uname -m) != aarch64 ]]; then
    echo 'SKIP: AArch64 Linux required (run with --docker for the release toolchain)'
    exit 77
fi
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-analysis-source.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
san=()
case ${SAN:-plain} in
    plain|'') ;;
    asan) san=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
    tsan) san=(-fsanitize=thread -fno-omit-frame-pointer) ;;
    *) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
"${HOST_CC:-gcc}" "${san[@]}" -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -fexceptions -fasynchronous-unwind-tables \
    -DAZ_STATUS_NATIVE_PRODUCER -DAZ_ANALYSIS_SOURCE_TESTING -c src/azstem/sdk_analysis_source.c -o "$task_tmp/source.o"
"${HOST_CC:-gcc}" -c src/common/aznative_guard.S -o "$task_tmp/guard.o"
"${HOST_CC:-gcc}" "${san[@]}" -std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror -pthread \
    tests/unit/sdk_analysis_source_test.c tests/unit/sdk_analysis_source_return.S "$task_tmp/source.o" "$task_tmp/guard.o" -ldl -lstdc++ -o "$task_tmp/test"
"$task_tmp/test"
