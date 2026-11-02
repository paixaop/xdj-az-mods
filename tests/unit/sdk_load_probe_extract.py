# SPDX-License-Identifier: GPL-3.0-only
"""Select the real optional private-load probe helper and independent controls."""
from pathlib import Path
import re, sys
root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
source = (root / 'tests/integration/sdk-loop-probe.c').read_text()
s = source
m = re.search(r'^static int load_probe_log\([^;{]*\{\n', s, re.M); assert m
s = s[m.start():s.index('\n}', m.start()) + 2] + '\n'
(out / 'real.inc').write_text(s)
controls = [
    ('size', 'prefix_size != sizeof *value', '((void) prefix_size, 0)'),
    ('version', 'value->version != 1', '0'),
    ('reserved', ' || sample->reserved', ''),
    ('required', '!sample->load_generation', '0'),
    ('future', ' || sample->observed_ns > now', ''),
    ('unit', 'sample->unit_generation < previous->unit_generation', '0'),
    ('generation', 'sample->load_generation < previous->load_generation', '0'),
    ('epoch', 'sample->invalidation_epoch <= previous->invalidation_epoch', '0'),
    ('time', 'sample->observed_ns < previous->observed_ns', '0'),
    ('native-id', 'sample->native_request_id <= previous->native_request_id', '0'),
    ('coherence', 'sample->load_generation == previous->load_generation &&', '0 &&'),
    ('unavailable', 'if (result == 0) {', 'if (result == 0 || result == -1) {'),
    ('history', 'if (sample) *previous = *sample;', 'if (sample) *previous = *sample; else memset(previous, 0, sizeof *previous);'),
    ('budget', 'position > 256L * 1024 - 128 - bytes', '0'),
]
for name, old, new in controls:
    assert s.count(old) == 1, (name, old)
    (out / (name + '.inc')).write_text(s.replace(old, new))

(out / 'poll-real.inc').write_text(source)
poll_controls = [
    ('export', 'dlsym(RTLD_DEFAULT, "azstem_load_observation_v1")', 'dlsym(RTLD_DEFAULT, "azstem_live_observation_v1")'),
    ('decks', 'for (int d = 0; d < 4; d++) {\n            struct azstem_load_observation_v1', 'for (int d = 0; d < 3; d++) {\n            struct azstem_load_observation_v1'),
    ('errors', '&load_availability[d], now_ns()) < 0) errors++;', '&load_availability[d], now_ns()) < 0) (void) 0;'),
    ('cap', 'position > 256L * 1024 - 16384', '0'),
    ('ticks', 'tick < AZSDK_LOOP_PROBE_TICKS', 'tick < AZSDK_LOOP_PROBE_TICKS + 1'),
    ('interval', 'struct timespec wait = {0, 100000000}', 'struct timespec wait = {0, 1}'),
]
for name, old, new in poll_controls:
    assert source.count(old) == 1, (name, old)
    (out / ('poll-' + name + '.inc')).write_text(source.replace(old, new))

operator_controls = [
    ('stop', 'access(stop_path, F_OK) == 0', '0'),
    ('expiry', 'AZSDK_LOOP_PROBE_OPERATOR || tick < AZSDK_LOOP_PROBE_TICKS', 'tick < AZSDK_LOOP_PROBE_TICKS'),
]
for name, old, new in operator_controls:
    assert source.count(old) == 1, (name, old)
    (out / ('operator-' + name + '.inc')).write_text(source.replace(old, new))
