#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Regression tests for source integrity and the native runner's failure semantics."""
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import tarfile
import unittest


HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, str(HERE / (name + ".py")))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


bundle = module("sdk_native_bundle")
prepare = module("sdk_completion_prepare")
dependencies = module("sdk_native_dependencies")


class NativeBundleTests(unittest.TestCase):
    def test_archive_has_exact_files_without_platform_sidecars(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary); payload = root / "payload"; payload.mkdir()
            (payload / "nested").mkdir(); (payload / "nested/binary").write_bytes(b"payload")
            if hasattr(os, "setxattr"):
                try:
                    os.setxattr(str(payload / "nested/binary"), "user.sdk-test" if sys.platform == "linux" else "com.codex.sdk-test", b"metadata")
                except OSError:
                    pass
            bundle.checksums(payload)
            destination = root / "packet.tar.gz"
            bundle.archive(payload, destination)
            with tarfile.open(str(destination), "r:gz") as packet:
                self.assertEqual(sorted(member.name for member in packet if member.isfile()), ["SHA256SUMS", "nested/binary"])
                self.assertEqual(packet.extractfile("nested/binary").read(), b"payload")
            with self.assertRaisesRegex(ValueError, "new"):
                bundle.archive(payload, destination)
            (payload / "alias").symlink_to("nested/binary")
            with self.assertRaisesRegex(ValueError, "symlink"):
                bundle.archive(payload, root / "bad.tar.gz")

    def test_hook_packet_requires_linux_before_any_compilation_or_packet_write(self):
        for platform in ("Darwin", "Linux"):
            with tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary); tools = root / "bin"; tools.mkdir()
                uname = tools / "uname"; uname.write_text("#!/bin/sh\necho " + platform + "\n"); uname.chmod(0o755)
                cc = tools / "cc"; cc.write_text('#!/bin/sh\ntouch "$HOOK_TEST_MARKER"\nexit 99\n'); cc.chmod(0o755)
                environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ.get("PATH", ""),
                                   HOST_CC=str(cc), HOOK_TEST_MARKER=str(root / "compiled"), SAN="plain")
                result = subprocess.run(["bash", str(ROOT / "tests/sdk-hook-install.sh"), "--prepare", str(root / "packet")],
                                        env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
                if platform == "Darwin":
                    self.assertEqual(result.returncode, 77, result.stdout)
                    self.assertFalse((root / "compiled").exists())
                    self.assertFalse((root / "packet").exists())
                else:
                    self.assertEqual(result.returncode, 99, result.stdout)
                    self.assertTrue((root / "compiled").exists())

    def test_native_transport_port_validation_precedes_build_or_remote_access(self):
        with tempfile.TemporaryDirectory() as temporary:
            for port in ("0", "-1", "65536", "0022", "22;echo", "9" * 100):
                result = subprocess.run(["bash", str(ROOT / "tests/sdk-data-deck.sh"), "--port", port,
                                         "127.0.0.1", temporary + "/absent-key", temporary + "/absent-known",
                                         temporary + "/logs"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn("Invalid SSH port", result.stderr)
                self.assertFalse((pathlib.Path(temporary) / "logs").exists())
            result = subprocess.run(["bash", str(ROOT / "tests/sdk-data-deck.sh"), "--port", "2230",
                                     "127.0.0.1", temporary + "/absent-key", temporary + "/absent-known",
                                     temporary + "/logs"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("SSH key", result.stderr)

    def test_snapshot_hashes_actual_copied_inputs_and_detects_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = root / "source"
            (source / "src").mkdir(parents=True)
            (source / "tests/unit").mkdir(parents=True)
            (source / "tests/integration").mkdir()
            (source / "src/example.c").write_text("int x;\n")
            (source / "tests/sdk-example.sh").write_text("exit 0\n")
            (source / "tests/sanitizer-preflight.sh").write_text("exit 0\n")
            manifest = root / "source.json"
            snapshot = root / "snapshot"
            bundle.snapshot(source, snapshot, manifest)
            bundle.verify(source, manifest)
            bundle.verify(snapshot, manifest)
            (source / "src/unrelated.c").write_text("int unrelated;\n")
            bundle.verify(source, manifest)
            (source / "src/example.c").write_text("int changed;\n")
            with self.assertRaisesRegex(ValueError, "source changed"):
                bundle.verify(source, manifest)

    def test_bundle_checksums_refuse_symlinks_and_repeated_generation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / "binary").write_bytes(b"test payload")
            (root / "alias").symlink_to("binary")
            with self.assertRaisesRegex(ValueError, "symlink"):
                bundle.checksums(root)
            (root / "alias").unlink()
            bundle.checksums(root)
            self.assertIn(bundle.sha256(root / "binary"), (root / "SHA256SUMS").read_text())
            with self.assertRaisesRegex(ValueError, "must be new"):
                bundle.checksums(root)

    def test_exact_reviewed_generators_and_build_plan_cover_all_six_families(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = pathlib.Path(temporary)
            prepare.generate(ROOT, work)
            build = prepare.Build(ROOT, work, work / "bin")
            build.plan()
            self.assertEqual(len(build.runs), 75)
            self.assertEqual(len(build.controls), 116)
            self.assertEqual(len(build.actions), 181)
            run_labels = {row[0] for row in build.runs}
            for mode in ("plain", "asan", "tsan"):
                self.assertIn("ext-mono-preview-" + mode, run_labels)
                self.assertIn("ext-live-sync-export-" + mode, run_labels)
                for kind in ("samples", "clock", "budget", "elapsed", "grid", "interval", "input"):
                    self.assertIn("ext-time-" + kind + "-" + mode, run_labels)
            labels = {row[0] for row in build.controls}
            self.assertIn("ext-native-commands-thread-reuse-control", labels)
            self.assertIn("ext-sources-native-fd-dup-close-control", labels)
            self.assertIn("ext-sources-candidate-result-control", labels)
            self.assertIn("ext-stored-preview-request-control", labels)
            self.assertIn("ext-live-lifetime-retire-control", labels)
            self.assertIn("ext-observer-loop-race-control", labels)
            self.assertIn("ext-observer-live-race-control", labels)
            self.assertIn("ext-live-allocator-nothrow-bypass-control", labels)
            self.assertIn("ext-sources-vfs-short-name-control", labels)
            self.assertIn("ext-sources-rtti-short-name-control", labels)
            for kind in ("mono-count", "mono-word", "mono-clock"):
                self.assertIn("ext-stored-" + kind + "-control", labels)
            for kind in ("valid", "source"):
                self.assertIn("ext-live-deck-master-tempo-" + kind + "-control", labels)
            for label in ("ext-sync-export-size-control", "ext-sync-export-version-control",
                          "ext-live-deck-sync-valid-control", "ext-live-deck-sync-source-control",
                          "ext-live-sync-slot-control", "ext-live-sync-mode-control",
                          "ext-live-sync-legacy-leak-control", "ext-live-sync-payload-short-control"):
                self.assertIn(label, labels)
            for slot in range(4):
                self.assertIn("ext-loop-strict-slot-" + str(slot) + "-control", labels)
            for kind in prepare.TIME_CPP + prepare.TIME_INPUT + prepare.TIME_ENGINE:
                self.assertIn("ext-time-" + kind + "-control", labels)
            self.assertEqual(len(labels), len(build.controls))
            for _, command, _ in build.actions:
                self.assertNotIn("-w", command)
                if "-fsanitize=address,undefined" in command:
                    if "-Wl,-rpath,$ORIGIN/runtime" not in command:
                        self.assertIn("-static-libasan", command)
                        self.assertIn("-static-libubsan", command)
                if "-fsanitize=thread" in command:
                    if "-Wl,-rpath,$ORIGIN/runtime" not in command:
                        self.assertIn("-static-libtsan", command)
            for name, command, _ in build.actions:
                if name in ("ext-live-observer-asan", "ext-live-observer-tsan", "ext-live-payload-overread-control", "ext-live-rtti-overread-control"):
                    self.assertIn("-Wl,-rpath,$ORIGIN/runtime", command)

    def test_generator_parser_ignores_runtime_helper_and_rejects_unterminated_block(self):
        with tempfile.TemporaryDirectory() as temporary:
            script = pathlib.Path(temporary) / "suite.sh"
            script.write_text("python3 - \"$1\" <<'PY'\nraise Exception('runtime only')\nPY\n"
                              "python3 - \"$task_tmp\" <<'PYX'\npass\nPYX\n")
            self.assertEqual(len(prepare.generators(script)), 1)
            script.write_text("python3 - \"$task_tmp\" <<'PY'\npass\n")
            with self.assertRaisesRegex(ValueError, "unterminated"):
                prepare.generators(script)

    def test_command_control_helper_runs_on_the_selected_python_and_requires_diagnostic(self):
        script = (ROOT / "tests/sdk-command.sh").read_text()
        match = re.search(r'python3 - "\$1" "\$2" "\$3" <<\'PY\'\n(.*?)\nPY\n', script, re.S)
        self.assertIsNotNone(match)
        with tempfile.TemporaryDirectory() as temporary:
            binary = pathlib.Path(temporary) / "control"
            binary.chmod(0o755) if binary.exists() else None
            for message, status in (("Assertion expected failed", 0), ("unrelated failure", 1)):
                binary.write_text("#!/bin/sh\necho '" + message + "' >&2\nexit 1\n")
                binary.chmod(0o755)
                result = subprocess.run([sys.executable, "-", str(binary), "Assertion.*failed", "fixture"],
                                        input=match.group(1), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        universal_newlines=True, timeout=10)
                self.assertEqual(result.returncode, status, result.stdout)

    def test_dependency_probe_keeps_output_binary_and_honors_real_preprocessor_flags(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source, binary = root / "fixture.c", root / "binary"
            source.write_text('#ifdef SELECTED\n#include "used.h"\n#else\n#include "unused.h"\n#endif\n')
            (root / "used.h").write_text("int selected;\n")
            (root / "unused.h").write_text("int unused;\n")
            binary.write_bytes(b"preserved executable bytes")
            command = dependencies.preprocess_command([compiler, "-DSELECTED", str(source), "-lm", "-o", str(binary)])
            output = subprocess.check_output(command, universal_newlines=True)
            paths = dependencies.parse_dependencies(output)
            self.assertIn(str(root / "used.h"), paths)
            self.assertNotIn(str(root / "unused.h"), paths)
            self.assertEqual(binary.read_bytes(), b"preserved executable bytes")
            with self.assertRaisesRegex(ValueError, "empty"):
                dependencies.parse_dependencies("")

    def test_unused_context_drift_is_recorded_but_compiled_input_changes_block(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for name in ("src", "tests/unit", "tests/integration"):
                (root / name).mkdir(parents=True)
            used, unused = root / "src/used.c", root / "src/unused.c"
            used.write_text("int used;\n")
            unused.write_text("int unused;\n")
            (root / "tests/sanitizer-preflight.sh").write_text("exit 0\n")
            original, required, report = root / "original.json", root / "required.json", root / "report.json"
            original.write_text(json.dumps({"sha256": bundle.source_hashes(root)}))
            required.write_text(json.dumps({"sha256": {"src/used.c": bundle.sha256(used)}}))
            unused.write_text("int unrelated_change;\n")
            bundle.context_drift(root, original, required, report)
            self.assertEqual(json.loads(report.read_text())["unused_context_changed"], ["src/unused.c"])
            self.assertEqual(json.loads(report.read_text())["unused_context_sha256"]["src/unused.c"]["current"], bundle.sha256(unused))
            used.write_text("int compiled_change;\n")
            with self.assertRaisesRegex(ValueError, "source changed"):
                bundle.context_drift(root, original, required, report)

    def run_native_fixture(self, scenario, argument="-"):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            shutil.copy2(str(HERE / "sdkdata_deck_run.sh"), str(root / "run.sh"))
            (root / "preflight").mkdir()
            (root / "path").mkdir()
            (root / "suite-foundation").write_text("0\n")
            (root / "preflight/run.sh").write_text("#!/bin/sh\necho 'fixture preflight'\nexit " + ("2" if scenario == "unavailable" else "0") + "\n")
            if scenario.startswith("shared-"):
                (root / "runtime").mkdir()
                (root / "shared-preflight").mkdir()
                (root / "shared-preflight/run.sh").write_text("#!/bin/sh\necho 'fixture private runtime'\nexit " + ("2" if scenario == "shared-unavailable" else "0") + "\n")
            limit = root / "preflight/limit"
            limit.write_text('#!/bin/sh\nshift\nexec "$@"\n')
            limit.chmod(0o755)
            pidof = root / "path/pidof"
            pidof.write_text("#!/bin/sh\necho 42\n" if scenario != "pid-change" else
                             '#!/bin/sh\nif [ -f "$PREFLIGHT_TEST_PID_STATE" ]; then echo 43; else : >"$PREFLIGHT_TEST_PID_STATE"; echo 42; fi\n')
            pidof.chmod(0o755)
            real = root / "fixture-real"
            argument_check = '[ "$#" = 1 ] && [ "$1" = "$PREFLIGHT_TEST_ARGUMENT" ] || exit 9\n' if argument != "-" else ""
            real.write_text("#!/bin/sh\n" + argument_check + ("echo 'SKIP: required test'\n" if scenario == "skip" else "echo 'fixture real ran'\n"))
            real.chmod(0o755)
            control = root / "fixture-control"
            text = "#!/bin/sh\necho 'Assertion expected failed' >&2\nexit 1\n"
            if scenario == "bad-diagnostic":
                text = "#!/bin/sh\necho 'unrelated error' >&2\nexit 1\n"
            elif scenario == "escaped":
                text = "#!/bin/sh\nexit 0\n"
            elif scenario == "timeout":
                text = "#!/bin/sh\necho 'Assertion expected failed' >&2\nexit 124\n"
            control.write_text(text.replace("#!/bin/sh\n", "#!/bin/sh\n" + argument_check))
            control.chmod(0o755)
            (root / "sdk-extension-runs.tsv").write_text("fixture-real\tfixture-real\t" + argument + "\n")
            (root / "sdk-extension-controls.tsv").write_text("fixture-control\tfixture-control\tAssertion.*failed\t" + argument + "\n")
            if scenario.startswith("shared-"):
                with (root / "sdk-extension-controls.tsv").open("a") as manifest:
                    for name, diagnostic in (("payload", "heap"), ("rtti", "global")):
                        label = "ext-live-" + name + "-overread-control"
                        binary = root / label
                        code = "0" if scenario == "shared-uninstrumented" and name == "payload" else "1"
                        binary.write_text("#!/bin/sh\necho 'AddressSanitizer: " + diagnostic + "-buffer-overflow' >&2\nexit " + code + "\n")
                        binary.chmod(0o755)
                        manifest.write(label + "\t" + label + "\tAddressSanitizer: " + diagnostic + "-buffer-overflow\t-\n")
            if scenario == "missing":
                (root / "sdk-extension-controls.tsv").unlink()
            environment = os.environ.copy()
            environment["PATH"] = str(root / "path") + os.pathsep + environment.get("PATH", "")
            environment["PREFLIGHT_TEST_PID_STATE"] = str(root / "pid-state")
            environment["PREFLIGHT_TEST_ARGUMENT"] = argument
            result = subprocess.run(["sh", str(root / "run.sh")], env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, universal_newlines=True, timeout=10)
            return result

    def test_timing_arguments_are_preserved_and_unrecognized_arguments_are_rejected(self):
        for argument in ("1", "2", "clock", "budget", "elapsed", "grid", "interval"):
            result = self.run_native_fixture("good", argument)
            self.assertEqual(result.returncode, 0, (argument, result.stdout))
        for argument in ("clock;echo", "clock elapsed", "../clock"):
            result = self.run_native_fixture("good", argument)
            self.assertNotEqual(result.returncode, 0, (argument, result.stdout))

    def test_native_runner_accepts_only_real_runs_and_required_diagnostics(self):
        result = self.run_native_fixture("good")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("SDK NATIVE COMPLETION: OK (1 standalone runs, 1 diagnostic controls", result.stdout)

    def test_native_runner_rejects_wrong_diagnostic_success_timeout_and_skip(self):
        for scenario in ("bad-diagnostic", "escaped", "timeout", "skip", "missing", "pid-change"):
            result = self.run_native_fixture(scenario)
            self.assertNotEqual(result.returncode, 0, (scenario, result.stdout))
            self.assertNotIn("SDK NATIVE COMPLETION: OK", result.stdout)

    def test_native_runner_does_not_run_suites_after_unavailable_preflight(self):
        result = self.run_native_fixture("unavailable")
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("SDK native suites: NOT RUN", result.stdout)
        self.assertNotIn("fixture real ran", result.stdout)

    def test_private_shared_runtime_readiness_is_required_before_native_suites(self):
        result = self.run_native_fixture("shared-unavailable")
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("private shared sanitizer preflight exit 2", result.stdout)
        self.assertNotIn("fixture real ran", result.stdout)

    def test_private_allocator_faults_qualify_runtime_before_suites_and_count_once(self):
        result = self.run_native_fixture("shared-good")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("1 standalone runs, 3 diagnostic controls", result.stdout)
        result = self.run_native_fixture("shared-uninstrumented")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("allocator-backed sanitizer controls failed", result.stdout)
        self.assertNotIn("fixture real ran", result.stdout)

    def run_phase_fixture(self, scenario):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            shutil.copy2(str(HERE / "sdk_native_phase_run.sh"), str(root / "run.sh"))
            (root / "path").mkdir()
            for name, text in {
                    "limit": '#!/bin/sh\nshift\nexec "$@"\n',
                    "path/pidof": '#!/bin/sh\necho 42\n',
                    "first": '#!/bin/sh\n[ "$#" = 2 ] && [ "$1" = 1 ] && [ "$2" = happy ] || exit 3\n' + ('echo "SKIP: fixture"\n' if scenario == "skip" else 'echo first-ran\n') + ('exit 1\n' if scenario == "first-fails" else ''),
                    "second": '#!/bin/sh\n[ "$#" = 0 ] || exit 3\necho second-ran\n',
                    "control": '#!/bin/sh\n' + ('echo unrelated\n' if scenario == "wrong-diagnostic" else 'echo "expected fixture failure"\n') + 'exit ' + ('124' if scenario == "timeout" else '1') + '\n',
            }.items():
                target = root / name; target.write_text(text); target.chmod(0o755)
            (root / "phase-runs.tsv").write_text('first\tfirst\t1\thappy\nsecond\tsecond\t-\t-\n')
            (root / "phase-controls.tsv").write_text('control\tcontrol\texpected fixture failure\t-\t-\n')
            (root / "phase-counts").write_text('3 1\n' if scenario == "count-mismatch" else '2 1\n')
            environment = os.environ.copy()
            environment["PATH"] = str(root / "path") + os.pathsep + environment.get("PATH", "")
            return subprocess.run(["sh", str(root / "run.sh")], env=environment, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, universal_newlines=True, timeout=15)

    def test_separate_phase_runner_preserves_two_arguments_and_no_argument_exports(self):
        result = self.run_phase_fixture("good")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('2 positives, 1 exact diagnostic controls', result.stdout)

    def test_separate_phase_runner_continues_after_failed_positive(self):
        result = self.run_phase_fixture("first-fails")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn('PASS native phase: second', result.stdout)
        self.assertIn('CONTROL caught native phase: control', result.stdout)

    def test_separate_phase_rejects_skip_wrong_diagnostic_timeout_and_missing_rows(self):
        for scenario in ('skip', 'wrong-diagnostic', 'timeout', 'count-mismatch'):
            result = self.run_phase_fixture(scenario)
            self.assertEqual(result.returncode, 1, (scenario, result.stdout))
            self.assertNotIn('SDK NATIVE PHASE: OK', result.stdout)


if __name__ == "__main__":
    unittest.main()
