#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Separate untouched analysis dependency findings from first-party diagnostics."""
from pathlib import Path
import re
import sys


def partition(lines, vendor):
    vendor = Path(vendor).resolve()
    owned, external = [], []
    for line in lines:
        match = re.match(r"([^:]+):[0-9]+:", line)
        outside = False
        if match:
            try:
                Path(match.group(1)).resolve().relative_to(vendor)
                outside = True
            except ValueError:
                pass
        (external if outside else owned).append(line)
    return owned, external


if __name__ == "__main__":
    source = Path(sys.argv[1])
    owned, external = partition(source.read_text().splitlines(True), sys.argv[2])
    source.write_text("".join(owned))
    Path(str(source) + ".vendor").write_text("".join(external))
