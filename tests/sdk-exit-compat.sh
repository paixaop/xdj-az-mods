#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-exit-compat.XXXXXX")
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
    tests/unit/sdkexit_public_test.c tests/unit/sdkexit_compat_test.c -o "$task_tmp/compat"
"$task_tmp/compat"
