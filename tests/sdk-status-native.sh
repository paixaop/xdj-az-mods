#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tests/unit/sdkstatus_timestamps_test.py
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
flags=(-O1 -g -fexceptions -fasynchronous-unwind-tables -Wno-unused-function -Wall -Wextra -Werror -pthread -DAZ_STATUS_NATIVE_PRODUCER -DAZSDK_STATUS_LANE_TEST -Isrc/common -Isrc/azmod)
case ${SAN:-plain} in
    asan) flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer);;
    tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer);;
    plain) ;;
    *) exit 2;;
esac
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" tests/unit/sdkstatus_identity_test.c -o "$dir/identities"
"$dir/identities"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -c src/azstem/sdk_status_capture.c -o "$dir/lane.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -D_GNU_SOURCE -Wno-unused-function \
    -c tests/unit/sdkstatus_map_alias_bridge.c -o "$dir/map-alias.o"
"${HOST_CC:-cc}" -std=gnu11 -fexceptions -fasynchronous-unwind-tables "${flags[@]}" -c src/azstem/sdk_native_commands.c -o "$dir/native-commands.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -fexceptions -fasynchronous-unwind-tables -c src/common/aznative_guard.c -o "$dir/native-guard.o"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -DAZSDK_EXIT_TESTING -DAZSDK_MAP_ALIAS_TESTING \
    tests/unit/sdkstatus_native_test.c "$dir/native-commands.o" "$dir/native-guard.o" "$dir/lane.o" "$dir/map-alias.o" -lstdc++ -ldl -o "$dir/test"
"$dir/test" transport-retirement
"$dir/test" hot-retirement
"$dir/test" hot-metadata
"$dir/test" loop-retirement
"$dir/test" delayed-loop-seed
"$dir/test" motion-pressure
"$dir/test"
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" tests/unit/sdkstatus_stems_test.c "$dir/lane.o" -o "$dir/stems"
"$dir/stems"
"${HOST_CC:-gcc}" -std=gnu11 -fexceptions -fasynchronous-unwind-tables -D_GNU_SOURCE "${flags[@]}" -DAZ_STATUS_MEMORY_PRODUCER tests/unit/sdkstatus_memory_test.c tests/unit/sdkstatus_memory_return.S src/azstem/sdk_status_memory_entry.S -ldl -lm -o "$dir/memory"
"$dir/memory" 1
"$dir/memory" 2
"${HOST_CC:-cc}" -std=gnu11 "${flags[@]}" -Wno-unused-function tests/unit/sdkstatus_native_reduce_test.c \
    -lm -ldl -o "$dir/reducer"
"$dir/reducer" source-authority
"$dir/reducer" native-load-join
"$dir/reducer" delayed-loop-seed
"$dir/reducer" slip-barrier
"$dir/reducer" early-native-head
"$dir/reducer" hot-metadata
"$dir/reducer" cue-storage-flags
"$dir/reducer"
"${HOST_CC:-gcc}" -std=gnu11 -fexceptions -fasynchronous-unwind-tables "${flags[@]}" -DAZSDK_LOAD_TESTING \
    tests/unit/sdkstatus_native_load_test.c src/azstem/sdk_load_observer.c "$dir/lane.o" src/common/aznative_guard.c -lstdc++ -ldl -o "$dir/load"
"$dir/load" 1
"$dir/load" 2
