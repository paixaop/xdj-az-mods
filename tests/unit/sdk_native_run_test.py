#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/integration"))
spec = importlib.util.spec_from_file_location("sdk_native_run", ROOT / "tests/integration/sdk_native_run.py")
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


class NativeTransportTests(unittest.TestCase):
    def fixture(self, folder, changes=None, extra=None):
        root = folder / "source"
        (root / "tests/unit").mkdir(parents=True)
        (root / "src").mkdir()
        (root / "tests/unit/sdkdata_deck_run.sh").write_bytes(b"trusted runner\n")
        (root / "src/test.c").write_bytes(b"int test;\n")
        sha = native.digest(root / "src/test.c")
        files = {"run.sh": b"trusted runner\n", "used-inputs.json": json.dumps({"sha256": {"src/test.c": sha}}).encode(),
                 "fixture": b"standalone binary bytes"}
        checks = "".join(hashlib.sha256(value).hexdigest() + "  " + name + "\n" for name, value in sorted(files.items()))
        files["SHA256SUMS"] = checks.encode()
        files.update(changes or {})
        archive = folder / "packet.tar.gz"
        with tarfile.open(archive, "w:gz") as packet:
            for name, value in files.items():
                info = tarfile.TarInfo(name); info.size = len(value)
                packet.addfile(info, io.BytesIO(value))
            if extra:
                info = tarfile.TarInfo(extra)
                info.type = tarfile.SYMTYPE; info.linkname = "/etc/passwd"
                packet.addfile(info)
        return archive, root

    def test_packet_is_bound_to_current_source_runner_and_all_file_checksums(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive, root = self.fixture(Path(temporary))
            self.assertEqual(native.validate_package(archive, root)["source_inputs"], 1)
            (root / "src/test.c").write_text("changed")
            with self.assertRaisesRegex(ValueError, "compiled source changed"):
                native.validate_package(archive, root)

    def test_packet_refuses_corrupt_runner_payload_links_and_paths(self):
        for changes, extra in (({"run.sh": b"other runner"}, None), ({"fixture": b"corruption"}, None),
                               ({}, "link"), ({}, "../escape"), ({}, "/absolute")):
            with tempfile.TemporaryDirectory() as temporary:
                archive, root = self.fixture(Path(temporary), changes, extra)
                with self.assertRaises(ValueError):
                    native.validate_package(archive, root)

    def run_case(self, scenario, **options):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            archive, root = self.fixture(folder)
            commands = []

            def remote(argv, **kwargs):
                command = argv[-1]; commands.append(command)
                if command == "uname -m":
                    return SimpleNamespace(returncode=0, stdout=b"x86_64\n" if scenario == "architecture" else b"aarch64\n")
                if command.startswith("umask"):
                    return SimpleNamespace(returncode=0, stdout=b"/unsafe\n" if scenario == "unsafe" else b"/tmp/az-sdk-native.fixture\n")
                if command.startswith("cd "):
                    if scenario == "timeout":
                        raise subprocess.TimeoutExpired(argv, 1)
                    text = b"SDK NATIVE COMPLETION: OK (2 standalone runs, 1 diagnostic controls; foundation=0)\n"
                    if scenario == "missing-marker": text = b"unrelated successful output\n"
                    kwargs["stdout"].write(text)
                    return SimpleNamespace(returncode=124 if scenario == "preflight-timeout" else 2 if scenario == "preflight" else 1 if scenario in ("failure", "failure-collection") else 0)
                if command.startswith("tar -czf"):
                    kwargs["stdout"].write(b"collected logs")
                    return SimpleNamespace(returncode=1 if scenario in ("collection", "failure-collection") else 0, stderr=b"")
                return SimpleNamespace(returncode=0, stdout=b"", stderr=b"")

            with patch.object(native.subprocess, "run", side_effect=remote):
                report = native.execute(archive, root, "127.0.0.1", 2230, Path("/key"), Path("/known"), folder / "output", 1, **options)
            return report, commands

    def test_preflight_bound_is_validated_before_packet_or_ssh_access(self):
        for bound in [0,301,-1,True,False,None,'120',1.0]:
            with self.subTest(bound=bound),patch.object(native,'validate_package') as validate,patch.object(native.subprocess,'run') as remote:
                with self.assertRaises(ValueError):
                    native.execute(Path('/missing'),Path('/source'),'127.0.0.1',2230,Path('/key'),Path('/known'),Path('/output'),preflight_timeout=bound)
                validate.assert_not_called();remote.assert_not_called()

    def test_preflight_bound_is_recorded_and_passed_as_strict_numeric_remote_env(self):
        for options,bound in [({},15),({'preflight_timeout':1},1),({'preflight_timeout':120},120),({'preflight_timeout':300},300)]:
            report,commands=self.run_case('good',**options)
            self.assertEqual(report['preflight_timeout'],bound)
            self.assertTrue(any('&& AZ_SAN_PREFLIGHT_TIMEOUT=%d sh ./run.sh'%bound in c for c in commands))

    def test_larger_bound_cannot_turn_timeout_into_pass(self):
        report,_=self.run_case('preflight-timeout',preflight_timeout=120)
        self.assertEqual(report['status'],'FAIL')
        self.assertEqual(report['exit_code'],124)
        self.assertEqual(report['preflight_timeout'],120)

    def test_success_uses_only_owned_scratch_and_collects_before_cleanup(self):
        report, commands = self.run_case("good")
        self.assertEqual(report["status"], "PASS")
        self.assertTrue(report["scratch_removed"])
        self.assertLess(next(i for i, c in enumerate(commands) if c.startswith("tar -czf")),
                        next(i for i, c in enumerate(commands) if c.startswith("rm -rf")))
        self.assertFalse(any("systemctl" in c or "LD_PRELOAD=" in c or "/media/usb" in c for c in commands))

    def test_timeout_and_collection_failure_preserve_scratch_and_are_incomplete(self):
        for scenario in ("timeout", "collection"):
            report, commands = self.run_case(scenario)
            self.assertEqual(report["status"], "INCOMPLETE")
            self.assertFalse(report["scratch_removed"])
            self.assertFalse(any(c.startswith("rm -rf") for c in commands))

    def test_preflight_failure_and_missing_success_marker_never_pass(self):
        for scenario, expected in (("preflight", "INCOMPLETE"), ("failure", "FAIL"), ("missing-marker", "FAIL")):
            report, _ = self.run_case(scenario)
            self.assertEqual(report["status"], expected)

    def test_log_collection_failure_cannot_erase_observed_test_failure(self):
        report, commands = self.run_case("failure-collection")
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["scratch_removed"])
        self.assertFalse(any(c.startswith("rm -rf") for c in commands))

    def test_wrong_architecture_and_untrusted_scratch_do_not_upload(self):
        for scenario in ("architecture", "unsafe"):
            report, commands = self.run_case(scenario)
            self.assertEqual(report["status"], "INCOMPLETE")
            self.assertFalse(any(c.startswith("tar") or c.startswith("rm") for c in commands))


if __name__ == "__main__":
    unittest.main()
