# SPDX-License-Identifier: GPL-3.0-only
"""Extract current live exports, projection and hook code, plus regression controls."""
from pathlib import Path
import re
import sys
root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
def mapped(source, path, start, end):
    return '#line {} "{}"\n'.format(source.count('\n', 0, start) + 1, root / path) + source[start:end] + '\n'
def unmapped(source):
    return re.sub(r'^#line[^\n]*\n', '', source, flags=re.M)
def fn(source, name, path='src/azstem/sdk_export.c'):
    match = re.search(r'^' + name + r'[^;{]*\{\n', source, re.M)
    assert match, name
    end = source.index('\n}', match.start()) + 2
    return mapped(source, path, match.start(), end)
def control(source, name, old, new):
    assert source.count(old) == 1, (name, old)
    (out / (name + '.inc')).write_text(unmapped(source.replace(old, new)))
s = (root / 'src/azstem/sdk_export.c').read_text()
export = fn(s, r'unsigned azstem_live_observer_mask_v1\(') + fn(s, r'int azstem_live_observation_v1\(')
(out / 'live-export.inc').write_text(export)
control(export, 'live-export-size', 'azdeck_record_size(out) < sizeof *out', '0')
control(export, 'live-export-version', 'out->version != 1', '0')
s = (root / 'src/azmod/sdk_deck.c').read_text()
from sdk_deck_extract import projection
deck = projection(s)
(out / 'live-deck.inc').write_text(deck)
control(deck, 'live-deck-prefix', 'memcpy(out, &v, bytes);', 'memcpy(out, &v, sizeof v);')
control(deck, 'live-deck-clock', 'v->quantize_observed_ns = sample->observed_ns[1];', 'v->quantize_observed_ns = sample->observed_ns[0];')
control(deck, 'live-deck-valid', 'sample->valid_fields & AZSDK_LIVE_QUANTIZE', 'sample->valid_fields & AZSDK_LIVE_PLAY')
control(deck, 'live-deck-normalized', 'v->valid |= AZ_LIVE_PLAY_FLAGS;', 'v->valid |= AZ_LIVE_PLAYBACK;')
control(deck, 'live-deck-master-tempo-valid', '(sample->key_lock == 0 || sample->key_lock == 1)', '1')
control(deck, 'live-deck-master-tempo-source', 'v->key_lock = v->master_tempo = sample->key_lock;', 'v->key_lock = sample->key_lock; v->master_tempo = sample->quantize;')
control(deck, 'live-deck-sync-valid', 'sync.valid == 1 && (sync.raw_mode == 0 || sync.raw_mode == 2)', 'sync.valid == 1')
control(deck, 'live-deck-sync-source', 'v->sync = sync.raw_mode == 2;', 'v->sync = v->quantize;')
sync_export = fn((root / 'src/azstem/sdk_export.c').read_text(), r'int azstem_sync_observation_v1\(')
(out / 'sync-export.inc').write_text(sync_export)
control(sync_export, 'sync-export-size', 'azdeck_record_size(out) < sizeof *out', '0')
control(sync_export, 'sync-export-version', 'out->version != 1', '0')
tempo_lock_export = fn((root / 'src/azstem/sdk_export.c').read_text(), r'int azstem_tempo_lock_observation_v1\(')
for name in ('sync-export', 'sync-export-size', 'sync-export-version'):
    path = out / (name + '.inc')
    addition = tempo_lock_export if name == 'sync-export' else unmapped(tempo_lock_export)
    path.write_text(path.read_text() + addition)
s = (root / 'src/azstem/stem_playback.c').read_text()
a = s.index('static _Atomic(void *) g_ls_self[NDECK];')
# Match the same published/development boundary used by tests/unit/run.sh.
m = re.search(r'^static DIR \*stem_opendir\(|^#ifndef AZMOD_RELEASE /\* the path scan|^static int is_dir\(const char \*p\) \{', s[a:], re.M)
assert m
lifetime = mapped(s, 'src/azstem/stem_playback.c', a, a + m.start())
s = (root / 'src/azstem/deck_track_map.c').read_text()
a = s.index('/* ---- DjPlayerUnit ctor hook'); b = s.index('static int lti_id_zero(', a)
lifetime += mapped(s, 'src/azstem/deck_track_map.c', a, b)
(out / 'live-lifetime.inc').write_text(lifetime)
legacy_register = re.search(r'if \(g_sdk_live_on && djpuctor_unit_owned\(self, claimed, exp\)\) '
                            r'\(void\) azsdk_live_register\(\(int\) n32, self\);', lifetime)
assert legacy_register
control(lifetime, 'live-lifetime-register', legacy_register.group(),
        '(void) n32; (void) self; (void) claimed; (void) exp;')
control(lifetime, 'live-lifetime-retire', 'if (g_sdk_live_on) azsdk_live_retire(d);', '(void) 0;')

s = (root / 'src/azstem/azstem.c').read_text()
config = fn(s, r'static void azstem_configure_live\(', 'src/azstem/azstem.c')
config += '#line 1 "{}"\n'.format(out / 'live-config-fixture.c')
config += 'static void live_configuration(int td) { azstem_configure_live((void *)(uintptr_t)td, sdk_loop_build); }\n'
(out / 'live-config.inc').write_text(config)
control(config, 'live-config-required', 'access("/home/root/settings/azmod.track-data.off", F_OK) != 0', '1')

s = (root / 'tests/integration/sdk-loop-probe.c').read_text()
probe = fn(s, r'static int live_probe_log\(', 'tests/integration/sdk-loop-probe.c')
(out / 'live-probe.inc').write_text(probe)
control(probe, 'live-probe-clock', 's->revisions[stream] == previous->revisions[stream]', 's->revisions[0] == previous->revisions[0]')
control(probe, 'live-probe-age', ' || compare_timestamps(s->observed_ns[stream], now, 1000000000, 1000000000, 1) == AZ_TIME_AFTER', '')
control(probe, 'live-probe-bound', 'position > 256L * 1024 - 128 - bytes', '0')

s = (root / 'src/azstem/deck_track_map.c').read_text()
flags = ''
for name in ('loop', 'live'):
    declaration = re.search(r'^static _Atomic int g_sdk_' + name + r'_on;$', s, re.M)
    assert declaration, name
    flags += declaration.group() + '\n'
s = (root / 'src/azstem/sdk_export.c').read_text()
for name in ('unsigned azstem_loop_observer_mask_v1', 'int azstem_loop_observation_v1', 'int azstem_loop_observation_v2', 'unsigned azstem_live_observer_mask_v1', 'int azstem_live_observation_v1'):
    flags += fn(s, name + r'\(')
(out / 'observer-flags.inc').write_text(flags)
control(flags, 'observer-loop-race', 'static _Atomic int g_sdk_loop_on;', 'static int g_sdk_loop_on;')
control(flags, 'observer-live-race', 'static _Atomic int g_sdk_live_on;', 'static int g_sdk_live_on;')
