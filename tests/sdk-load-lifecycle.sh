#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Current C lifecycle/configuration extraction; no native objects or deck access.
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-load-lifecycle.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
# Extracted regions intentionally contain helpers unused by individual fixtures.
flags=(-std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -pthread -Itests/unit)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import re, sys
out = Path(sys.argv[1])
source = Path('src/azstem/stem_playback.c').read_text()
a = source.index('static _Atomic(void *) g_ls_self[NDECK];')
m = re.search(r'^#ifndef AZMOD_RELEASE /\* the path scan|^static int is_dir\(const char \*p\) \{', source[a:], re.M)
assert m
lifetime = source[a:a + m.start()]
source = Path('src/azstem/deck_track_map.c').read_text()
a = source.index('/* ---- DjPlayerUnit ctor hook')
b = source.index('static int lti_id_zero(', a)
lifetime += source[a:b]
(out / 'real.inc').write_text(lifetime)
variants = [
    ('cue-register', '(void) azsdk_cue_register((int) n32, self);', '(void) 0;'),
    ('cue-retire', 'if (g_sdk_live_on) azsdk_cue_retire(d);', '(void) 0;'),
    ('register', '(void) azsdk_load_register((int) n32, self);', '(void) 0;'),
    ('retire', 'if (g_sdk_load_on) azsdk_load_retire(d);', '(void) 0;'),
    ('owner', 'if (g_sdk_load_on && (claimed || exp == self) &&', 'if (g_sdk_load_on &&'),
    ('disabled', 'if (g_sdk_load_on && (claimed || exp == self) &&', 'if ((claimed || exp == self) &&'),
    ('unit-vtable', 'if (g_sdk_load_on && (claimed || exp == self) &&\n            *(const uintptr_t *) self == atomic_load_explicit(&g_djpu_vt, memory_order_relaxed))', 'if (g_sdk_load_on && (claimed || exp == self))'),
]
for name, old, new in variants:
    assert lifetime.count(old) == 1, name
    (out / (name + '.inc')).write_text(lifetime.replace(old, new))
old = '    g_orig_djpuctor(self, id, ctx, cap);\n'
assert lifetime.count(old) == 1
early = lifetime.replace(old, '')
marker = '/* ---- DjPlayerUnit destructor hook'
finish = early.rfind('\n}', 0, early.index(marker))
assert finish > 0
(out / 'ctor-order.inc').write_text(early[:finish] + '\n' + old + early[finish:])

source = Path('src/azstem/azstem.c').read_text()
a = source.index('        g_djpuctor_addr = (uintptr_t) v;')
b = source.index('\n    }\n    /* gui::LoadState ctor', a)
body = source[a:b]
config = '''#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL load lifecycle config: %s:%d: %s\\n",__FILE__,__LINE__,#x); abort(); } } while (0)
#define F_OK 0
#define DJPUCTOR_DEFAULT ((uintptr_t)0x10000)
#define DJPUDTOR_DEFAULT ((uintptr_t)0x20000)
#define logln(...) ((void)0)
typedef int (*patch_fn)(uintptr_t,void*,void**);
static int track_on, load_on, ctor_ok, dtor_ok, configured, configured_build, bad_patch, ctor_calls, dtor_calls;
static int sdk_loop_build, g_sdk_loop_on, g_sdk_live_on;
static _Atomic int g_sdk_load_on;
static uintptr_t g_djpuctor_addr;
static void *g_orig_djpuctor,*g_orig_djpudtor;
static volatile int different;
static void djpuctor_hook(void) { different=1; }
static void djpudtor_hook(void) { different=2; }
static int access(const char *path,int mode) {
    CHECK(mode==F_OK);
    if (!strcmp(path,"/home/root/settings/azmod.track-data.on")) return track_on?0:-1;
    if (!strcmp(path,"/home/root/settings/azmod.native-load.on")) return load_on?0:-1;
    return -1;
}
static void *install_hook_at(uintptr_t address,void *hook,void **original) {
    if (hook==(void*)djpuctor_hook) { ++ctor_calls; CHECK(address==g_djpuctor_addr); if(!ctor_ok)return NULL; }
    else { ++dtor_calls; CHECK(hook==(void*)djpudtor_hook && address==DJPUDTOR_DEFAULT); if(!dtor_ok)return NULL; }
    *original=hook; return hook;
}
static int sdk_patch_slot(uintptr_t slot,void *hook,void **original) { (void)slot;(void)hook;(void)original;return 1; }
static void azsdk_load_configure(int on,int build,patch_fn patch) { CHECK(on==1);++configured;configured_build=build;bad_patch|=patch!=sdk_patch_slot; }
static void azsdk_loop_configure(int on,int build) { (void)on;(void)build; }
static void azsdk_live_configure(int on,int build) { (void)on;(void)build; }
static void azsdk_cue_configure(int on,int build) { (void)on;(void)build; }
static int azsdk_exit_configure(int on,int build,void *(*install)(uintptr_t,void*,void**)) { (void)on;(void)build;(void)install;return 0; }
static void configure(unsigned long long v) {
''' + body + '''
}
int main(void) {
    for (int build=1;build<=2;++build) for(int flags=0;flags<4;++flags)
      for(int ctor=0;ctor<2;++ctor) for(int dtor=0;dtor<2;++dtor) for(int def=0;def<2;++def) {
        sdk_loop_build=build;track_on=flags&1;load_on=flags&2;ctor_ok=ctor;dtor_ok=dtor;
        configured=bad_patch=ctor_calls=dtor_calls=0;g_sdk_load_on=0;
        configure(def?DJPUCTOR_DEFAULT:0x30000);
        int expected=flags==3 && ctor && dtor && def;
        CHECK(configured==expected && atomic_load(&g_sdk_load_on)==expected);
        CHECK(ctor_calls==1 && dtor_calls==ctor);
        CHECK(!expected || (configured_build==build && !bad_patch));
    }
    puts("PASS load lifecycle config: flags, constructor/destructor success, default address and build");
}
'''
(out / 'config.c').write_text(config)
controls = [
    ('master-flag', 'access("/home/root/settings/azmod.track-data.on", F_OK) == 0 &&\n                access("/home/root/settings/azmod.native-load.on", F_OK) == 0', 'access("/home/root/settings/azmod.native-load.on", F_OK) == 0'),
    ('load-flag', 'access("/home/root/settings/azmod.native-load.on", F_OK) == 0', '1'),
    ('default-address', 'if (td && g_djpuctor_addr == DJPUCTOR_DEFAULT &&\n                access("/home/root/settings/azmod.track-data.on", F_OK) == 0 &&\n                access("/home/root/settings/azmod.native-load.on", F_OK) == 0)', 'if (td && access("/home/root/settings/azmod.track-data.on", F_OK) == 0 && access("/home/root/settings/azmod.native-load.on", F_OK) == 0)'),
    ('dtor-required', 'if (td && g_djpuctor_addr == DJPUCTOR_DEFAULT &&\n                access("/home/root/settings/azmod.track-data.on", F_OK) == 0 &&\n                access("/home/root/settings/azmod.native-load.on", F_OK) == 0)', 'if (g_djpuctor_addr == DJPUCTOR_DEFAULT && access("/home/root/settings/azmod.track-data.on", F_OK) == 0 && access("/home/root/settings/azmod.native-load.on", F_OK) == 0)'),
    ('build', 'azsdk_load_configure(1, sdk_loop_build, sdk_patch_slot);', 'azsdk_load_configure(1, 1, sdk_patch_slot);'),
]
for name, old, new in controls:
    assert config.count(old)==1,(name,config.count(old))
    (out / ('config-'+name+'.c')).write_text(config.replace(old,new))
snapshot = Path('tests/unit/ltisnap_test.c').read_text()
old = '        free(allocation->address);\n        free(allocation);'
assert snapshot.count(old) == 1
(out / 'snapshot-leak.c').write_text(snapshot.replace(old, '        /* deliberately lost fake allocations */'))
PY
compiler=${HOST_CC:-cc}
"$compiler" "${flags[@]}" -DLIFE_INC="\"$task_tmp/real.inc\"" tests/unit/lifetime_test.c -o "$task_tmp/real"
"$task_tmp/real"
"$compiler" "${flags[@]}" -DLIFE_INC="\"$task_tmp/real.inc\"" tests/unit/ltisnap_test.c -o "$task_tmp/snapshot"
"$task_tmp/snapshot" 0.2
"$compiler" "${flags[@]}" "$task_tmp/config.c" -o "$task_tmp/config"
"$task_tmp/config"
for control in register retire owner disabled unit-vtable ctor-order cue-register cue-retire; do
    "$compiler" "${flags[@]}" -DLIFE_INC="\"$task_tmp/$control.inc\"" tests/unit/lifetime_test.c -o "$task_tmp/$control"
    if (ulimit -c 0; "$task_tmp/$control" >"$task_tmp/$control.log" 2>&1) 2>/dev/null; then
        echo "FAIL: load lifecycle $control control passed" >&2; exit 1
    fi
    grep -q 'FAIL  lifetime:' "$task_tmp/$control.log" || { cat "$task_tmp/$control.log" >&2; exit 1; }
    echo "CONTROL caught: load lifecycle $control"
done
for control in master-flag load-flag default-address dtor-required build; do
    "$compiler" "${flags[@]}" "$task_tmp/config-$control.c" -o "$task_tmp/config-$control"
    if (ulimit -c 0; "$task_tmp/config-$control" >"$task_tmp/config-$control.log" 2>&1) 2>/dev/null; then
        echo "FAIL: load lifecycle config $control control passed" >&2; exit 1
    fi
    grep -q 'FAIL load lifecycle config:' "$task_tmp/config-$control.log" || { cat "$task_tmp/config-$control.log" >&2; exit 1; }
    echo "CONTROL caught: load lifecycle config $control"
done
if [ "$(uname -s)" = Linux ]; then
    "$compiler" -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -pthread -Itests/unit \
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -DLIFE_INC="\"$task_tmp/real.inc\"" "$task_tmp/snapshot-leak.c" -o "$task_tmp/snapshot-leak"
    if (ulimit -c 0; ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$task_tmp/snapshot-leak" 0.2 >"$task_tmp/snapshot-leak.log" 2>&1) 2>/dev/null; then
        echo 'FAIL: load lifecycle snapshot leak control passed' >&2; exit 1
    fi
    grep -q 'LeakSanitizer: detected memory leaks' "$task_tmp/snapshot-leak.log" || { cat "$task_tmp/snapshot-leak.log" >&2; exit 1; }
    echo 'CONTROL caught: load lifecycle snapshot leak'
else
    echo 'SKIP: snapshot leak control requires GNU/Linux LeakSanitizer'
fi
