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
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -pthread -DAZS_SAMPLE_BOUNDARY)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
python3 - "$PWD/src/azmod/sdk_native_sources.cpp" "$task_tmp" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
# Inject a deterministic native-owner retirement boundary into test copies only.
# The source implementation and public entry points contain no test hook.
def instrument(value):
    needle = 'azsdk_source_sample scalar_sample(const Entry &e) noexcept {'
    assert value.count(needle) == 1
    return value.replace(needle, needle + '\n    if (source_sample_boundary) source_sample_boundary();')
pathlib.Path(sys.argv[2], 'sample-boundary.cpp').write_text(instrument(source).replace('"../common/', '"'))
# Disabling must linearize under the same mutex as reenabling. An early store before
# waiting for the mutex can be overwritten by the preceding configurator's enable.
a = source.index('extern "C" int azsdk_sources_configure(')
b = source.index('\nextern "C" int azsdk_sources_observe_owned_message(', a)
configure = source[a:b]
lock = 'std::lock_guard<std::mutex> lock(source_state().registry_mutex);'
disable = 'source_state().enabled.store(0, std::memory_order_release);'
assert configure.count(lock) == configure.count(disable) == 1
assert configure.index(lock) < configure.index(disable), 'disable must be serialized with enable'
broken = configure.replace('        ' + disable + '\n', '').replace('        ' + lock, '        ' + disable + '\n        ' + lock)
assert broken.index(disable) < broken.index(lock)
print('CONTROL caught: unserialized configuration disable')
controls = {
    'worker-marker': ('load<unsigned char>(server + 0xd2) != 1', 'false'),
    'worker-present': ('!canonical(load<std::uintptr_t>(server + 0x28))', 'false'),
    'worker-affinity': ('return !reinterpret_cast<NeedsAsync>(entry)(reinterpret_cast<void *>(server));', 'return true;'),
    'raw-owner-reuse': ('return !weak.owner_before(strong) && !strong.owner_before(weak);', '(void)weak; (void)strong; return true;'),
    'expired-owner': ('!e.mount.expired() && !e.database.expired()', 'true'),
    'candidate-result': ('if (!(s.flags & AZS_CURRENT)) continue;', '(void)s;'),
    'forgotten-ambiguity': ('return reclaimed;', 'std::memset(source_state().ambiguous_media, 0, sizeof source_state().ambiguous_media); return reclaimed;'),
    'physical-profile': ('if (!p.ready || !p.file_control) return AZS_PHYSICAL_PROFILE;', 'if (!p.file_control) return AZS_PHYSICAL_PROFILE;'),
    'fdinfo-bound': ('attempt < 8 && used < sizeof text', 'attempt < 16 && used < sizeof text'),
    'descriptor-recheck': ('load<std::int32_t>(address + 0x18) != native_fd', 'false'),
    'native-fd-dup-close': ('struct stat before, after;', 'int unsafe_copy = ::dup(native_fd); if (unsafe_copy >= 0) ::close(unsafe_copy);\n    struct stat before, after;'),
    'rtti-short-name': ('return image_string_equal(source_state().main_image, text, name) ? table : 0;', '''auto length = std::strlen(name) + 1;
    return source_state().main_image.contains(text, length, kRead) &&
                   !std::memcmp(reinterpret_cast<const void *>(text), name, length) ? table : 0;'''),
}
for name, (old, new) in controls.items():
    assert source.count(old) == 1, (name, source.count(old))
    # Keep the production relative private-header include valid in the temporary TU.
    changed = source.replace(old, new).replace('"../common/', '"')
    pathlib.Path(sys.argv[2], name + '.cpp').write_text(instrument(changed))
# Restore the actual ASan-found fixed-size read. The dedicated guard-page fixture
# must catch this exact regression under plain GNU builds as well as sanitizers.
a = source.index('bool supported_name(')
b = source.index('\nint capture_physical(', a)
unsafe = '''bool supported_name(const SqlProfile &p, const char *name) noexcept {
    static const char *const names[] = {"unix", "unix-excl", "unix-none", "unix-dotfile"};
    for (auto expected : names) {
        auto length = std::strlen(expected) + 1;
        if (p.image.contains(reinterpret_cast<std::uintptr_t>(name), length, kRead) &&
            !std::memcmp(name, expected, length)) return true;
    }
    return false;
}
'''
changed = (source[:a] + unsafe + source[b:]).replace('"../common/', '"')
pathlib.Path(sys.argv[2], 'vfs-short-name.cpp').write_text(instrument(changed))
# GNU7 constant-initializes the current aggregate, so a plain global-State mutation is
# not a reproduced reset bug. Force dynamic test-only growth to model the timing risk
# if future members need initialization after the priority101 C-style constructor.
a = source.index('SourceState &source_state() noexcept {')
b = source.index('\n}\n', a) + 3
dynamic = '''struct DynamicState : SourceState {
    std::string dynamic_member;
    DynamicState() : dynamic_member(std::to_string(static_cast<long>(::getpid()))) {}
};
DynamicState global_state;
SourceState &source_state() noexcept { return global_state; }
'''
changed = (source[:a] + dynamic + source[b:]).replace('#include <memory>', '#include <memory>\n#include <string>')
pathlib.Path(sys.argv[2], 'constructor-order.cpp').write_text(instrument(changed.replace('"../common/', '"')))
PY
${HOST_CXX:-c++} "${flags[@]}" -Isrc/common -DSOURCES_INC="\"$task_tmp/sample-boundary.cpp\"" tests/unit/sdknativesources_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_tmp/real"
"$task_tmp/real" 1
"$task_tmp/real" 2
${HOST_CXX:-c++} "${flags[@]}" -Isrc/common -DAZS_CONSTRUCTOR_REGRESSION -DSOURCES_INC="\"$task_tmp/sample-boundary.cpp\"" tests/unit/sdknativesources_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_tmp/constructor"
"$task_tmp/constructor" 1
# Compile the production TU independently too: fixtures must not supply missing definitions.
${HOST_CXX:-c++} "${flags[@]}" -Isrc/common -c src/azmod/sdk_native_sources.cpp -o "$task_tmp/production.o"
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
    ${HOST_CXX:-c++} "${flags[@]}" "${extra[@]}" -Isrc/common -DSOURCES_INC="\"$task_tmp/$control.cpp\"" tests/unit/sdknativesources_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_tmp/$control"
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
${HOST_CXX:-c++} "${flags[@]}" -Isrc/common -DAZS_CONSTRUCTOR_REGRESSION -DSOURCES_INC="\"$task_tmp/constructor-order.cpp\"" tests/unit/sdknativesources_test.cpp src/azmod/sdk_connection_hooks.cpp -ldl -o "$task_tmp/constructor-order"
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
#include <cassert>
#include <cstdlib>
// Message-only link: generic dispatcher dependencies must never be called.
namespace azsdk_cache_birth {
void retire_forward(Forward) { std::abort(); }
void selection_forward(Forward) { std::abort(); }
void observe_construction_write_owned(const WriteBorrow &, Forward) { std::abort(); }
void observe_factory_owned(const FactoryBorrow &, Forward) { std::abort(); }
}
extern "C" void azsdk_cache_invoke_original(azce_call *) { std::abort(); }
static int g_sdk_sources_on, stage, owner, source_calls;
static int g_ep147_build = 1;
static void *expected_server, *expected_message;
static void original(void *server, void *message, unsigned param) {
    assert(server == expected_server && message == expected_message && param == 37);
    assert(stage == 0); owner = 2; stage = 1;
}
static void (*g_orig_mlserver_onmsg)(void *, void *, unsigned) = original;
extern "C" int azsdk_sources_observe_owned_message(void *server, void *out, size_t size) noexcept {
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
pathlib.Path(sys.argv[2], 'bridge.cpp').write_text(prefix + tail + suffix)
swapped = observe_line + '\n' + original_block
pathlib.Path(sys.argv[2], 'bridge-control.cpp').write_text(prefix + tail.replace(needle, swapped) + suffix)
PY
${HOST_CXX:-c++} "${flags[@]}" -Isrc/common "$task_tmp/bridge.cpp" src/azmod/sdk_cache_message.cpp -o "$task_tmp/bridge"
"$task_tmp/bridge"
${HOST_CXX:-c++} "${flags[@]}" -Isrc/common "$task_tmp/bridge-control.cpp" src/azmod/sdk_cache_message.cpp -o "$task_tmp/bridge-control"
if ( ulimit -c 0; "$task_tmp/bridge-control" >"$task_tmp/bridge-control.log" 2>&1 ) 2>/dev/null; then
    echo 'FAIL: post-original source bridge control was not caught' >&2; exit 1
fi
require_fixture_diagnostic "$task_tmp/bridge-control.log"
echo 'CONTROL caught: post-original source bridge'
