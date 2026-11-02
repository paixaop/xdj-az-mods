#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run only this directory's standalone binaries. No EP147 control or native entry points.
set -u
cd "$(dirname "$0")" || exit 2
unset LD_PRELOAD
ulimit -c 0
mkdir logs || exit 2
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
export TSAN_OPTIONS=halt_on_error=1
unset LSAN_OPTIONS
export LC_ALL=C
rc=0
passed=0; caught=0; failures=0
completed=0
before=$(pidof EP147 || true)
printf 'Architecture: '; uname -m
printf 'EP147 before: %s\n' "$before"
cat /tmp/az-decks 2>/dev/null || true
finish() {
    after=$(pidof EP147 || true)
    printf 'EP147 after: %s\n' "$after"
    cat /tmp/az-decks 2>/dev/null || true
    if [ "$before" != "$after" ]; then
        echo 'FAIL EP147 changed during standalone validation; investigate concurrent activity'
        rc=1; failures=$((failures + 1))
    fi
    if [ "$completed" -eq 1 ]; then
        if [ "$rc" -eq 0 ]; then
            printf 'SDK NATIVE COMPLETION: OK (%s standalone runs, %s diagnostic controls; foundation=%s)\n' "$passed" "$caught" "$foundation"
        else
            printf 'SDK NATIVE COMPLETION: FAILED (%s run/control failures; foundation=%s)\n' "$failures" "$foundation"
        fi
    fi
    exit "$rc"
}
preflight_status=0
nice -n 10 sh preflight/run.sh "$PWD/preflight" >logs/sanitizer-preflight.log 2>&1 || preflight_status=$?
cat logs/sanitizer-preflight.log
[ ! -d preflight/logs ] || cp -R preflight/logs logs/sanitizer-preflight
if [ "$preflight_status" -ne 0 ]; then
    echo "SDK native suites: NOT RUN (sanitizer preflight exit $preflight_status)"
    rc=$preflight_status
    finish
fi
if [ -d runtime ]; then
    shared_status=0
    export LD_LIBRARY_PATH="$PWD/runtime"
    nice -n 10 sh shared-preflight/run.sh "$PWD/shared-preflight" >logs/shared-sanitizer-preflight.log 2>&1 || shared_status=$?
    cat logs/shared-sanitizer-preflight.log
    [ ! -d shared-preflight/logs ] || cp -R shared-preflight/logs logs/shared-sanitizer-preflight
    if [ "$shared_status" -ne 0 ]; then
        echo "SDK native suites: NOT RUN (private shared sanitizer preflight exit $shared_status)"
        rc=$shared_status
        finish
    fi
fi
run() {
    name=$1; shift
    run_named "$name" "$name" "$@"
}
run_named() {
    name=$1; binary=$2; shift 2
    status=0
    nice -n 10 ./preflight/limit "${AZ_SDK_NATIVE_TIMEOUT:-120}" "./$binary" "$@" >"logs/$name.log" 2>&1 || status=$?
    if [ "$status" -eq 0 ] && ! grep -q -E '^SKIP([[:space:]:]|$)' "logs/$name.log"; then
        printf 'PASS %s\n' "$name"; passed=$((passed + 1))
    else
        printf 'FAIL %s exit=%s\n' "$name" "$status"
        rc=1; failures=$((failures + 1))
    fi
    tail -n 3 "logs/$name.log"
}
control() {
    name=$1; diagnostic=$2
    control_named "$name" "$name" "$diagnostic"
}
control_named() {
    name=$1; binary=$2; diagnostic=$3; shift 3
    status=0
    nice -n 10 ./preflight/limit "${AZ_SDK_NATIVE_TIMEOUT:-120}" "./$binary" "$@" >"logs/$name.log" 2>&1 || status=$?
    if [ "$status" -ne 0 ] && [ "$status" -ne 124 ] && [ "$status" -ne 125 ] && [ "$status" -ne 126 ] && [ "$status" -ne 127 ] && grep -q -E "$diagnostic" "logs/$name.log"; then
        printf 'CONTROL caught %s exit=%s\n' "$name" "$status"
        caught=$((caught + 1))
    else
        printf 'FAIL control %s exit=%s (required diagnostic absent)\n' "$name" "$status"
        rc=1; failures=$((failures + 1))
    fi
}
early_shared_controls=0
if [ -d runtime ]; then
    early_shared_controls=1
    control ext-live-payload-overread-control 'AddressSanitizer: heap-buffer-overflow'
    control ext-live-rtti-overread-control 'AddressSanitizer: global-buffer-overflow'
    if [ "$rc" -ne 0 ]; then
        echo 'SDK native suites: NOT RUN (private allocator-backed sanitizer controls failed)'
        finish
    fi
fi
foundation=$(cat suite-foundation 2>/dev/null || true)
case "$foundation" in 0|1) ;; *) echo 'FAIL missing foundation selection'; rc=1; finish ;; esac
if [ "$foundation" -eq 1 ]; then
for pass in plain asan tsan; do
    run "data-$pass"
    run "waveform-$pass"
    run "source-identity-$pass"
    run "provider-$pass"
    run "deck-$pass"
    run "loop-$pass"
    run "observation-$pass"
    run "loop-export-$pass"
    if [ -d samples ]; then
        if [ -f samples/ANLZ0000.2EX ]; then
            run "range-$pass" samples/ANLZ0000.EXT samples/ANLZ0000.2EX
        else
            run "range-$pass" samples/ANLZ0000.EXT
        fi
        if [ -f samples/ANLZ0000.2EX ]; then
            run "real-$pass" samples/ANLZ0000.DAT samples/ANLZ0000.EXT samples/ANLZ0000.2EX
        else
            run "real-$pass" samples/ANLZ0000.DAT samples/ANLZ0000.EXT
        fi
    fi
done
control identity-control 'Assertion.*failed|assertion.*failed'
control source-tuple-control 'Assertion.*failed|assertion.*failed'
control source-wrap-control 'Assertion.*failed|assertion.*failed'
control prefix-control 'Assertion.*failed|assertion.*failed'
control loop-player-control 'Assertion.*failed|assertion.*failed'
control loop-generation-control 'Assertion.*failed|assertion.*failed'
control loop-byte-zero-control 'Assertion.*failed|assertion.*failed'
control loop-zero-time-control 'Assertion.*failed|assertion.*failed'
control loop-reason-control 'Assertion.*failed|assertion.*failed'
control loop-numeric-control 'Assertion.*failed|assertion.*failed'
control wave-first-control 'Assertion.*failed|assertion.*failed'
control wave-count-control 'Assertion.*failed|assertion.*failed'
control observation-size-control 'Assertion.*failed|assertion.*failed'
control observation-padding-control 'Assertion.*failed|assertion.*failed'
control loop-export-v1-size-control 'Assertion.*failed|assertion.*failed'
control loop-export-v2-size-control 'Assertion.*failed|assertion.*failed'
for name in timing-size timing-version; do
    control "review-$name-control" 'Assertion.*failed|assertion.*failed|ERROR: AddressSanitizer:|runtime error:'
done
for name in database-alias database-uri missing-parent wave-errors analysis-binding track-fifo wave-fifo analysis-path-limit preview-extension; do
    control "review-$name-control" 'Assertion.*failed|assertion.*failed|^FAIL  '
done
control asan-control 'ERROR: AddressSanitizer:|runtime error:'
control tsan-control 'WARNING: ThreadSanitizer: data race'
fi
# These manifests are generated from the same frozen source tree as the binaries.
# No eval, shell command string or target-installed timeout utility is required.
if [ ! -s sdk-extension-runs.tsv ] || [ ! -s sdk-extension-controls.tsv ]; then
    echo 'FAIL required SDK extension manifests are missing or empty'; rc=1; finish
fi
tab=$(printf '\t')
while IFS="$tab" read -r label binary argument; do
    case "$label:$binary" in *[!a-zA-Z0-9_.:-]*|:*) echo 'FAIL invalid run manifest'; rc=1; continue ;; esac
    if [ "$argument" = - ]; then
        run_named "$label" "$binary"
    else
        case "$argument" in 1|2|clock|budget|elapsed|grid|interval) run_named "$label" "$binary" "$argument" ;; *) echo 'FAIL invalid run argument'; rc=1 ;; esac
    fi
    case "$binary" in
        ext-mono-preview-*)
            if [ -d samples ]; then
                run_named "$label-real-files" "$binary" samples/ANLZ0000.DAT
            fi ;;
        ext-stored-*)
            if [ -d samples ]; then
                if [ -f samples/ANLZ0000.2EX ]; then
                    run_named "$label-real-files" "$binary" samples/ANLZ0000.DAT samples/ANLZ0000.EXT samples/ANLZ0000.2EX
                else
                    run_named "$label-real-files" "$binary" samples/ANLZ0000.DAT samples/ANLZ0000.EXT
                fi
            fi ;;
    esac
done <sdk-extension-runs.tsv
while IFS="$tab" read -r label binary diagnostic argument; do
    case "$label" in
        ext-live-payload-overread-control|ext-live-rtti-overread-control)
            [ "$early_shared_controls" -eq 0 ] || continue ;;
    esac
    case "$label:$binary" in *[!a-zA-Z0-9_.:-]*|:*) echo 'FAIL invalid control manifest'; rc=1; continue ;; esac
    if [ "$argument" = - ]; then
        control_named "$label" "$binary" "$diagnostic"
    else
        case "$argument" in 1|2|clock|budget|elapsed|grid|interval|--sync-payload-short) control_named "$label" "$binary" "$diagnostic" "$argument" ;; *) echo 'FAIL invalid control argument'; rc=1 ;; esac
    fi
done <sdk-extension-controls.tsv
completed=1
finish
