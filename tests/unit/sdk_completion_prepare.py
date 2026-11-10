#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compile the standalone SDK extensions for a native ARM Linux kernel.

Uses each reviewed suite's exact mutation generators. It never executes the
compiled binaries or contacts a deck. Native execution is delegated to run.sh.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys


ASSERTION = r"Assertion.*failed|assertion.*failed"
BASE_FAMILIES = ("command", "stored", "live", "sources", "native-commands")
FAMILIES = BASE_FAMILIES + ("time",)
TIME_CPP = ("strict", "fixed", "boundary", "narrow")
TIME_INPUT = ("input-clock", "input-budget", "input-elapsed")
TIME_ENGINE = ("clock", "grid", "interval")
LOOP_DECK = tuple("loop-strict-slot-" + str(deck) for deck in range(4))
COMMAND = "owner arguments id-replay load revision persistence deadline cancel-claim deck-barrier timeout-barrier completion-load completion-revision completion-order premature-completion lease terminal result-size".split()
WRAPPERS = "wrapper-submit-owner wrapper-poll-owner wrapper-cancel-owner wrapper-release-owner wrapper-arguments wrapper-poll-state wrapper-cancel-state wrapper-release-state".split()
STORED = "short-cue cue-header active-status active-identity memory-color preview-clock preview-stride preview-header preview-validity preview-capacity phrase-version phrase-unknown preview-request".split()
MONO_STORED = "mono-count mono-word mono-clock".split()
LIVE_C = "live-export-size live-export-version sync-export-size sync-export-version live-deck-prefix live-deck-clock live-deck-valid live-deck-normalized live-deck-master-tempo-valid live-deck-master-tempo-source live-deck-sync-valid live-deck-sync-source live-lifetime-register live-lifetime-retire live-config-required live-probe-clock live-probe-age live-probe-bound".split()
LIVE_CPP = "player generation slot boolean invalid-retention allocation-rollback sync-slot sync-mode sync-legacy-leak".split()
LIVE_FLAGS = ["observer-loop-race", "observer-live-race"]
SOURCES = "worker-marker worker-present worker-affinity raw-owner-reuse expired-owner candidate-result forgotten-ambiguity physical-profile fdinfo-bound descriptor-recheck native-fd-dup-close vfs-short-name rtti-short-name".split()
NATIVE_COMMANDS = "capability owner-thread thread-reuse foreign-release generation retirement-wait reentrant-close predicate-release exhaustion".split()


def generators(script):
    """Select mutation heredocs with a task directory; skip runtime helper code."""
    lines = script.read_text().splitlines(True)
    result = []
    index = 0
    while index < len(lines):
        match = re.match(r"\s*python3 - (.*?) <<'(PY[A-Z]*)'\s*$", lines[index])
        index += 1
        if not match:
            continue
        body = []
        while index < len(lines) and lines[index].strip() != match.group(2):
            body.append(lines[index])
            index += 1
        if index == len(lines):
            raise ValueError("unterminated Python generator in " + str(script))
        index += 1
        arguments = shlex.split(match.group(1))
        if "$task_tmp" in arguments:
            result.append((arguments, "".join(body)))
    return result


def generate(root, work):
    expected = {"command": 1, "stored": 1, "live": 3, "sources": 2, "native-commands": 1, "time": 1}
    evidence = {}
    for family in FAMILIES:
        directory = work / family
        directory.mkdir()
        blocks = generators(root / ("tests/sdk-" + family + ".sh"))
        if len(blocks) != expected[family]:
            raise ValueError("mutation generator count changed for " + family + "; review native preparation")
        evidence[family] = []
        for index, (arguments, body) in enumerate(blocks):
            expanded = []
            for value in arguments:
                if value == "$task_tmp":
                    value = str(directory)
                elif value.startswith("$PWD/"):
                    value = str(root / value[5:])
                elif "$" in value or "`" in value:
                    raise ValueError("unsupported generator argument: " + value)
                expanded.append(value)
            source = directory / ("generator-" + str(index) + ".py")
            source.write_text(body)
            subprocess.run([sys.executable, "-"] + expanded, input=body,
                           universal_newlines=True, cwd=str(root), check=True)
            evidence[family].append(hashlib.sha256(body.encode()).hexdigest())
    subprocess.run([sys.executable, str(root / "tests/unit/sdk_live_extract.py"), str(work / "live")],
                   cwd=str(root), check=True)
    subprocess.run([sys.executable, str(root / "tests/unit/sdk_loop_controls.py"), str(work / "live")],
                   cwd=str(root), check=True)
    inventories = {
        "command": ({name for name in COMMAND + WRAPPERS + ["request-growth"]}, None),
        "stored": (set(STORED + MONO_STORED), None),
        "live": (None, set(LIVE_CPP + list(LOOP_DECK) + ["payload-overread", "rtti-overread", "loop-rtti-overread", "allocator-nothrow-bypass"])),
        "sources": (None, set(SOURCES + ["sample-boundary", "constructor-order", "bridge", "bridge-control"])),
        "native-commands": (None, set(NATIVE_COMMANDS)),
        "time": (set(TIME_ENGINE) | {"common"}, None),
    }
    for family, (directories, cpp_files) in inventories.items():
        target = work / family
        if directories is not None and {path.name for path in target.iterdir() if path.is_dir()} != directories:
            raise ValueError("unreviewed native mutation directory inventory: " + family)
        if cpp_files is not None and {path.stem for path in target.glob("*.cpp")} != cpp_files:
            raise ValueError("unreviewed native mutation source inventory: " + family)
    if {path.stem for path in (work / "live").glob("*.inc")} != set(LIVE_C + LIVE_FLAGS + ["live-export", "sync-export", "live-deck", "live-lifetime", "live-config", "live-probe", "observer-flags"]):
        raise ValueError("unreviewed native live C mutation inventory")
    if {path.stem for path in (work / "time").glob("*.inc")} != set(TIME_CPP + TIME_INPUT + ("sample", "input-time")):
        raise ValueError("unreviewed native timing mutation inventory")
    return evidence


class Build:
    def __init__(self, root, work, output):
        self.root, self.work, self.output = root, work, output
        self.actions, self.runs, self.controls = [], [], []
        self.cc = os.environ.get("HOST_CC", "gcc")
        self.cxx = os.environ.get("HOST_CXX", "g++")
        self.common = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread"]

    def flags(self, mode, cpp=False):
        result = ["-std=c++11" if cpp else "-std=c11"] + self.common
        if mode == "asan":
            result += ["-fsanitize=address,undefined", "-static-libasan", "-static-libubsan",
                       "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        elif mode == "tsan":
            result += ["-fsanitize=thread", "-static-libtsan", "-fno-omit-frame-pointer"]
        return result

    def compile(self, name, sources, mode="plain", cpp=False, extra=(), links=(), run=True,
                argument="-", diagnostic=None, label=None):
        output = self.output / name
        command = [self.cxx if cpp else self.cc] + self.flags(mode, cpp)
        if cpp and mode in ("asan", "tsan") and any(pathlib.Path(source).name == "sdkliveobserver_test.cpp" for source in sources):
            # GNU7 forces static sanitizer archives whole, whose strong new/delete
            # definitions conflict with this fixture's real allocation-failure injector.
            # Private shared runtimes preserve both instrumentation and the injector.
            command = [value for value in command if value not in ("-static-libasan", "-static-libubsan", "-static-libtsan")]
            command.append("-Wl,-rpath,$ORIGIN/runtime")
        command += list(extra) + [str(source) for source in sources] + list(links) + ["-o", str(output)]
        self.actions.append((name, command, None))
        if diagnostic is not None:
            self.controls.append((label or name, name, diagnostic, argument))
        elif run:
            self.runs.append((label or name, name, argument))
        return name

    def fixture(self, name):
        return self.root / ("tests/unit/" + name)

    def macro(self, name, value):
        return '-D' + name + '="' + str(value) + '"'

    def plan(self):
        azmod = self.root / "src/azmod"
        common = self.root / "src/common"
        command, stored, live, sources, native = [self.work / name for name in BASE_FAMILIES]
        timing = self.work / "time"
        mod_inc = ["-I" + str(azmod)]
        common_inc = ["-I" + str(common)]
        hidden = ["-fvisibility=hidden", "-fvisibility-inlines-hidden"]
        live_flags = common_inc + hidden + ["-Wl,--version-script=" + str(self.root / "src/azstem/sdk_native_cpp.map")]
        c_live_flags = mod_inc + ["-I" + str(self.root / "tests/unit"), "-Wno-unused-function", "-Wno-unused-variable"]
        owner = self.macro("SDK_COMMAND_OWNER_INC", command / "owner.inc")
        submit = self.macro("SDK_COMMAND_SUBMIT_INC", command / "submit.inc")
        live_config = self.macro("LIVE_CONFIG_INC", live / "live-config.inc")
        c_services = {"export": ("sdkliveexport_test.c", "LIVE_EXPORT_INC", "live-export"),
                      "sync-export": ("sdksyncexport_test.c", "SYNC_EXPORT_INC", "sync-export"),
                      "deck": ("sdkdeckdata_test.c", "DECK_DATA_INC", "live-deck"),
                      "lifetime": ("lifetime_test.c", "LIFE_INC", "live-lifetime"),
                      "probe": ("sdkliveprobe_test.c", "LIVE_PROBE_INC", "live-probe"),
                      "flags": ("sdkobserverflags_test.c", "OBSERVER_FLAGS_INC", "observer-flags")}
        for mode in ("plain", "asan", "tsan"):
            self.compile("ext-time-samples-" + mode, [self.fixture("sdksampletime_test.cpp")], mode, cpp=True,
                         extra=common_inc + [self.macro("SAMPLE_TIME_IMPL", timing / "sample.inc")], links=["-lm"])
            binary = self.compile("ext-time-contract-" + mode, [self.fixture("sdktime_test.c")], mode,
                                  extra=mod_inc + [self.macro("INPUT_TIME_IMPL", timing / "input-time.inc")], links=["-lm"], run=False)
            for kind in ("clock", "budget", "elapsed", "grid", "interval"):
                self.runs.append(("ext-time-" + kind + "-" + mode, binary, kind))
            self.compile("ext-time-input-" + mode, [self.fixture("sdkinput_test.c")], mode,
                         extra=c_live_flags + ["-Wno-missing-field-initializers", self.macro("SDK_INPUT_C", azmod / "sdk_input.c")], links=["-lm"])
            self.compile("ext-command-" + mode, [self.fixture("sdkcommand_test.c")], mode, extra=mod_inc, links=["-lm"])
            self.compile("ext-command-wrappers-" + mode, [self.fixture("sdkcommand_wrapper_test.c")], mode,
                         extra=mod_inc + [owner, submit], links=["-lm"])
            self.compile("ext-stored-" + mode, [self.fixture("sdkstored_test.c")], mode, extra=mod_inc, links=["-lm"])
            self.compile("ext-mono-preview-" + mode, [self.fixture("sdkmonopreview_test.c")], mode, extra=mod_inc, links=["-lm"])
            for service, (fixture, macro, inc) in c_services.items():
                self.compile("ext-live-" + service + "-" + mode, [self.fixture(fixture)], mode,
                             extra=c_live_flags + [live_config, self.macro(macro, live / (inc + ".inc"))], links=["-lm"])
            for service in ("live", "loop"):
                self.compile("ext-" + service + "-observer-" + mode,
                             [self.root / ("src/azstem/sdk_" + service + "_observer.cpp"), self.fixture("sdk" + service + "observer_test.cpp")],
                             mode, cpp=True, extra=live_flags, links=["-ldl"])
            binary = self.compile("ext-sources-" + mode, [self.fixture("sdknativesources_test.cpp"), self.root / "src/azmod/sdk_connection_hooks.cpp"], mode,
                                  cpp=True, extra=common_inc + ["-DAZS_SAMPLE_BOUNDARY", self.macro("SOURCES_INC", sources / "sample-boundary.cpp")], links=["-ldl"], run=False)
            for profile in ("1", "2"):
                self.runs.append(("ext-sources-profile" + profile + "-" + mode, binary, profile))
            self.compile("ext-sources-constructor-" + mode, [self.fixture("sdknativesources_test.cpp"), self.root / "src/azmod/sdk_connection_hooks.cpp"], mode,
                         cpp=True, extra=common_inc + ["-DAZS_CONSTRUCTOR_REGRESSION", "-DAZS_SAMPLE_BOUNDARY", self.macro("SOURCES_INC", sources / "sample-boundary.cpp")], links=["-ldl"], argument="1")
            self.compile("ext-sources-bridge-" + mode, [sources / "bridge.cpp", self.root / "src/azmod/sdk_cache_message.cpp"], mode, cpp=True, extra=common_inc)
            self.compile("sources-production-" + mode + ".o", [self.root / "src/azmod/sdk_native_sources.cpp"], mode,
                         cpp=True, extra=common_inc + ["-c"], run=False)
            for testing in (False, True):
                self.compile("ext-native-commands-" + ("testing-" if testing else "production-") + mode,
                             [self.root / "src/azstem/sdk_native_commands.cpp", self.fixture("sdknativecommands_test.cpp")], mode,
                             cpp=True, extra=common_inc + mod_inc + hidden + (["-DAZSDK_NATIVE_COMMANDS_TESTING"] if testing else []))
        for name in TIME_CPP:
            self.compile("ext-time-" + name + "-control", [self.fixture("sdksampletime_test.cpp")], cpp=True,
                         extra=common_inc + [self.macro("SAMPLE_TIME_IMPL", timing / (name + ".inc"))], links=["-lm"], diagnostic=ASSERTION)
        for name in TIME_INPUT + TIME_ENGINE:
            include = mod_inc if name in TIME_INPUT else ["-I" + str(timing / name)]
            self.compile("ext-time-" + name + "-control", [self.fixture("sdktime_test.c")],
                         extra=include + [self.macro("INPUT_TIME_IMPL", timing / ((name if name in TIME_INPUT else "input-time") + ".inc"))],
                         links=["-lm"], diagnostic=ASSERTION, argument=name.replace("input-", ""))
        for name in LOOP_DECK:
            self.compile("ext-" + name + "-control", [live / (name + ".cpp"), self.fixture("sdkloopobserver_test.cpp")],
                         cpp=True, extra=live_flags, links=["-ldl"], diagnostic=ASSERTION)
        for name in COMMAND:
            self.compile("ext-command-" + name + "-control", [self.fixture("sdkcommand_test.c")],
                         extra=["-I" + str(command / name)] + mod_inc, links=["-lm"], diagnostic=ASSERTION)
        for name in WRAPPERS:
            self.compile("ext-command-" + name + "-control", [self.fixture("sdkcommand_wrapper_test.c")],
                         extra=mod_inc + [owner, self.macro("SDK_COMMAND_SUBMIT_INC", command / name / "submit.inc"),
                                         self.macro("SDK_COMMAND_IMPL", command / name / "sdk_command.c")], links=["-lm"], diagnostic=ASSERTION)
        for name in STORED:
            self.compile("ext-stored-" + name + "-control", [self.fixture("sdkstored_test.c")],
                         extra=["-I" + str(stored / name)] + mod_inc, links=["-lm"], diagnostic="FAIL stored:")
        for name in MONO_STORED:
            self.compile("ext-stored-" + name + "-control", [self.fixture("sdkmonopreview_test.c")],
                         extra=["-I" + str(stored / name)] + mod_inc, links=["-lm"], diagnostic=ASSERTION)
        for name in LIVE_C:
            service = "sync-export" if name.startswith("sync-export-") else "export"
            for candidate in ("deck", "lifetime", "probe"):
                if name.startswith("live-" + candidate + "-"):
                    service = candidate
            fixture, macro, inc = c_services[service]
            extra = [live_config, self.macro(macro, live / (name + ".inc"))]
            if name == "live-config-required":
                extra = [self.macro("LIVE_EXPORT_INC", live / "live-export.inc"), self.macro("LIVE_CONFIG_INC", live / (name + ".inc"))]
            self.compile("ext-" + name + "-control", [self.fixture(fixture)], extra=c_live_flags + extra, links=["-lm"],
                         diagnostic=ASSERTION + "|FAIL  lifetime")
        for name in LIVE_CPP:
            self.compile("ext-live-" + name + "-control", [live / (name + ".cpp"), self.fixture("sdkliveobserver_test.cpp")],
                         cpp=True, extra=live_flags, links=["-ldl"], diagnostic="FAIL live:")
        self.controls.append(("ext-live-sync-payload-short-control", "ext-live-observer-asan",
                              "AddressSanitizer: heap-buffer-overflow", "--sync-payload-short"))
        self.compile("ext-live-allocator-nothrow-bypass-control",
                     [self.root / "src/azstem/sdk_live_observer.cpp", live / "allocator-nothrow-bypass.cpp"],
                     cpp=True, extra=live_flags, links=["-ldl"], diagnostic="FAIL live:")
        for name in LIVE_FLAGS:
            self.compile("ext-" + name + "-control", [self.fixture("sdkobserverflags_test.c")], "tsan",
                         extra=c_live_flags + [self.macro("OBSERVER_FLAGS_INC", live / (name + ".inc"))],
                         links=["-lm"], diagnostic="WARNING: ThreadSanitizer: data race")
        for name in ("payload-overread", "rtti-overread", "loop-rtti-overread"):
            fixture = "sdkloopobserver_test.cpp" if name.startswith("loop-") else "sdkliveobserver_test.cpp"
            diagnostic = "AddressSanitizer: " + ("heap" if name == "payload-overread" else "global") + "-buffer-overflow"
            self.compile("ext-live-" + name + "-control", [live / (name + ".cpp"), self.fixture(fixture)], "asan",
                         cpp=True, extra=live_flags, links=["-ldl"], diagnostic=diagnostic)
        for name in SOURCES + ["constructor-order"]:
            extra = common_inc + ["-DAZS_SAMPLE_BOUNDARY", self.macro("SOURCES_INC", sources / (name + ".cpp"))]
            if name == "worker-affinity":
                extra.append("-Wno-unused-local-typedefs")
            if name == "constructor-order":
                extra.append("-DAZS_CONSTRUCTOR_REGRESSION")
            self.compile("ext-sources-" + name + "-control", [self.fixture("sdknativesources_test.cpp"), self.root / "src/azmod/sdk_connection_hooks.cpp"], cpp=True,
                         extra=extra, links=["-ldl"], diagnostic=("Assertion.*vfs_name_child_status.*failed" if name == "vfs-short-name" else
                                                               "Assertion.*rtti_name_child_status.*failed" if name == "rtti-short-name" else ASSERTION), argument="1")
        self.compile("ext-sources-bridge-control", [sources / "bridge-control.cpp", self.root / "src/azmod/sdk_cache_message.cpp"], cpp=True,
                     extra=common_inc, diagnostic=ASSERTION)
        for name in NATIVE_COMMANDS:
            self.compile("ext-native-commands-" + name + "-control", [native / (name + ".cpp"), self.fixture("sdknativecommands_test.cpp")],
                         cpp=True, extra=common_inc + mod_inc + hidden + ["-DAZSDK_NATIVE_COMMANDS_TESTING"], diagnostic="FAIL nativecommands:")
        for name, mode, macro, extra, diagnostic in [
                ("heap", "asan", "SCMD_TEST_HEAP", "-fno-sanitize=undefined", "AddressSanitizer: heap-buffer-overflow"),
                ("signed", "asan", "SCMD_TEST_SIGNED", "-fno-sanitize=address", "runtime error: signed integer overflow"),
                ("race", "tsan", "SCMD_TEST_RACE", None, "ThreadSanitizer: data race")]:
            self.compile("ext-command-" + name + "-control", [self.fixture("sdkcommand_test.c")], mode,
                         extra=mod_inc + ["-D" + macro] + ([extra] if extra else []), links=["-lm"], diagnostic=diagnostic)
        growth = [self.cc] + self.flags("plain") + ["-I" + str(command / "request-growth")] + mod_inc
        growth += [str(self.fixture("sdkcommand_test.c")), "-lm", "-o", str(self.work / "request-growth-rejected")]
        self.actions.append(("command-request-growth", growth, "command request grew"))
        adapter = self.work / "adapter.cpp"
        adapter.write_text('#include "sdk_command_engine.h"\n#include <type_traits>\nstatic_assert(std::is_standard_layout<scmd_adapter>::value, "adapter C layout");\n')
        self.actions.append(("command-cpp-header", [self.cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror"] + mod_inc + ["-fsyntax-only", str(adapter)], None))
        self.actions.append(("native-commands-c-header", [self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror"] + common_inc + ["-x", "c", "-fsyntax-only", str(common / "aznativecommands.h")], None))

    def execute(self, jobs):
        logdir = self.work / "compile-logs"
        logdir.mkdir()
        def action(item):
            name, command, rejection = item
            result = subprocess.run(command, cwd=str(self.root), stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, universal_newlines=True)
            (logdir / (name + ".log")).write_text(shlex.join(command) + "\n" + result.stdout if hasattr(shlex, "join")
                                                   else " ".join(shlex.quote(arg) for arg in command) + "\n" + result.stdout)
            if rejection is not None:
                valid = result.returncode != 0 and rejection in result.stdout
            else:
                valid = result.returncode == 0
            if not valid:
                raise RuntimeError("native compile/control failed: " + name + "\n" + result.stdout[-4000:])
            print(("BUILD CONTROL caught: " if rejection else "BUILT: ") + name, flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            # Consume every result; a failure cannot hide a later compiler error.
            failures = []
            for future in [pool.submit(action, item) for item in self.actions]:
                try:
                    future.result()
                except Exception as error:
                    failures.append(str(error))
            if failures:
                raise RuntimeError("\n".join(failures))
        for name, command, rejection in self.actions:
            if rejection or not (self.output / name).is_file() or name.endswith(".o"):
                continue
            wanted = []
            if "-fsanitize=address,undefined" in command:
                if "-fno-sanitize=address" not in command:
                    wanted.append("__asan_init")
                if "-fno-sanitize=undefined" not in command:
                    wanted.append("__ubsan_handle")
            if "-fsanitize=thread" in command:
                wanted.append("__tsan_init")
            if wanted:
                symbols = subprocess.check_output(["nm", str(self.output / name)], universal_newlines=True)
                if any(symbol not in symbols for symbol in wanted):
                    raise RuntimeError("required native instrumentation missing: " + name)
        for path in self.output.glob("*.o"):
            path.unlink()  # Compile-only evidence remains in the log; do not upload objects.
        (self.output / "sdk-extension-runs.tsv").write_text("".join("\t".join(row) + "\n" for row in self.runs))
        (self.output / "sdk-extension-controls.tsv").write_text("".join("\t".join(row) + "\n" for row in self.controls))


def stage_shared_runtimes(root, work, output, compiler):
    directory = output / "runtime"
    directory.mkdir()
    records = []
    for name in ("libasan.so", "libubsan.so", "libtsan.so"):
        source = pathlib.Path(subprocess.check_output([compiler, "-print-file-name=" + name], universal_newlines=True).strip()).resolve()
        if not source.is_file():
            raise RuntimeError("shared sanitizer runtime unavailable: " + name)
        dynamic = subprocess.check_output(["readelf", "-d", str(source)], universal_newlines=True)
        match = re.search(r'\(SONAME\).*?\[([^/\]]+)\]', dynamic)
        if not match:
            raise RuntimeError("sanitizer SONAME unavailable: " + str(source))
        target = directory / match.group(1)
        shutil.copy2(str(source), str(target))
        records.append({"soname": target.name, "sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "compiler_runtime": str(source)})
    wrapper = work / "shared-preflight-cc"
    wrapper.write_text('#!/bin/sh\nexec gcc \'-Wl,-rpath,$ORIGIN/../runtime\' "$@"\n')
    wrapper.chmod(0o755)
    environment = os.environ.copy()
    environment["HOST_CC"] = str(wrapper)
    subprocess.run(["bash", str(root / "tests/sanitizer-preflight.sh"), "--output", str(output / "shared-preflight"), "--compile-only"],
                   env=environment, cwd=str(root), check=True)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=pathlib.Path)
    parser.add_argument("work", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error("jobs must be between 1 and 8")
    args.work.mkdir()
    args.output.mkdir(parents=True, exist_ok=True)
    evidence = generate(args.root, args.work)
    build = Build(args.root, args.work, args.output)
    build.plan()
    runtimes = stage_shared_runtimes(args.root, args.work, args.output, build.cxx)
    build.execute(args.jobs)
    (args.output / "sdk-extension-manifest.json").write_text(json.dumps({
        "scope": "six standalone SDK extensions; synthetic native objects, no EP147 native calls",
        "runs": len(build.runs), "runtime_controls": len(build.controls), "families": list(FAMILIES),
        "generator_sha256": evidence,
        "private_shared_sanitizer_runtimes": runtimes,
        "compilers": [subprocess.check_output([compiler, "--version"], universal_newlines=True).splitlines()[0]
                      for compiler in (build.cc, build.cxx)],
        "compile_commands": [{"label": name, "argv": command, "required_compile_rejection": rejection}
                             for name, command, rejection in build.actions]}, indent=2) + "\n")
    print("SDK EXTENSIONS COMPILED: %d runs, %d diagnostic controls; runtime NOT RUN" % (len(build.runs), len(build.controls)))


if __name__ == "__main__":
    main()
