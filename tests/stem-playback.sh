#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Run the existing stem-control and load/eject fixtures against exact source
# excerpts. Map only unchanged excerpts for coverage; controls stay unmapped.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 - "$work" <<'PY'
import json, re, sys
from pathlib import Path
out = Path(sys.argv[1])

def mapped(path, text, start, end):
    return '#line {} {}\n'.format(text[:start].count('\n') + 1, json.dumps(str(path.resolve()))) + text[start:end] + '\n'

path = Path('src/azstem/azstem.c')
text = path.read_text()
start = text.index('#define CTL_MASTER_BIT ')
end = text.index('\n}', text.index('static void read_stems(', start)) + 2
control = mapped(path, text, start, end)
(out/'control.inc').write_text(control)
raw_control = re.sub(r'^#line[^\n]*\n', '', control, flags=re.M)
control_mutations = {
    'overwrite': (r'if \(direct >> d & 1u\)\n\s*continue;', ''),
    'edge': (r'atomic_fetch_or_explicit\(&g_evict_req, 1u << d, memory_order_relaxed\);', '(void) 0;'),
    'engaged': (r'unsigned em = v & 0x0Fu;', 'unsigned em = v & 0xFFu;'),
    'master': (r'if \(!\(direct & CTL_MASTER_BIT\)\) \{', '{'),
}
for name, (pattern, replacement) in control_mutations.items():
    changed, count = re.subn(pattern, replacement, raw_control)
    assert count, name
    (out/('control-'+name+'.inc')).write_text(changed)

path = Path('src/azstem/stem_playback.c')
text = path.read_text()
tick = ''
for name in ('deck_track_changed', 'stems_discover_base', 'stems_tick_empty', 'stems_same_file',
             'stems_tick_existing', 'stems_confirm_change', 'stems_tick_changed',
             'stems_tick_fallback', 'stems_tick_deck', 'stems_tick'):
    match = re.search(r'^static (?:void|int) '+name+r'\([^;]*?\) \{', text, re.M)
    assert match, name
    tick += mapped(path, text, match.start(), text.index('\n}', match.start()) + 2)
(out/'tick.inc').write_text(tick)
raw_tick = re.sub(r'^#line[^\n]*\n', '', tick, flags=re.M)
variants = {
    'eject': raw_tick.replace('    deck_track_changed(deck); /* generation + identity: ejected */', ''),
    'order': re.sub(r'(publish_deck_file\(deck, "curtrack", tr\);\n\s*publish_src\(deck\);)',
                    r'\1\n    deck_track_changed(deck);',
                    raw_tick.replace('    deck_track_changed(deck);\n    if (!have_base)', '    if (!have_base)')),
    'base': raw_tick.replace('int have_base = g_stems_base[0] != 0;',
                             'int have_base = g_stems_base[0] != 0;\n    if (!have_base) return;'),
    'deferred': raw_tick.replace('g_load_deferred[deck] = 1;', 'g_load_deferred[deck] = 0;'),
}
for name, changed in variants.items():
    assert changed != raw_tick, name
    (out/('tick-'+name+'.inc')).write_text(changed)
PY
flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -pthread -Itests/unit)
case ${SAN:-plain} in
plain|'') ;;
asan) flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer) ;;
tsan) flags+=(-fsanitize=thread -fno-omit-frame-pointer) ;;
*) exit 2 ;;
esac
for family in control tick; do
    fixture=stemctl; macro=STEMCTL_INC; variants=(overwrite edge engaged master)
    if [ "$family" = tick ]; then fixture=trackgen; macro=TICK_INC; variants=(eject order base deferred); fi
    "${HOST_CC:-cc}" "${flags[@]}" "-D$macro=\"$work/$family.inc\"" "tests/unit/${fixture}_test.c" -o "$work/$family"
    "$work/$family"
    [ "${SAN:-plain}" = plain ] || continue
    for variant in "${variants[@]}"; do
        name="$family-$variant"
        "${HOST_CC:-cc}" "${flags[@]}" "-D$macro=\"$work/$name.inc\"" "tests/unit/${fixture}_test.c" -o "$work/$name"
        if "$work/$name" >"$work/$name.log" 2>&1; then
            echo "FAIL: $name control was not caught" >&2; exit 1
        fi
        grep -q FAIL "$work/$name.log"
        echo "CONTROL caught: $name"
    done
done
