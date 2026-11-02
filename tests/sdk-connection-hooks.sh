#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Forwarding-adapter fixture. No native hooks, SQLite, DB/key or device IO.
# Module proof is separate; EXPECT_RED=1 accepts only the concrete missing
# observation after real typed forwarding, never compiler/startup failures.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: connection-hooks requires Linux and GNU libstdc++'
    exit 77
fi
task_parent=${EVIDENCE_DIR:-${TMPDIR:-/tmp}}
mkdir -p "$task_parent"
task_tmp=$(mktemp -d "$task_parent/az-sdk-connection-hooks.XXXXXX")
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$task_tmp"' EXIT; fi
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -DAZSDK_CONNECTION_HOOKS_TESTING)
sources=(tests/unit/sdkconnectionhooks_test.cpp)
if [ -f src/azmod/sdk_connection_hooks.cpp ]; then sources+=(src/azmod/sdk_connection_hooks.cpp); fi
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
{
    uname -a
    "${HOST_CXX:-c++}" --version
    sha256sum tests/unit/sdkconnectionhooks_test.cpp tests/sdk-connection-hooks.sh src/common/azconnectionepoch.h
    for input in src/common/azconnectionhooks.h src/azmod/sdk_connection_hooks.cpp; do
        if [ -f "$input" ]; then sha256sum "$input"; else echo "MISSING $input"; fi
    done
} >"$task_tmp/inputs.txt"
"${HOST_CXX:-c++}" "${flags[@]}" "${sources[@]}" -ldl -o "$task_tmp/connection-hooks"
failures=0
scenarios=(ordinary close-fails open-fails empty-close empty-open failures)
if [ "${EXPECT_RED:-0}" = 1 ]; then scenarios=(ordinary); fi
for profile in 1 2; do
    for scenario in "${scenarios[@]}"; do
        status=0
        (ulimit -c 0; "$task_tmp/connection-hooks" "$profile" "$scenario") >"$task_tmp/$profile-$scenario.log" 2>&1 || status=$?
        cat "$task_tmp/$profile-$scenario.log"
        if [ "${EXPECT_RED:-0}" = 1 ]; then
            first_failure=$(sed -n '/FAIL connection-hooks:/ { p; q; }' "$task_tmp/$profile-$scenario.log")
            if [ "$status" = 0 ] || [ "$status" = 77 ] ||
               ! grep -q "NATIVE_FORWARD_OK build=$profile context=0 adapter=0" "$task_tmp/$profile-$scenario.log" ||
               [[ "$first_failure" != *'missing adapter-owned successful connection birth'* ]] ||
               grep -Eq 'ERROR: (AddressSanitizer|ThreadSanitizer)|runtime error:|undefined symbol|Segmentation fault' "$task_tmp/$profile-$scenario.log"; then
                echo "FAIL: profile $profile did not reach the concrete absent-adapter runtime RED" >&2
                failures=$((failures + 1)); continue
            fi
            echo "EXPECTED_RUNTIME_RED profile=$profile: typed native arguments/results forwarded, adapter birth observation missing"
        elif [ "$status" != 0 ]; then failures=$((failures + 1)); fi
    done
done
if [ -n "${EVIDENCE_DIR:-}" ]; then echo "EVIDENCE_RETAINED=$task_tmp"; fi
[ "$failures" = 0 ]
