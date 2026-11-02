#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: GNU Linux waveform schema fixture required'; exit 77; }
task_parent=${EVIDENCE_DIR:-${TMPDIR:-/tmp}}; mkdir -p "$task_parent"
task_tmp=$(mktemp -d "$task_parent/az-wave-layers.XXXXXX")
if [ -z "${EVIDENCE_DIR:-}" ];then trap 'rm -rf "$task_tmp"' EXIT;fi
flags=(-std=c++11 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers)
case ${SAN:-plain} in
plain|'') ;;
asan)flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
ubsan)flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
tsan)flags+=(-fsanitize=thread -fno-omit-frame-pointer);;
*)exit 2;;esac
: "${SDK_WAVE_ORACLE:?Set SDK_WAVE_ORACLE to the locally supplied oracle source; firmware code is not distributed}"
"${HOST_CC:-gcc}" -g -c "$SDK_WAVE_ORACLE" -o "$task_tmp/oracle.o"
readelf -h -s -r -wf "$task_tmp/oracle.o" >"$task_tmp/oracle-elf-fde.txt"
"${HOST_OBJDUMP:-objdump}" -dr "$task_tmp/oracle.o" >"$task_tmp/oracle-disassembly.txt"
python3 - "$task_tmp/oracle.o" "$task_tmp/oracle-proof.json" <<'PY'
from pathlib import Path
import hashlib,json,struct,sys
p=Path(sys.argv[1]);b=p.read_bytes();h=struct.unpack_from('<16sHHIQQQIHHHHHH',b);assert b[:6]==b'\x7fELF\x02\x01'
proof={'object_sha256':hashlib.sha256(b).hexdigest(),'elf_machine':h[2],'native_decoder_only':True,'audit_executes_code':False,'functions':[]}
if h[2]==183:
 sections=[struct.unpack_from('<IIQQQQIIQQ',b,h[6]+i*h[11])for i in range(h[12])]
 for sec in sections:
  if sec[1]!=2:continue
  names=sections[sec[6]];strings=b[names[4]:names[4]+names[5]]
  for o in range(sec[4],sec[4]+sec[5],sec[9]):
   n,info,other,index,value,size=struct.unpack_from('<IBBHQQ',b,o)
   name=strings[n:strings.find(b'\0',n)].decode()
   if name not in ['sww_layer_native_id_130','sww_layer_native_id_200']:continue
   text=sections[index];code=b[text[4]+value:text[4]+value+size]
   assert size==20 and info&15==2
   assert not any(s[1]in(4,9) and s[7]==index and s[5]for s in sections),'oracle text must have no relocations'
   proof['functions'].append({'name':name,'section_value':value,'size':size})
 assert len(proof['functions'])==2
Path(sys.argv[2]).write_text(json.dumps(proof,indent=2)+'\n')
PY
leaf_expected=$(python3 - "$task_tmp/oracle-proof.json" <<'PY'
from pathlib import Path
import json,sys
print(int(json.loads(Path(sys.argv[1]).read_text())['elf_machine']==183))
PY
)
if [ "${REQUIRE_ARM_ORACLE:-0}" = 1 ] && [ "$leaf_expected" != 1 ];then echo 'FAIL: actual AArch64 oracle object required' >&2;exit 1;fi
command=("${HOST_CXX:-g++}" "${flags[@]}" tests/unit/sdkwavelayers_test.cpp "$task_tmp/oracle.o" -lm -o "$task_tmp/wave-layers")
SDK_WAVE_ORACLE="$SDK_WAVE_ORACLE" python3 - "$task_tmp/command.json" "${command[@]}" <<'PY'
from pathlib import Path
import hashlib,json,sys,os
files=['tests/unit/sdkwavelayers_test.cpp',os.environ['SDK_WAVE_ORACLE'],'tests/sdk-wave-layers.sh','src/azmod/sdk_waveform.h']
Path(sys.argv[1]).write_text(json.dumps({'argv':sys.argv[2:],'sha256':{p:hashlib.sha256(Path(p).read_bytes()).hexdigest()for p in files},'private_marker_present':'#define SDKW_LAYERED3_VERSION'in Path(files[-1]).read_text()},indent=2)+'\n')
PY
if ! "${command[@]}" >"$task_tmp/compile.log" 2>&1;then cat "$task_tmp/compile.log";echo 'FAIL: compilation is not runtime RED' >&2;exit 1;fi
failures=0
for profile in 1 2;do
 status=0;(ulimit -c 0;"$task_tmp/wave-layers" "$profile") >"$task_tmp/profile-$profile.log" 2>&1 || status=$?
 printf '%s\n' "$status" >"$task_tmp/profile-$profile.status";cat "$task_tmp/profile-$profile.log"
 if [ "${EXPECT_RED:-0}" = 1 ];then
  first=$(sed -n '/^FAIL /{p;q;}' "$task_tmp/profile-$profile.log")
  if [ "$status" != 134 ] || ! grep -q "WAVE_LAYER_BASELINE_OK profile=$profile old_decoder=1 native_leaf=$leaf_expected" "$task_tmp/profile-$profile.log" ||
     [[ "$first" != *'missing exact raw three-band native layered encoding'* ]] ||
     grep -Eq 'ERROR: (AddressSanitizer|ThreadSanitizer)|FATAL: ThreadSanitizer|runtime error:|undefined symbol|Segmentation fault|Illegal instruction|Bus error' "$task_tmp/profile-$profile.log";then
   failures=$((failures+1));echo 'FAIL: required concrete layered-record runtime assertion not reached' >&2
  else echo "EXPECTED_RUNTIME_RED profile=$profile: old PWV7/native-leaf baseline passed; layered encoding missing";fi
 elif [ "$status" != 0 ] || ! grep -q "PASS wave-layers profile=$profile" "$task_tmp/profile-$profile.log";then failures=$((failures+1));fi
done
if [ -n "${EVIDENCE_DIR:-}" ];then echo "EVIDENCE_RETAINED=$task_tmp";fi
[ "$failures" = 0 ]
