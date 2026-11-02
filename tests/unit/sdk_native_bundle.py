#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Bind native standalone artifacts to a consistent source snapshot and SHA256 list."""
import argparse
import hashlib
import json
import pathlib
import shutil
import sys
import tarfile


def sha256(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def source_files(root):
    files = []
    for directory in ("src", "tests/unit", "tests/integration"):
        files.extend(path for path in (root / directory).rglob("*")
                     if path.is_file() and "__pycache__" not in path.parts)
    files.extend((root / "tests").glob("sdk-*.sh"))
    files.append(root / "tests/sanitizer-preflight.sh")
    return sorted(set(files))


def source_hashes(root):
    return {str(path.relative_to(root)): sha256(path) for path in source_files(root)}


def verify(root, manifest):
    expected = json.loads(manifest.read_text())["sha256"]
    # Newly added, unrelated files cannot change this frozen snapshot's inputs.
    # Changed/deleted inputs still fail, including any newly added include directive.
    actual = {name: sha256(root / name) if (root / name).is_file() else None for name in expected}
    if expected != actual:
        changed = sorted(name for name in set(expected) | set(actual)
                         if expected.get(name) != actual.get(name))
        raise ValueError("source changed during native preparation: " + ", ".join(changed))


def snapshot(root, destination, manifest):
    if destination.exists() or manifest.exists():
        raise ValueError("snapshot directory and manifest must be new")
    before = source_hashes(root)
    destination.mkdir()
    for name in before:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(root / name), str(target))
    after, copied = source_hashes(root), source_hashes(destination)
    if before != after or before != copied:
        changed = sorted(name for name in set(before) | set(after) | set(copied)
                         if before.get(name) != after.get(name) or before.get(name) != copied.get(name))
        raise ValueError("source changed while taking native snapshot; snapshot rejected: " + ", ".join(changed))
    manifest.write_text(json.dumps({"scope": "standalone SDK suites; no firmware or deployment",
                                    "sha256": before}, indent=2, sort_keys=True) + "\n")


def checksums(directory):
    target = directory / "SHA256SUMS"
    if target.exists():
        raise ValueError("SHA256SUMS must be new")
    files = sorted(path for path in directory.rglob("*") if path.is_file())
    if any(path.is_symlink() for path in directory.rglob("*")):
        raise ValueError("native bundle must not contain symlinks")
    lines = []
    for path in files:
        name = str(path.relative_to(directory))
        if "\n" in name or "\r" in name or "\\" in name:
            raise ValueError("unrepresentable checksum filename")
        lines.append(sha256(path) + "  " + name + "\n")
    if not lines:
        raise ValueError("empty native bundle")
    target.write_text("".join(lines))


def archive(directory, destination):
    """Portable file-only tar: native macOS tar adds unmanifested AppleDouble data."""
    if destination.exists() or directory.resolve() in destination.resolve().parents:
        raise ValueError("archive destination must be new and outside its inputs")
    paths = sorted(directory.rglob("*"))
    if directory.is_symlink() or any(path.is_symlink() for path in paths):
        raise ValueError("archive inputs must not contain symlinks")
    files = [path for path in paths if path.is_file()]
    if not files:
        raise ValueError("archive inputs are empty")
    with tarfile.open(str(destination), "w:gz", format=tarfile.PAX_FORMAT) as packet:
        for path in files:
            packet.add(str(path), arcname=str(path.relative_to(directory)), recursive=False)


def context_drift(root, original, required, destination):
    verify(root, required)
    expected = json.loads(original.read_text())["sha256"]
    used = json.loads(required.read_text())["sha256"]
    actual = source_hashes(root)
    changed = sorted(name for name in expected if expected[name] != actual.get(name))
    if set(changed) & set(used):
        raise ValueError("compiled inputs changed during context comparison")
    destination.write_text(json.dumps({"unused_context_changed": changed,
        "unused_context_sha256": {name: {"snapshot": expected[name], "current": actual.get(name)} for name in changed},
        "added_unused_context": sorted(set(actual) - set(expected)),
        "compiled_input_guard": "passed; every required byte hash matched"}, indent=2) + "\n")
    print("NATIVE CONTEXT: %d unused changes recorded; compiled-input guard passed" % len(changed))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command")
    make = commands.add_parser("snapshot")
    make.add_argument("root", type=pathlib.Path)
    make.add_argument("destination", type=pathlib.Path)
    make.add_argument("manifest", type=pathlib.Path)
    check = commands.add_parser("verify")
    check.add_argument("root", type=pathlib.Path)
    check.add_argument("manifest", type=pathlib.Path)
    sums = commands.add_parser("checksums")
    sums.add_argument("directory", type=pathlib.Path)
    pack = commands.add_parser("archive")
    pack.add_argument("directory", type=pathlib.Path)
    pack.add_argument("destination", type=pathlib.Path)
    drift = commands.add_parser("context-drift")
    for name in ("root", "original", "required", "destination"):
        drift.add_argument(name, type=pathlib.Path)
    arguments = parser.parse_args()
    try:
        if arguments.command == "snapshot":
            snapshot(arguments.root, arguments.destination, arguments.manifest)
        elif arguments.command == "verify":
            verify(arguments.root, arguments.manifest)
        elif arguments.command == "checksums":
            checksums(arguments.directory)
        elif arguments.command == "archive":
            archive(arguments.directory, arguments.destination)
        elif arguments.command == "context-drift":
            context_drift(arguments.root, arguments.original, arguments.required, arguments.destination)
        else:
            parser.error("a command is required")
    except (ValueError, OSError, KeyError) as error:
        print("FAIL native bundle: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
