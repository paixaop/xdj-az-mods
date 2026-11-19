#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-analysis-install.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -O1 -g -fexceptions -fasynchronous-unwind-tables -Wall -Wextra -Werror -pthread -D_GNU_SOURCE -DAZ_STATUS_NATIVE_PRODUCER)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" tests/unit/sdk_analysis_install_test.c tests/unit/native_unwind.c -Wl,--wrap=dl_iterate_phdr -lstdc++ -ldl -lm -o "$task_tmp/test"
"$task_tmp/test"
