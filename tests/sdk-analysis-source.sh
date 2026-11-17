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
"${HOST_CXX:-g++}" -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread \
    tests/unit/sdk_analysis_source_test.cpp -ldl -o "$task_tmp/test"
"$task_tmp/test"
