#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Run every finalized SDK delivery suite. Missing services and skips are not passes.
set -uo pipefail
cd "$(dirname "$0")/.."
extensions_only=0; output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --extensions-only) extensions_only=1; shift ;;
        --output) [ "$#" -ge 2 ] || exit 2; output=$2; shift 2 ;;
        *) echo 'Usage: sdk-completion.sh [--extensions-only] [--output NEW_DIRECTORY]' >&2; exit 2 ;;
    esac
done
case ${SAN:-plain} in plain|''|asan|tsan) ;; *) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;; esac
if [ -n "$output" ]; then
    [ ! -e "$output" ] || { echo 'SDK log directory must be new' >&2; exit 2; }
    mkdir "$output" || exit 2
else
    output=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-completion.XXXXXX") || exit 2
fi
output=$(cd "$output" && pwd)
printf 'SDK completion logs: %s\n' "$output"
suites=(automation time command exit cue-actor cue-store cue-lifecycle cue-persist cue-recall cue-stage cue-commands stored live sources native-commands load load-lifecycle vtable hook-install source-probe load-probe wave-overview wave-report)
[ "$extensions_only" -eq 1 ] || suites=(data "${suites[@]}")
failures=0; incomplete=0
for name in "${suites[@]}"; do
    script=tests/sdk-$name.sh
    case "$name" in wave-*) script=tests/$name.sh ;; esac
    if [ ! -f "$script" ]; then
        echo "SDK FAIL: required suite $script is missing"
        failures=$((failures + 1)); continue
    fi
    status=0
    bash "$script" >"$output/$name.log" 2>&1 || status=$?
    cat "$output/$name.log"
    if [ "$status" -eq 77 ] || { [ "$status" -eq 0 ] && grep -q -E '^SKIP([[:space:]:]|$)' "$output/$name.log"; }; then
        echo "SDK INCOMPLETE: $name (${SAN:-plain}; required checks skipped)"
        incomplete=$((incomplete + 1))
    elif [ "$status" -ne 0 ]; then
        echo "SDK FAIL: $name (${SAN:-plain}; exit $status)"
        failures=$((failures + 1))
    else
        echo "SDK PASS: $name (${SAN:-plain})"
    fi
done
if [ "$failures" -gt 0 ]; then
    echo "SDK COMPLETION: FAILED ($failures suite(s) failed, $incomplete incomplete)"
    exit 1
fi
if [ "$incomplete" -gt 0 ]; then
    echo "SDK COMPLETION: INCOMPLETE ($incomplete suite(s)); no full SDK pass"
    exit 2
fi
echo "SDK COMPLETION: OK (${SAN:-plain}; all required suites and controls)"
