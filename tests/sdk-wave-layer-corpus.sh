#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Explicit local immutable 2EX corpus; genuine streaming backend, independent inverse.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Linux ] || { echo 'SKIP: Linux file-provider corpus probe required'; exit 77; }
[ "$#" -eq 1 ] || { echo 'usage: sdk-wave-layer-corpus.sh <provided USBANLZ root>' >&2; exit 2; }
corpus_root=$1
task_parent=${EVIDENCE_DIR:-${TMPDIR:-/tmp}}
mkdir -p "$task_parent"
task_tmp=$(mktemp -d "$task_parent/az-wave-layer-corpus.XXXXXX")
if [ -z "${EVIDENCE_DIR:-}" ]; then trap 'rm -rf "$task_tmp"' EXIT; fi
python3 - "$corpus_root" "$task_tmp" <<'PY'
from pathlib import Path
import hashlib,json,sys
root=Path(sys.argv[1]).resolve(strict=True); out=Path(sys.argv[2])
paths=sorted(root.rglob('*.2EX'))
assert len(paths)==91, 'exactly91 provided 2EX inputs required'
records=[]
for p in paths:
 assert p.is_file() and not p.is_symlink(), 'regular immutable input required'
 b=p.read_bytes(); records.append({'relative':p.relative_to(root).as_posix(),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()})
(out/'corpus-before.json').write_text(json.dumps({'root':str(root),'files':records,'count':len(records)},indent=2)+'\n')
(out/'inputs.nul').write_bytes(b''.join(str(p).encode()+b'\0' for p in paths))
PY
inputs=()
while IFS= read -r -d '' corpus_path; do inputs+=("$corpus_path"); done < "$task_tmp/inputs.nul"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
ubsan) flags+=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer);;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer);;
*) echo 'SAN must be plain, asan, ubsan or tsan' >&2; exit 2;;
esac
command=("${HOST_CC:-gcc}" "${flags[@]}" tests/unit/sdklayeredcorpus_probe.c -lm -o "$task_tmp/corpus")
python3 - "$task_tmp/command.json" "${command[@]}" <<'PY'
from pathlib import Path
import hashlib,json,sys
files=['tests/unit/sdklayeredcorpus_probe.c','tests/sdk-wave-layer-corpus.sh',
       'src/azmod/sdk_wave_provider.h','src/azmod/sdk_waveform.h','src/azmod/sdk_analysis.h',
       'src/azmod/hc_anlz.h','src/azmod/azmod-mod.h']
Path(sys.argv[1]).write_text(json.dumps({'argv':sys.argv[2:],'sha256':{f:hashlib.sha256(Path(f).read_bytes()).hexdigest()for f in files},'scope':'genuine st_wave_read backend; all provided PWV7 columns, independent subset inverse; no native execution/render/calibration or source/load claim'},indent=2)+'\n')
PY
if ! "${command[@]}" > "$task_tmp/compile.log" 2>&1; then
    cat "$task_tmp/compile.log"; echo 'FAIL: corpus compilation' >&2; exit 1
fi
status=0
(ulimit -c 0; "$task_tmp/corpus" "${inputs[@]}") > "$task_tmp/runtime.log" 2>&1 || status=$?
printf '%s\n' "$status" > "$task_tmp/runtime.status"
cat "$task_tmp/runtime.log"
# Also check integrity after a failing probe. No corpus file is opened writable.
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1]); before=json.loads((out/'corpus-before.json').read_text());root=Path(before['root'])
paths=sorted(root.rglob('*.2EX')); after=[]
for p in paths:
 assert p.is_file() and not p.is_symlink()
 b=p.read_bytes(); after.append({'relative':p.relative_to(root).as_posix(),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()})
same=after==before['files']
(out/'corpus-after.json').write_text(json.dumps({'root':str(root),'files':after,'count':len(after),'identical_to_before':same},indent=2)+'\n')
assert same, 'immutable corpus changed'
PY
if [ -n "${EVIDENCE_DIR:-}" ]; then echo "EVIDENCE_RETAINED=$task_tmp"; fi
[ "$status" = 0 ] &&
    grep -q '^LAYERED_CORPUS_BASELINE_OK real_st_wave_read=1 raw_encoding=3 independent_PWV7=1$' "$task_tmp/runtime.log" &&
    grep -q '^WAVE_LAYER_CORPUS_PASS files=91 ' "$task_tmp/runtime.log"
