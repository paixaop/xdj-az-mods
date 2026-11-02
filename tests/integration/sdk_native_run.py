#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run one source-verified standalone SDK packet on an emulator or real ARM deck.

Reuses the native packet runner, sanitizer preflights and exact diagnostic controls.
Only an owned /tmp directory is written. No firmware, settings, preload or service
is changed. A packet can be compiled once and tested on several native kernels.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

from sdk_runtime_check import ssh_arguments


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or "\n" in name or "\r" in name:
        raise ValueError("unsafe packet path")
    return str(path)


def validate_package(archive, root):
    files, metadata, total = {}, {}, 0
    with tarfile.open(archive, "r|gz") as packet:
        for count, member in enumerate(packet, 1):
            if count > 5000:
                raise ValueError("packet member limit")
            name = safe_name(member.name)
            if member.isdir():
                continue
            if not member.isfile() or name in files:
                raise ValueError("packet links/duplicate files are forbidden")
            total += member.size
            if member.size > 256 * 1024 * 1024 or total > 1024 * 1024 * 1024:
                raise ValueError("packet byte limit")
            stream = packet.extractfile(member)
            h = hashlib.sha256()
            saved = bytearray()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                h.update(block)
                if name in ("run.sh", "used-inputs.json", "SHA256SUMS"):
                    saved.extend(block)
                    if len(saved) > 1024 * 1024:
                        raise ValueError("packet metadata byte limit")
            files[name] = h.hexdigest()
            if saved:
                metadata[name] = bytes(saved)
    if not all(name in metadata for name in ("run.sh", "used-inputs.json", "SHA256SUMS")):
        raise ValueError("required packet metadata missing")
    if files["run.sh"] != digest(root / "tests/unit/sdkdata_deck_run.sh"):
        raise ValueError("native runner changed; rebuild packet")
    checksums = {}
    for line in metadata["SHA256SUMS"].decode().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            raise ValueError("invalid packet checksum manifest")
        name = safe_name(match[2])
        if name in checksums:
            raise ValueError("duplicate packet checksum")
        checksums[name] = match[1]
    if checksums != {name: sha for name, sha in files.items() if name != "SHA256SUMS"}:
        raise ValueError("packet checksum mismatch")
    used = json.loads(metadata["used-inputs.json"])["sha256"]
    if not used:
        raise ValueError("empty source closure")
    for name, sha in used.items():
        path = root / safe_name(name)
        if root.resolve() not in path.resolve().parents or not re.fullmatch(r"[0-9a-f]{64}", sha):
            raise ValueError("invalid source closure")
        if not path.is_file() or digest(path) != sha:
            raise ValueError("compiled source changed: " + name)
    return {"archive_sha256": digest(archive), "files": len(files), "source_inputs": len(used)}


def execute(archive, root, host, port, key, known, output, timeout=1800, preflight_timeout=15):
    if type(preflight_timeout) is not int or not 1 <= preflight_timeout <= 300:
        raise ValueError("sanitizer preflight timeout must be an integer in 1..300 seconds")
    identity = validate_package(archive, root)
    ssh = ssh_arguments(host, port, key, known)
    if output.exists():
        raise ValueError("output directory must be new")
    output.mkdir(parents=True)
    report = {"scope": "standalone native-kernel tests; no EP147 feature/control acceptance", "packet": identity,
              "host": host, "port": port, "preflight_timeout": preflight_timeout, "status": "INCOMPLETE"}
    remote, completed, collected = None, False, False

    def small(command):
        result = subprocess.run(ssh + [command], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        if result.returncode or len(result.stdout) > 4096:
            raise RuntimeError("remote setup/read failed")
        return result.stdout.decode().strip()

    try:
        report["architecture"] = small("uname -m")
        if report["architecture"] != "aarch64":
            raise RuntimeError("native ARM64 kernel required")
        remote = small("umask 077; mktemp -d /tmp/az-sdk-native.XXXXXX")
        if not re.fullmatch(r"/tmp/az-sdk-native\.[A-Za-z0-9]+", remote):
            remote = None
            raise RuntimeError("unexpected remote scratch path")
        report["remote_directory"] = remote
        with archive.open("rb") as payload:
            result = subprocess.run(ssh + ["tar -xzf - -C " + remote], stdin=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
            if result.returncode:
                raise RuntimeError("packet upload failed")
        with (output / "results.log").open("wb") as log:
            result = subprocess.run(ssh + ["cd " + remote + " && sha256sum -c SHA256SUMS && AZ_SAN_PREFLIGHT_TIMEOUT=" + str(preflight_timeout) + " sh ./run.sh"],
                                    stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        completed = True
        report["exit_code"] = result.returncode
        text = (output / "results.log").read_text(errors="replace")
        marker = re.search(r"^SDK NATIVE COMPLETION: OK \([1-9][0-9]* standalone runs, [1-9][0-9]* diagnostic controls;", text, re.M)
        report["status"] = "PASS" if result.returncode == 0 and marker else "INCOMPLETE" if result.returncode == 2 else "FAIL"
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    finally:
        if remote:
            try:
                with (output / "native-logs.tar.gz").open("wb") as log:
                    result = subprocess.run(ssh + ["tar -czf - -C " + remote + " logs"], stdout=log, stderr=subprocess.PIPE, timeout=60)
                collected = result.returncode == 0
                if not collected:
                    if report["status"] != "FAIL":
                        report["status"] = "INCOMPLETE"
                    report["collection_error"] = "remote logs retained for inspection"
            except subprocess.TimeoutExpired:
                if report["status"] != "FAIL":
                    report["status"] = "INCOMPLETE"
                report["collection_error"] = "log collection timed out"
            if completed and collected:
                try:
                    small("rm -rf " + remote)
                    report["scratch_removed"] = True
                except (RuntimeError, subprocess.TimeoutExpired):
                    report["scratch_removed"] = False
            else:
                report["scratch_removed"] = False
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("host")
    parser.add_argument("key", type=Path)
    parser.add_argument("known_hosts", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--timeout", type=int, choices=range(1, 7201), default=1800)
    parser.add_argument("--preflight-timeout", type=int, choices=range(1, 301), default=15)
    args = parser.parse_args()
    if not args.key.is_file() or not args.known_hosts.is_file():
        parser.error("pinned known-hosts file and SSH key are required")
    report = execute(args.archive, Path(__file__).resolve().parents[2], args.host, args.port, args.key,
                     args.known_hosts, args.output, args.timeout, args.preflight_timeout)
    print(json.dumps(report, indent=2))
    return {"PASS": 0, "FAIL": 1, "INCOMPLETE": 2}[report["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
