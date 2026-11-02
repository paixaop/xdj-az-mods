#!/usr/bin/env bash
set -euo pipefail
# SPDX-License-Identifier: GPL-3.0-only
# Synthetic OWN-helper identity guard and scalar diagnostic tests.
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: source probe guards require Linux ELF'
    exit 77
fi
probe_out=$(mktemp -d "${TMPDIR:-/tmp}/az-source-probe.XXXXXX")
trap 'rm -rf "$probe_out"' EXIT
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
cat > "$probe_out/fake.c" <<'FAKE'
/* Synthetic OWN helper; no EP147 or SDK implementation is loaded. */
#include "azsources.h"
#include <string.h>
static unsigned calls, anchors;
int azmod_dm_key(void) { ++anchors; return 0; }
unsigned source_fake_calls(void) { return calls; }
unsigned source_fake_anchors(void) { return anchors; }
int source_fake_data = 1;
__attribute__((visibility("hidden"))) int fake_candidate(uint16_t media, void *out, size_t size) {
    ++calls;
    if (!out || size < sizeof(struct azsdk_source_sample)) return AZS_INVALID;
    struct azsdk_source_sample value = {0};
    value.struct_size = sizeof value;
    value.flags = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE;
    value.owner_epoch = 9;
    value.revision = 2;
    value.observed_ns = 100;
    value.native_media_id = media;
    value.physical_status = AZS_PHYSICAL_DISABLED;
    memcpy(out, &value, sizeof value);
    return AZS_OK;
}
FAKE
${HOST_CC:-cc} "${flags[@]}" -shared -fPIC -Isrc/common "$probe_out/fake.c" -Wl,--build-id=sha1 -o "$probe_out/azmod.so"
cp "$probe_out/azmod.so" "$probe_out/wrong.so"
python3 - "$probe_out" "$PWD/tests/integration/sdk-source-probe.c" <<'PY'
import pathlib,subprocess,sys
p=pathlib.Path(sys.argv[1])
symbols={}
for line in subprocess.check_output(['nm','-an',str(p/'azmod.so')],universal_newlines=True).splitlines():
    fields=line.split()
    if len(fields)==3 and fields[2] in ['azmod_dm_key','fake_candidate','source_fake_data']:
        symbols[fields[2]]=int(fields[0],16)
notes=subprocess.check_output(['readelf','-n',str(p/'azmod.so')],universal_newlines=True)
build=notes.split('Build ID: ')[1].splitlines()[0].strip()
(p/'args').write_text(build+'\n'+str(symbols['fake_candidate']-symbols['azmod_dm_key'])+'\n'+str(symbols['source_fake_data']-symbols['azmod_dm_key'])+'\n')
s=pathlib.Path(sys.argv[2]).read_text()
controls={
 'id':('m->valid = build_ids == 1 && matched == 1;', 'm->valid = 1; (void)build_ids; (void)matched;'),
 'name':('if (strcmp(base_name, "azmod.so")) return NULL;', '(void)base_name;'),
 'rx':('if (!diagnostic_rx(ranges, range_count, m->anchor) ||\n        !diagnostic_rx(ranges, range_count, m->target)) return 1;', 'if (0) return 1;'),
 'delta':('if (magnitude > anchor) return 0;', 'if (0) return 0;'),
}
for name,(old,new) in controls.items():
    assert s.count(old)==1,(name,s.count(old))
    (p/(name+'.c')).write_text(s.replace(old,new).replace('"../../src/common/azsources.h"','"azsources.h"'))
PY
mapfile -t args < "$probe_out/args"
${HOST_CC:-cc} "${flags[@]}" -Isrc/common -DPROBE_INC="\"$PWD/tests/integration/sdk-source-probe.c\"" tests/unit/sdksourceprobe_test.c -ldl -o "$probe_out/real"
"$probe_out/real" "$probe_out/azmod.so" "$probe_out/wrong.so" "${args[@]}"
for control in id name rx delta; do
    ${HOST_CC:-cc} "${flags[@]}" -Wno-unused-function -Isrc/common -DPROBE_INC="\"$probe_out/$control.c\"" tests/unit/sdksourceprobe_test.c -ldl -o "$probe_out/$control"
    if ( ulimit -c 0; "$probe_out/$control" "$probe_out/azmod.so" "$probe_out/wrong.so" "${args[@]}" >"$probe_out/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: source probe $control control was not caught" >&2; exit 1
    fi
    grep -Eq 'Assertion.*failed|Assertion failed' "$probe_out/$control.log" || { cat "$probe_out/$control.log"; exit 1; }
    echo "CONTROL caught: source probe $control"
done
# Production preload configuration compiles independently, with no fake-module code.
${HOST_CC:-cc} "${flags[@]}" -shared -fPIC -DAZSDK_SOURCE_DIAGNOSTIC_DELTA="${args[1]}LL" -DAZSDK_SOURCE_DIAGNOSTIC_BUILD_ID="\"${args[0]}\"" tests/integration/sdk-source-probe.c -ldl -o "$probe_out/probe.so"
