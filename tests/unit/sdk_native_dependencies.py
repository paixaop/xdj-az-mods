#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Record the exact GNU-preprocessed inputs of an immutable native SDK build."""
import argparse
import concurrent.futures
import hashlib
import json
import pathlib
import shlex
import subprocess


OPAQUE_INPUTS = [
    "src/azmod/sdk_deck.c", "src/azmod/sdk_track.c", "src/azmod/playlist_editor.c",
    "src/azstem/sdk_export.c", "src/azstem/deck_track_map.c",
    "src/azstem/stem_playback.c", "src/azstem/azstem.c", "tests/integration/sdk-loop-probe.c",
    "src/azstem/sdk_native_cpp.map",
    "tests/unit/sdk_observation_extract.py", "tests/unit/sdk_review_controls.py",
    "tests/unit/sdk_live_extract.py", "tests/unit/sdkdata_deck_run.sh",
    "tests/unit/sdk_source_identity_extract.py",
    "tests/unit/sdk_loop_controls.py", "src/azmod/sdk_input.c",
    "tests/unit/sanitizer_preflight.c", "tests/unit/sanitizer_preflight_timeout.c",
    "tests/unit/sanitizer_preflight_run.sh", "tests/sanitizer-preflight.sh",
] + ["tests/sdk-" + family + ".sh" for family in ("command", "stored", "live", "sources", "native-commands", "time")]


def preprocess_command(command):
    result = [command[0]]
    index = 1
    while index < len(command):
        value = command[index]
        index += 1
        if value == "-o":
            if index == len(command):
                raise ValueError("missing compiler output argument")
            index += 1
        elif value in ("-c", "-fsyntax-only") or value.startswith(("-l", "-Wl,")) or value.endswith(".a"):
            continue
        else:
            result.append(value)
    # GNU7 treats -MF '-' as a literal filename; -MM without -MF writes stdout.
    return result + ["-MM", "-MT", "SDK_NATIVE_INPUTS"]


def parse_dependencies(output):
    result = set()
    for line in output.replace("\\\n", " ").splitlines():
        if not line.strip():
            continue
        if not line.startswith("SDK_NATIVE_INPUTS:"):
            raise ValueError("unexpected compiler dependency output: " + line)
        result.update(shlex.split(line.split(":", 1)[1]))
    if not result:
        raise ValueError("empty compiler dependency closure")
    return result


def foundation_commands(root, work):
    flags = ["gcc", "-std=gnu11", "-O1", "-g", "-pthread", "-I" + str(work),
             "-I" + str(root / "src/azmod"), "-I" + str(root / "tests/unit")]
    result = []
    for name, fixture, extra in [
        ("data", "sdkdata_test.c", []), ("waveform", "sdkwaveform_test.c", []),
        ("source-identity", "sdksourceidentity_test.c", ['-DSOURCE_IDENTITY_INC="' + str(work / "source-identity.inc") + '"']),
        ("provider", "sdktrack_test.c", ['-DLIB_INC="sdk_lib.c"']),
        ("real", "sdkrealanlz_probe.c", []), ("range", "sdkwaveprovider_probe.c", []),
        ("deck", "sdkdeckdata_test.c", ['-DDECK_DATA_INC="' + str(work / "deck.inc") + '"']),
        ("observation", "sdkobservation_test.c", ['-DOBS_INC="' + str(work / "observation.inc") + '"']),
        ("loop-export", "sdkloopexport_test.c", ['-DLOOP_EXPORT_INC="' + str(work / "loop-export.inc") + '"']),
        ("sanitizer-control", "sdkdata_sanitizer_control.c", []),
    ]:
        result.append(("foundation-" + name, flags + extra + [str(root / "tests/unit" / fixture)]))
    return result


def record(root, work, original_manifest, extension_manifest, destination, foundation, jobs):
    original = json.loads(original_manifest.read_text())["sha256"]
    commands = [(item["label"], item["argv"]) for item in json.loads(extension_manifest.read_text())["compile_commands"]]
    if foundation:
        commands += foundation_commands(root, work)
    inputs = set(OPAQUE_INPUTS)
    generated = {}
    def one(item):
        label, command = item
        result = subprocess.run(preprocess_command(command), cwd=str(root), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, universal_newlines=True)
        if result.returncode:
            raise RuntimeError("dependency preprocessing failed for " + label + ": " + result.stderr)
        return label, parse_dependencies(result.stdout)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for label, dependencies in pool.map(one, commands):
            for dependency in dependencies:
                path = pathlib.Path(dependency)
                if not path.is_absolute():
                    path = root / path
                path = path.resolve()
                if root in path.parents:
                    inputs.add(str(path.relative_to(root)))
                elif work in path.parents:
                    generated[str(path.relative_to(work))] = hashlib.sha256(path.read_bytes()).hexdigest()
                else:
                    raise ValueError("project dependency escaped frozen source/work directories: " + str(path))
    missing = inputs - set(original)
    if missing:
        raise ValueError("compiled inputs absent from original snapshot manifest: " + ", ".join(sorted(missing)))
    # The snapshot itself must still match its original byte hashes.
    for name in inputs:
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != original[name]:
            raise ValueError("immutable compiled snapshot changed: " + name)
    destination.write_text(json.dumps({
        "scope": "exact GNU preprocessing closure plus mutation/extraction/runtime inputs; excludes unused context",
        "source_manifest_sha256": hashlib.sha256(original_manifest.read_bytes()).hexdigest(),
        "preprocessed_actions": len(commands), "sha256": {name: original[name] for name in sorted(inputs)},
        "generated_sha256": generated}, indent=2, sort_keys=True) + "\n")
    print("NATIVE INPUT CLOSURE: %d verified inputs, %d compiler actions" % (len(inputs), len(commands)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "work", "original_manifest", "extension_manifest", "destination"):
        parser.add_argument(name, type=pathlib.Path)
    parser.add_argument("--foundation", action="store_true")
    parser.add_argument("--jobs", type=int, default=2)
    arguments = parser.parse_args()
    if not 1 <= arguments.jobs <= 8:
        parser.error("jobs must be between 1 and 8")
    record(arguments.root.resolve(), arguments.work.resolve(), arguments.original_manifest,
           arguments.extension_manifest, arguments.destination, arguments.foundation, arguments.jobs)


if __name__ == "__main__":
    main()
