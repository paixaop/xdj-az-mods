#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# GNU ownership and exact-profile metadata capture with planted regression controls.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: native source fixtures require Linux ELF and GNU libstdc++'
    exit 77
fi
task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-sdk-sources.XXXXXX")
trap 'rm -rf "$task_tmp"' EXIT
flags=(-std=gnu11 -fexceptions -fasynchronous-unwind-tables -O1 -g -Wall -Wextra -Werror -pthread -DAZS_SAMPLE_BOUNDARY)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
python3 - "$PWD/src/azmod/sdk_native_sources.c" "$task_tmp" <<'PY'
import pathlib,sys,re
source=pathlib.Path(sys.argv[1]).read_text();out=pathlib.Path(sys.argv[2])
def instrument(value):
    needle='static struct azsdk_source_sample scalar_sample(const Entry *e) {'
    assert value.count(needle)==1
    return value.replace(needle,needle+'\n    if (source_sample_boundary) source_sample_boundary();').replace('"../common/','"')
(out/'sample-boundary.c').write_text(instrument(source))
a=source.index('static int azsdk_sources_configure_guard_1_body(');b=source.index('\nint azsdk_sources_configure(',a);configure=source[a:b]
lock='AZ_MUTEX_GUARD(lock, source_state().registry_mutex, false);';disable='atomic_store_explicit(&(source_state().enabled), 0, memory_order_release);'
assert configure.count(lock)==configure.count(disable)==1 and configure.index(lock)<configure.index(disable)
broken=configure.replace(disable,'').replace(lock,disable+'\n'+lock)
assert broken.index(disable)<broken.index(lock)
print('CONTROL caught: unserialized configuration disable')
controls={
'worker-marker':('load(unsigned char, server + 0xd2) != 1','false'),
'worker-present':('!canonical(load(uintptr_t, server + 0x28))','false'),
'worker-affinity':('return !((NeedsAsync) (entry))(((void *) (server)));','return true;'),
'raw-owner-reuse':('return weak->control == strong->control;','(void)weak;(void)strong;return true;'),
'expired-owner':('!(!az_native_alive(e->mount.control)) && !(!az_native_alive(e->database.control))','true'),
'candidate-result':('if (!(s.flags & AZS_CURRENT)) continue;','(void)s;'),
'forgotten-ambiguity':('return reclaimed;','memset(source_state().ambiguous_media,0,sizeof source_state().ambiguous_media);return reclaimed;'),
'physical-profile':('if (!p->ready || !p->file_control) return AZS_PHYSICAL_PROFILE;','if (!p->file_control) return AZS_PHYSICAL_PROFILE;'),
'fdinfo-bound':('attempt < 8 && used < sizeof text','attempt < 16 && used < sizeof text'),
'descriptor-recheck':('load(int32_t, address + 0x18) != native_fd','false'),
'native-fd-dup-close':('struct stat before, after;','int unsafe_copy=dup(native_fd);if(unsafe_copy>=0)close(unsafe_copy);\nstruct stat before,after;'),
'rtti-short-name':('return image_string_equal(&(source_state().main_image), text, name) ? table : 0;','size_t length=strlen(name)+1;return image_contains(&source_state().main_image,text,length,kRead)&&!memcmp((const void*)text,name,length)?table:0;')}
for name,(old,new) in controls.items():
    assert source.count(old)==1,(name,source.count(old));(out/(name+'.c')).write_text(instrument(source.replace(old,new)))
a=source.index('static bool supported_name(');b=source.index('\nstatic int capture_physical(',a)
unsafe='''static bool supported_name(const SqlProfile *p,const char *name){
 const char *const names[]={"unix","unix-excl","unix-none","unix-dotfile"};
 for(unsigned i=0;i<4;i++){size_t length=strlen(names[i])+1;if(image_contains(&p->image,(uintptr_t)name,length,kRead)&&!memcmp(name,names[i],length))return true;}return false;
}
'''
(out/'vfs-short-name.c').write_text(instrument(source[:a]+unsafe+source[b:]))
# Deliberately reset the actual C state after the priority101 capture. This is
# the same early-record loss regression, without a C++ dynamic constructor.
a=source.index('static SourceState sources = ');b=source.index(';',a)+1
initial=source[a:b].split(' = ',1)[1][:-1]
reset='\n__attribute__((constructor(200))) static void late_state_reset(void) {sources=(SourceState)'+initial+';}\n'
(out/'constructor-order.c').write_text(instrument(source[:b]+reset+source[b:]))
PY
${HOST_CC:-gcc} "${flags[@]}" -Isrc/common -DSOURCES_INC="\"$task_tmp/sample-boundary.c\"" tests/unit/sdknativesources_test.c src/azmod/sdk_connection_hooks.c src/common/azcache_scopes.c src/common/aznative_guard.S -lstdc++ -ldl -o "$task_tmp/real"
"$task_tmp/real" 1
"$task_tmp/real" 2
${HOST_CC:-gcc} "${flags[@]}" -Isrc/common -DAZS_CONSTRUCTOR_REGRESSION -DSOURCES_INC="\"$task_tmp/sample-boundary.c\"" tests/unit/sdknativesources_test.c src/azmod/sdk_connection_hooks.c src/common/azcache_scopes.c src/common/aznative_guard.S -lstdc++ -ldl -o "$task_tmp/constructor"
"$task_tmp/constructor" 1
# Compile the production TU independently too: fixtures must not supply missing definitions.
${HOST_CC:-gcc} "${flags[@]}" -Isrc/common -c src/azmod/sdk_native_sources.c -o "$task_tmp/production.o"
require_fixture_diagnostic() {
    if ! grep -Eq 'Assertion.*failed|Assertion failed|runtime error:|ERROR: AddressSanitizer|WARNING: ThreadSanitizer' "$1"; then
        cat "$1" >&2
        echo 'FAIL: control failed without an assertion or sanitizer fixture diagnostic' >&2
        exit 1
    fi
}
for control in worker-marker worker-present worker-affinity raw-owner-reuse expired-owner candidate-result forgotten-ambiguity physical-profile fdinfo-bound descriptor-recheck native-fd-dup-close vfs-short-name rtti-short-name; do
    extra=()
    [ "$control" != worker-affinity ] || extra+=(-Wno-unused-local-typedefs)
    ${HOST_CC:-gcc} "${flags[@]}" "${extra[@]}" -Isrc/common -DSOURCES_INC="\"$task_tmp/$control.c\"" tests/unit/sdknativesources_test.c src/azmod/sdk_connection_hooks.c src/common/azcache_scopes.c src/common/aznative_guard.S -lstdc++ -ldl -o "$task_tmp/$control"
    if ( ulimit -c 0; "$task_tmp/$control" 1 >"$task_tmp/$control.log" 2>&1 ) 2>/dev/null; then
        echo "FAIL: $control control was not caught" >&2; exit 1
    fi
    require_fixture_diagnostic "$task_tmp/$control.log"
    if [ "$control" = vfs-short-name ] && ! grep -q 'vfs_name_child_status' "$task_tmp/$control.log"; then
        cat "$task_tmp/$control.log" >&2
        echo 'FAIL: short VFS name control missed its intended boundary assertion' >&2
        exit 1
    fi
    if [ "$control" = rtti-short-name ] && ! grep -q 'rtti_name_child_status' "$task_tmp/$control.log"; then
        cat "$task_tmp/$control.log" >&2
        echo 'FAIL: short RTTI name control missed its intended boundary assertion' >&2
        exit 1
    fi
    echo "CONTROL caught: $control"
done
${HOST_CC:-gcc} "${flags[@]}" -Isrc/common -DAZS_CONSTRUCTOR_REGRESSION -DSOURCES_INC="\"$task_tmp/constructor-order.c\"" tests/unit/sdknativesources_test.c src/azmod/sdk_connection_hooks.c src/common/azcache_scopes.c src/common/aznative_guard.S -lstdc++ -ldl -o "$task_tmp/constructor-order"
if ( ulimit -c 0; "$task_tmp/constructor-order" 1 >"$task_tmp/constructor-order.log" 2>&1 ) 2>/dev/null; then
    echo 'FAIL: constructor-order control was not caught' >&2; exit 1
fi
require_fixture_diagnostic "$task_tmp/constructor-order.log"
grep -q 'early_epoch' "$task_tmp/constructor-order.log" || {
    cat "$task_tmp/constructor-order.log" >&2
    echo 'FAIL: synthetic dynamic-state control missed its intended early-state assertion' >&2
    exit 1
}
echo 'CONTROL caught: synthetic dynamic constructor order'
# Exercise the actual production hook tail, rather than a hand-copied ordering model.
python3 - src/azmod/playlist_editor.c "$task_tmp" <<'PY'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
a = s.index('static void mlserver_onmsg_hook(')
b = s.index('\n}', a) + 2
body = s[a:b]
a = body.index('    /* Original message forwarding boundary. */')
tail = body[a:]
prefix = '''#include "azsources.h"
#include "azcachebirth.h"
#include "azcacheentry.h"
#include <assert.h>
#include <stdlib.h>
// Message-only link: generic dispatcher dependencies must never be called.
void azcb_retire_forward(struct azcb_forward f) { (void)f;abort(); }
void azcb_selection_forward(struct azcb_forward f) { (void)f;abort(); }
void azcb_observe_construction_write_owned(const struct azcb_write_borrow *b,struct azcb_forward f) { (void)b;(void)f;abort(); }
void azcb_observe_factory_owned(const struct azcb_factory_borrow *b,struct azcb_forward f) { (void)b;(void)f;abort(); }
void azsdk_cache_invoke_original(struct azce_call *c) { (void)c;abort(); }
static int g_sdk_sources_on, stage, owner, source_calls;
static int g_ep147_build = 1;
static void *expected_server, *expected_message;
static void original(void *server, void *message, unsigned param) {
    assert(server == expected_server && message == expected_message && param == 37);
    assert(stage == 0); owner = 2; stage = 1;
}
static void (*g_orig_mlserver_onmsg)(void *, void *, unsigned) = original;
int azsdk_sources_observe_owned_message(void *server, void *out, size_t size) {
    assert(server == expected_server && !out && !size && stage == 1 && owner == 2);
    ++source_calls; stage = 2; return AZS_OK;
}
static void pst_msg_done(void *server) {
    assert(server == expected_server && stage == (g_sdk_sources_on ? 2 : 1)); stage = 3;
}
static void hook_tail(void *server, void *msg, unsigned param) {
'''
suffix = '''
int main() {
    static int server, message; expected_server = &server; expected_message = &message;
    for (int on = 0; on < 2; ++on) {
        g_sdk_sources_on = on; stage = 0; owner = 1; source_calls = 0;
        hook_tail(expected_server, expected_message, 37);
        assert(stage == 3 && source_calls == on);
    }
}
'''
original_block = """    /* Original message forwarding boundary. */
    if (g_sdk_sources_on)
        azsdk_sources_forward_owned_message(g_ep147_build, server, msg, param, g_orig_mlserver_onmsg);
    else
        g_orig_mlserver_onmsg(server, msg, param);"""
observe_line = '    if (g_sdk_sources_on) azsdk_sources_observe_owned_message(server, NULL, 0);'
needle = original_block + '\n' + observe_line
assert tail.count(needle) == 1
pathlib.Path(sys.argv[2], 'bridge.c').write_text(prefix + tail + suffix)
swapped = observe_line + '\n' + original_block
pathlib.Path(sys.argv[2], 'bridge-control.c').write_text(prefix + tail.replace(needle, swapped) + suffix)
PY
${HOST_CC:-gcc} "${flags[@]}" -Isrc/common "$task_tmp/bridge.c" src/azmod/sdk_cache_message.c src/common/azcache_scopes.c -o "$task_tmp/bridge"
"$task_tmp/bridge"
${HOST_CC:-gcc} "${flags[@]}" -Isrc/common "$task_tmp/bridge-control.c" src/azmod/sdk_cache_message.c src/common/azcache_scopes.c -o "$task_tmp/bridge-control"
if ( ulimit -c 0; "$task_tmp/bridge-control" >"$task_tmp/bridge-control.log" 2>&1 ) 2>/dev/null; then
    echo 'FAIL: post-original source bridge control was not caught' >&2; exit 1
fi
require_fixture_diagnostic "$task_tmp/bridge-control.log"
echo 'CONTROL caught: post-original source bridge'
