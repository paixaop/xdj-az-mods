#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only SDK transcript validation and bounded remote runtime health checks.

Does not install code, inject controls, edit USB data or restart any service.
PASS describes only the selected check, never full firmware/SDK acceptance.
"""
from sdk_timestamps import compare_timestamps
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import time


U64 = (1 << 64) - 1


def number(row, key, positive=False):
    value = row[key]
    if not re.fullmatch(r"[0-9]+", value):
        raise ValueError("invalid " + key)
    value = int(value)
    if value > U64 or (positive and not value):
        raise ValueError("invalid " + key)
    return value


def validate_transcript(source, required_decks=()):
    errors, streams, ownership, coverage = [], {}, {}, {str(d + 1): [] for d in range(4)}
    if len(source.encode()) > 262144:
        return {"status": "FAIL", "errors": ["probe log exceeds byte cap"], "coverage": coverage}
    for index, line in enumerate(source.splitlines(), 1):
        if not line.startswith(("deck=", "live_deck=", "load_deck=")):
            continue
        try:
            fields = [part.split("=", 1) for part in line.split()]
            if any(len(part) != 2 for part in fields) or len({part[0] for part in fields}) != len(fields):
                raise ValueError("malformed/duplicate field")
            row = dict(fields)
            kind = next(key for key in ("deck", "live_deck", "load_deck") if key in row)
            deck = number(row, kind)
            if deck >= 4:
                raise ValueError("deck outside supported range")
            if kind == "load_deck" and row.get("availability") == "unavailable":
                continue
            unit, received = number(row, "unit", True), number(row, "received_ns", True)
            revision = number(row, "load_generation" if kind == "load_deck" else "revision", True)
            stream = row.get("stream", "loop" if kind == "deck" else "load")
            if (stream, unit) in ownership and ownership[stream, unit] != deck:
                raise ValueError("unit generation shared by different decks")
            ownership[stream, unit] = deck
            key = (deck, stream)
            if key in streams:
                old_unit, old_revision, old_received = streams[key]
                if unit < old_unit or (unit == old_unit and (revision <= old_revision or compare_timestamps(received, old_received) < 0)):
                    raise ValueError("nonmonotonic stream publication")
            streams[key] = unit, revision, received
            if stream not in coverage[str(deck + 1)]:
                coverage[str(deck + 1)].append(stream)
            if "age_ns" in row:
                number(row, "age_ns")
            if kind == "live_deck":
                valid = number(row, "valid")
                if valid > 1 or stream not in ("play", "quantize", "key_lock"):
                    raise ValueError("invalid live stream")
                values = ("mode_is_2", "play_pause", "play_aux") if stream == "play" else (stream,)
                if valid and any(row[name] not in ("0", "1") for name in values):
                    raise ValueError("invalid raw value marked valid")
            elif kind == "deck":
                engaged, bounds = number(row, "engaged"), number(row, "bounds")
                start, end = float(row["in_s"]), float(row["out_s"])
                if engaged > 1 or bounds > 1 or not all(map(math.isfinite, (start, end))):
                    raise ValueError("invalid loop values")
                rate = number(row, "check_rate", True) if "check_rate" in row else 1_000_000_000
                if bounds and (not engaged or compare_timestamps(start, 0., units_per_second=1, sample_rate=rate) < 0 or compare_timestamps(end, start, units_per_second=1, sample_rate=rate) <= 0):
                    raise ValueError("invalid engaged loop range")
                if not bounds and (start or end):
                    raise ValueError("unavailable bounds retain old coordinates")
                reason = row.get("diagnostic_reason")
                if (bounds and reason not in (None, "0", "-1")) or (not bounds and reason == "0"):
                    raise ValueError("public bounds contradict diagnostic")
                if "check_rate" in row:
                    rate = number(row, "check_rate", True)
                    samples = [number(row, "check_" + p + "_samples") for p in ("in", "out")]
                    cached = [number(row, "check_" + p + "_cached_us") for p in ("in", "out")]
                    if rate >= 0x7fffffff or any(n > 2**53 for n in samples + cached) or compare_timestamps(samples[1], samples[0], units_per_second=rate, sample_rate=rate) <= 0:
                        raise ValueError("invalid native coordinate domain")
                    consistent = all(compare_timestamps(2*n*1000000, 2*u*rate, units_per_second=1, sample_rate=1, window_samples=2000000+rate+1) == 0 for n, u in zip(samples, cached))
                    if bounds:
                        if not consistent:
                            raise ValueError("published inconsistent source-clock range")
                        for value, n in zip((start, end), samples):
                            expected = n / rate
                            if compare_timestamps(value, expected, units_per_second=1, sample_rate=rate) != 0:
                                raise ValueError("coordinates are not sample-derived")
                    elif engaged and row.get("diagnostic_reason") == "12" and consistent:
                        raise ValueError("SDK rejected quantization-consistent coordinates")
        except (KeyError, ValueError, OverflowError) as error:
            errors.append("line %d: %s" % (index, error))
    missing = [d for d in required_decks if "loop" not in coverage[str(d)]]
    return {"status": "FAIL" if errors else "INCOMPLETE" if missing or not streams else "PASS",
            "errors": errors, "missing_loop_decks": missing, "coverage": coverage,
            "scope": "recorded scalar/stream invariants; no button latency, commands or source binding acceptance"}


def ssh_arguments(host, port, key, known):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.:-]*", host) or type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("invalid SSH endpoint")
    return ["ssh", "-i", str(key), "-p", str(port), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
            "-o", "LogLevel=ERROR", "-o", "StrictHostKeyChecking=yes", "-o", "UserKnownHostsFile=" + str(known),
            "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=2", "root@" + host]


def validate_health(samples):
    errors = []
    if not samples or (not any(s.get("pid") for s in samples) and
                       not any(s.get("service", {}).get("ActiveState") == "failed" for s in samples)):
        return {"status": "INCOMPLETE", "errors": ["EP147 is not running"], "scope": "process/service health only"}
    first = samples[0]
    previous = None
    for s in samples:
        if not re.fullmatch(r"[1-9][0-9]*", s.get("pid", "")) or s.get("pid") != first.get("pid"):
            errors.append("EP147 process changed or is ambiguous")
        if s["service"].get("NRestarts") != first["service"].get("NRestarts"):
            errors.append("automatic service restart")
        if not re.fullmatch(r"[0-9]+", s["service"].get("NRestarts", "")):
            errors.append("missing/invalid service restart counter")
        tick = s.get("local_monotonic")
        if type(tick) not in (int, float) or not math.isfinite(tick) or compare_timestamps(tick, 0., units_per_second=1) < 0 or (previous is not None and compare_timestamps(tick, previous, units_per_second=1) <= 0):
            errors.append("invalid/backward health clock")
        else:
            previous = tick
        if s["service"].get("ActiveState") != "active" or s["service"].get("SubState") != "running":
            errors.append("service is not running")
    if not errors and len(samples) < 2:
        errors.append("two health readings are required")
        status = "INCOMPLETE"
    else:
        status = "FAIL" if errors else "PASS"
    return {"status": status, "errors": errors, "samples": len(samples),
            "scope": "process/service health only; no playback/control acceptance"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action")
    log = sub.add_parser("log")
    log.add_argument("path", type=Path)
    log.add_argument("--required-decks", nargs="*", type=int, choices=(1, 2, 3, 4), default=(1, 2, 3, 4))
    health = sub.add_parser("health")
    health.add_argument("host")
    health.add_argument("key", type=Path)
    health.add_argument("known_hosts", type=Path)
    health.add_argument("--port", type=int, default=22)
    health.add_argument("--seconds", type=int, choices=range(1, 3601), default=60)
    for p in (log, health):
        p.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not args.action:
        parser.error("a check action is required")
    if args.output.exists():
        parser.error("output must be new")
    args.output.mkdir(parents=True)
    if args.action == "log":
        report = validate_transcript(args.path.read_text(), args.required_decks)
    else:
        ssh = ssh_arguments(args.host, args.port, args.key, args.known_hosts)
        samples = []
        deadline = time.monotonic() + args.seconds
        try:
            while True:
                result = subprocess.run(ssh + ["pidof EP147 || true; systemctl show EP147.service -p NRestarts -p ActiveState -p SubState"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=20)
                if result.returncode:
                    raise RuntimeError("SSH health read failed: " + result.stderr[:300])
                lines = result.stdout.splitlines()
                pid = next((line for line in lines if "=" not in line), "")
                service = dict(line.split("=", 1) for line in lines if "=" in line)
                samples.append({"pid": pid, "service": service, "local_monotonic": time.monotonic()})
                if compare_timestamps(time.monotonic(), deadline, units_per_second=1) >= 0:
                    break
                time.sleep(min(1, max(0, deadline - time.monotonic())))
            report = validate_health(samples)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            report = validate_health(samples)
            if report["status"] != "FAIL":
                report["status"] = "INCOMPLETE"
            report["errors"].append(str(error))
            report["scope"] = "remote health; transport unavailable"
        (args.output / "samples.json").write_text(json.dumps(samples, indent=2) + "\n")
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return {"PASS": 0, "FAIL": 1, "INCOMPLETE": 2}[report["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
