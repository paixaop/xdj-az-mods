#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-cue-recall.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -Isrc/azmod -DAZSDK_CUE_RECALL_TESTING)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" -std=gnu11 -fexceptions -fasynchronous-unwind-tables -c src/azstem/sdk_cue_recall.c -o "$task_tmp/recall.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -fexceptions -fasynchronous-unwind-tables -c tests/unit/native_unwind.c -o "$task_tmp/guard.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" "$task_tmp/recall.o" "$task_tmp/guard.o" tests/unit/sdk_cue_recall_test.c -lstdc++ -ldl -o "$task_tmp/test"
"$task_tmp/test"
echo 'PASS native cue recall: owned preparation, unit bindings, owner affinity and DSP bridge admission'
