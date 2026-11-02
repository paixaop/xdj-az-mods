#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The preflight must reject unavailable runtimes and broken fault controls."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

EXTENSIONS = ["automation", "time", "command", "exit", "cue-actor", "cue-store", "cue-lifecycle", "cue-persist", "cue-recall", "cue-stage", "cue-commands", "stored", "live", "sources", "native-commands", "load",
              "load-lifecycle", "vtable", "hook-install", "source-probe", "load-probe", "wave-overview", "wave-report"]


FAKE_PROBE = '''#!/usr/bin/env python3
import os, pathlib, sys, time
mode = sys.argv[1]
runtime = pathlib.Path(__file__).name
scenario = os.environ.get("PREFLIGHT_TEST_SCENARIO", "good")
if "LSAN_OPTIONS" in os.environ:
    print("inherited LSan options escaped preflight isolation", file=sys.stderr)
    sys.exit(1)
if mode == "clean":
    if scenario == "vma" and runtime == "tsan":
        print("FATAL: ThreadSanitizer: unsupported VMA range", file=sys.stderr)
        sys.exit(66)
    if scenario == "lsan" and runtime == "asan":
        print("LeakSanitizer has encountered a fatal error.", file=sys.stderr)
        sys.exit(1)
    if scenario == "memory-failure":
        print("ERROR: AddressSanitizer: heap-buffer-overflow", file=sys.stderr)
        sys.exit(1)
    if scenario == "hang":
        time.sleep(60)
    if scenario != "silent-clean":
        print("preflight clean-ready")
    sys.exit(0)
if mode == "expected-failure":
    print("preflight expected failure", file=sys.stderr)
    sys.exit(73)
if scenario == "uninstrumented":
    sys.exit(0)
if scenario == "wrong-diagnostic":
    print("some unrelated failure", file=sys.stderr)
    sys.exit(1)
reports = {
    "heap": "ERROR: AddressSanitizer: heap-buffer-overflow",
    "signed": "runtime error: signed integer overflow",
    "leak": "ERROR: LeakSanitizer: detected memory leaks",
    "race": "WARNING: ThreadSanitizer: data race",
}
print(reports[mode], file=sys.stderr)
sys.exit(1)
'''


class SanitizerPreflightTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform not in ("linux", "darwin"):
            raise unittest.SkipTest("POSIX process-group test")
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler unavailable")
        cls.directory = tempfile.TemporaryDirectory(prefix="az-preflight-tests-")
        cls.root = pathlib.Path(__file__).resolve().parents[2]
        cls.limit = pathlib.Path(cls.directory.name) / "limit"
        subprocess.run([compiler, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                        str(cls.root / "tests/unit/sanitizer_preflight_timeout.c"),
                        "-o", str(cls.limit)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_preflight(self, scenario, modes=("plain", "asan", "tsan")):
        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory:
            work = pathlib.Path(directory)
            shutil.copy2(str(self.limit), str(work / "limit"))
            for mode in modes:
                binary = work / mode
                binary.write_text(FAKE_PROBE)
                binary.chmod(0o755)
            environment = os.environ.copy()
            environment["PREFLIGHT_TEST_SCENARIO"] = scenario
            environment["AZ_SAN_PREFLIGHT_TIMEOUT"] = "1"
            environment["LSAN_OPTIONS"] = "suppressions=/unused-fixture-suppression"
            result = subprocess.run(["sh", str(self.root / "tests/unit/sanitizer_preflight_run.sh"),
                                     str(work)] + list(modes), env=environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    universal_newlines=True, timeout=15)
            return result

    def test_clean_and_every_fault_diagnostic_required(self):
        result = self.run_preflight("good")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.count("PREFLIGHT CONTROL caught:"), 5)
        self.assertIn("PREFLIGHT: OK", result.stdout)

    def test_vma_unavailable_is_not_a_pass(self):
        result = self.run_preflight("vma")
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("PREFLIGHT UNAVAILABLE: tsan", result.stdout)
        self.assertNotIn("PREFLIGHT: OK", result.stdout)

    def test_lsan_failure_cannot_be_turned_into_a_pass(self):
        result = self.run_preflight("lsan")
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("PREFLIGHT UNAVAILABLE: asan", result.stdout)
        self.assertNotIn("PREFLIGHT: OK", result.stdout)

    def test_real_clean_run_report_is_a_failure(self):
        result = self.run_preflight("memory-failure", ("asan",))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("PREFLIGHT FAIL: asan clean", result.stdout)
        self.assertNotIn("UNAVAILABLE", result.stdout)

    def test_dropped_instrumentation_is_detected(self):
        result = self.run_preflight("uninstrumented")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("PREFLIGHT: OK", result.stdout)

    def test_unrelated_nonzero_exit_does_not_count_as_detection(self):
        result = self.run_preflight("wrong-diagnostic")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("PREFLIGHT CONTROL caught: asan", result.stdout)

    def test_clean_probe_must_reach_its_marker(self):
        result = self.run_preflight("silent-clean", ("plain",))
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_runtime_hang_is_bounded_and_unavailable(self):
        result = self.run_preflight("hang", ("asan",))
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("timed out", result.stdout)
        self.assertNotIn("PREFLIGHT: OK", result.stdout)

    def test_limit_forwards_exit_status_and_rejects_bad_deadline(self):
        result = subprocess.run([str(self.limit), "1", "/bin/sh", "-c", "exit 17"])
        self.assertEqual(result.returncode, 17)
        result = subprocess.run([str(self.limit), "0", "/bin/sh", "-c", "exit 0"])
        self.assertEqual(result.returncode, 125)

    def run_linux_fixture(self, preflight_status, only=False, fail_mode="", sdk_fail_mode="", overview_fail_mode=""):
        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory:
            root = pathlib.Path(directory)
            (root / "tests").mkdir()
            (root / "tests/unit").mkdir()
            (root / "bin").mkdir()
            for name in ("sanitizer_preflight_test", "sdk_native_integration_test", "sdk_completion_test", "sdk_exit_link_test"):
                (root / ("tests/unit/" + name + ".py")).write_text('print("fixture controller regression ran")\n')
            shutil.copy2(str(self.root / "tests/run-linux.sh"), str(root / "tests/run-linux.sh"))
            (root / "tests/sanitizer-preflight.sh").write_text(
                '#!/bin/sh\nprintf "fixture preflight exit=%s\\n" "$PREFLIGHT_TEST_STATUS"\nexit "$PREFLIGHT_TEST_STATUS"\n')
            (root / "tests/run.sh").write_text(
                '#!/bin/sh\nprintf "%s\\n" "${SAN:-plain}" >>"$PREFLIGHT_TEST_CALLS"\n'
                'echo "UNIT TESTS: fixture"\n[ "${SAN:-plain}" != "$PREFLIGHT_TEST_FAIL_MODE" ]\n')
            (root / "tests/sdk-completion.sh").write_text(
                '#!/bin/sh\n[ "$1" = --extensions-only ] || exit 2\n'
                'printf "%s\\n" "${SAN:-plain}" >>"$PREFLIGHT_TEST_SDK_CALLS"\n'
                'echo "fixture overview ${SAN:-plain}"\necho "SDK COMPLETION: fixture"\n'
                '[ "${SAN:-plain}" != "$PREFLIGHT_TEST_SDK_FAIL_MODE" ] && '
                '[ "${SAN:-plain}" != "$PREFLIGHT_TEST_OVERVIEW_FAIL_MODE" ]\n')
            for name in ("sdk-cache-reply-native", "sdk-cache-entry", "sdk-cache-entry-caller",
                         "sdk-cache-entry-fp", "sdk-cue", "sdk-cue-public", "sdk-cue-export",
                         "sdk-cue-events", "sdk-deck-listeners", "sdk-data-lifecycle", "sdk-slip", "sdk-exit"):
                (root / ("tests/" + name + ".sh")).write_text("#!/bin/sh\nexit 0\n")
            docker = root / "bin/docker"
            docker.write_text('''#!/usr/bin/env python3
import os, subprocess, sys
args = sys.argv[1:]
if "bash" not in args:
    sys.exit(0)
i = args.index("bash")
script = args[i+2].replace("/preflight/run", os.environ["PREFLIGHT_TEST_RUN_DIR"])
result = subprocess.run(["bash", "-c", script] + args[i+3:])
sys.exit(result.returncode)
''')
            docker.chmod(0o755)
            calls = root / "calls.txt"
            sdk_calls = root / "sdk-calls.txt"
            environment = os.environ.copy()
            environment.update({
                "PATH": str(root / "bin") + os.pathsep + environment.get("PATH", ""),
                "PREFLIGHT_TEST_STATUS": str(preflight_status),
                "PREFLIGHT_TEST_CALLS": str(calls),
                "PREFLIGHT_TEST_FAIL_MODE": fail_mode,
                "PREFLIGHT_TEST_SDK_CALLS": str(sdk_calls),
                "PREFLIGHT_TEST_SDK_FAIL_MODE": sdk_fail_mode,
                "PREFLIGHT_TEST_OVERVIEW_FAIL_MODE": overview_fail_mode,
                "PREFLIGHT_TEST_RUN_DIR": str(root / "preflight-run"),
                "TMPDIR": str(root),
            })
            command = ["bash", str(root / "tests/run-linux.sh")]
            if only:
                command.append("--preflight-only")
            result = subprocess.run(command, env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, universal_newlines=True, timeout=15)
            modes = calls.read_text().splitlines() if calls.exists() else []
            sdk_modes = sdk_calls.read_text().splitlines() if sdk_calls.exists() else []
            return result, modes, sdk_modes

    def test_full_linux_suite_requires_ready_runtime(self):
        result, modes, sdk_modes = self.run_linux_fixture(0)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(modes, ["plain", "asan", "tsan"])
        self.assertEqual(sdk_modes, ["plain", "asan", "tsan"])
        self.assertEqual(result.stdout.count("fixture controller regression ran"), 4)
        self.assertEqual(result.stdout.count("fixture overview"), 3)

    def test_unavailable_runtime_blocks_full_linux_suite(self):
        result, modes, sdk_modes = self.run_linux_fixture(2)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertEqual(modes, [])
        self.assertEqual(sdk_modes, [])
        self.assertIn("UNIT TESTS: NOT RUN", result.stdout)

    def test_failed_preflight_is_not_ignored(self):
        result, modes, sdk_modes = self.run_linux_fixture(1)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(modes, [])
        self.assertEqual(sdk_modes, [])

    def test_preflight_only_cannot_claim_full_tests(self):
        result, modes, sdk_modes = self.run_linux_fixture(0, only=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(modes, [])
        self.assertEqual(sdk_modes, [])
        self.assertIn("full unit suite NOT RUN", result.stdout)

    def test_later_linux_pass_runs_after_a_failure(self):
        result, modes, sdk_modes = self.run_linux_fixture(0, fail_mode="asan")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(modes, ["plain", "asan", "tsan"])
        self.assertEqual(sdk_modes, ["plain", "asan", "tsan"])

    def test_extension_failure_propagates_and_later_pass_runs(self):
        result, modes, sdk_modes = self.run_linux_fixture(0, sdk_fail_mode="asan")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(modes, ["plain", "asan", "tsan"])
        self.assertEqual(sdk_modes, ["plain", "asan", "tsan"])

    def test_overview_failure_propagates_and_all_later_passes_run(self):
        result, modes, sdk_modes = self.run_linux_fixture(0, overview_fail_mode="asan")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(modes, ["plain", "asan", "tsan"])
        self.assertEqual(sdk_modes, ["plain", "asan", "tsan"])
        self.assertEqual(result.stdout.count("fixture overview"), 3)

    def run_completion_fixture(self, scenario="good", extensions_only=False):
        with tempfile.TemporaryDirectory(dir=self.directory.name) as directory:
            root = pathlib.Path(directory)
            (root / "tests").mkdir()
            shutil.copy2(str(self.root / "tests/sdk-completion.sh"), str(root / "tests/sdk-completion.sh"))
            names = ["data", *EXTENSIONS]
            for name in names:
                if scenario == "missing" and name == "stored":
                    continue
                text = '#!/bin/sh\necho "' + name + '" >>"$PREFLIGHT_TEST_CALLS"\n'
                if name == "stored":
                    if scenario == "failed":
                        text += 'echo "fixture failure"; exit 1\n'
                    elif scenario == "skip-zero":
                        text += 'echo "SKIP: required fixture unavailable"\n'
                    elif scenario == "skip-77":
                        text += 'echo "required fixture unavailable"; exit 77\n'
                script = name + ".sh" if name.startswith("wave-") else "sdk-" + name + ".sh"
                (root / "tests" / script).write_text(text)
            calls = root / "calls.txt"
            environment = os.environ.copy()
            environment.update({"PREFLIGHT_TEST_CALLS": str(calls), "SAN": "plain", "TMPDIR": str(root)})
            command = ["bash", str(root / "tests/sdk-completion.sh")]
            if extensions_only:
                command.append("--extensions-only")
            result = subprocess.run(command, env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, universal_newlines=True, timeout=15)
            return result, calls.read_text().splitlines()

    def test_completion_runs_foundation_and_all_extensions(self):
        result, names = self.run_completion_fixture()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(names, ["data", *EXTENSIONS])
        self.assertIn("SDK COMPLETION: OK", result.stdout)

    def test_completion_extensions_do_not_repeat_foundation(self):
        result, names = self.run_completion_fixture(extensions_only=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(names, EXTENSIONS)

    def test_completion_missing_required_suite_is_failure(self):
        result, names = self.run_completion_fixture("missing")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("native-commands", names)
        self.assertNotIn("SDK COMPLETION: OK", result.stdout)

    def test_completion_failed_suite_does_not_hide_later_results(self):
        result, names = self.run_completion_fixture("failed")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertEqual(names[-1], "wave-report")

    def test_completion_zero_and_77_skips_are_incomplete(self):
        for scenario in ("skip-zero", "skip-77"):
            result, names = self.run_completion_fixture(scenario)
            self.assertEqual(result.returncode, 2, result.stdout)
            self.assertEqual(names[-1], "wave-report")
            self.assertIn("SDK COMPLETION: INCOMPLETE", result.stdout)
            self.assertNotIn("SDK COMPLETION: OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
