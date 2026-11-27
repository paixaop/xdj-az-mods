#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Isolated regressions for the SDK review's size, freshness and read-error tests."""
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:])


def variant(name, filename, old, new, count=1):
    directory = output / name
    directory.mkdir(parents=True, exist_ok=True)
    text = (source / filename).read_text()
    assert text.count(old) == count, (filename, old)
    (directory / filename).write_text(text.replace(old, new))
    if filename != "track_storage.c":
        (directory / "track_storage.c").write_bytes((source / "track_storage.c").read_bytes())
    if filename != "track_service.c":
        (directory / "track_service.c").write_bytes((source / "track_service.c").read_bytes())
    companion = "sdk_data_engine.h" if filename == "sdk_analysis.h" else "sdk_track.c"
    if companion != filename:
        (directory / companion).write_bytes((source / companion).read_bytes())


variant("timing-size", "sdk_analysis.h", "st_record_size(s) < sizeof *s || ", "", 3)
variant("timing-version", "sdk_analysis.h", "s->version != AZ_DATA_VERSION", "0", 3)
variant("timing-prefix-read", "sdk_analysis.h",
        "memcpy(&size, record, sizeof size);", "size = ((const az_track_snapshot *) record)->size;")
variant("database-alias", "track_service.c",
        'if (!st_source_database(&src, data->files[0])) return AZ_STALE;',
        'if (snprintf(data->files[0], sizeof data->files[0], "%s/%s/rekordbox/exportLibrary.db", src.mount, st_source_export(&src)) >= (int) sizeof data->files[0]) return AZ_FAILED;')
variant("missing-parent", "track_storage.c",
        "if (realpath(prefix, parent)) break;\n        if (errno != ENOENT) return 0;",
        "if (realpath(prefix, parent)) break;\n        return 0;")
variant("wave-errors", "sdk_wave_provider.h",
        "if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? AZ_UNSUPPORTED : AZ_FAILED;",
        "if (fd < 0) return AZ_UNSUPPORTED;")
variant("database-uri", "sdk_lib.c",
        "if (*p == '%' || *p == '?' || *p == '#')", "if (0)")
variant("analysis-binding", "track_service.c",
        "if (strcmp(current, data->files[i])) return 0;", "(void) current;")
variant("track-fifo", "track_storage.c",
        "O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);\n    if (fd < 0) return NULL;",
        "O_RDONLY | O_CLOEXEC | O_NOFOLLOW);\n    if (fd < 0) return NULL;")
variant("wave-fifo", "sdk_wave_provider.h", "O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK",
        "O_RDONLY | O_CLOEXEC | O_NOFOLLOW")
variant("analysis-path-limit", "track_service.c",
        "(mask & ~AZ_DATA_METADATA)) rc = AZ_LIMIT;", "((void) mask, 0)) rc = AZ_LIMIT;")
variant("preview-extension", "track_service.c",
        "file_encoding == AZ_WAVE_PWV6_PREVIEW", "0")
