#!/usr/bin/env python3
"""Extract unchanged loader lifecycle definitions for the real runtime fixture."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).resolve()
lines = source.read_text().splitlines(keepends=True)
for name in ("cm_retire_analysis", "cm_run_init", "cm_live_stoppable", "cm_wait_ms",
             "cm_stop_drained", "cm_drain_threads", "cm_live_off"):
    matches = [i for i, line in enumerate(lines)
               if re.match(r"static (?:int|void) " + name + r"\(", line)]
    if not matches and name == "cm_retire_analysis":
        continue  # Historical wiring control predates the shared helper.
    if len(matches) != 1:
        raise SystemExit(f"Expected one definition of {name}")
    start = matches[0]
    # All these existing definitions end with a column-zero closing brace.
    end = next(i for i in range(start, len(lines)) if lines[i] == "}\n")
    print(f'#line {start + 1} "{source}"')
    print("".join(lines[start:end + 1]), end="\n")
