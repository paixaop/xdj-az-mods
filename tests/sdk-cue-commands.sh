#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-cue-commands.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-DAZSDK_CUE_COMMANDS_TESTING -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread)
selection=()
case ${1:-all} in
all) ;;
hot-set) selection=(hot-set) ;;
admission) selection=(admission) ;;
priority) selection=(priority) ;;
selection) selection=(selection) ;;
refresh) selection=(refresh) ;;
*) echo 'select all, hot-set, admission, priority, selection or refresh' >&2; exit 2 ;;
esac
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
"${HOST_CXX:-c++}" "${flags[@]}" src/azstem/sdk_cue_commands.cpp tests/unit/sdkcuecommands_test.cpp -o "$task_tmp/commands"
if [[ ${1:-all} == all ]]; then "$task_tmp/commands" priority; "$task_tmp/commands" admission; "$task_tmp/commands" selection; "$task_tmp/commands" refresh; fi
"$task_tmp/commands" "${selection[@]}"
