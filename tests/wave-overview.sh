#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Asset-independent checks of the actual native-overview hook/scaler source.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -n ${WAVE_OVERVIEW_BUILD_ONLY:-} ]]; then
    task_tmp=$WAVE_OVERVIEW_BUILD_ONLY
    mkdir -p "$task_tmp"
else
    task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-wave-overview.XXXXXX")
    trap 'rm -rf "$task_tmp"' EXIT
fi
flags=(-O2 -g -Wall -Wextra -Werror -pthread -fexceptions -fasynchronous-unwind-tables)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
if [[ -n ${WAVE_OVERVIEW_EXTRA_FLAGS:-} ]]; then
    read -r -a extra_flags <<< "$WAVE_OVERVIEW_EXTRA_FLAGS"
    flags+=("${extra_flags[@]}")
fi
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import re,sys
target=Path(sys.argv[1]); text=Path('src/azmod/waveform.c').read_text()
names=['OV_STRIDE','OV_MAX','OV_COL_A','OV_COL_B','OV_HGT_A','OV_HGT_B','OVINST_MAX','OV_IN_TILE']
constants=[]
for name in names:
    matches=re.findall(r'^#define '+name+r'\s+.*$',text,re.M)
    assert len(matches)==1,(name,matches)
    constants.append(matches[0])
atten=Path('src/azmod/drum_roll_display.c').read_text()
constants+=re.findall(r'^#define ATT_N\s+.*$',atten,re.M)
assert len(constants)==9
status=re.search(r'enum ov_lease_binding_status \{[^}]*\};',text,re.S)
assert status,'lease binding status enum missing'
constants.append(status.group(0))
(target/'constants.inc').write_text('\n'.join(constants)+'\n')
prefix='static unsigned char *ov_scaled_for('
assert text.count(prefix)==1,prefix
start=text.index(prefix);end=text.index('\n}',start)+2
functions=[text[start:end]]
start=text.index('static struct azov_lease_api g_ovlease;')
body=re.search(r'^static void ov_hook_owned\([^;]*?\) \{',text,re.M|re.S)
assert body,'overview dispatcher body missing'
end=text.index('\n}',body.end())+2
functions.append(text[start:end])
functions.append('static void ov_hook(long self) { ov_hook_owned(self,0,0,0); }')
start=text.index('#if defined(__aarch64__)\n__asm__(".text\\n.align 2\\n.globl azm_ov_entry')
end=text.index('#endif',start)+len('#endif')
functions.append('#if defined(__linux__)\n'+text[start:end]+'\n#endif')
(target/'functions.inc').write_text('\n'.join(functions)+'\n')
PY
compile() {
    local task_constants=$1 task_target=$2 task_functions=${3:-$task_tmp/functions.inc} task_broker=${4:-src/azstem/overview_lease.c}
    ${HOST_CC:-cc} -std=c11 "${flags[@]}" -Dmain=overview_scalar_tests \
        -DOVERVIEW_CONSTANTS_INC="\"$task_constants\"" -DOVERVIEW_FUNCTIONS_INC="\"$task_functions\"" \
        -c tests/unit/waveoverview_test.c -o "$task_target-c.o"
    ${HOST_CC:-cc} -std=c11 "${flags[@]}" -DAZOV_LEASE_UNIT -Isrc/azstem \
        -c "$task_broker" -o "$task_target-broker.o"
    ${HOST_CXX:-c++} -std=c++11 "${flags[@]}" tests/unit/waveoverview_exception_test.cpp \
        "$task_target-c.o" "$task_target-broker.o" -o "$task_target"
}
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import re,sys
root=Path(sys.argv[1]);text=(root/'constants.inc').read_text()
changed,n=re.subn(r'(#define OV_HGT_B\s+)5\b',r'\g<1>10',text)
assert n==1,'expected fixed native overview second-height offset5'
(root/'old-index.inc').write_text(changed)
PY
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import sys
root=Path(sys.argv[1]); text=(root/'functions.inc').read_text()
old='scope __attribute__((cleanup(ov_lease_cleanup))) ='
assert text.count(old)==1
text=text.replace(old,'scope =')
old='    g_orig_ov(self);\n}'
assert text.count(old)==1
(root/'normal-only.inc').write_text(text.replace(old,'    g_orig_ov(self);\n    ov_lease_cleanup(&scope);\n}'))
text=Path('src/azstem/overview_lease.c').read_text()
old='rc, &count, count + 1,'
assert text.count(old)==1
(root/'unpinned.c').write_text(text.replace(old,'rc, &count, count,'))
PY
compile "$task_tmp/constants.inc" "$task_tmp/real"
compile "$task_tmp/old-index.inc" "$task_tmp/control"
compile "$task_tmp/constants.inc" "$task_tmp/normal-only" "$task_tmp/normal-only.inc"
compile "$task_tmp/constants.inc" "$task_tmp/unpinned" "$task_tmp/functions.inc" "$task_tmp/unpinned.c"
cat > "$task_tmp/run-wave-overview.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
./real
./real --throw-only
./real --lifetime-only
check_control() {
    local binary=$1 argument=$2 description=$3 assertion=$4
    if ( ulimit -c 0; if [[ -n $argument ]]; then "./$binary" "$argument"; else "./$binary"; fi ) >"$binary.log" 2>&1; then
        echo "FAIL: control was not caught: $description" >&2; exit 1
    fi
    grep -q 'FAIL overview:' "$binary.log" && grep -Fq "$assertion" "$binary.log" || { cat "$binary.log" >&2; exit 1; }
    echo "CONTROL caught: $description"
}
check_control control '' 'old overview second-height offset10' '!memcmp(observed + i * 12, expected, sizeof expected)'
check_control normal-only --throw-only 'old normal-return-only overview restoration' 'read_pointer(throw_content, 0x28) == destroy_expected_points'
check_control unpinned --lifetime-only 'old unpinned overview lifetime' 'read_pointer(object, 0x28) == destroy_expected_points'
SH
chmod +x "$task_tmp/run-wave-overview.sh"
if [[ -n ${WAVE_OVERVIEW_BUILD_ONLY:-} ]]; then
    python3 - "$task_tmp" "${SAN:-plain}" "${flags[@]}" <<'PY'
from pathlib import Path
import hashlib,json,os,shlex,subprocess,sys
root=Path(sys.argv[1])
sources=['src/azmod/waveform.c','src/azmod/drum_roll_display.c','src/azstem/overview_lease.c',
         'src/common/azoverviewlease.h','tests/unit/waveoverview_test.c',
         'tests/unit/waveoverview_exception_test.cpp','tests/wave-overview.sh']
digest=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
compiler={}
for name,default in [('HOST_CC','cc'),('HOST_CXX','c++')]:
    command=shlex.split(os.environ.get(name,default))
    compiler[name]=subprocess.run(command+['--version'],check=True,stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,universal_newlines=True).stdout
manifest={'mode':sys.argv[2],'flags':sys.argv[3:],'compiler':compiler,
          'sources':{name:digest(Path(name)) for name in sources},
          'executables':{name:digest(root/name) for name in ['real','control','normal-only','unpinned']},
          'controls':['old-index10','normal-return-only','unpinned'],
          'runtime':'bash run-wave-overview.sh; no firmware GUI/native calls'}
(root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
PY
    echo "Compile-only overview bundle: $task_tmp"
else
    bash "$task_tmp/run-wave-overview.sh"
fi
