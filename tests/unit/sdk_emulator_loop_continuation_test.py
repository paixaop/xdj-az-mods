#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic continuation contracts; no emulator, SSH, or wall-clock wait."""
import contextlib
import io
import json
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "integration"))
import sdk_emulator_loop_continuation as continuation
import sdk_emulator_loop_runtime as runtime
from sdk_emulator_controls import position_motion as real_position_motion


PID = 44
BINDING = {"provider_build_id": "b" * 40, "header_sha256": "c" * 64,
           "engine_rate": 44100, "abi_size": 296}


def packet(rows, **deck_changes):
    samples = []
    for seconds, position in rows:
        deck = dict(read_rc=0, loaded=1, title="Bad Romance", pos_s=position)
        deck.update(deck_changes)
        samples.append(dict(native_monotonic_ns=int(seconds * 1e9), decks=[deck]))
    return {"samples": samples,
            "native_monotonic_ns": samples[-1]["native_monotonic_ns"]}


class Clock:
    def __init__(self):
        self.value = 0.0

    def now(self):
        return self.value

    def sleep(self, seconds):
        self.value += seconds


class Fixture:
    def __init__(self, loop_packets=None, exit_packets=None, proof=None, final_load=None,
                 boottime_offset=0):
        self.clock = Clock()
        # LENGTH was queried at native second 1 and acknowledged at second 6:
        # its original deadline is second 11, not receipt + ten seconds.
        self.loop_packets = loop_packets or [
            packet([(8, 10.9), (8.5, 10.1), (9, 10.8), (9.5, 10.2)]),
            packet([(10, 10.9), (12, 10.1)]),
            packet([(13, 10.9), (14, 10.1)]),
            packet([(15, 10.9), (16, 10.1)]),
        ]
        self.exit_packets = exit_packets or [packet([(17, 10.8), (18, 11.2), (19, 11.4)])]
        self.proof = proof or {"binding_verified": True, "fresh": True}
        self.final_load = final_load
        self.boottime_offset = boottime_offset
        self.calls = []
        self.loop_reads = 0
        self.exit_reads = 0
        self.exit_after_loop_reads = None
        self.exited = False
        self.length_started = False
        self.validation_calls = []
        fixture = self

        class Client:
            def __init__(self, read_log, send_request, health, pid, **options):
                if pid != PID or options["timeout"] != 20:
                    raise AssertionError("client must use the qualified native process")
                self.evidence = []

            def submit(self, deck, operation, **options):
                fixture.calls.append((deck, operation, options))
                if operation == 9:
                    fixture.length_started = True
                if operation == 4 and fixture.length_started:
                    fixture.exited = True
                    fixture.exit_after_loop_reads = fixture.loop_reads
                self.evidence.append({
                    "before": {"query_ns": 1_000_000_000},
                    "terminal": {"receipt_ns": 17_000_000_000 if fixture.exited else 6_000_000_000},
                    "observed": {"engaged": int(operation != 4), "bounds_valid": 1, "in": 10.0, "out": 11.0,
                                 "native_load": 7, "load_unit": 3},
                })
                if operation == 6 and fixture.final_load is not None:
                    self.evidence[-1]["observed"].update(zip(("native_load", "load_unit"), fixture.final_load))

        self.client = Client

    def read_guest(self, command):
        if f"/tmp/az-sdk-position.{PID}.json" not in command or "/proc/uptime" not in command:
            raise AssertionError("position proof must come from the selected native process")
        if self.exited:
            index = self.exit_reads
            self.exit_reads += 1
            packets = self.exit_packets
        else:
            index = self.loop_reads
            self.loop_reads += 1
            packets = self.loop_packets
        value = packets[min(index, len(packets) - 1)]
        uptime = (max(sample["native_monotonic_ns"] for sample in value["samples"]) / 1e9
                  + self.boottime_offset)
        return json.dumps(value) + "\n__UPTIME__\n" + str(uptime) + " 0\n"

    def validate(self, value, deck, pid, binding, *, now_boottime_ns):
        # position_motion's binding and freshness implementation has its own
        # tests; here verify the continuation client requires its qualification.
        if (deck, pid) != (1, str(PID)) or binding is not BINDING:
            raise AssertionError("position validation lost its deck/process binding")
        self.validation_calls.append((value, now_boottime_ns))
        return self.proof

    def run(self, **options):
        with patch.object(continuation, "LoopClient", self.client), \
                patch.object(continuation, "position_motion", side_effect=self.validate), \
                contextlib.redirect_stdout(io.StringIO()):
            return continuation.run_continuation(
                lambda: "", lambda request: None, lambda: {"pid": PID},
                PID, self.read_guest, BINDING,
                clock=self.clock.now, sleep=self.clock.sleep, **options,
            )


class LoopContinuationTests(unittest.TestCase):
    def test_two_qualified_wraps_after_original_deadline_then_exit_and_clear(self):
        case = Fixture()
        self.assertEqual(case.run(), {"status": "PASS", "post_deadline_wraps": 2})
        self.assertEqual(case.calls, [
            (0, 4, {}), (0, 1, {}), (0, 2, {}),
            (0, 9, {"beats": 1}), (0, 4, {}), (0, 6, {}),
        ])
        # Two early wraps and a wrap crossing the deadline must earn no credit.
        # EXIT occurs only after both endpoints of two later wraps exceed 11s.
        self.assertEqual(case.exit_after_loop_reads, 4)
        self.assertEqual(case.exit_reads, 1)
        self.assertEqual(len(case.validation_calls), 5)

    def test_real_position_qualifier_accepts_full_bound_packets_and_pid_representation(self):
        def native_packet(start, positions):
            samples = []
            for sequence, position in enumerate(positions, 1):
                stamp = int(start * 1e9) + sequence * 100_000_000
                frame = round(position * BINDING["engine_rate"])
                decks = [dict(deck=deck, read_rc=0, loaded=1, title="Bad Romance",
                              playhead=frame, pos_s=frame / BINDING["engine_rate"])
                         for deck in range(4)]
                samples.append(dict(sequence=sequence, native_monotonic_ns=stamp, decks=decks))
            published = samples[-1]["native_monotonic_ns"]
            return dict(BINDING, schema=1, pid=PID, status="ok", sequence=len(samples),
                        native_monotonic_ns=published, native_boottime_ns=published, samples=samples)

        case = Fixture(
            loop_packets=[native_packet(11.5, [10.6, 10.9, 10.1, 10.5, 10.9, 10.1])],
            exit_packets=[native_packet(17.5, [10.6, 10.8, 11.1, 11.3, 11.4, 11.5])],
        )
        # Exercise the actual schema/PID/clock validator. Mocking its result
        # would conceal an incompatible integer PID at this call boundary.
        with patch.object(continuation, "LoopClient", case.client), \
                patch.object(continuation, "position_motion", wraps=real_position_motion) as qualifier, \
                contextlib.redirect_stdout(io.StringIO()):
            observed = continuation.run_continuation(
                lambda: "", lambda request: None, lambda: {"pid": PID},
                PID, case.read_guest, BINDING, clock=case.clock.now, sleep=case.clock.sleep,
            )
        self.assertEqual(observed, {"status": "PASS", "post_deadline_wraps": 2})
        self.assertEqual(qualifier.call_count, 2)
        self.assertTrue(all(call[0][2] == str(PID) for call in qualifier.call_args_list))
        self.assertEqual([op for _, op, _ in case.calls], [4, 1, 2, 9, 4, 6])

    def test_predeadline_wraps_alone_cannot_pass(self):
        case = Fixture(loop_packets=[packet([(8, 10.9), (9, 10.1), (9.5, 10.8), (10, 10.2)])])
        with self.assertRaisesRegex(RuntimeError, "did not wrap twice"):
            case.run()
        self.assertFalse(case.exited)
        self.assertEqual([op for _, op, _ in case.calls], [4, 1, 2, 9])

    def test_boottime_offset_cannot_satisfy_monotonic_deadline(self):
        case = Fixture(loop_packets=[packet([(8, 10.9), (9, 10.1), (9.5, 10.8), (10, 10.2)])],
                       boottime_offset=1000)
        with self.assertRaisesRegex(RuntimeError, "did not wrap twice"):
            case.run()
        self.assertFalse(case.exited)

    def test_out_of_bounds_backward_motion_is_not_a_loop_wrap(self):
        case = Fixture(loop_packets=[packet([(12, 12.0), (13, 10.1), (14, 12.0), (15, 10.1)])])
        with self.assertRaisesRegex(RuntimeError, "did not wrap twice"):
            case.run()
        self.assertFalse(case.exited)

    def test_unloaded_or_different_source_fails_before_exit(self):
        for changes in ({"loaded": 0}, {"title": "Another track"}):
            with self.subTest(changes=changes):
                case = Fixture(loop_packets=[packet([(12, 10.9), (13, 10.1)], **changes)])
                with self.assertRaisesRegex(RuntimeError, "source identity changed"):
                    case.run()
                self.assertFalse(case.exited)

    def test_failed_position_read_is_not_credited_even_in_a_healthy_packet(self):
        value = packet([(12, 10.9), (13, 10.1)])
        value["samples"][1]["decks"][0]["read_rc"] = -1
        case = Fixture(loop_packets=[value])
        with self.assertRaisesRegex(RuntimeError, "source identity changed"):
            case.run()
        self.assertFalse(case.exited)

    def test_nonfinite_positions_cannot_prove_continuation(self):
        for value in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(position=value):
                case = Fixture(loop_packets=[packet([(12, 10.9), (13, value)])])
                with self.assertRaisesRegex(RuntimeError, "source identity changed"):
                    case.run()
                self.assertFalse(case.exited)

    def test_binding_and_freshness_are_both_required(self):
        for proof in ({"binding_verified": False, "fresh": True},
                      {"binding_verified": True, "fresh": False}):
            with self.subTest(proof=proof):
                case = Fixture(proof=proof)
                with self.assertRaisesRegex(RuntimeError, "clock/binding unavailable"):
                    case.run()
                self.assertFalse(case.exited)

    def test_exit_must_advance_beyond_previous_out_before_clear(self):
        case = Fixture(exit_packets=[packet([(18, 10.7), (19, 10.8)])])
        with self.assertRaisesRegex(RuntimeError, "EXIT did not resume source beyond loop OUT"):
            case.run()
        self.assertTrue(case.exited)
        self.assertEqual([op for _, op, _ in case.calls], [4, 1, 2, 9, 4])

    def test_same_title_reload_or_unit_replacement_cannot_pass(self):
        for final_load in ((8, 3), (7, 4)):
            with self.subTest(final_load=final_load):
                case = Fixture(final_load=final_load)
                with self.assertRaisesRegex(RuntimeError, "native load changed"):
                    case.run()
                self.assertEqual([op for _, op, _ in case.calls], [4, 1, 2, 9, 4, 6])

    def test_configured_track_title_is_used_for_source_qualification(self):
        case = Fixture()
        for value in case.loop_packets + case.exit_packets:
            for sample in value["samples"]:
                sample["decks"][0]["title"] = "Configured track"
        self.assertEqual(case.run(expected_titles=("Configured track",))["status"], "PASS")


class ContinuationPanelLeaseTests(unittest.TestCase):
    @staticmethod
    def sessions(close):
        # Use the real group's close method without opening a receiver/session.
        group = runtime.PanelTransportSessions.__new__(runtime.PanelTransportSessions)
        group.closed = False
        group.active = SimpleNamespace(close=close)
        return group

    def test_neutral_then_acknowledged_close_precedes_sdk_only_observation(self):
        events = []
        receiver_close = Mock(side_effect=lambda: events.append("close"))
        group = self.sessions(receiver_close)
        controller = SimpleNamespace(panel_inputs_neutral=lambda: events.append("neutral") or True)
        outcome = {"status": "PASS"}
        reader, writer = object(), object()

        def sdk(*args, **options):
            events.append("sdk")
            self.assertTrue(group.closed)
            receiver_close.assert_called_once_with()
            return outcome

        with patch.object(runtime, "run_continuation", side_effect=sdk) as callback:
            try:
                actual = runtime.run_continuation_after_panel_close(
                    controller, group, reader, writer, expected_titles=("Track",),
                )
            finally:
                group.close()  # normal outer-runner cleanup must not close twice
        self.assertIs(actual, outcome)
        self.assertEqual(events, ["neutral", "close", "sdk"])
        receiver_close.assert_called_once_with()
        callback.assert_called_once_with(reader, writer, expected_titles=("Track",))

    def test_non_neutral_inputs_block_close_and_sdk_observation(self):
        events = []
        controller = SimpleNamespace(panel_inputs_neutral=lambda: events.append("neutral") or False)
        sessions = Mock()
        with patch.object(runtime, "run_continuation") as callback:
            with self.assertRaisesRegex(runtime.MissingEvidence, "inputs must be neutral"):
                runtime.run_continuation_after_panel_close(controller, sessions, object())
        self.assertEqual(events, ["neutral"])
        sessions.close.assert_not_called()
        callback.assert_not_called()

    def test_receiver_close_failure_propagates_without_sdk_work_or_retry(self):
        events = []
        failure = RuntimeError("receiver did not acknowledge close")

        def failed_close():
            events.append("close")
            raise failure

        receiver_close = Mock(side_effect=failed_close)
        group = self.sessions(receiver_close)
        group.close = Mock(wraps=group.close)
        controller = SimpleNamespace(panel_inputs_neutral=lambda: events.append("neutral") or True)
        with patch.object(runtime, "run_continuation") as callback:
            with self.assertRaises(RuntimeError) as caught:
                try:
                    runtime.run_continuation_after_panel_close(controller, group, object())
                finally:
                    group.close()
        self.assertIs(caught.exception, failure)
        self.assertEqual(events, ["neutral", "close"])
        receiver_close.assert_called_once_with()
        self.assertEqual(group.close.call_count, 2)  # helper once, outer finally once
        callback.assert_not_called()


if __name__ == "__main__":
    unittest.main()
