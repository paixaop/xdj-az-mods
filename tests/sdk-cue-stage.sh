#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-cue-stage.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -Isrc/azmod -Wno-unused-function)
case ${1:-all} in
all) ;;
hot-clear) flags+=(-DAZSDK_HOT_CLEAR_ONLY) ;;
hot-set) flags+=(-DAZSDK_HOT_SET_ONLY) ;;
memory-recall) flags+=(-DAZSDK_MEMORY_RECALL_ONLY) ;;
*) echo 'select all, hot-clear, hot-set or memory-recall' >&2; exit 2 ;;
esac
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
awk '/^static int st_mod_owner\(/,/^}/' src/azmod/sdk_track.c > "$task_tmp/owner.inc"
awk '/^static int sdk_command_submit\(/,/^}/' src/azmod/sdk_track.c > "$task_tmp/submit.inc"
"${HOST_CC:-cc}" -std=c11 "${flags[@]}" -DSDK_EXIT_OWNER_INC="\"$task_tmp/owner.inc\"" -DSDK_EXIT_SUBMIT_INC="\"$task_tmp/submit.inc\"" \
    -c tests/unit/sdkexit_public_test.c -o "$task_tmp/public.o"
"${HOST_CC:-cc}" -std=gnu11 -fexceptions -fasynchronous-unwind-tables "${flags[@]}" -c src/azstem/sdk_native_commands.c -o "$task_tmp/native-commands.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -fexceptions -fasynchronous-unwind-tables -c tests/unit/native_unwind.c -o "$task_tmp/native-guard.o"
"${HOST_CC:-cc}" -std=gnu11 -fexceptions -fasynchronous-unwind-tables "${flags[@]}" -DAZSDK_EXIT_TESTING -DAZSDK_CUE_STAGE_TESTING \
    src/azstem/sdk_exit.c "$task_tmp/native-commands.o" "$task_tmp/native-guard.o" tests/unit/sdkexit_test.c "$task_tmp/public.o" ${NATIVE_RUNTIME:--lstdc++ -ldl} -o "$task_tmp/stage"
for build in 1 2; do "$task_tmp/stage" "$build" 8; done
