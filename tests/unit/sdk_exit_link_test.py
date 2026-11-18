#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Production visibility and caller authentication, without loading firmware."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ExitPrivateLinkTests(unittest.TestCase):
    def test_endpoint_is_exported_and_only_handed_to_core_module(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="az-exit-link-") as temporary:
            folder = Path(temporary)
            stub = folder / "load.c"
            stub.write_text('#include "azloadobserver.h"\n'
                            'int azsdk_load_read(int deck, struct azsdk_load_sample *out) { (void)deck; (void)out; return 0; }\n')
            library = folder / "exit.so"
            shared = ["-O1", "-fPIC", "-fvisibility=hidden", "-pthread", "-fexceptions", "-fasynchronous-unwind-tables",
                      "-I" + str(root / "src/common"), "-I" + str(root / "src/azmod")]
            sources = [root / "src/azstem/sdk_exit.c", root / "src/azstem/sdk_native_commands.c"]
            for name in ("actor", "store", "lifecycle", "persist", "recall", "commands", "observer"):
                source = root / ("src/azstem/sdk_cue_" + name + ".c")
                sources.append(source)
            sources += [stub, root / "src/common/aznative_guard.c"]
            objects = []
            for index, source in enumerate(sources):
                obj = folder / (str(index) + ".o")
                compiler = os.environ.get("HOST_CC", "cc")
                command = [compiler, "-std=gnu11"] + shared
                subprocess.run(command + ["-c", str(source), "-o", str(obj)], check=True, capture_output=True, text=True)
                objects.append(str(obj))
            command = [os.environ.get("HOST_CC", "cc"), "-shared", "-pthread"] + objects + ["-lstdc++", "-ldl", "-lm", "-o", str(library)]
            if sys.platform.startswith("linux"):
                command.append("-ldl")
            subprocess.run(command, check=True, capture_output=True, text=True)
            bridge = folder / "core.c"
            bridge.write_text('#include <stdint.h>\n'
                              'void azmod_trackid_path(void) {}\n'
                              'void *obtain(void *(*get)(void), uint32_t *version) {\n'
                              '  uint32_t *api = get(); if (api) *version = api[1]; return api;\n'
                              '}\n')
            core_path = folder / "core.so"
            subprocess.run([os.environ.get("HOST_CC", "cc"), "-O1", "-fPIC", "-shared", str(bridge),
                            "-o", str(core_path)], check=True, capture_output=True, text=True)
            # Run the loader assertions in a child so no test-library mappings
            # or synthetic canonical symbols survive into another test.
            code = r'''
import ctypes, sys
library = ctypes.CDLL(sys.argv[1])
get = library.azstem_exit_service_v1
get.restype = ctypes.c_void_p
assert get() is None
core = ctypes.CDLL(sys.argv[2], mode=ctypes.RTLD_GLOBAL)
assert get() is None
core.obtain.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
core.obtain.restype = ctypes.c_void_p
version = ctypes.c_uint32()
assert core.obtain(ctypes.cast(get, ctypes.c_void_p), ctypes.byref(version))
assert version.value == 1
assert get() is None
'''
            result = subprocess.run([sys.executable, "-c", code, str(library), str(core_path)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
