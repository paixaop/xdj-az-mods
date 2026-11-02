#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Optional private-load logger checks; no native process or firmware is executed.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-load-probe.XXXXXX")
trap 'rm -rf "$work"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Isrc/common -Itests/integration)
san=()
case ${SAN:-plain} in
plain|'') ;;
asan) san=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) san=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
python3 tests/unit/sdk_load_probe_extract.py "$work"
${HOST_CC:-cc} "${flags[@]}" "${san[@]}" -DLOAD_PROBE_INC="\"$work/real.inc\"" tests/unit/sdkloadprobe_test.c -o "$work/real"
"$work/real"
${HOST_CC:-cc} "${flags[@]}" "${san[@]}" -DLOAD_POLL_INC="\"$work/poll-real.inc\"" tests/unit/sdkloadprobepoll_test.c -pthread -ldl -o "$work/poll-real"
"$work/poll-real"
${HOST_CC:-cc} "${flags[@]}" "${san[@]}" -DAZSDK_LOOP_PROBE_TICKS=36000 -DLOAD_POLL_INC="\"$work/poll-real.inc\"" tests/unit/sdkloadprobepoll_test.c -pthread -ldl -o "$work/poll-long"
"$work/poll-long"
${HOST_CC:-cc} "${flags[@]}" "${san[@]}" -DAZSDK_LOOP_PROBE_OPERATOR=1 -DLOAD_POLL_INC="\"$work/poll-real.inc\"" tests/unit/sdkloadprobepoll_test.c -pthread -ldl -o "$work/poll-operator"
"$work/poll-operator"
for limit in 0 36001; do
    if ${HOST_CC:-cc} "${flags[@]}" -DAZSDK_LOOP_PROBE_TICKS="$limit" -DLOAD_POLL_INC="\"$work/poll-real.inc\"" tests/unit/sdkloadprobepoll_test.c -pthread -ldl -o "$work/poll-invalid" >"$work/poll-invalid.log" 2>&1; then
        echo "FAIL: out-of-range probe duration $limit compiled" >&2; exit 1
    fi
    grep -q 'probe duration must be between' "$work/poll-invalid.log" || { cat "$work/poll-invalid.log" >&2; exit 1; }
    echo "CONTROL caught: out-of-range probe duration $limit"
done
for name in stop expiry; do
    ${HOST_CC:-cc} "${flags[@]}" -DAZSDK_LOOP_PROBE_OPERATOR=1 -DLOAD_POLL_INC="\"$work/operator-$name.inc\"" tests/unit/sdkloadprobepoll_test.c -pthread -ldl -o "$work/operator-$name"
    status=0
    (ulimit -c 0; "$work/operator-$name" >"$work/operator-$name.log" 2>&1) 2>/dev/null || status=$?
    if [ "$status" -ne 134 ]; then echo "FAIL: operator probe $name control exit=$status" >&2; cat "$work/operator-$name.log" >&2; exit 1; fi
    grep -Eq 'Assertion .* failed|Assertion failed' "$work/operator-$name.log" || { cat "$work/operator-$name.log" >&2; exit 1; }
    echo "CONTROL caught: operator probe $name"
done
if ${HOST_CC:-cc} "${flags[@]}" -DAZSDK_LOOP_PROBE_OPERATOR=2 -DLOAD_POLL_INC="\"$work/poll-real.inc\"" tests/unit/sdkloadprobepoll_test.c -pthread -ldl -o "$work/operator-invalid" >"$work/operator-invalid.log" 2>&1; then
    echo "FAIL: invalid operator flag compiled" >&2; exit 1
fi
grep -q 'operator probe flag must be zero or one' "$work/operator-invalid.log"
for name in size version reserved required future unit generation epoch time native-id coherence unavailable history budget; do
    ${HOST_CC:-cc} "${flags[@]}" -DLOAD_PROBE_INC="\"$work/$name.inc\"" tests/unit/sdkloadprobe_test.c -o "$work/$name"
    status=0
    (ulimit -c 0; "$work/$name" >"$work/$name.log" 2>&1) 2>/dev/null || status=$?
    if [ "$status" -ne 134 ]; then echo "FAIL: private load probe $name control exit=$status (expected assertion abort134)" >&2; cat "$work/$name.log" >&2; exit 1; fi
    grep -Eq 'Assertion .* failed|Assertion failed' "$work/$name.log" || { echo "FAIL: private load probe $name lacked its assertion" >&2; cat "$work/$name.log" >&2; exit 1; }
    echo "CONTROL caught: private load probe $name exit=$status"
done
for name in export decks errors cap ticks interval; do
    ${HOST_CC:-cc} "${flags[@]}" -DLOAD_POLL_INC="\"$work/poll-$name.inc\"" tests/unit/sdkloadprobepoll_test.c -pthread -ldl -o "$work/poll-$name"
    status=0
    (ulimit -c 0; "$work/poll-$name" >"$work/poll-$name.log" 2>&1) 2>/dev/null || status=$?
    if [ "$status" -ne 134 ]; then echo "FAIL: private load polling $name control exit=$status (expected assertion abort134)" >&2; cat "$work/poll-$name.log" >&2; exit 1; fi
    grep -Eq 'Assertion .* failed|Assertion failed' "$work/poll-$name.log" || { echo "FAIL: private load polling $name lacked its assertion" >&2; cat "$work/poll-$name.log" >&2; exit 1; }
    echo "CONTROL caught: private load polling $name exit=$status"
done
