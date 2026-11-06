#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) != Linux ]]; then
    echo 'SKIP: command delivery requires Linux eventfd and timerfd'
    exit 77
fi
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-command-delivery.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -Wno-unused-function -pthread -Isrc/common -Isrc/azmod)
link=()
if [[ ${AZ_COMMAND_REAL_X:-0} == 1 ]]; then
    flags+=(-DAZ_COMMAND_REAL_X)
    link+=(-lX11)
fi
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
awk '/^static int st_mod_owner\(/,/^}/' src/azmod/sdk_track.c > "$task_tmp/owner.inc"
awk '/^static int sdk_command_submit\(/,/^}/' src/azmod/sdk_track.c > "$task_tmp/submit.inc"
awk '
/^static const az_library_v1 g_sdk_library =/ { table = 1; print; next }
table && /^};/ { table = 0; print; next }
table && /^[ ]+\.(abi|size|command_[a-z_]+) =/ { print }
/^static const az_library_v1 \*sdk_library_service\(/,/^}/ { print }
/^static const az_core_v1 g_cm_core =/ { core = 1; print; next }
core && /^};/ { core = 0; print; next }
core && /^[ ]+\.(abi|size|library_service|command_submit) =/ { print }
' src/azmod/community.c > "$task_tmp/library.inc"
awk '/^\/\* X input can start an early frame/,/^}$/' src/azmod/mod_menu_overlay.c > "$task_tmp/overlay.inc"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" \
    -DSDK_OWNER_INC="\"$task_tmp/owner.inc\"" -DSDK_SUBMIT_INC="\"$task_tmp/submit.inc\"" \
    -DSDK_LIBRARY_INC="\"$task_tmp/library.inc\"" -DSDK_OVERLAY_INC="\"$task_tmp/overlay.inc\"" \
    -c tests/unit/sdk_command_delivery_test.c -o "$task_tmp/core.o"
"${HOST_CXX:-c++}" -std=c++11 "${flags[@]}" tests/unit/sdk_command_delivery_native.cpp "$task_tmp/core.o" \
    "${link[@]}" -o "$task_tmp/delivery"
"$task_tmp/delivery"
