#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Restore the superseded exact-cache policy on one deck at a time."""
from pathlib import Path
import sys


def generate(source_root, destination):
    source = (source_root / "src/azstem/sdk_loop_observer.cpp").read_text()
    source = source.replace('../common/azloopobserver.h', 'azloopobserver.h')
    anchor = 'Bounds bounds = working_loop_bounds(notification, engaged);'
    if source.count(anchor) != 1:
        raise ValueError("loop callback changed; review deck-isolation controls")
    for deck in range(4):
        regression = anchor + '''
        if (deck == SLOT && bounds.valid &&
            (std::round((1000000.0 / bounds.check_rate) * bounds.check_in_samples) != bounds.check_in_cached_us ||
             std::round((1000000.0 / bounds.check_rate) * bounds.check_out_samples) != bounds.check_out_cached_us)) {
            bounds.valid = false;
            bounds.in_s = bounds.out_s = 0;
            bounds.reason = 12;
        }'''.replace('SLOT', str(deck))
        (destination / ('loop-strict-slot-' + str(deck) + '.cpp')).write_text(source.replace(anchor, regression))


if __name__ == '__main__':
    generate(Path(__file__).resolve().parents[2], Path(sys.argv[1]))
