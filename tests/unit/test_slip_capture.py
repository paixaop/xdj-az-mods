# SPDX-License-Identifier: GPL-3.0-only
"""Actual C producer with host original-frame double; no native/Slip semantics."""
import os
import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROBE = ROOT / "src/azmod/slip_diagnostic"


class SlipCapture(unittest.TestCase):
    def run_fixture(self, modes, sanitizer=False, missing=False, actual_arm=False):
        cc = shutil.which(os.environ.get("CC", "clang"))
        if not cc:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            obj = pathlib.Path(temporary) / "capture.o"
            binary = pathlib.Path(temporary) / "fixture"
            flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fexceptions", "-fasynchronous-unwind-tables", "-I" + str(PROBE), "-I" + str(ROOT / "src/azmod")]
            if sanitizer:
                flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            producer = PROBE / "capture.c"
            absent = missing or not producer.exists()
            extra = ["-DSLIP_CAPTURE_FORWARD_ONLY"] if absent else []
            if not actual_arm:
                extra += ["-DSLIP_CAPTURE_HOST_BOUNDARY"]
            objects = []
            if not absent:
                subprocess.run([cc, "-std=gnu11", *flags, "-DSLIP_CAPTURE_TEST_CLOCK", "-c", str(producer), "-o", str(obj)], check=True, capture_output=True, text=True)
                objects.append(str(obj))
            if actual_arm:
                asm = pathlib.Path(temporary) / "entry.o"
                subprocess.run([cc, *flags, "-c", str(PROBE / "entry.S"), "-o", str(asm)], check=True, capture_output=True, text=True)
                objects.append(str(asm))
            subprocess.run([cc, "-std=gnu11", *flags, *extra, "-pthread", str(ROOT / "tests/unit/slip-capture-test.c"), *objects, str(ROOT / "tests/unit/native_unwind.c"), "-lc++" if platform.system() == "Darwin" else "-lstdc++", "-ldl", "-o", str(binary)], check=True, capture_output=True, text=True)
            for mode in modes:
                result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=10)
                self.assertIn("SLIP_NATIVE_FORWARD_BASELINE_OK", result.stdout)
                if missing or os.environ.get("SLIP_CAPTURE_EXPECT_RED") == "1":
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("missing by-value Slip capture record after native forwarding", result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, mode + ": " + result.stdout + result.stderr)
                    self.assertIn("PASS slip capture " + mode, result.stdout)

    @unittest.skipUnless(platform.system() == "Linux", "ELF program-header binding requires Linux")
    def test_probe_binding_and_worker_lifetime(self):
        cc = shutil.which(os.environ.get("HOST_CC", "clang"))
        if not cc:
            self.skipTest("Clang unavailable")
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "probe"
            built = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + str(ROOT / "src/azmod"),
                str(ROOT / "tests/unit/slip-probe.c"), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS slip probe", result.stdout)

    def test_core_startup(self):
        cc = shutil.which(os.environ.get("CC", "clang"))
        if not cc:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "startup"
            subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-pthread",
                            str(ROOT / "tests/unit/slip-startup-test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_by_value_record(self):
        self.run_fixture(["capture"])

    def test_guards_and_forwarding(self):
        self.run_fixture(["reject", "clock", "reentrant", "throws", "ring"])

    def test_missing_capture_control(self):
        self.run_fixture(["capture"], missing=True)

    def test_host_asan_ubsan(self):
        self.run_fixture(["capture", "reject", "clock", "reentrant", "throws", "ring"], sanitizer=True)

    @unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "aarch64", "actual entry0 requires AArch64 Linux")
    def test_actual_arm_entry(self):
        self.run_fixture(["arm"], actual_arm=True)


if __name__ == "__main__":
    unittest.main()
