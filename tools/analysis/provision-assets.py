#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Provision the core's private analysis model on a locally mounted mods USB.

Run explicitly for a clean install or an upgrade from Track Analysis. The model
is never included in a core release or SDK client. No deck network access occurs.
"""
import argparse
import os
from pathlib import Path
import struct
import tempfile

MODEL = "pvdi-model.bin"
PAYLOAD_SIZE = 298 * 8 + 1669314 * 4
LICENSES = Path(__file__).resolve().parents[2] / "assets" / "analysis"


def validated_model(path):
    with path.open("rb") as source:
        data = source.read(PAYLOAD_SIZE + 33)
    if len(data) != PAYLOAD_SIZE + 32:
        raise ValueError("model length does not match the supported format")
    magic, length, expected, *reserved = struct.unpack_from("<8s6I", data)
    if magic != b"AZPVDI01" or length != PAYLOAD_SIZE or any(reserved):
        raise ValueError("unsupported model format")
    checksum = 2166136261
    for byte in memoryview(data)[32:]:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    if checksum != expected:
        raise ValueError("model checksum mismatch")
    return data


def atomic_write(path, data):
    fd, temporary = tempfile.mkstemp(prefix=".analysis-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as target:
            target.write(data)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def provision(source_assets, mods_directory):
    data = validated_model(source_assets / MODEL)
    if not mods_directory.is_dir():
        raise ValueError("mods directory must already exist on the mounted USB")
    destination = mods_directory / "analysis" / "assets"
    destination.mkdir(parents=True, exist_ok=True)
    for license_file in sorted(LICENSES.glob("*.txt")):
        atomic_write(destination / license_file.name, license_file.read_bytes())
    atomic_write(destination / MODEL, data)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--from-assets", required=True, type=Path,
                        help="local assets directory containing the imported private model")
    parser.add_argument("--mods-dir", required=True, type=Path,
                        help="existing MODS directory on the locally mounted USB")
    args = parser.parse_args()
    try:
        provision(args.from_assets, args.mods_dir)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Analysis provisioning failed: {error}\n")
    print("Core analysis assets provisioned. No community mod is required.")


if __name__ == "__main__":
    main()
