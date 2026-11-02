#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run already-built, owned probes with a deadline. A runtime blocker never counts as a pass.
set -u
[ "$#" -ge 1 ] || { echo 'Usage: sanitizer_preflight_run.sh BINARY_DIRECTORY [plain asan tsan]' >&2; exit 2; }
work=$(cd "$1" && pwd) || exit 2
shift
[ "$#" -gt 0 ] || set -- plain asan tsan
mkdir "$work/logs" || { echo 'Preflight log directory must be new' >&2; exit 2; }
unset LD_PRELOAD
unset LSAN_OPTIONS
ulimit -c 0
export LC_ALL=C
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
export TSAN_OPTIONS=halt_on_error=1
failures=0
unavailable=0
status=0
probe() {
    mode=$1; operation=$2
    status=0
    "$work/limit" "${AZ_SAN_PREFLIGHT_TIMEOUT:-15}" "$work/$mode" "$operation" 64 >"$work/logs/$mode-$operation.log" 2>&1 || status=$?
}
runtime_blocker() {
    grep -q -E 'ThreadSanitizer: unsupported VMA range|LeakSanitizer has encountered a fatal error|Unable to find a guest_base|cannot mmap the shadow memory|preflight probe timed out' "$1"
}
control() {
    mode=$1; operation=$2; wanted=$3
    probe "$mode" "$operation"
    if [ "$status" -ne 0 ] && [ "$status" -ne 124 ] && [ "$status" -ne 125 ] && [ "$status" -ne 126 ] && [ "$status" -ne 127 ] && grep -q -E "$wanted" "$work/logs/$mode-$operation.log"; then
        printf 'PREFLIGHT CONTROL caught: %s %s\n' "$mode" "$operation"
    else
        printf 'PREFLIGHT FAIL: %s %s control exit=%s, required diagnostic absent\n' "$mode" "$operation" "$status"
        failures=$((failures + 1))
    fi
}
for mode in "$@"; do
    case "$mode" in plain|asan|tsan) ;; *) echo "Invalid preflight mode: $mode" >&2; exit 2 ;; esac
    probe "$mode" clean
    if [ "$status" -ne 0 ] || ! grep -q '^preflight clean-ready$' "$work/logs/$mode-clean.log"; then
        if runtime_blocker "$work/logs/$mode-clean.log"; then
            printf 'PREFLIGHT UNAVAILABLE: %s clean runtime (exit=%s)\n' "$mode" "$status"
            unavailable=$((unavailable + 1))
        else
            printf 'PREFLIGHT FAIL: %s clean probe (exit=%s)\n' "$mode" "$status"
            failures=$((failures + 1))
        fi
        cat "$work/logs/$mode-clean.log"
        continue
    fi
    printf 'PREFLIGHT PASS: %s clean runtime\n' "$mode"
    case "$mode" in
        plain) control "$mode" expected-failure '^preflight expected failure$' ;;
        asan)
            control "$mode" heap 'ERROR: AddressSanitizer: heap-buffer-overflow'
            control "$mode" signed 'runtime error: signed integer overflow'
            control "$mode" leak 'ERROR: LeakSanitizer: detected memory leaks'
            ;;
        tsan) control "$mode" race 'WARNING: ThreadSanitizer: data race' ;;
    esac
done
if [ "$failures" -ne 0 ]; then
    printf 'PREFLIGHT: FAILED (%s failure(s), %s unavailable runtime(s))\n' "$failures" "$unavailable"
    exit 1
fi
if [ "$unavailable" -ne 0 ]; then
    printf 'PREFLIGHT: UNAVAILABLE (%s runtime(s)); full suite not established\n' "$unavailable"
    exit 2
fi
echo 'PREFLIGHT: OK (clean runs and all required fault controls)'
