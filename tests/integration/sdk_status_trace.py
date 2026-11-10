#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check public status-audit callbacks captured during a single firmware exercise."""
import argparse
from pathlib import Path
from sdk_timestamps import AFTER, EQUAL, compare_timestamps


def cuts(lines, deck, complete=False):
    records = {}
    last_event = 0
    for line in lines:
        words = line.split()
        if not words:
            continue
        kind = "event" if words[0].startswith("d=") else words[0]
        fields = dict(w.split("=", 1) for w in words if "=" in w)
        if fields.get("d") != str(deck) or "seq" not in fields:
            continue
        sequence = int(fields["seq"])
        if kind == "event":
            require(sequence > last_event, f"deck sequence did not advance at {sequence}")
            last_event = sequence
        record = records.setdefault(sequence, {})
        if kind in record and kind not in ("memory-row", "grid-limits"):
            raise ValueError(f"duplicate {kind} at sequence {sequence}")
        record[kind] = fields
    if not records:
        raise ValueError("the trace contains no callbacks for this deck")
    if complete or any("end" in row for row in records.values()):
        labels = ("load", "metadata", "key", "cues", "loop", "grid", "phrases", "transport",
                  "position", "tempo", "sync", "settings", "phase", "stems", "overview")
        for sequence, row in records.items():
            require("event" in row and "end" in row, f"incomplete callback at sequence {sequence}")
            changed = int(row["event"]["changed"], 16)
            for topic, label in enumerate(labels):
                require(not changed & (1 << topic) or label in row,
                        f"missing {label} payload at sequence {sequence}")
            for label in ("occurrence", "phrase"):
                require(row["end"].get(label) in ("0", "1"), f"invalid {label} marker at {sequence}")
                require((row["end"][label] == "1") == (label in row),
                        f"incomplete {label} payload at sequence {sequence}")
            for label in ("loop-reference-row", "loop-target-row"):
                if label in row:
                    cue_comment(row, label, sequence)
    return records


def require(condition, message):
    if not condition:
        raise ValueError(message)


def cue_comment(row, label, sequence):
    comment = row.get(label + "-comment", {})
    encoded = comment.get("hex", "")
    require(comment.get("bytes") == "128" and len(encoded) == 256 and
            all(character in "0123456789abcdef" for character in encoded),
            f"missing, truncated or malformed {label} comment at {sequence}")
    return encoded


def timestamp_order(first, second, units, rate, window):
    try:
        return compare_timestamps(first, second, units_per_second=units,
                                  sample_rate=rate, window_samples=window)
    except ValueError as error:
        raise ValueError(f"invalid timestamp or source clock: {error}") from error


def continuity(records, topics):
    require(topics > 0 and topics < 1 << 16, "topics must select defined deck-status bits")
    for sequence, row in records.items():
        event = row.get("event")
        require(event is not None, f"missing event header at {sequence}")
        lost = int(event["lost"], 16) & topics
        require(not lost, f"selected topic history lost at {sequence}, mask {lost:#x}")
    return [min(records), max(records)]


def coordinate(text):
    samples, rate = map(int, text.split("/"))
    require(rate > 0, "coordinate source rate is unavailable")
    require(timestamp_order(samples, 0, rate, rate, 1) in (EQUAL, AFTER), "negative audible coordinate")
    return samples, rate


def coordinate_order(text, expected, window):
    samples, rate = coordinate(text)
    return timestamp_order(samples, expected, rate, rate, window)


def opening(records, lead, window, origin=None):
    starts = [(n, r) for n, r in records.items()
              if r.get("phrase", {}).get("from") == "1/0"]
    require(len(starts) == 1, f"expected one departure from START, found {len(starts)}")
    n, row = starts[0]
    require(row["phrase"]["to"] == "2/0" and row["phrase"]["kind"] == "0" and
            row["event"]["kind"] == "2" and int(row["event"]["lost"], 16) == 0,
            f"START did not enter the first item by uninterrupted traversal at {n}")
    epoch = row["event"]["epoch"]
    heads = [(k, r["position"]["head"]) for k, r in records.items()
             if k < n and "position" in r and int(r["position"].get("valid", "0"), 16) & 1 and
             r.get("event", {}).get("epoch") == epoch]
    # The initial source adoption is an integer setter commit. An ordinary
    # auto-cue case supplies its independently observed nonzero source point.
    expected, expected_rate = coordinate(origin) if origin else (0, None)
    if origin:
        require(timestamp_order(expected, 0, expected_rate, expected_rate, 1) == AFTER,
                "cue opening requires a positive measured source coordinate")
    zeros = [k for k, head in heads
             if (expected_rate is None or coordinate(head)[1] == expected_rate) and
             coordinate_order(head, expected, 1) == EQUAL]
    require(zeros, "missing measured opening before START" if origin else
            "missing measured sample-zero opening before START")
    if lead:
        require(window is not None, "lead-in requires the observed native --window-samples")
        require(any(k > zeros[-1] and coordinate_order(head, 0, window) == AFTER for k, head in heads),
                "missing positive measured lead-in before the first phrase")
    return [zeros[-1], n]


def natural_end(records, departure, target):
    transitions = [(n, r) for n, r in records.items()
                   if r.get("phrase", {}).get("to") == "3/0"]
    require(len(transitions) == 1, f"expected one transition to END, found {len(transitions)}")
    n, row = transitions[0]
    phrase, event = row["phrase"], row.get("event", {})
    require(phrase["from"].split("/")[0] == ("2" if departure == "item" else "4"),
            f"unexpected END departure at {n}")
    require(phrase["kind"] == "0" and event.get("kind") == "2" and int(event["lost"], 16) == 0,
            f"END was not an uninterrupted traversal at {n}")
    expected, expected_rate = coordinate(target)
    # The probe prints fields when their topic changes. Unchanged values in
    # this immutable cut retain the last complete publication for this load.
    view = {}
    epoch = event["epoch"]
    for k, previous in records.items():
        if k > n:
            break
        if previous.get("event", {}).get("epoch") != epoch:
            view.clear()
            continue
        for topic in ("position", "transport"):
            if topic in previous:
                view[topic] = previous[topic]
    position, transport = view.get("position", {}), view.get("transport", {})
    actual, rate = coordinate(position.get("head", "0/0"))
    # The stock EOF range clamp commits one exact integer source coordinate.
    require(transport.get("playback") == "5" and
            int(transport.get("valid", "0"), 16) & 1 and
            int(position.get("valid", "0"), 16) & 1 and rate == expected_rate and
            timestamp_order(actual, expected, rate, rate, 1) == EQUAL,
            f"ENDED and measured head {target} must belong to the same cut {n}")
    steps = [(k, r["phrase"]) for k, r in records.items() if k < n and "phrase" in r]
    if departure == "gap":
        require(steps and steps[-1][1]["to"] == "4/0", "missing entry into the preceding unlabelled tail")
    else:
        require(not steps or steps[-1][1]["to"] == phrase["from"], "terminal section differs from prior landing")
    begin = steps[-1][0] if steps else min(records)
    for k, r in records.items():
        if begin < k < n and "event" in r:
            require(not int(r["event"]["lost"], 16) & (1 << 15), f"phrase continuity lost at {k}")
    return [n]


def same_bounds(loop, bounds, working, historical_rate):
    names = ("win", "wout") if working else ("in", "out")
    for name, (seconds, rate) in zip(names, bounds):
        actual_rate = int(loop.get(name + "_rate", historical_rate or 0))
        require(actual_rate > 0, "loop source rate is unavailable; supply verified --rate-hz for an older trace")
        if actual_rate != rate or timestamp_order(float(loop[name]), seconds, 1, rate, 1) != EQUAL:
            return False
    return True


def reloop(records, historical_rate):
    before = ready = exited = None
    revision = bounds = None
    for n, row in records.items():
        source, loop = row.get("loop-source", {}), row.get("loop", {})
        if source.get("availability") != "2" or not loop:
            continue
        if ready is None:
            if source["reloop"] == "0":
                before = n
            elif before is not None and loop["engaged"] == "1":
                ready, revision = n, source["revision"]
                bounds = [(float(loop[name]), int(loop.get(name + "_rate", historical_rate or 0)))
                          for name in ("in", "out")]
            continue
        require(source["reloop"] == "1" and source["revision"] == revision and
                same_bounds(loop, bounds, False, historical_rate), f"retained eligibility or bounds changed at {n}")
        if loop["engaged"] == "0":
            require(int(loop["valid"], 16) & 0x33 == 3, f"EXIT retained working points at {n}")
            exited = n
        elif exited is not None:
            require(same_bounds(loop, bounds, True, historical_rate), f"re-entry did not restore bounds at {n}")
            return [before, ready, exited, n]
    raise ValueError("missing false readiness, acquisition, EXIT or re-entry")


def cue_link(records, field, entry_id, bounds=None):
    for n, row in records.items():
        info = row.get("loop-source", {})
        if (info.get("cue_field") == str(field) and info.get("cue_id") == str(entry_id) and
                int(info.get("cue_revision", 0)) > 0 and row.get("loop", {}).get("engaged") == "1"):
            provenance = "1" if field in (1, 2) else "2"
            target = row.get("loop-target", {})
            require(info.get("cue_avail") == "2" and info.get("cue_p") == provenance and
                    int(info.get("cue_provider", 0)) > 0 and int(info.get("cue_epoch", 0)) > 0 and
                    info["cue_epoch"] == row["event"]["epoch"], f"invalid loop association source at {n}")
            require(target.get("availability") == "2" and target.get("scope") == "2" and
                    target.get("matches") == "1" and int(target.get("count", 0)) > 0,
                    f"missing unique READY load-scoped target at {n}")
            for source, name in (("cue_field", "field"), ("cue_epoch", "epoch"), ("cue_provider", "provider"),
                                 ("cue_revision", "revision"), ("cue_p", "provenance")):
                require(info[source] == target.get(name), f"loop target {name} mismatch at {n}")
            reference, canonical = row.get("loop-reference-row", {}), row.get("loop-target-row", {})
            require(reference.get("type") == "2" and int(reference.get("valid", "0"), 16) & 3 == 3,
                    f"loop reference is not a complete loop at {n}")
            for name in ("size", "version", "valid", "classification", "id", "slot", "type", "rgb",
                         "saved", "modified", "auto", "color_id"):
                require(name in reference and reference[name] == canonical.get(name),
                        f"loop canonical row {name} mismatch at {n}")
            require(cue_comment(row, "loop-reference-row", n) == cue_comment(row, "loop-target-row", n),
                    f"loop canonical row comment mismatch at {n}")
            require(reference["id"] == str(entry_id), f"wrong canonical loop ID at {n}")
            require(reference["classification"] == ("1" if field in (1, 3) else "2"),
                    f"wrong canonical cue classification at {n}")
            for index, name in enumerate(("in", "out")):
                # FILE rows own a millisecond clock. Native row coordinates
                # share the proven working endpoint's actual source rate.
                rate = 1000 if provenance == "2" else int(row["loop"].get(("win_rate", "wout_rate")[index], 0))
                actual = float(reference[name])
                require(timestamp_order(actual, float(canonical[name]), 1, rate, 1) == EQUAL,
                        f"loop canonical row {name} changed at {n}")
                if bounds is not None:
                    require(timestamp_order(actual, bounds[index], 1, rate, 1) == EQUAL,
                            f"loop {name} differs from independent fixture at {n}")
            return [n]
    raise ValueError("no engaged loop referred to the specified cue collection and entry")


def maps(records):
    for n, row in records.items():
        if not all(row.get(f"phrase-{name}-map", {}).get("availability") == "2"
                   for name in ("current", "saved")):
            continue
        for field, name, provenance in ((0, "current", 1), (1, "saved", 2)):
            mapping = row[f"phrase-{name}-map"]
            require(mapping["field"] == str(field) and mapping["grid_avail"] == "2" and
                    mapping["provenance"] == str(provenance), f"wrong {name} grid source at {n}")
            for key in ("epoch", "provider", "revision", "provenance"):
                target = "grid_p" if key == "provenance" else "grid_" + key
                require(int(mapping[key]) > 0 and mapping[key] == mapping[target],
                        f"{name} grid {key} does not identify the published grid at {n}")
            require(mapping["epoch"] == row["event"]["epoch"], f"stale {name} grid load at {n}")
        return [n]
    raise ValueError("no callback carried qualified current and saved phrase mappings")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--deck", type=int, choices=range(4), required=True)
    parser.add_argument("--complete", action="store_true", help="require complete callback end markers")
    cases = parser.add_subparsers(dest="case", required=True)
    intact = cases.add_parser("continuity")
    intact.add_argument("--topics", type=lambda value: int(value, 0), default=(1 << 16) - 1,
                        help="selected topic mask, default all v1 topics")
    start = cases.add_parser("start")
    start.add_argument("opening", choices=("zero", "lead-in", "cue"))
    start.add_argument("--coordinate", help="independently measured samples/rate for a nonzero cue opening")
    start.add_argument("--window-samples", type=int, help="actual native observation window for lead-in measurements")
    end = cases.add_parser("end")
    end.add_argument("departure", choices=("item", "gap"))
    end.add_argument("coordinate", help="expected terminal samples/rate, e.g. 9086364/44100")
    loop = cases.add_parser("reloop")
    loop.add_argument("--rate-hz", type=int, help="verified loop source rate for historical traces without rate fields")
    cue = cases.add_parser("cue-link")
    cue.add_argument("field", type=int, choices=(1, 2, 3, 4))
    cue.add_argument("entry_id", type=int)
    cue.add_argument("--bounds", nargs=2, type=float, metavar=("IN_SECONDS", "OUT_SECONDS"),
                     help="independently verified FILE/native fixture bounds")
    cases.add_parser("maps", help="require native current and FILE saved phrase/grid identities")
    args = parser.parse_args()
    try:
        records = cuts(args.log.read_text().splitlines(), args.deck, args.complete)
        if args.case == "continuity":
            seq = continuity(records, args.topics)
        elif args.case == "start":
            require((args.opening == "cue") == (args.coordinate is not None),
                    "--coordinate is required only for a cue opening")
            seq = opening(records, args.opening == "lead-in", args.window_samples, args.coordinate)
        elif args.case == "end":
            seq = natural_end(records, args.departure, args.coordinate)
        elif args.case == "reloop":
            seq = reloop(records, args.rate_hz)
        elif args.case == "maps":
            seq = maps(records)
        else:
            seq = cue_link(records, args.field, args.entry_id, args.bounds)
    except (KeyError, ValueError) as error:
        parser.exit(1, f"FAIL: {error}\n")
    print(f"PASS {args.case}, deck {args.deck}, sequences {', '.join(map(str, seq))}")


if __name__ == "__main__":
    main()
