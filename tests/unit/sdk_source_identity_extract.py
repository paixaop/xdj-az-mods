#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Extract actual source publication and its exact tuple/exhaustion controls."""
from pathlib import Path
import sys

source = Path(__file__).resolve().parents[2] / "src/azmod/sdk_track.c"
s = source.read_text()
a = s.index("struct st_source {")
b = s.index("\n};", a) + 3
c = s.index("static int st_source_read(")
d = s.index("\n}", c) + 2
body = ("static pthread_mutex_t g_st_source_mu = PTHREAD_MUTEX_INITIALIZER;\n"
        "static uint64_t g_st_generation;\n" + s[a:b] + "\n"
        "static struct st_source g_st_sources[2];\n" + s[c:d] + "\n")
root = Path(sys.argv[1])
(root / "source-identity.inc").write_text(body)
for name, old, new in [
    ("source-tuple", "s->device != (uint64_t) sb.st_dev || s->inode != (uint64_t) sb.st_ino", "s->ref.library_id != lib"),
    ("source-wrap", "if (g_st_generation == UINT64_MAX)", "if (0)"),
]:
    assert body.count(old) == 1
    (root / (name + ".inc")).write_text(body.replace(old, new))
