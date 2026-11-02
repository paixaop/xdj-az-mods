#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-exit.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -pthread -Isrc/common -Isrc/azmod -Wno-unused-function)
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
"${HOST_CXX:-c++}" -std=c++11 "${flags[@]}" -DAZSDK_EXIT_TESTING \
    src/azstem/sdk_exit.cpp src/azstem/sdk_native_commands.cpp tests/unit/sdkexit_test.cpp "$task_tmp/public.o" -o "$task_tmp/exit"
for build in 1 2; do
    "$task_tmp/exit" "$build"
    "$task_tmp/exit" "$build" 1
    "$task_tmp/exit" "$build" 2
    "$task_tmp/exit" "$build" 4
    "$task_tmp/exit" "$build" 7
    "$task_tmp/exit" "$build" 8
    "$task_tmp/exit" "$build" 15
done

# Keep private endpoint prefix compatibility in the same required suite.
bash tests/sdk-exit-compat.sh
