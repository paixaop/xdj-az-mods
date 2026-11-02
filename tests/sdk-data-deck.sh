#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Standalone native-kernel checks. Never installs a core/mod, changes settings or restarts EP147.
# Usage: tests/sdk-data-deck.sh [--extensions-only] [--compile-only] HOST SSH_KEY KNOWN_HOSTS NEW_LOG_DIRECTORY
# Optional: ANLZ_SAMPLE_DIR=/path/to/user-owned/ANLZ0000.{DAT,EXT,2EX}
set -euo pipefail
cd "$(dirname "$0")/.."
original=$PWD
foundation=1; compile_only=0; port=22
while [ "$#" -gt 0 ]; do
    case "$1" in
        --extensions-only) foundation=0; shift ;;
        --compile-only) compile_only=1; shift ;;
        --port) [ "$#" -ge 2 ] || exit 2; port=$2; shift 2 ;;
        *) break ;;
    esac
done
[[ $port =~ ^[1-9][0-9]{0,4}$ ]] && [ "$port" -le 65535 ] || { echo 'Invalid SSH port' >&2; exit 2; }
if [ "$#" != 4 ]; then
    echo 'Usage: tests/sdk-data-deck.sh [--extensions-only] [--compile-only] [--port PORT] HOST SSH_KEY KNOWN_HOSTS NEW_LOG_DIRECTORY' >&2
    exit 2
fi
host=$1; key=$2; known=$3; logs=$4
[[ $host =~ ^[a-zA-Z0-9][a-zA-Z0-9.:-]*$ ]] || { echo 'Invalid host' >&2; exit 2; }
[ -f "$key" ] && [ -f "$known" ] || { echo 'SSH key and known-hosts files are required' >&2; exit 2; }
[ ! -e "$logs" ] || { echo 'Log directory must be new' >&2; exit 2; }
mkdir -p "$logs"
logs=$(cd "$logs" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-native.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
image=${AZ_TEST_IMAGE:-azmod-test:18.04}
ssh_args=(-i "$key" -p "$port" -o IdentitiesOnly=yes -o BatchMode=yes -o LogLevel=ERROR
    -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$known" -o ConnectTimeout=8
    -o ServerAliveInterval=15 -o ServerAliveCountMax=2 "root@$host")
python3 tests/unit/sanitizer_preflight_test.py >"$logs/controller-regressions.log" 2>&1
python3 tests/unit/sdk_native_integration_test.py >"$logs/native-runner-regressions.log" 2>&1
docker image inspect "$image" >/dev/null || { echo 'Build the image described by tests/run-linux.sh first' >&2; exit 2; }
docker run --rm --platform=linux/arm64 "$image" sh -c 'command -v g++ >/dev/null' >/dev/null || {
    echo 'The native listener fixture requires the updated test image with GNU g++' >&2; exit 2;
}
mkdir "$tmp/bin"
python3 tests/unit/sdk_native_bundle.py snapshot "$original" "$tmp/source" "$logs/source-manifest.json"
cp "$logs/source-manifest.json" "$tmp/source-manifest.json"
cd "$tmp/source"
if [ "$foundation" -eq 1 ]; then
python3 tests/unit/sdk_observation_extract.py "$tmp"
python3 tests/unit/sdk_review_controls.py src/azmod "$tmp/review"
python3 tests/unit/sdk_source_identity_extract.py "$tmp"
cp src/azmod/{sdk_data_engine.h,sdk_analysis.h,azmod-mod.h,hc_anlz.h} "$tmp/"
python3 - "$tmp/sdk_data_engine.h" "$tmp/broken-engine.h" <<'PY'
import sys
s = open(sys.argv[1]).read()
a = s.index('    return a->source_id == b->source_id')
b = s.index('\n}', a)
open(sys.argv[2], 'w').write(s[:a] + '    return a->track_id==b->track_id;' + s[b:])
PY
awk '/^static uint32_t sd_live_fields\(/,/^}/; /^static int sd_deck_snapshot_v1\(/,/^}/' src/azmod/sdk_deck.c > "$tmp/deck.inc"
python3 - "$tmp" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
s = Path('src/azstem/sdk_loop_observer.cpp').read_text()
old = 'player != deck'
assert s.count(old) == 1
(root / 'loop-player.cpp').write_text(s.replace(old, 'false'))
old = 'p.active_generation.load(std::memory_order_acquire) != generation'
assert s.count(old) == 2
(root / 'loop-generation.cpp').write_text(s.replace(old, 'false'))
old = 'bool engaged = working_loop_engaged(notification);'
assert s.count(old) == 1
(root / 'loop-byte-zero.cpp').write_text(s.replace(old, 'bool engaged = *reinterpret_cast<const unsigned char *>(&notification) != 0;'))
old = 'position != UINT64_C(0x7fffffffffffffff)'
assert s.count(old) == 1
(root / 'loop-zero-time.cpp').write_text(s.replace(old, old + ' && position != 0'))
old = 'p.bounds_reason.store(bounds.reason, std::memory_order_relaxed);'
assert s.count(old) == 1
(root / 'loop-reason.cpp').write_text(s.replace(old, 'p.bounds_reason.store(0, std::memory_order_relaxed);'))
old = 'p.check_in_samples.store(bounds.check_in_samples, std::memory_order_relaxed);'
assert s.count(old) == 1
(root / 'loop-numeric.cpp').write_text(s.replace(old, 'p.check_in_samples.store(bounds.check_in_samples ^ 1u, std::memory_order_relaxed);'))
s = Path('src/azmod/sdk_data_engine.h').read_text()
for name, condition in [('first', 's->wave_query.first_column == wave->first_column'), ('count', 's->wave_query.column_count == wave->column_count')]:
    assert s.count(condition) == 1
    (root / ('wave-' + name + '.h')).write_text(s.replace(condition, '1'))
s = (root / 'deck.inc').read_text()
old = 'memcpy(out, &v, bytes);'
assert s.count(old) == 1
(root / 'deck-control.inc').write_text(s.replace(old, 'memcpy(out, &v, bytes + (sizeof v - bytes));'))
PY
fi
docker run --rm --platform=linux/arm64 -v "$PWD:/src:ro" -v "$tmp:/work" "$image" bash -euc '
    cd /src
    HOST_CC=gcc bash tests/sanitizer-preflight.sh --output /work/bin/preflight --compile-only --static-runtimes
    if [ "$1" = 1 ]; then
    flags=(-std=gnu11 -O1 -g -pthread -I/work -I/src/src/azmod -I/src/tests/unit)
    cxxflags=(-std=c++11 -O1 -g -pthread -I/src/src/common -I/src/src/azstem -fvisibility=hidden -fvisibility-inlines-hidden)
    cppmap=-Wl,--version-script=/src/src/azstem/sdk_native_cpp.map
    for pass in plain asan tsan; do
        san=()
        case "$pass" in
            asan) san=(-fsanitize=address,undefined -static-libasan -static-libubsan -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
            tsan) san=(-fsanitize=thread -static-libtsan -fno-omit-frame-pointer) ;;
        esac
        gcc "${flags[@]}" "${san[@]}" /src/tests/unit/sdkdata_test.c -lm -o /work/bin/data-$pass
        gcc "${flags[@]}" "${san[@]}" /src/tests/unit/sdkwaveform_test.c -o /work/bin/waveform-$pass
        gcc "${flags[@]}" "${san[@]}" -DSOURCE_IDENTITY_INC=\"/work/source-identity.inc\" /src/tests/unit/sdksourceidentity_test.c -o /work/bin/source-identity-$pass
        gcc "${flags[@]}" "${san[@]}" -rdynamic -DLIB_INC=\"sdk_lib.c\" /src/tests/unit/sdktrack_test.c \
            /usr/lib/aarch64-linux-gnu/libsqlite3.a -ldl -lm -o /work/bin/provider-$pass
        gcc "${flags[@]}" "${san[@]}" /src/tests/unit/sdkrealanlz_probe.c -lm -o /work/bin/real-$pass
        gcc "${flags[@]}" "${san[@]}" /src/tests/unit/sdkwaveprovider_probe.c -lm -o /work/bin/range-$pass
        gcc "${flags[@]}" "${san[@]}" -DDECK_DATA_INC=\"/work/deck.inc\" /src/tests/unit/sdkdeckdata_test.c -o /work/bin/deck-$pass
        gcc "${flags[@]}" "${san[@]}" -DOBS_INC=\"/work/observation.inc\" /src/tests/unit/sdkobservation_test.c -o /work/bin/observation-$pass
        gcc "${flags[@]}" "${san[@]}" -DLOOP_EXPORT_INC=\"/work/loop-export.inc\" /src/tests/unit/sdkloopexport_test.c -o /work/bin/loop-export-$pass
        g++ "${cxxflags[@]}" "${san[@]}" "$cppmap" /src/src/azstem/sdk_loop_observer.cpp \
            /src/tests/unit/sdkloopobserver_test.cpp -ldl -o /work/bin/loop-$pass
    done
    for control in observation-size observation-padding; do
        gcc "${flags[@]}" -DOBS_INC=\"/work/$control.inc\" /src/tests/unit/sdkobservation_test.c -o /work/bin/$control-control
    done
    for control in source-tuple source-wrap; do
        gcc "${flags[@]}" -DSOURCE_IDENTITY_INC=\"/work/$control.inc\" /src/tests/unit/sdksourceidentity_test.c -o /work/bin/$control-control
    done
    for control in loop-export-v1-size loop-export-v2-size; do
        gcc "${flags[@]}" -DLOOP_EXPORT_INC=\"/work/$control.inc\" /src/tests/unit/sdkloopexport_test.c -o /work/bin/$control-control
    done
    for control in timing-size timing-version; do
        gcc "${flags[@]}" -I/work/review/$control -DST_ENGINE_HEADER=\"/work/review/$control/sdk_data_engine.h\" -fsanitize=address,undefined -static-libasan -static-libubsan -fno-sanitize-recover=all /src/tests/unit/sdkdata_test.c -lm -o /work/bin/review-$control-control
    done
    for control in database-alias database-uri missing-parent wave-errors analysis-binding track-fifo wave-fifo analysis-path-limit preview-extension; do
        gcc -I/work/review/$control "${flags[@]}" -Wno-unused-function -Wno-missing-field-initializers -rdynamic -DLIB_INC=\"sdk_lib.c\" /src/tests/unit/sdktrack_test.c /usr/lib/aarch64-linux-gnu/libsqlite3.a -ldl -lm -o /work/bin/review-$control-control
    done
    gcc "${flags[@]}" -DDECK_DATA_INC=\"/work/deck-control.inc\" /src/tests/unit/sdkdeckdata_test.c -o /work/bin/prefix-control
    for control in player generation byte-zero zero-time reason numeric; do
        g++ "${cxxflags[@]}" "$cppmap" /work/loop-$control.cpp /src/tests/unit/sdkloopobserver_test.cpp -ldl -o /work/bin/loop-$control-control
    done
    for control in first count; do
        gcc "${flags[@]}" -DST_ENGINE_HEADER=\"wave-$control.h\" /src/tests/unit/sdkdata_test.c -lm -o /work/bin/wave-$control-control
    done
    gcc "${flags[@]}" -DST_ENGINE_HEADER=\"broken-engine.h\" /src/tests/unit/sdkdata_test.c -lm -o /work/bin/identity-control
    gcc "${flags[@]}" -fsanitize=address,undefined -static-libasan -static-libubsan -fno-sanitize-recover=all \
        /src/tests/unit/sdkdata_sanitizer_control.c -o /work/bin/asan-control
    gcc "${flags[@]}" -DTEST_RACE -fsanitize=thread -static-libtsan \
        /src/tests/unit/sdkdata_sanitizer_control.c -o /work/bin/tsan-control
    fi
    HOST_CC=gcc HOST_CXX=g++ python3 tests/unit/sdk_completion_prepare.py /src /work/extensions /work/bin --jobs "$2"
    dependency_args=(); [ "$1" = 0 ] || dependency_args+=(--foundation)
    python3 tests/unit/sdk_native_dependencies.py /src /work /work/source-manifest.json \
        /work/bin/sdk-extension-manifest.json /work/bin/used-inputs.json --jobs "$2" "${dependency_args[@]}"
' _ "$foundation" "${SDK_NATIVE_BUILD_JOBS:-2}" >"$logs/build.log" 2>&1
python3 tests/unit/sdk_native_bundle.py verify "$original" "$tmp/bin/used-inputs.json"
cp "$tmp/bin/used-inputs.json" "$logs/used-inputs.json"
python3 tests/unit/sdk_native_bundle.py context-drift "$original" "$logs/source-manifest.json" "$logs/used-inputs.json" "$logs/context-drift.json"
cp -R "$tmp/extensions/compile-logs" "$logs/compile-logs"
python3 tests/unit/sdk_native_bundle.py archive "$tmp/source" "$logs/source.tar.gz"
cp tests/unit/sdkdata_deck_run.sh "$tmp/bin/run.sh"
printf '%s\n' "$foundation" >"$tmp/bin/suite-foundation"
cp "$logs/source-manifest.json" "$tmp/bin/source-manifest.json"
cp "$tmp/bin/sdk-extension-manifest.json" "$logs/extension-manifest.json"
if [ -n "${ANLZ_SAMPLE_DIR:-}" ]; then
    mkdir "$tmp/bin/samples"
    cp "$ANLZ_SAMPLE_DIR/ANLZ0000.DAT" "$ANLZ_SAMPLE_DIR/ANLZ0000.EXT" "$tmp/bin/samples/"
    [ ! -f "$ANLZ_SAMPLE_DIR/ANLZ0000.2EX" ] || cp "$ANLZ_SAMPLE_DIR/ANLZ0000.2EX" "$tmp/bin/samples/"
fi
python3 tests/unit/sdk_native_bundle.py checksums "$tmp/bin"
cp "$tmp/bin/SHA256SUMS" "$logs/SHA256SUMS"
python3 tests/unit/sdk_native_bundle.py archive "$tmp/bin" "$logs/bundle.tar.gz"
if [ "$compile_only" -eq 1 ]; then
    echo "SDK NATIVE COMPILED: runtime NOT RUN; bundle $logs/bundle.tar.gz"
    exit 0
fi
remote=$(ssh "${ssh_args[@]}" 'umask 077; mktemp -d /tmp/az-sdk-native.XXXXXX')
[[ $remote =~ ^/tmp/az-sdk-native\.[a-zA-Z0-9]+$ ]] || { echo 'Unexpected remote temporary path' >&2; exit 2; }
printf '%s\n' "$remote" >"$logs/remote-directory.txt"
ssh "${ssh_args[@]}" "tar -xzf - -C $remote" <"$logs/bundle.tar.gz"
rc=0
ssh "${ssh_args[@]}" "cd $remote && sha256sum -c SHA256SUMS && sh ./run.sh" >"$logs/results.log" 2>&1 || rc=$?
# Collect first: a failed SSH transfer leaves the unique directory available for inspection.
ssh "${ssh_args[@]}" "tar -czf - -C $remote logs" >"$logs/native-logs.tar.gz"
ssh "${ssh_args[@]}" "rm -rf $remote"
cat "$logs/results.log"
exit "$rc"
