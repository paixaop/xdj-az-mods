#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# tests/run-linux.sh: the unit tests on arm64 Linux, all three passes, on any host with Docker.
#
# Runs tests/run.sh in azmod-test:18.04 three times: plain, SAN=asan (AddressSanitizer +
# UndefinedBehaviorSanitizer) and SAN=tsan (ThreadSanitizer). Every pass runs even when an
# earlier one failed; the summary lists each pass's result line, and the exit status is nonzero
# if any pass failed. The images are built from docker/ when missing, and the test image is
# rebuilt when it lacks one of its pinned tools (clang-format 21.1.8, clang-10 with libFuzzer,
# clang-tidy-10, cppcheck 1.82); the layers it already has are reused. AZ_TEST_IMAGE names
# another tag for the test image (default azmod-test:18.04). Some suites start their
# own containers through the host's docker socket, so the tree is mounted at its own path
# (on macOS it must sit where Docker can share it, e.g. inside your home folder).
# ThreadSanitizer needs ASLR control (personality), hence seccomp=unconfined.
#
# A bounded preflight first proves clean sanitizer startup and planted fault detection.
# An unavailable runtime returns 2 before the long suite; it is not a pass or suppression.
# --preflight-only runs just these asset-independent checks.
# Needs: Docker, and the asset headers that tools/extract-assets.py writes (see tests/run.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
P=$PWD
preflight_only=0
case $# in
    0) ;;
    1) [ "$1" = --preflight-only ] || { echo 'Usage: tests/run-linux.sh [--preflight-only]' >&2; exit 2; }; preflight_only=1 ;;
    *) echo 'Usage: tests/run-linux.sh [--preflight-only]' >&2; exit 2 ;;
esac
python3 tests/unit/sanitizer_preflight_test.py
python3 tests/unit/sdk_native_integration_test.py
python3 tests/unit/sdk_completion_test.py
python3 tests/unit/sdk_exit_link_test.py
if ! docker image inspect azmod-build:18.04 >/dev/null 2>&1 ||
   ! docker run --rm --platform=linux/arm64 azmod-build:18.04 sh -c 'command -v g++ >/dev/null' >/dev/null 2>&1; then
    docker build --platform=linux/arm64 -f docker/azmod-build.Dockerfile -t azmod-build:18.04 .
fi
IMG=${AZ_TEST_IMAGE:-azmod-test:18.04}
has() { docker run --rm --platform=linux/arm64 "$IMG" sh -c "$1" >/dev/null 2>&1; }
if ! docker image inspect "$IMG" >/dev/null 2>&1 ||
   ! has 'command -v g++ >/dev/null' ||
   ! has 'clang-format --version | grep -q "clang-format version 21\.1\.8$"' ||
   ! has 'clang-10 --version && ls /usr/lib/llvm-10/lib/clang/10.0.0/lib/linux/libclang_rt.fuzzer-aarch64.a' ||
   ! has 'clang-tidy-10 --version | grep -q "LLVM version 10\.0\.0" && cppcheck --version | grep -q "^Cppcheck 1\.82$"'; then
    echo "building $IMG (missing, or lacking a pinned tool)"
    docker build --platform=linux/arm64 -t "$IMG" -f docker/azmod-test.Dockerfile docker
fi
preflight_logs=$(mktemp -d "${TMPDIR:-/tmp}/az-linux-preflight.XXXXXX")
printf 'Linux sanitizer preflight logs: %s\n' "$preflight_logs"
exec docker run --rm --platform=linux/arm64 --security-opt seccomp=unconfined \
    -v /var/run/docker.sock:/var/run/docker.sock -v "$P:$P" -v "$preflight_logs:/preflight" -w "$P" "$IMG" bash -c '
        preflight_status=0
        bash tests/sanitizer-preflight.sh --output /preflight/run || preflight_status=$?
        if [ "$preflight_status" -ne 0 ]; then
            echo "UNIT TESTS: NOT RUN (sanitizer preflight exit $preflight_status; no full integration pass)"
            exit "$preflight_status"
        fi
        if [ "$1" = 1 ]; then
            echo "PREFLIGHT ONLY: full unit suite NOT RUN"
            exit 0
        fi
        rc=0; sum=()
        for p in plain asan tsan; do
            log=$(mktemp); s=; [ "$p" = plain ] || s=$p; t0=$SECONDS
            SAN=$s bash tests/run.sh 2>&1 | tee "$log"
            st=${PIPESTATUS[0]}; [ "$st" -eq 0 ] || rc=1
            line=$(grep "^UNIT TESTS" "$log" | tail -1 || true)
            sum+=("$p ($((SECONDS - t0)) s, exit $st): ${line:-no UNIT TESTS line}"); rm -f "$log"
            sdk_log=$(mktemp); sdk_t0=$SECONDS
            SAN=$s bash tests/sdk-completion.sh --extensions-only 2>&1 | tee "$sdk_log"
            sdk_status=${PIPESTATUS[0]}; [ "$sdk_status" -eq 0 ] || rc=1
            sdk_line=$(grep "^SDK COMPLETION:" "$sdk_log" | tail -1 || true)
            sum+=("SDK $p ($((SECONDS - sdk_t0)) s, exit $sdk_status): ${sdk_line:-no SDK COMPLETION line}"); rm -f "$sdk_log"
            if [ "$p" != tsan ]; then
                reply_t0=$SECONDS
                SAN=$s bash tests/sdk-cache-reply-native.sh
                reply_status=$?; [ "$reply_status" -eq 0 ] || rc=1
                sum+=("native reply $p ($((SECONDS - reply_t0)) s, exit $reply_status)")
            fi
            if [ "$p" = plain ]; then
                for entry_suite in sdk-cache-entry sdk-cache-entry-caller sdk-cache-entry-fp; do
                    entry_t0=$SECONDS
                    bash "tests/$entry_suite.sh"
                    entry_status=$?; [ "$entry_status" -eq 0 ] || rc=1
                    sum+=("$entry_suite ($((SECONDS - entry_t0)) s, exit $entry_status)")
                done
            fi
            case "$p" in
                plain) status_suites=(sdk-status sdk-status-ui sdk-status-gui sdk-status-loader sdk-status-native sdk-status-maps sdk-status-map-source sdk-status-source sdk-status-store);;
                asan) status_suites=(sdk-status-ui sdk-status-native sdk-status-maps sdk-status-map-source sdk-status-source sdk-status-store);;
                tsan) status_suites=(sdk-status-source sdk-status-store);;
            esac
            for status_suite in "${status_suites[@]}"; do
                status_t0=$SECONDS
                SAN=$s bash "tests/$status_suite.sh"
                status_rc=$?; [ "$status_rc" -eq 0 ] || rc=1
                sum+=("$status_suite $p ($((SECONDS - status_t0)) s, exit $status_rc)")
            done
            for cue_suite in sdk-cue sdk-cue-public sdk-cue-export sdk-cue-events sdk-deck-listeners sdk-data-lifecycle sdk-slip sdk-exit; do
                cue_t0=$SECONDS
                SAN=$s bash "tests/$cue_suite.sh"
                cue_status=$?; [ "$cue_status" -eq 0 ] || rc=1
                sum+=("$cue_suite $p ($((SECONDS - cue_t0)) s, exit $cue_status)")
            done
        done
        echo; echo "=== tests/run-linux.sh: every pass ==="; printf "  %s\n" "${sum[@]}"
        exit $rc' _ "$preflight_only"
