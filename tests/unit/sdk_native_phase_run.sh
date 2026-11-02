#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Bounded standalone manifest runner for separately frozen native SDK phases.
set -u
cd "$(dirname "$0")" || exit 2
mkdir logs || exit 2
unset LD_PRELOAD LSAN_OPTIONS
ulimit -c 0
export LC_ALL=C ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
before=$(pidof EP147 || true)
printf 'EP147 before: %s\n' "$before"
rc=0; passed=0; caught=0; runs=0; controls=0
read -r expected_runs expected_controls <phase-counts || exit 2
case "$expected_runs:$expected_controls" in *[!0-9:]*|:*|*:) exit 2 ;; esac
[ "$expected_runs" -gt 0 ] && [ "$expected_controls" -gt 0 ] || exit 2
invoke() {
    status=0
    case "$label:$executable" in *[!a-zA-Z0-9_.:-]*|:*) status=125; return ;; esac
    case "$one:$two" in *[!a-zA-Z0-9_.:-]*) status=125; return ;; esac
    set --
    [ "$one" = - ] || set -- "$one"
    [ "$two" = - ] || set -- "$@" "$two"
    nice -n 10 ./limit "${AZ_SDK_NATIVE_TIMEOUT:-120}" "./$executable" "$@" >"logs/$label.log" 2>&1 || status=$?
}
tab=$(printf '\t')
while IFS="$tab" read -r label executable one two; do
    runs=$((runs + 1)); invoke
    if [ "$status" -eq 0 ] && ! grep -q -E '^SKIP([[:space:]:]|$)' "logs/$label.log"; then
        printf 'PASS native phase: %s\n' "$label"; passed=$((passed + 1))
    else
        printf 'FAIL native phase: %s exit=%s\n' "$label" "$status"; rc=1
        tail -n 5 "logs/$label.log" 2>/dev/null || true
    fi
done <phase-runs.tsv
while IFS="$tab" read -r label executable diagnostic one two; do
    controls=$((controls + 1)); invoke
    if [ "$status" -ne 0 ] && [ "$status" -ne 124 ] && [ "$status" -ne 125 ] && [ "$status" -ne 126 ] && [ "$status" -ne 127 ] && grep -q -F "$diagnostic" "logs/$label.log"; then
        printf 'CONTROL caught native phase: %s\n' "$label"; caught=$((caught + 1))
    else
        printf 'FAIL native phase control: %s exit=%s (required diagnostic absent)\n' "$label" "$status"; rc=1
        tail -n 5 "logs/$label.log" 2>/dev/null || true
    fi
done <phase-controls.tsv
if [ "$runs" -ne "$expected_runs" ] || [ "$controls" -ne "$expected_controls" ]; then
    echo 'FAIL native phase manifest count mismatch'; rc=1
fi
after=$(pidof EP147 || true)
printf 'EP147 after: %s\n' "$after"
[ "$before" = "$after" ] || { echo 'FAIL EP147 changed during standalone phase'; rc=1; }
if [ "$rc" -eq 0 ]; then
    printf 'SDK NATIVE PHASE: OK (%s positives, %s exact diagnostic controls)\n' "$passed" "$caught"
else
    printf 'SDK NATIVE PHASE: FAILED (%s/%s positives passed, %s/%s controls caught)\n' "$passed" "$runs" "$caught" "$controls"
fi
exit "$rc"
