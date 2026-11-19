#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise release compilation paths, including the recursive analysis archive."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from compdb import compilation_entries
from diagnostics import partition


class CompilationDatabaseTests(unittest.TestCase):
    def test_only_canonical_vendor_paths_leave_the_first_party_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            vendor = root / "vendor/analysis"
            vendor.mkdir(parents=True)
            (root / "alias").symlink_to(vendor, target_is_directory=True)
            owned = [str(root / "src/analysis/kernel.c") + ":12:3: warning: vendor/analysis appears in message\n",
                     str(root / "vendor/analysis-extra/header.h") + ":14: warning: retained\n",
                     "unrecognized diagnostic must remain a failure\n"]
            external = [str(vendor / "decoder.h") + ":8:5: warning: vendor header\n",
                        str(root / "alias/audio.h") + ":9: warning: alias of vendor header\n"]
            actual, dependencies = partition(owned + external, vendor)
            self.assertEqual(actual, owned)
            self.assertEqual(dependencies, external)

    def test_recursive_archive_keeps_real_sources_and_include_context(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            guest = root / "guest"
            (guest / "analysis").mkdir(parents=True)
            (guest / "common").mkdir()
            (root / "src").symlink_to("guest", target_is_directory=True)
            (guest / "common/config.h").write_text("#define BUILD_VALUE 7\n")
            (guest / "analysis/kernel.c").write_text(
                '#include "config.h"\n_Static_assert(BUILD_VALUE == 7, "include context");\n')
            (guest / "host.c").write_text("int host(void) { return 0; }\n")
            lines = [
                "make: Entering directory '{}'".format(guest),
                "gcc -Icommon -O2 -Werror -c host.c -o host.o",
                "make[1]: Entering directory '{}'".format(root),
                "gcc -Isrc/common -I/usr/include -O3 -fexceptions \\",
                "  -c src/analysis/kernel.c -o kernel.o",
                "gcc -Ivendor/analysis/dsp -c vendor/analysis/dsp/vendor.c -o vendor.o",
                "make[1]: Leaving directory '{}'".format(root),
                "gcc -Icommon -c azstem/stb_vorbis.c -o vorbis.o",
                "make: Leaving directory '{}'".format(guest),
            ]
            entries = compilation_entries(guest, lines)
            self.assertEqual([e["file"] for e in entries], ["host.c", "analysis/kernel.c"])
            self.assertEqual(entries[1]["directory"], str(guest))
            self.assertIn("-Icommon", entries[1]["arguments"])
            self.assertIn("-I/usr/include", entries[1]["arguments"])
            self.assertIn("-fexceptions", entries[1]["arguments"])
            compiler = shutil.which("cc")
            self.assertIsNotNone(compiler, "a host compiler is required")
            for entry in entries:
                subprocess.run([compiler] + [a for a in entry["arguments"][1:] if a != "-c"] + ["-fsyntax-only"],
                               cwd=entry["directory"], check=True)

    def test_simple_make_output_retains_macros_and_filters_link_flags(self):
        entries = compilation_entries("/tmp/core/src", [
            'gcc -Iazmod -DNAME="two words" -O2 -fPIC -shared -Wl,--export-dynamic '
            'azmod/azmod.c -o azmod.so -lpthread'])
        self.assertEqual(entries[0]["arguments"], [
            "gcc", "-Iazmod", "-DNAME=two words", "-O2", "-fPIC", "-c", "azmod/azmod.c"])

    def test_unbalanced_directory_output_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unbalanced"):
            compilation_entries("/tmp/core/src", ["make[1]: Leaving directory '/tmp/core'"])
        with self.assertRaisesRegex(ValueError, "unterminated make directory"):
            compilation_entries("/tmp/core/src", ["make[1]: Entering directory '/tmp/core'"])
        with self.assertRaisesRegex(ValueError, "continuation"):
            compilation_entries("/tmp/core/src", ["gcc -O2 \\"])


if __name__ == "__main__":
    unittest.main()
