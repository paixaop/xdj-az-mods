#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Extract actual source publication and its exact tuple/exhaustion controls."""
from pathlib import Path
import sys

source = Path(__file__).resolve().parents[2] / "src/azmod/track_storage.c"
s = source.read_text()
header = source.with_suffix(".h").read_text()
a = header.index("struct st_source {")
b = header.index("\n};", a) + 3
functions = []
for name in ("st_source_probe", "st_source_assign", "st_source_read"):
    signature = "static int " if name == "st_source_probe" else "int "
    start = s.index(signature + name + "(")
    end = s.index("\n}", start) + 2
    functions.append(s[start:end])
body = ("static pthread_mutex_t g_st_source_mu = PTHREAD_MUTEX_INITIALIZER;\n"
        "static uint64_t g_st_generation;\n" + header[a:b] + "\n"
        "static struct st_source g_st_sources[2];\n" + "\n".join(functions) + "\n")
root = Path(sys.argv[1])
(root / "source-identity.inc").write_text(body)
for name, old, new in [
    ("source-tuple", "s->device != (uint64_t) sb.st_dev || s->inode != (uint64_t) sb.st_ino", "s->ref.library_id != lib"),
    ("source-wrap", "if (*generation == UINT64_MAX)", "if (0)"),
]:
    assert body.count(old) == 1
    (root / (name + ".inc")).write_text(body.replace(old, new))
