#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Actual startup caller + canonical State + existing real island object. No
# source/model registry or installer fallback. Native :memory: APIs only.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: GNU Linux native startup packet required'; exit 77; }
case ${MODE:-build} in build|run) ;; *) echo 'MODE must be build or run' >&2; exit 2 ;; esac
: "${OUT_DIR:?new retained startup packet directory required}"
mkdir -p "$OUT_DIR"
task_out=$(cd "$OUT_DIR" && pwd)
if [ "${MODE:-build}" = build ]; then
    : "${ISLAND_OBJECT:?existing exact real island-wrapper object required}"
    [ ! -e "$task_out/source-startup" ] && [ ! -e "$task_out/config-caller.c" ]
    python3 - "$task_out" <<'PY'
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1]);src=Path('src/azmod/azmod.c').resolve();text=src.read_text()
init=text.index('__attribute__((constructor)) static void azmod_init(void)')
start=text.index('        if (sdk_passive_sources || (access("/home/root/settings/azmod.track-data.on", F_OK)==0 &&',init)
end=text.index('        msg_thread_note();',start)
# This packet qualifies the source/SQL lifecycle stage. The separately tested
# cache stage follows it and requires its own generated profile/entry backend.
cache_stage=text.find('        sdk_cache_at_start();',start,end)
if cache_stage != -1: end=cache_stage
fragment=text[start:end]
if fragment.count('azsdk_sources_configure')!=1: raise SystemExit('FAIL: exact existing startup config boundary not found once')
(out/'startup-config.inc').write_text(fragment)
helper=Path('src/azmod/sdk_connections.c').resolve()
include_helper=('#include '+json.dumps(str(helper))+'\n') if helper.exists() else ''
header=Path('src/common/azsources.h').resolve()
(out/'config-caller.c').write_text('''#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include '''+json.dumps(str(header))+'''
extern int azsdk_test_startup_access(const char *,int);
extern int azsdk_test_connection_island(uintptr_t,void*,void**);
static int g_sdk_sources_on;
static int sdk_passive_sources; /* existing explicit-provider matrix with status off */
/* Link adapter only: the existing object executes the unmodified real
 * azm_install_island_hook_at. No mocked original/entry/protection semantics. */
static int azm_install_island_hook_at(uintptr_t,void*,void**,const char*) __attribute__((unused));
static int azm_install_island_hook_at(uintptr_t e,void*h,void**o,const char*n) {
    (void)n;return azsdk_test_connection_island(e,h,o);
}
#define dbg(...) ((void)0)
#define access azsdk_test_startup_access
'''+include_helper+'''
void azsdk_test_run_startup_config(int b) {
#include '''+json.dumps(str(out/'startup-config.inc'))+'''
}
#undef access
int azsdk_test_startup_sources_on(void) { return g_sdk_sources_on; }
''')
record={'azmod_source':str(src),'azmod_sha256':hashlib.sha256(text.encode()).hexdigest(),'fragment_sha256':hashlib.sha256(fragment.encode()).hexdigest(),'start_line':text[:start].count('\n')+1,'end_exclusive_line':text[:end].count('\n')+1,'helper_present':helper.exists(),'helper_path':str(helper),'helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest()if helper.exists()else None,'fake_scope':'access(flag,F_OK) in exact copied helper/config only; module/State/observer/island backend real'}
(out/'caller-extraction.json').write_text(json.dumps(record,indent=2)+'\n')
PY
    "${HOST_CC:-gcc}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -c "$task_out/config-caller.c" -o "$task_out/config-caller.o"
    "${HOST_CXX:-g++}" -std=c++11 -O1 -g -Wall -Wextra -Werror -pthread tests/unit/sdksourcestartup_test.cpp \
        src/azmod/sdk_connection_hooks.cpp "$task_out/config-caller.o" "$ISLAND_OBJECT" -ldl -o "$task_out/source-startup"
    {
        "${HOST_CXX:-g++}" --version
        sha256sum tests/unit/sdksourcestartup_test.cpp tests/sdk-source-startup.sh src/azmod/azmod.c \
            src/azmod/sdk_native_sources.cpp src/azmod/sdk_connection_hooks.cpp src/common/azsources.h \
            src/common/azcachebirth.h src/common/azconnectionepoch.h src/common/azconnectionhooks.h \
            src/azmod/inline_hooks.c src/common/azprot.h "$ISLAND_OBJECT" "$task_out/config-caller.c" \
            "$task_out/config-caller.o" "$task_out/startup-config.inc" "$task_out/source-startup"
        if [ -e src/azmod/sdk_connections.c ]; then sha256sum src/azmod/sdk_connections.c; else echo 'SDK_CONNECTIONS_HELPER_ABSENT'; fi
    } >"$task_out/build-inputs.txt"
    readelf -h -l -d -V "$task_out/source-startup" >"$task_out/probe-elf-requirements.txt"
    echo "STARTUP_PACKET_BUILT=$task_out"; exit 0
fi
: "${MODULE_PATH:?exact retained native module required}" "${MODULE_BUILD:?1or2}" \
  "${RUNTIME_LOADER:?explicit coherent native loader required}" "${RUNTIME_LIBRARY_PATH:?coherent native library directory required}"
case $MODULE_BUILD in
1) expected=4b41d53a1b50d14e0d01cb08384546ba5f561a71cb4bd7a2784628095521f15d ;;
2) expected=9d0fd0efd8caf4053df104ef02ef45dc18f45092552bae98f62a52808a610b5b ;;
*) exit 2 ;;
esac
actual=$(sha256sum "$MODULE_PATH"); [ "${actual%% *}" = "$expected" ] || { echo 'FAIL: exact module SHA mismatch' >&2; exit 1; }
task_run=$(mktemp -d "$task_out/startup-build$MODULE_BUILD.XXXXXX")
{
    uname -a
    printf 'MODULE_BUILD=%s\nMODULE_PATH=%s\nRUNTIME_LOADER=%s\nRUNTIME_LIBRARY_PATH=%s\n' "$MODULE_BUILD" "$MODULE_PATH" "$RUNTIME_LOADER" "$RUNTIME_LIBRARY_PATH"
    sha256sum "$MODULE_PATH" "$RUNTIME_LOADER" "$task_out/source-startup"
} >"$task_run/runtime-inputs.txt"
failures=0
# Separate processes: all guarded combinations first, then each fully opted-in
# physical preparation mode. No preexisting process binding is reset or reused.
for mask in 0 1 2 3 4 5 6 7; do
    for physical in 0 1; do
        log="$task_run/mask-$mask-physical-$physical.log"; status=0
        (ulimit -c 0; LD_PRELOAD= LD_LIBRARY_PATH="$RUNTIME_LIBRARY_PATH" OPENSSL_CONF=/dev/null \
            "$RUNTIME_LOADER" --library-path "$RUNTIME_LIBRARY_PATH" "$task_out/source-startup" \
            "$MODULE_PATH" "$MODULE_BUILD" "$mask" "$physical") >"$log" 2>&1 || status=$?
        printf '%s\n' "$status" >"$log.status"; cat "$log"
        if [ "${EXPECT_RED:-0}" = 1 ] && [ "$mask" = 7 ]; then
            first_failure=$(sed -n '/^FAIL / { p; q; }' "$log")
            if [ "$status" != 134 ] ||
               ! grep -q "NATIVE_STARTUP_BASELINE_OK build=$MODULE_BUILD mask=7 physical=$physical memory_open_close=1" "$log" ||
               ! grep -q "NATIVE_STARTUP_BOUNDARY_OK build=$MODULE_BUILD mask=7 physical=$physical post_open_close=1 canonical_known=0" "$log" ||
               [[ "$first_failure" != *'missing canonical SourceState receipt after opted-in azmod_init boundary'* ]] ||
               grep -Eq 'DLOPEN_FAILURE|Segmentation fault|Illegal instruction|Bus error|runtime error:|undefined symbol' "$log"; then
                echo 'FAIL: missing actual startup receipt runtime assertion was not reached' >&2; failures=$((failures + 1))
            else echo "EXPECTED_RUNTIME_RED build=$MODULE_BUILD physical=$physical: real startup caller omitted canonical native connection install"; fi
        elif [ "$status" != 0 ] || ! grep -q "PASS source-startup build=$MODULE_BUILD mask=$mask physical=$physical" "$log"; then
            failures=$((failures + 1))
        fi
    done
done
echo "STARTUP_RUNTIME_EVIDENCE=$task_run"
[ "$failures" = 0 ]
