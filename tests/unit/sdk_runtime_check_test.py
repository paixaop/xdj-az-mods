#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import math
from pathlib import Path
import subprocess
import tempfile
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/integration'))
spec = importlib.util.spec_from_file_location("sdk_runtime_check", ROOT / "tests/integration/sdk_runtime_check.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


def loop(deck, revision=1, unit=None, **changes):
    row = dict(deck=str(deck), unit=str(unit or deck + 1), revision=str(revision), received_ns=str(revision * 100),
               engaged="1", bounds="1", in_s="1.851995465", out_s="5.601995465", mask="15", diagnostic_reason="0",
               check_in_samples="81673", check_out_samples="247048", check_in_cached_us="1852000",
               check_out_cached_us="5601995", check_rate="44100")
    row.update({key: str(value) for key, value in changes.items()})
    return " ".join(key + "=" + value for key, value in row.items())


def health(pid="123", restarts="0", tick=1):
    return {"pid": pid, "service": {"NRestarts": restarts, "ActiveState": "active", "SubState": "running"},
            "local_monotonic": tick}


class RuntimeCheckTests(unittest.TestCase):
    def test_all_decks_and_unit_generations_use_the_same_quantization_rule(self):
        rows = [loop(deck) for deck in range(4)]
        rows += [loop(deck, unit=deck + 10, received_ns=1000) for deck in range(4)]
        self.assertEqual(runtime.validate_transcript("\n".join(rows), (1, 2, 3, 4))["status"], "PASS")

    def test_no_events_or_missing_required_decks_are_incomplete(self):
        for text in ("", "pid=123 exports=1,1", loop(0)):
            self.assertEqual(runtime.validate_transcript(text, (1, 2, 3, 4))["status"], "INCOMPLETE")

    def test_old_quantization_rejection_is_detected_on_every_deck(self):
        for deck in range(4):
            report = runtime.validate_transcript(loop(deck, bounds=0, in_s=0, out_s=0, diagnostic_reason=12))
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("quantization-consistent", report["errors"][0])

    def test_zero_in_exit_and_unavailable_load_are_valid(self):
        rows = [loop(0, in_s=0, check_in_samples=0, check_in_cached_us=0),
                loop(0, revision=2, engaged=0, bounds=0, in_s=0, out_s=0, diagnostic_reason=1),
                "load_deck=0 availability=unavailable read_rc=-1 loaded=unknown"]
        self.assertEqual(runtime.validate_transcript("\n".join(rows))["status"], "PASS")

    def test_source_coordinate_failures_are_rejected(self):
        for changes in ({"in_s": "nan"}, {"out_s": "inf"}, {"in_s": -1}, {"out_s": 1},
                        {"engaged": 0}, {"bounds": 0}, {"check_in_cached_us": 1853000},
                        {"check_rate": 0}, {"in_s": 1.853}, {"diagnostic_reason": 12}):
            self.assertEqual(runtime.validate_transcript(loop(0, **changes))["status"], "FAIL", changes)

    def test_subsample_source_time_difference_is_equivalent(self):
        for rate in (44100, 48000, 96000):
            point = 81673 / rate
            value = loop(0, check_rate=rate, in_s=point + .5/rate,
                         out_s=247048/rate, check_in_cached_us=round(point*1e6),
                         check_out_cached_us=round(247048/rate*1e6))
            self.assertEqual(runtime.validate_transcript(value)['status'], 'PASS')

    def test_stream_revisions_and_unit_ownership_are_checked(self):
        for rows in ([loop(0), loop(0)], [loop(0, unit=2), loop(0, unit=1)],
                     [loop(0), loop(0, revision=2, received_ns=99)], [loop(0), loop(1, unit=1)]):
            self.assertEqual(runtime.validate_transcript("\n".join(rows))["status"], "FAIL")

    def test_streams_have_independent_receipt_clocks(self):
        live = "live_deck=0 unit=1 stream=play revision=1 received_ns=1 age_ns=4 valid=1 mode_is_2=1 play_pause=0 play_aux=0"
        self.assertEqual(runtime.validate_transcript(loop(0) + "\n" + live)["status"], "PASS")
        self.assertEqual(runtime.validate_transcript(live.replace("play_pause=0", "play_pause=2"))["status"], "FAIL")
        self.assertEqual(runtime.validate_transcript(live.replace("valid=1", "valid=0").replace("play_pause=0", "play_pause=2"))["status"], "PASS")

    def test_corrupt_transcripts_cannot_pass(self):
        for row in (loop(4), loop(0) + " deck=0", loop(0, revision=0), loop(0, received_ns=-1),
                    loop(0, check_rate=2147483647), loop(0, check_in_samples=2**64), "x" * 262145):
            self.assertEqual(runtime.validate_transcript(row)["status"], "FAIL")

    def test_health_does_not_promote_absence_or_one_sample_to_a_pass(self):
        for samples in ([], [health()], [health(pid=""), health(pid="", tick=2)]):
            self.assertEqual(runtime.validate_health(samples)["status"], "INCOMPLETE")

    def test_health_requires_stable_process_and_service_clocks(self):
        self.assertEqual(runtime.validate_health([health(), health(tick=2)])["status"], "PASS")
        cases = [[health(), health(pid="124", tick=2)], [health(), health(restarts="1", tick=2)],
                 [health(), health(pid="123 124", tick=2)], [health(), health(tick=0)],
                 [health(), health(tick=math.nan)]]
        bad = health(tick=2); del bad["service"]["NRestarts"]; cases.append([health(), bad])
        for samples in cases:
            self.assertEqual(runtime.validate_health(samples)["status"], "FAIL", samples)

    def test_observed_process_loss_and_failed_service_are_failures(self):
        failed = health(pid="", tick=2)
        failed["service"].update(ActiveState="failed", SubState="failed")
        for samples in ([health(), health(pid="", tick=2)], [failed, failed], [failed], [health(pid="123 124")]):
            self.assertEqual(runtime.validate_health(samples)["status"], "FAIL")

    def test_transport_loss_does_not_erase_an_observed_process_restart(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = SimpleNamespace(action="health", host="127.0.0.1", port=2230, key=Path("/key"),
                                   known_hosts=Path("/known"), seconds=10, output=Path(temporary) / "out")
            readings = [SimpleNamespace(returncode=0, stdout=pid + "\nNRestarts=0\nActiveState=active\nSubState=running\n", stderr="")
                        for pid in ("123", "124")]
            readings.append(subprocess.TimeoutExpired("ssh", 1))
            with patch.object(runtime.argparse.ArgumentParser, "parse_args", return_value=args), \
                 patch.object(runtime.subprocess, "run", side_effect=readings), \
                 patch.object(runtime.time, "monotonic", side_effect=range(100)), patch.object(runtime.time, "sleep"):
                self.assertEqual(runtime.main(), 1)
            self.assertIn('"status": "FAIL"', (args.output / "report.json").read_text())

    def test_transport_is_pinned_and_rejects_injection(self):
        args = runtime.ssh_arguments("127.0.0.1", 2230, "/key", "/known")
        self.assertIn("StrictHostKeyChecking=yes", args)
        self.assertIn("UserKnownHostsFile=/known", args)
        self.assertEqual(args[-1], "root@127.0.0.1")
        for host, port in (("-oProxyCommand=evil", 22), ("host;echo", 22), ("host\nother", 22),
                           ("localhost", True), ("localhost", 0), ("localhost", 65536)):
            with self.assertRaises(ValueError):
                runtime.ssh_arguments(host, port, "/key", "/known")


if __name__ == "__main__":
    unittest.main()
