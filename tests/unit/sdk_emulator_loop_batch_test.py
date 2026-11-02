#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fixed-plan validation and orchestration, without running a target."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'integration'))
from sdk_emulator_loop_test import FakeProbe, line, READY
import sdk_emulator_loop_batch as batch


class FakeBatch(FakeProbe):
    def __init__(self):
        super().__init__()
        self.stream = []

    def emit(self, deck, **updates):
        super().emit(deck, **updates)
        self.stream.append(line(self.rows[-1]))

    def plan(self, event, kind, identity, deck, load, deadline, status='INCOMPLETE', completed=0):
        self.stream.append('SDK_LOOP_PLAN ' + ' '.join(f'{k}={v}' for k, v in dict(v=1, event=event, kind=kind, id=identity, deck=deck, load=load, status=status, completed=completed, query_ns=int(self.now * 1e9), deadline_ns=deadline).items()) + '\n')

    def read(self):
        return READY + ''.join(self.stream)

    def send(self, text):
        raw = dict(p.split('=', 1) for p in text.split())
        if raw['action'] not in batch.COUNTS:
            return super().send(text)
        kind, identity, deck, load = raw['action'], int(raw['id']), int(raw['deck']), int(raw['load'])
        deadline = int(self.now * 1e9) + 180_000_000_000
        self.plan('start', kind, identity, deck, load, deadline)
        operations = range(1, 13) if kind == 'refusals' else batch.OPERATIONS
        for step, op in enumerate(operations):
            self.emit(deck)
            values = dict(raw, action='submit', id=identity + step, op=op, position=.05 if op in (10, 11) else 0, end=.05 if op == 12 else 0, beats=8 if op == 9 else 0)
            super().send(' '.join(f'{k}={v}' for k, v in values.items()))
            self.emit(deck)
        self.plan('done', kind, identity, deck, load, deadline, 'PASS', batch.COUNTS[kind])


class BatchTests(unittest.TestCase):
    def case(self, kind='matrix'):
        probe = FakeBatch()
        load = 2**64 - 1 if kind == 'refusals' else 7
        probe.send(batch.batch_request('123', kind, 1, 0, load, 30_000_000_000))
        return probe, load

    def test_validates_each_public_api_result_not_only_plan_marker(self):
        for kind in ('refusals', 'matrix'):
            probe, load = self.case(kind)
            result = batch.validate(probe.read(), kind, 1, 0, load, sample_rate=44100)
            self.assertEqual(result['status'], 'PASS')
            self.assertEqual(len(result['evidence']), batch.COUNTS[kind])
            self.assertEqual(result['operations'], list(range(1, 13)))
        text = ''.join(p for p in probe.stream if p.startswith('SDK_LOOP_PLAN '))
        with self.assertRaises(batch.MissingEvidence):
            batch.validate(text, 'matrix', 1, 0, 7, sample_rate=44100)

    def test_missing_release_wrong_target_and_bad_terminal_never_pass(self):
        probe, load = self.case()
        for mutate in (
            lambda s: s.replace('event=release', 'event=poll'),
            lambda s: s.replace('target_load=7', 'target_load=8'),
            lambda s: s.replace('outcome=1', 'outcome=4'),
        ):
            with self.subTest(mutate=mutate), self.assertRaises((ValueError, batch.MissingEvidence)):
                batch.validate(mutate(probe.read()), 'matrix', 1, 0, load, sample_rate=44100)

    def test_plan_identity_bounds_and_partial_id_reservation(self):
        probe, _ = self.case()
        for change in ('status=PASS completed=14', 'status=PASS completed=16'):
            text = probe.read().replace('status=PASS completed=15', change)
            with self.assertRaises(ValueError):
                batch.plans(text)
        text = probe.read().replace('status=PASS completed=15', 'status=INCOMPLETE completed=2')
        with self.assertRaises(batch.MissingEvidence):
            batch.validate(text, 'matrix', 1, 0, 7, sample_rate=44100)
        self.assertEqual(batch.next_id(text, sample_rate=44100), 16)
        text = ''.join(p for p in probe.stream if p.startswith('SDK_LOOP_PLAN '))
        self.assertEqual(batch.next_id(text), 16)
        with self.assertRaises(ValueError):
            batch.batch_request('123', 'matrix', 1, 0, 2**64 - 1, 100)

    def test_four_deck_batch_run_and_monotonic_ranges_across_phases(self):
        probe = FakeBatch()
        kwargs = dict(sleep=probe.sleep, clock=lambda: probe.now, sample_rate=44100)
        refusals = batch.run_batch_refusals(probe.read, probe.send, lambda: True, '123', **kwargs)
        self.assertEqual(refusals['status'], 'PASS')
        self.assertEqual(len(refusals['evidence']), 48)
        matrix = batch.run_batch_commands(probe.read, probe.send, lambda: True, '123', **kwargs)
        self.assertEqual(matrix['status'], 'PASS')
        self.assertEqual(len(matrix['evidence']), 60)
        self.assertGreater(min(r['request_id'] for r in matrix['evidence']), 48)

    def test_wrong_independent_boundary_state_fails_even_with_applied_receipt(self):
        probe, load = self.case()
        for index, text in enumerate(probe.stream):
            if 'event=snapshot' in text and 'out=2.7' in text:
                probe.stream[index] = text.replace('in=1.0', 'in=1.5')
        with self.assertRaises(ValueError):
            batch.validate(probe.read(), 'matrix', 1, 0, load, sample_rate=44100)


if __name__ == '__main__':
    unittest.main()
