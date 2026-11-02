#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Private bookkeeping fixture only. No native hooks, DB, key, VM or deck access.
# EXPECT_RED=1 accepts only the concrete runtime missing-birth assertion after
# successful forwarding. Compiler/startup/sanitizer failures are not RED.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: connection-epoch fixture requires Linux and GNU libstdc++'
    exit 77
fi
task_parent=${EVIDENCE_DIR:-${TMPDIR:-/tmp}}
mkdir -p "$task_parent"
task_tmp=$(mktemp -d "$task_parent/az-sdk-connection-epoch.XXXXXX")
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$task_tmp"' EXIT; fi
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
if [ -n "${EPOCH_HEADER:-}" ]; then flags+=("-DAZCONNECTION_EPOCH_HEADER=\"${EPOCH_HEADER}\""); fi
{
    uname -a
    "${HOST_CXX:-c++}" --version
    sha256sum tests/unit/sdkconnectionepoch_test.cpp tests/sdk-connection-epoch.sh
    if [ -e src/common/azconnectionepoch.h ]; then sha256sum src/common/azconnectionepoch.h; else echo 'PRODUCTION_HEADER_MISSING'; fi
} >"$task_tmp/inputs.txt"
"${HOST_CXX:-c++}" "${flags[@]}" tests/unit/sdkconnectionepoch_test.cpp -o "$task_tmp/connection-epoch"
failures=0
for profile in 1 2; do
    status=0
    (ulimit -c 0; "$task_tmp/connection-epoch" "$profile") >"$task_tmp/profile-$profile.log" 2>&1 || status=$?
    cat "$task_tmp/profile-$profile.log"
    if [ "${EXPECT_RED:-0}" = 1 ]; then
        first_failure=$(sed -n '/FAIL connection-epoch:/ { p; q; }' "$task_tmp/profile-$profile.log")
        if [ "$status" = 0 ] || [ "$status" = 77 ] ||
           ! grep -q "NATIVE_FORWARD_OK build=$profile context=0 implementation=0" "$task_tmp/profile-$profile.log" ||
           [[ "$first_failure" != *'missing observed successful connection birth'* ]] ||
           grep -Eq 'ERROR: (AddressSanitizer|ThreadSanitizer)|runtime error:|undefined symbol|Segmentation fault' "$task_tmp/profile-$profile.log"; then
            echo "FAIL: profile $profile did not produce the required missing-helper runtime RED" >&2
            failures=$((failures + 1)); continue
        fi
        echo "EXPECTED_RUNTIME_RED profile=$profile: successful native-operation forwarding has no observed connection generation"
    elif [ "$status" != 0 ]; then failures=$((failures + 1)); fi
done
if [ -n "${EVIDENCE_DIR:-}" ]; then echo "EVIDENCE_RETAINED=$task_tmp"; fi
[ "$failures" = 0 ]
