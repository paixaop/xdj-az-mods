#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Keep native motion decisions on the shared timestamp comparison contract."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class NativeTimestampTests(unittest.TestCase):
    def test_wrap_and_beat_targets_use_the_shared_comparator(self):
        source = (ROOT / "src/azstem/sdk_exit.cpp").read_text()
        action = source.split("static unsigned status_action(Slot &slot", 1)[1].split("\n#endif", 1)[0]
        for endpoint in ("in", "out"):
            self.assertRegex(
                action,
                r"compare_timestamps\(\s*from\.sample\s*,\s*v\.points\." + endpoint + r"\s*,",
            )
        self.assertRegex(action, r"compare_timestamps\(\s*scope->beat_sample\s*,\s*to\.sample\s*,")
        self.assertNotRegex(action, r"(?:from\.sample|scope->beat_sample)\s*(?:[<>]=?|[!=]=)")

    def test_stationary_and_memory_decisions_avoid_raw_time_comparisons(self):
        motion = (ROOT / "src/azstem/sdk_status_motion.inc").read_text()
        self.assertNotRegex(motion, r"head\.sample\s*[!=]=\s*m\.previous")
        self.assertNotRegex(motion, r"word\(requested\s*\+\s*0x18\)\s*[!=]=\s*to\.sample")
        self.assertNotRegex(motion, r"!head\.sample\b")
        memory = (ROOT / "src/azstem/sdk_status_memory_recall.inc").read_text()
        self.assertNotRegex(memory, r"end_s\s*[<>]=?\s*start_s")
        self.assertRegex(memory, r"compare_timestamps\(\s*times\.out_sample\s*,\s*times\.in_sample\s*,")


if __name__ == "__main__":
    unittest.main()
