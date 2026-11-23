#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""compdb.py: compile_commands.json for the static check, built from the real lines of guest/Makefile.

Reads `make -n -B azmod azstem azhandoff CC=gcc` (run in guest/) on stdin and writes one entry
per C translation unit to stdout. Each entry keeps the line's preprocessor and code-generation
flags (-I, -D, -O, -f, -std=) and drops what only the link or the warning policy needs (-W, -w, -o,
-shared, -static*, -l, -Wl): the analyzers report their own findings, and a compiler warning is
already an error in the real build. Vendored stb_vorbis and analysis dependencies
have no entry. Recursive make directories are rebased onto the source directory,
so both analyzers use the same paths for sources and local include directories.

  compdb.py <guest-dir> < make-n-output > compile_commands.json
"""
import json
import os
from pathlib import Path
import re
import shlex
import sys

SKIP_SOURCES = {"azstem/stb_vorbis.c"}  # vendored, kept as upstream wrote it
MAKE_DIRECTORY = re.compile(r"^make(?:\[\d+\])?: (Entering|Leaving) directory [`'](.*)'$")


def logical_lines(lines):
    pending = ""
    for line in lines:
        line = line.rstrip("\n")
        if line.endswith("\\"):
            pending += line[:-1] + " "
            continue
        yield pending + line
        pending = ""
    if pending:
        raise ValueError("unterminated make command continuation")


def rebase(path, directory, guest):
    return os.path.relpath((directory / path).resolve(), guest)


def compilation_entries(guest, lines):
    guest = Path(guest).resolve()
    directory = guest
    parents = []
    entries = []
    for line in logical_lines(lines):
        change = MAKE_DIRECTORY.match(line)
        if change:
            location = Path(change.group(2)).resolve()
            if change.group(1) == "Entering":
                parents.append(directory)
                directory = location
            else:
                if location != directory or not parents:
                    raise ValueError("unbalanced make directory: " + line)
                directory = parents.pop()
            continue
        toks = shlex.split(line)
        if not toks or toks[0] != "gcc":
            continue
        flags, srcs, skip_next = [], [], False
        for t in toks[1:]:
            if skip_next:
                skip_next = False
                continue
            if t == "-o":
                skip_next = True
            elif t.endswith(".c"):
                srcs.append(rebase(t, directory, guest))
            elif t.startswith("-I"):
                flags.append(t if os.path.isabs(t[2:]) else "-I" + rebase(t[2:], directory, guest))
            elif t.startswith(("-D", "-O", "-f", "-std=")):
                flags.append(t)
        for source in srcs:
            if source in SKIP_SOURCES or source.startswith("../vendor/analysis/"):
                continue
            entries.append({"directory": str(guest), "file": source,
                            "arguments": ["gcc"] + flags + ["-c", source]})
    if parents:
        raise ValueError("unterminated make directory")
    return entries


def main():
    entries = compilation_entries(sys.argv[1], sys.stdin)
    if not entries:
        sys.exit("compdb.py: no gcc compile lines in the input")
    json.dump(entries, sys.stdout, indent=1)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
