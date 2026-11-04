#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -n ${WAVE_REPORT_BUILD_ONLY:-} ]]; then
    task_tmp=$WAVE_REPORT_BUILD_ONLY
    mkdir -p "$task_tmp"
else
    task_tmp=$(mktemp -d "${TMPDIR:-/tmp}/az-wave-report.XXXXXX")
    trap 'rm -rf "$task_tmp"' EXIT
fi
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread -fexceptions -fasynchronous-unwind-tables)
if [[ $(uname -s) == Darwin ]]; then
    # Apple Clang rejects the fixture's deliberately placed inline-assembly
    # labels when it emits asynchronous CFI. The control validates the exact
    # fault PC itself, so keep the same probe without malformed unwind tables.
    flags+=(-fno-asynchronous-unwind-tables)
fi
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) echo 'SAN must be plain, asan or tsan' >&2; exit 2 ;;
esac
if [[ -n ${WAVE_REPORT_EXTRA_FLAGS:-} ]]; then
    read -r -a extra_flags <<< "$WAVE_REPORT_EXTRA_FLAGS"
    flags+=("${extra_flags[@]}")
fi
python3 - "$task_tmp" <<'PY'
from pathlib import Path
import re,sys
root=Path(sys.argv[1]); text=Path('src/azmod/waveform.c').read_text()
prefix='static void wave_prov_report(void) {'
assert text.count(prefix)==1
start=text.index(prefix); end=text.index('\n}',start)+2
body=text[start:end]
status=re.search(r'enum ov_lease_binding_status \{[^}]*\};',text,re.S)
assert status,'lease binding status enum missing'
(root/'real.inc').write_text(status.group(0)+'\n'+body+'\n')
needle='    fprintf(f, "overview:'
assert body.count(needle)==1
probe=r'''    for (int i=0;i<__atomic_load_n(&g_ovi_n,__ATOMIC_RELAXED);++i) {
        long key=g_ovi[i].self;
        if (key) {
            extern char wave_report_probe_begin[], wave_report_probe_end[];
            wave_report_expect_probe((uintptr_t)(key+0x80), __FILE__, __LINE__+2, wave_report_probe_begin, wave_report_probe_end);
            __asm__ volatile(".globl " WAVE_REPORT_LABEL("wave_report_probe_begin") "\n" WAVE_REPORT_LABEL("wave_report_probe_begin") ":\n" ::: "memory");
            volatile int old_value=*(volatile int *)(key+0x80);
            __asm__ volatile(".globl " WAVE_REPORT_LABEL("wave_report_probe_end") "\n" WAVE_REPORT_LABEL("wave_report_probe_end") ":\n" ::: "memory");
            fprintf(f,"old probe=%d\n",old_value);
        }
    }
'''
(root/'old-probe.inc').write_text(status.group(0)+'\n'+body.replace(needle,probe+needle)+'\n')
PY
for variant in real old-probe; do
    variant_flags=()
    # Keep the control's two machine labels in source order. Sanitizers remain enabled;
    # the real production body above is still checked with the release optimization.
    if [[ $variant == old-probe ]]; then variant_flags+=(-O0); fi
    ${HOST_CC:-cc} "${flags[@]}" "${variant_flags[@]}" -DWAVE_REPORT_INC="\"$task_tmp/$variant.inc\"" \
        tests/unit/wavereport_test.c -o "$task_tmp/$variant"
done
# This parent must not inherit the child's sanitizer interception. The child's complete
# SAN flags/options remain enabled; the parent validates its exact signal/ASan evidence.
controller_flags=(-std=c11 -O2 -g -Wall -Wextra -Werror)
${HOST_CC:-cc} "${controller_flags[@]}" tests/unit/wavereport_control.c -o "$task_tmp/check-control"
cat > "$task_tmp/run-wave-report.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
./real
./real --retired
./check-control --self-test
./check-control ./old-probe control-poisoned.log
./check-control ./old-probe control-retired.log --retired
echo 'CONTROL caught: restored borrowed-widget probe for poisoned and retired keys'
SH
chmod +x "$task_tmp/run-wave-report.sh"
if [[ -n ${WAVE_REPORT_BUILD_ONLY:-} ]]; then
    python3 - "$task_tmp" "${SAN:-plain}" "${flags[@]}" <<'PY'
from pathlib import Path
import hashlib,json,sys
root=Path(sys.argv[1]); digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources=['src/azmod/waveform.c','tests/unit/wavereport_test.c','tests/unit/wavereport_control.c','tests/wave-report.sh']
(root/'manifest.json').write_text(json.dumps({'mode':sys.argv[2],'flags':sys.argv[3:],
    'sources':{p:digest(Path(p)) for p in sources},
    'controller_flags':['-std=c11','-O2','-g','-Wall','-Wextra','-Werror'],
    'old_probe_extra_flags':['-O0'],
    'executables':{p:digest(root/p) for p in ['real','old-probe','check-control']},
    'controls':['exact address/PC/source signal or ASan proof; poisoned and retired keys',
                'validator self-tests reject unrelated faults and zero exits']},indent=2)+'\n')
PY
    echo "Compile-only report bundle: $task_tmp"
else
    bash "$task_tmp/run-wave-report.sh"
fi
