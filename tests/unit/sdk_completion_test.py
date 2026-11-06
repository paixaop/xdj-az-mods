#!/usr/bin/env python3
"""Runner contract: all required suites execute, and skips never become passes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
REQUIRED = (
    "records", "automation", "time", "command", "command-delivery", "exit", "cue-actor", "cue-store", "cue-lifecycle", "cue-persist", "cue-recall", "cue-stage", "cue-commands", "stored", "live", "sources", "native-commands", "load",
    "load-lifecycle", "vtable", "hook-install", "source-probe", "load-probe",
    "wave-overview", "wave-report",
)


class CompletionRunner(unittest.TestCase):
    def run_fixture(self, changes=None, *, foundation=False, sanitizer="plain"):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tests = root / "tests"
            tests.mkdir()
            shutil.copyfile(ROOT / "tests/sdk-completion.sh", tests / "sdk-completion.sh")
            for name in ("data", *REQUIRED):
                script = tests / (name + ".sh" if name.startswith("wave-") else "sdk-" + name + ".sh")
                body = (changes or {}).get(name, "exit 0\n")
                if body is not None:
                    script.write_text("#!/bin/sh\nprintf '%s\\n' '" + name + "' >> ran\n" + body)
            args = ["bash", str(tests / "sdk-completion.sh"), "--output", str(root / "logs")]
            if not foundation:
                args.append("--extensions-only")
            result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    universal_newlines=True,
                                    env={**os.environ, "SAN": sanitizer})
            ran = (root / "ran").read_text().splitlines() if (root / "ran").exists() else []
            logs = {path.stem for path in (root / "logs").glob("*.log")}
            return result, ran, logs

    def test_every_required_suite_in_every_mode(self):
        for sanitizer in ("plain", "asan", "tsan"):
            with self.subTest(sanitizer=sanitizer):
                result, ran, logs = self.run_fixture(sanitizer=sanitizer)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(ran, list(REQUIRED))
                self.assertEqual(logs, set(REQUIRED))
                self.assertIn("SDK COMPLETION: OK", result.stdout)

    def test_foundation_is_required_by_default(self):
        result, ran, _ = self.run_fixture(foundation=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(ran, ["data", *REQUIRED])

    def test_failure_does_not_stop_later_suites(self):
        for name in REQUIRED:
            with self.subTest(name=name):
                result, ran, _ = self.run_fixture({name: "exit 1\n"})
                self.assertEqual(result.returncode, 1)
                self.assertEqual(ran, list(REQUIRED))
                self.assertNotIn("SDK COMPLETION: OK", result.stdout)

    def test_missing_suite_fails_and_later_suites_run(self):
        result, ran, _ = self.run_fixture({"load": None})
        self.assertEqual(result.returncode, 1)
        self.assertEqual(ran, [name for name in REQUIRED if name != "load"])
        self.assertIn("required suite tests/sdk-load.sh is missing", result.stdout)

    def test_skip_status_and_skip_text_are_incomplete(self):
        for body in ("exit 77\n", "echo 'SKIP: unavailable runtime'\nexit 0\n"):
            with self.subTest(body=body):
                result, ran, _ = self.run_fixture({"sources": body})
                self.assertEqual(result.returncode, 2)
                self.assertEqual(ran, list(REQUIRED))
                self.assertIn("SDK COMPLETION: INCOMPLETE", result.stdout)
                self.assertNotIn("SDK COMPLETION: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
