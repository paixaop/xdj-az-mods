#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic protocol/controller checks; these never execute firmware."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'integration'))
import sdk_emulator_loop as loop


def row(**updates):
    value = dict(v=1, seq=1, event='snapshot', id=0, deck=0, op=0, target_load=0,
                 rc=0, outcome=-1, reason=0, revision=0, receipt_ns=0,
                 query_ns=20_000_000_000, load_valid=1, native_load=7,
                 load_unit=3, load_ns=10_000_000_000, loop_unit=8, loop_rev=1, loop_ns=10_000_000_000, engaged_valid=1, engaged=0,
                 bounds_valid=0, **{'in': 0, 'out': 0})
    value.update(updates)
    return value


def line(value):
    return 'SDK_LOOP ' + ' '.join(k + '=' + str(v) for k, v in value.items()) + '\n'


READY = 'SDK_LOOP_READY v=1 pid=123 commands=8190 data=/media/usb/sda1/MODS/community-mods/sdk-loop-command-probe/data\n'


class FakeProbe:
    def __init__(self):
        self.now = 20.0
        self.rows = []
        self.decks = [dict(engaged=0, bounds_valid=0, **{'in': 1.0, 'out': 2.0}) for _ in range(4)]
        self.failure = None
        self.freeze_observer = False

    def emit(self, deck, **updates):
        self.now += .01
        value = row(seq=len(self.rows) + 1, deck=deck, query_ns=int(self.now * 1e9), **self.decks[deck])
        value.update(updates)
        value['loop_ns'] = 10_000_000_000 if self.freeze_observer else value['query_ns'] - 1
        value['loop_rev'] = 1 if self.freeze_observer else value['seq']
        self.rows.append(value)

    def send(self, text):
        raw = dict(p.split('=', 1) for p in text.split())
        deck, op, identity = int(raw['deck']), int(raw['op']), int(raw['id'])
        if raw['action'] == 'snapshot':
            self.emit(deck)
            return
        if int(raw['load']) == 2**64 - 1:
            if self.failure == 'local-refusal':
                self.emit(deck, event='refused', id=identity, op=op, target_load=2**64 - 1, rc=-3)
                return
            if self.failure == 'expired-refusal':
                self.now += 11
            self.emit(deck, event='submit', id=identity, op=op, target_load=2**64 - 1, rc=-4, outcome=2, reason=-4, receipt_ns=int((self.now + .005) * 1e9))
            return
        state = self.decks[deck]
        common = dict(id=identity, op=op, target_load=int(raw['load']))
        self.emit(deck, event='submit', outcome=0, reason=1, **common)
        if op in (2, 3, 5):
            state.update(engaged=1, bounds_valid=1)
        if op == 4:
            state.update(engaged=0, bounds_valid=0)
        if op == 7:
            state['out'] += .7
        if op == 8:
            state['out'] -= .35
        if op == 9:
            state['out'] += .4
        if op in (10, 11):
            state['in'] += float(raw['position'])
        if op == 10:
            state['out'] += float(raw['position'])
        if op == 12:
            state['out'] += float(raw['end'])
        terminal = dict(outcome=1, reason=0, revision=identity, receipt_ns=int(self.now * 1e9))
        if self.failure == 'accepted-only':
            return
        if self.failure == 'uncertain':
            terminal.update(outcome=4, reason=-1, revision=0)
        if self.failure == 'wrong-load':
            common['target_load'] += 1
        self.emit(deck, event='poll', **common, **terminal)
        self.emit(deck, event='release', **common, **terminal)

    def read(self):
        return READY + ''.join(line(r) for r in self.rows)

    def sleep(self, seconds):
        self.now += seconds

    def run(self):
        return loop.run_commands(self.read, self.send, lambda: True, '123', sleep=self.sleep, clock=lambda: self.now, sample_rate=44100)


class LoopProtocolTests(unittest.TestCase):
    def test_full_controller_covers_every_operation_and_deck(self):
        result = FakeProbe().run()
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(set(result['decks']), {'0', '1', '2', '3'})
        for value in result['decks'].values():
            self.assertEqual(value['operations'], list(range(1, 13)))
        self.assertEqual(len(result['evidence']), 60)

    def test_actual_wrapper_refusals_remain_separate_from_native_application(self):
        probe = FakeProbe()
        result = loop.run_refusals(probe.read, probe.send, lambda: True, '123', sleep=probe.sleep, clock=lambda: probe.now)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(len(result['evidence']), 48)
        self.assertTrue(all(r['outcome'] == 2 for r in result['evidence']))
        applied = probe.run()
        self.assertEqual(applied['status'], 'PASS')
        self.assertGreater(min(r['request_id'] for r in applied['evidence']), 48)

    def test_probe_refusal_and_expiry_cannot_count_as_sdk_generation_checks(self):
        for failure in ('local-refusal', 'expired-refusal'):
            probe = FakeProbe()
            probe.failure = failure
            result = loop.run_refusals(probe.read, probe.send, lambda: True, '123', decks=(0,), sleep=probe.sleep, clock=lambda: probe.now)
            self.assertEqual(result['status'], 'INCOMPLETE')
            self.assertFalse(result['evidence'])

    def test_accepted_is_not_success(self):
        probe = FakeProbe()
        probe.failure = 'accepted-only'
        result = probe.run()
        self.assertEqual(result['status'], 'INCOMPLETE')
        self.assertFalse(result['evidence'])

    def test_uncertain_or_other_load_cannot_pass(self):
        for failure in ('uncertain', 'wrong-load'):
            probe = FakeProbe()
            probe.failure = failure
            self.assertEqual(probe.run()['status'], 'FAIL')

    def test_missing_fresh_observer_is_incomplete_despite_applied(self):
        probe = FakeProbe()
        probe.freeze_observer = True
        result = probe.run()
        self.assertEqual(result['status'], 'INCOMPLETE')
        self.assertTrue(all(v['status'] == 'INCOMPLETE' for v in result['decks'].values()))

    def test_process_health_guard_prevents_writes(self):
        probe = FakeProbe()
        client = loop.LoopClient(probe.read, lambda text: self.fail('unexpected write'), lambda: False, '123')
        with self.assertRaises(ValueError):
            client.snapshot(0)

    def test_receipt_schema_identity_clocks_and_bounds(self):
        changes = [dict(seq=2), dict(deck=4), dict(query_ns=0), dict(receipt_ns=30_000_000_000),
                   dict(load_unit=0), dict(load_ns=30_000_000_000), dict(load_valid=2),
                   dict(engaged=2), dict(bounds_valid=1, **{'in': 2, 'out': 1}),
                   dict(outcome=1, revision=0), dict(id=1), dict(extra=1)]
        for updates in changes:
            with self.subTest(updates=updates), self.assertRaises(ValueError):
                loop.records(line(row(**updates)), sample_rate=44100)
        for text in (line(row()).replace('in=0', 'in=nan'), line(row()).replace('seq=1', 'seq=01'), line(row()) + line(row())):
            with self.assertRaises(ValueError):
                loop.records(text)
        self.assertEqual(len(loop.records(line(row()))), 1)

    def test_source_bounds_use_the_bound_sample_window(self):
        for rate in (44100, 48000, 96000, 192000):
            with self.subTest(rate=rate):
                for end in (0., .5 / rate):
                    with self.assertRaises(ValueError):
                        loop.records(line(row(bounds_valid=1, **{'in': 0., 'out': end})), sample_rate=rate)
                self.assertEqual(len(loop.records(line(row(bounds_valid=1, **{'in': 0., 'out': 2. / rate})), sample_rate=rate)), 1)
                with self.assertRaises(ValueError):
                    loop.records(line(row(bounds_valid=1, **{'in': -2. / rate, 'out': 2. / rate})), sample_rate=rate)
        self.assertEqual(len(loop.records(line(row(bounds_valid=1, **{'in': 0., 'out': 1. / 128})), sample_rate=128)), 1)

    def test_source_bounds_require_source_rate(self):
        with self.assertRaises(loop.MissingEvidence):
            loop.records(line(row(bounds_valid=1, **{'in': 0., 'out': 1.})))

    def test_readiness_pinned_pid_safe_path_and_capability_refresh(self):
        self.assertEqual(loop.ready(READY.replace('commands=8190', 'commands=0') + READY, '123')['commands'], 8190)
        for text in (READY.replace('pid=123', 'pid=124'), READY.replace('/sda1/', '/sda1/../'), READY.replace('/data', '/data;bad'), READY + READY.replace('/sda1/', '/sda2/')):
            with self.assertRaises(ValueError):
                loop.ready(text, '123')

    def test_request_process_and_unused_arguments(self):
        text = loop.request('123', 'submit', 9, 2, 10, 7, 30_000_000_000, position=-.05)
        self.assertIn('action=submit id=9 deck=2 op=10 load=7', text)
        for args in (('0', 'snapshot'), ('123;bad', 'snapshot'), ('123', 'execute')):
            with self.assertRaises(ValueError):
                loop.request(*args)
        for kwargs in (dict(operation=1), dict(position=1), dict(request_id=1)):
            with self.assertRaises(ValueError):
                loop.request('123', 'snapshot', **kwargs)
        with self.assertRaises(ValueError):
            loop.request('123', 'submit', 1, 0, 1, 7, 100, position=float('inf'))

    def test_visible_postconditions_are_checked_independently(self):
        before = row(bounds_valid=1, engaged=1, **{'in': 1.0, 'out': 3.0})
        after = row(event='poll', id=1, op=10, target_load=7, outcome=1, revision=1, receipt_ns=19_000_000_000,
                    engaged=1, bounds_valid=1, **{'in': 1.05, 'out': 3.05})
        self.assertTrue(loop.applied(before, after, 10, 7, sample_rate=44100))
        broken = copy.deepcopy(after)
        broken['out'] = 3
        with self.assertRaises(ValueError):
            loop.applied(before, broken, 10, 7, sample_rate=44100)
        after['engaged'] = 0
        with self.assertRaises(ValueError):
            loop.applied(before, after, 3, 7)


if __name__ == '__main__':
    unittest.main()
