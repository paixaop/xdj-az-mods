#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cue-actor.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-DAZSDK_CUE_ACTOR_TESTING -std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CC:-cc}" "${flags[@]}" src/azstem/sdk_cue_actor.c tests/unit/sdkcueactor_test.c src/common/aznative_guard.c -lstdc++ -ldl -o "$task_tmp/actor"
"$task_tmp/actor"
