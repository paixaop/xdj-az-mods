# SPDX-License-Identifier: GPL-3.0-only
"""Exercise clean install, replacement and refusal with a synthetic model."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("provision", ROOT / "tools/analysis/provision-assets.py")
PROVISION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROVISION)


class ProvisioningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        payload = bytes(PROVISION.PAYLOAD_SIZE)
        checksum = 2166136261
        for byte in payload:
            checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
        cls.synthetic = struct.pack("<8s6I", b"AZPVDI01", len(payload), checksum, 0, 0, 0, 0) + payload

    def test_clean_install_and_upgrade_without_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "local-assets"
            mods = root / "MODS"
            source.mkdir(); mods.mkdir()
            (source / PROVISION.MODEL).write_bytes(self.synthetic)
            target = PROVISION.provision(source, mods)
            self.assertEqual((target / PROVISION.MODEL).read_bytes(), self.synthetic)
            self.assertTrue((target / "THIRD-PARTY-NOTICES.txt").is_file())
            self.assertFalse((mods / "community-mods").exists())
            (target / PROVISION.MODEL).write_bytes(b"old incomplete copy")
            PROVISION.provision(source, mods)
            self.assertEqual((target / PROVISION.MODEL).read_bytes(), self.synthetic)
            self.assertFalse(list(target.glob(".analysis-*")))

    def test_invalid_source_does_not_replace_valid_model(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "local-assets"; source.mkdir()
            mods = root / "MODS"; mods.mkdir()
            model = source / PROVISION.MODEL
            model.write_bytes(self.synthetic)
            target = PROVISION.provision(source, mods)
            for bad in (b"short", b"wrongone" + self.synthetic[8:], self.synthetic[:-1] + b"x"):
                model.write_bytes(bad)
                with self.assertRaises(ValueError):
                    PROVISION.provision(source, mods)
                self.assertEqual((target / PROVISION.MODEL).read_bytes(), self.synthetic)


if __name__ == "__main__":
    unittest.main()
