#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Linux real-page-table regression for the actual constructor-only azstem installer.
# --prepare DIR builds a relocatable native packet; it never contacts a deck.
set -euo pipefail
cd "$(dirname "$0")/.."
mode=run
if [[ ${1:-} == --prepare && $# == 2 ]]; then
    mode=prepare
    packet=$2
elif [[ $# == 0 ]]; then
    :
else
    echo 'usage: sdk-hook-install.sh [--prepare DIR]' >&2; exit 2
fi
if [[ $(uname -s) != Linux ]]; then
    echo 'SKIP: hook installer qualification requires Linux process maps and executable paths'
    exit 77
fi
if [[ $mode == prepare ]]; then
    mkdir -p "$packet"
    packet=$(cd "$packet" && pwd)
else
    packet=$(mktemp -d "${TMPDIR:-/tmp}/az-hook-install.XXXXXX")
    trap 'rm -rf "$packet"' EXIT
fi
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2 ;;
esac
if [[ ${REQUIRE_AARCH64:-0} == 1 ]]; then flags+=(-DHOOK_REQUIRE_AARCH64); fi
compiler=${HOST_CC:-cc}
python3 - "$packet" <<'PY'
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1])
source=Path('src/azstem/azstem.c').read_text()
start=source.index('static int pcrel_kind(')
end=source.index('\nstatic int install_hook(void)',start)
real=source[start:end]+'\n'
(out/'installer-real.inc').write_text(real)
(out/'azprot.h').write_bytes(Path('src/common/azprot.h').read_bytes())
begin=real.index('    if (azprot_relock(&pt) != 0) {')
finish=real.index('    logln("install: hooked @',begin)
old_success='''    (void) previous_orig;
    (void) install_hook_fatal;
    if (azprot_relock(&pt) != 0)
        logln("install: @0x%lx code page left writable (mprotect failed)", (unsigned long) FUNC);
'''
variants={
 'old-success':real[:begin]+old_success+real[finish:],
 'original-protection':real.replace('rollback.prot = pt.prot;', '(void) pt;'),
 'original-bytes':real.replace('memcpy(func, original_entry, sizeof original_entry);', '(void) original_entry;'),
 'original-pointer':real.replace('__atomic_store_n(orig, previous_orig, __ATOMIC_RELEASE);', '__atomic_store_n(orig, NULL, __ATOMIC_RELEASE);\n        (void) previous_orig;'),
 'cache-clear':real.replace('__builtin___clear_cache((char *) func, (char *) func + sizeof original_entry);', '(void) func;'),
 'late':real.replace('if (atomic_load_explicit(&g_hooks_late, memory_order_acquire)) {', 'if (0 && atomic_load_explicit(&g_hooks_late, memory_order_acquire)) {'),
 'fatal-status':real.replace('_exit(126);', '_exit(0);'),
}
for name,variant in variants.items():
 assert variant!=real,name
 (out/('installer-'+name+'.inc')).write_text(variant)
manifest={'source_sha256':hashlib.sha256(source.encode()).hexdigest(),'installer_sha256':hashlib.sha256(real.encode()).hexdigest(),
 'fixture_sha256':hashlib.sha256(Path('tests/unit/hookinstall_test.c').read_bytes()).hexdigest(),
 'runner_sha256':hashlib.sha256(Path('tests/sdk-hook-install.sh').read_bytes()).hexdigest(),
 'azprot_sha256':hashlib.sha256((out/'azprot.h').read_bytes()).hexdigest(),'sanitizer':__import__('os').environ.get('SAN','plain'),
 'scenarios':18,'controls':list(variants),'scope':'synthetic mmap code only; no firmware/hardware/installation'}
(out/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
PY
for name in real old-success original-protection original-bytes original-pointer cache-clear late fatal-status; do
    "$compiler" "${flags[@]}" -DAZPROT_INC="\"$packet/azprot.h\"" \
        -DHOOK_INSTALL_INC="\"$packet/installer-$name.inc\"" tests/unit/hookinstall_test.c -o "$packet/hookinstall-$name"
done
copy_runtime() {
    local path soname
    path=$("$compiler" -print-file-name="$1")
    [[ -f $path ]] || { echo "missing compiler runtime $1" >&2; exit 1; }
    soname=$(readelf -d "$path" | awk '/SONAME/ {gsub(/[][]/, "", $NF); print $NF}')
    [[ -n $soname && $soname != */* ]] || { echo "missing runtime SONAME for $path" >&2; exit 1; }
    mkdir -p "$packet/lib"
    cp -L "$path" "$packet/lib/$soname"
}
case ${SAN:-plain} in
asan)
    nm "$packet/hookinstall-real" | grep '__asan_init' >/dev/null
    nm "$packet/hookinstall-real" | grep '__ubsan_handle' >/dev/null
    copy_runtime libasan.so; copy_runtime libubsan.so; copy_runtime libstdc++.so.6; copy_runtime libgcc_s.so.1 ;;
ubsan)
    nm "$packet/hookinstall-real" | grep '__ubsan_handle' >/dev/null
    copy_runtime libubsan.so; copy_runtime libstdc++.so.6; copy_runtime libgcc_s.so.1 ;;
tsan)
    nm "$packet/hookinstall-real" | grep '__tsan_init' >/dev/null
    copy_runtime libtsan.so; copy_runtime libstdc++.so.6; copy_runtime libgcc_s.so.1 ;;
esac
cat > "$packet/run.sh" <<'SH'
#!/bin/sh
set -eu
cd "$(dirname "$0")"
sha256sum -c packet-sha256.txt
if [ -d lib ]; then
    LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi
./hookinstall-real
for control in old-success original-protection original-bytes original-pointer cache-clear late fatal-status; do
    expected=restore-once
    [ "$control" != late ] || expected=late
    [ "$control" != fatal-status ] || expected=restore-persistent
    rc=0
    ./hookinstall-"$control" >"control-$control.log" 2>&1 || rc=$?
    if [ "$rc" != 1 ] || ! grep -q "FAIL hookinstall: parent case=$expected " "control-$control.log" ||
       ! grep -q "FAIL hookinstall: case=$expected " "control-$control.log"; then
        echo "FAIL hookinstall: control $control exit=$rc lacked exact child/parent assertion" >&2
        cat "control-$control.log" >&2
        exit 1
    fi
    echo "CONTROL caught: hookinstall $control"
done
echo 'PASS hookinstall packet: real syscall failures and seven exact-outcome controls'
SH
chmod +x "$packet/run.sh"
python3 - "$packet" <<'PY'
from pathlib import Path
import hashlib,json,sys
p=Path(sys.argv[1]); files=[f for f in sorted(p.rglob('*')) if f.is_file() and f.name not in ('packet-sha256.json','packet-sha256.txt')]
hashes={str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
(p/'packet-sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
(p/'packet-sha256.txt').write_text(''.join(h+'  '+name+'\n' for name,h in hashes.items()))
print('prepared hookinstall packet:',p)
PY
if [[ $mode == run ]]; then "$packet/run.sh"; fi
