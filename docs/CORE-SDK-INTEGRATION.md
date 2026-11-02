# SDK integration validation

The standalone SDK suites exercise production policy, parser/provider code, native-shaped
ownership fixtures, copied exports and lifecycle hooks. They do not install a core, invoke
EP147 commands, change menu settings or establish firmware acceptance. The behavioral
limits remain in [the support matrix](CORE-SDK-SUPPORT.md).

## Command development workflow

For current command work, follow [the active plan](../../anchor/CORE_SDK_IMPLEMENTATION_PLAN.md).
The full aggregates and historical packet procedures below are integration tools,
not mandatory work after every edit. Run affected tests first, exercise the real
public-SDK command on 2.00/one deck, and expand a working family to both builds and
all four decks. Reuse the prepared emulator and rebuild changed artifacts only.
Docs, Python and host-harness-only changes do not invalidate unrelated native
binaries. Check the actual loaded candidate once per process to catch stale packages.

Tests must prove the requested native effect and the SDK result; saved operations
must survive reload/restart. Mocked APPLIED and passing standalone fixtures alone
are insufficient. No evidence ledger, dossier or per-command manifest is required.
Physical deployment is a separate requested stage and does not follow every build.

## Repeatable entry points

Run the foundation and all delivery and safety regression suites on a compatible Linux kernel:

```sh
SAN=plain bash tests/sdk-completion.sh
SAN=asan bash tests/sdk-completion.sh
SAN=tsan bash tests/sdk-completion.sh
```

The extensions are `sdk-automation.sh`, `sdk-time.sh`, `sdk-command.sh`, `sdk-stored.sh`, `sdk-live.sh`, `sdk-sources.sh`,
`sdk-native-commands.sh`, `sdk-load.sh`, `sdk-load-lifecycle.sh`, `sdk-vtable.sh`,
`sdk-hook-install.sh`, `sdk-source-probe.sh`, `sdk-load-probe.sh`, `wave-overview.sh` and `wave-report.sh`.
`--extensions-only` avoids repeating the foundation when another
runner already covers it. Every required suite runs even if an earlier suite fails. Each
log is retained in a fresh directory printed at startup; `--output NEW_DIRECTORY` selects
it explicitly. Exit 1 means failure; exit 2 means incomplete required coverage. A missing
suite fails, while exit 77 or a successful script containing a `SKIP` line is incomplete.
Neither produces `SDK COMPLETION: OK`. The GNU ELF fixtures require Linux; their macOS
skips cannot establish a full SDK pass.

`tests/run-linux.sh` retains the complete existing unit suite and adds these fifteen suites
in each plain, ASan/UBSan and TSan pass. It first proves sanitizer readiness. Use
`bash tests/run-linux.sh --preflight-only` to test readiness without running the long suites.
The pinned ARM image needs GNU g++ as well as gcc. On a Linux host the same standalone
scripts can run directly with `HOST_CC` and `HOST_CXX` selecting the compilers.
Both the Linux driver and native package builder run the controller/integrity regression
tests before their main workloads.

## Sanitizer readiness is a separate result

```sh
bash tests/sanitizer-preflight.sh
bash tests/sanitizer-preflight.sh --output /tmp/new-preflight plain
HOST_CC=gcc bash tests/sanitizer-preflight.sh \
  --output /tmp/new-arm-preflight --compile-only --static-runtimes
```

The preflight requires clean threaded heap workloads in plain, ASan/UBSan/LSan and TSan
modes. It then requires five opposite outcomes: an intentional ordinary failure, ASan
heap overflow, UBSan signed overflow, an unreachable 64-byte LSan allocation and a TSan
race. Every control needs a nonzero exit and its own diagnostic. A timeout, launcher
failure, unrelated crash or successful uninstrumented binary cannot count as detection.
The clean marker must be reached. `--compile-only` records compiler and SHA256 evidence
and explicitly reports runtime `NOT RUN`.

The runner replaces inherited sanitizer options, removes inherited LSan options and uses
`detect_leaks=1`; it uses no suppression file. A small unsanitized POSIX launcher bounds
each owned process group without requiring a target-installed `timeout`. Its default
preflight bound is 15 seconds; `AZ_SAN_PREFLIGHT_TIMEOUT` may select a bound up to 300
seconds. Runtime startup limitations return 2 and block the full Linux run. A finding in
a clean workload or a broken planted-fault control returns 1. Logs and binary/source
hashes remain available for investigation.

## Native-kernel standalone run

The deck runner cross-compiles the standalone suites in the pinned ARM GNU toolchain,
then stages them in one unique `/tmp/az-sdk-native.*` directory. It verifies the user's
SSH key and pinned known-hosts file. It never changes preload, flags, settings, services,
core/mod installations or native controls. Reserve the deck for this bounded test interval;
the runner checks `pidof EP147` before and after and fails if the PID changes.

```sh
bash tests/sdk-data-deck.sh HOST SSH_KEY KNOWN_HOSTS NEW_LOG_DIRECTORY

# Build and retain the package without contacting the target:
bash tests/sdk-data-deck.sh --compile-only \
  HOST SSH_KEY KNOWN_HOSTS NEW_LOG_DIRECTORY

# The native package covers six families, including time.
# Run the remaining safety suites separately with recorded native packets.
bash tests/sdk-data-deck.sh --extensions-only \
  HOST SSH_KEY KNOWN_HOSTS NEW_LOG_DIRECTORY
```

`ANLZ_SAMPLE_DIR` optionally adds only the supplied `ANLZ0000.DAT`, `.EXT` and optional
`.2EX`; firmware and audio assets are not required. With no sample directory the parser
fixtures remain asset independent. `SDK_NATIVE_BUILD_JOBS` selects 1–8 compile workers,
default 2. Each target workload has a separate 120-second owned-process deadline selected
by `AZ_SDK_NATIVE_TIMEOUT`; a timed-out control is a failure, even if it printed a matching
assertion before hanging. Successful fixtures containing a skip line also fail this native
acceptance run. All fixture allocations are released before final LSan scanning; the live
lifetime fixture retains its synthetic objects through all assertions and joined threads,
then frees them at final teardown.

The source snapshot records the context under `src`, `tests/unit`, `tests/integration` and
the standalone SDK scripts. Its SHA256 list is checked against the copied snapshot. GNU
preprocessing with the actual compile arguments records the exact compiled dependency
closure; extraction, mutation and target-runtime inputs are added explicitly. Every used
input must still match the current tree before staging. Unused context changes are
recorded separately and cannot invalidate an unrelated binary or conceal a changed used
input. Mutation code comes from the
reviewed standalone scripts' exact Python generators; an unexpected generator or mutation
inventory fails preparation. Production C/C++ headers and independent production
translation units compile too. Sanitized binaries must contain their required runtime
symbols. The command-request growth control must fail compilation with its specific
layout-review diagnostic.

The package contains the source manifest, compiler/command manifest and `SHA256SUMS` for
all staged files. The target checks every checksum before executing `run.sh`. Local
artifacts retain `bundle.tar.gz`, `source.tar.gz`, manifests, individual compile logs,
the build log, results and target logs. Target
cleanup follows successful log collection; a failed collection preserves its named scratch
directory for inspection. The current extended plan has 69 clean runs across the three modes and
102 runtime controls (the historical packet had 48 runs and 86 controls). Load, installer, overview and probe suites have separate native
packets; the original package does not cover them merely because the aggregate runner lists
them. Runtime controls require specific assertions or sanitizer reports,
including thread-token reuse, record-lock preservation, lifecycle retirement, public-wrapper
authentication and preview request/path checks. Foundation controls additionally cover
analysis/track FIFOs, path bounds, failed-source identity and ABI prefixes.

GNU7 links static sanitizer archives whole. Their strong `new`/`delete` definitions
conflict with the live fixture's real allocation-failure injector. The live ASan and TSan
fixtures and two live ASan overread controls therefore use private shared sanitizer DSOs
with an `$ORIGIN/runtime` search path. Other units retain static sanitizer runtimes.
The exact DSO hashes and four affected units are recorded in the package manifest. No
system library is installed or changed, and no multiple-definition override is used.
A separate unsuppressed shared-runtime preflight must pass clean runs and heap, signed
overflow, leak and race faults. The allocator-backed heap and RTTI global-overread controls
must then pass before any suite runs. The fixture also checks throwing/nothrow allocation
budgets, zero-byte allocation, and a deliberately bypassed nothrow allocation budget.

## Recorded environment matrix

| Environment | Established result | Limit |
|---|---|---|
| Current automation/controller regressions | 180 tests pass on Linux; 161 pass and 19 Linux-only cases skip on macOS; five completion-runner tests also pass on both | All-deck controllers, position publication, pinned identity, bounded readiness, transport and evidence integrity; separate preflight/aggregate evidence retained |
| Compatible x86_64 Linux, GNU 7.5 | All 16 required SDK suites pass in plain, ASan/UBSan/LSan and TSan modes, with clean sanitizer preflights and planted faults | SDK standalone aggregate; the complete legacy core Linux suite remains separate |
| ARM GNU 7.5 under Docker qemu-user on this Intel Mac | Plain readiness passes; ASan/LSan and TSan startup are unavailable; Linux driver exits 2 before the full suite | This is not a full sanitizer or full unit-suite pass |
| Earlier physical 2.00 readiness, native kernel | Both static and private shared preflights pass clean plain, ASan/UBSan/LSan and TSan probes plus all five planted faults; EP147 remained PID 967549 | Historical runtime readiness; separate SDK package results must be recorded |
| Earlier full-system 2.00 guest, native ARM kernel | Plain/ASan/UBSan/TSan probe behavior verified; a separate 120-second leak probe detects the intentional allocation | Initial 30-second leak control timed out, so that original combined preflight failed |
| Current full-system 1.30 and 2.00, native ARM kernel | Source-qualified standalone packet passes 90 runs and 129 diagnostic controls in both profiles | Standalone coverage; source inputs/kernel/resources are checked before reusing a result |
| Current 1.30 and 2.00 firmware startup | Source-built bounded startup barrier reaches main dispatch without native mixer abort or grace expiry | Loaded controls, source-qualified identity, commands, persistence and calibrated audio retain separate acceptance gates |

The Docker TSan failure reports `Found 47 - Supported 39, 42 and 48`. Its LSan runtime
reports a fatal ptrace-related startup failure even for a clean workload. Bounded
per-container `QEMU_RESERVED_VA` probes did not establish a usable mapping: some values
reported unsupported 38/41-bit ranges, others failed guest-base reservation or crashed.
No global binfmt, QEMU, Docker or security configuration was changed to hide this result.
A compatible native kernel, with the same positive/negative preflight, is the working
validation route. Disabling leak detection is not full LSan coverage.

The full-system guest's verbose leak log also warns that its dynamic linker/TLS handling
is incomplete. Detecting the explicit unreachable allocation therefore does not prove
complete TLS-root leak coverage. The guest was restored to its original paused state;
the physical deck and both emulated firmware processes were left configured as found.

Readiness evidence is retained in the local task artifacts
`anchor/output/sdk-native-preflight-mzv6ojz8` and
`anchor/output/sdk-system-emulator-preflight-mcydyxac`. The latter includes exact
before/after container state, the failed initial combined run and separate leak diagnostic.

## Coverage boundaries

The repository's code graph was unavailable for this project (`project not found or not
indexed`); this review used the exact source and scripts within the bounded SDK integration
scope. It does not claim an exhaustive codebase audit. Existing full-suite baseline
failures in crash-blame, executable-file rejection, failed-spawn cleanup and streaming-key
controls remain distinct from these SDK results until an actual full rerun establishes
otherwise. Public-tree development/firmware omissions remain explicit skips.

These standalone results do not close all-deck/both-build in-process acceptance, native
source association, native commands, loaded-analysis preference, persistence, event
freshness, audio timing or a long multi-mod soak. Those require the separately coordinated
firmware/hardware evidence described by the support and native-evidence documents.

The repeatable unit → emulator → physical automation workflow, its ownership guards
and its precise coverage boundaries are in [automated acceptance](CORE-SDK-AUTOMATION.md).
The startup repair, red/green tests, exact ARM builds, retained failed attempts and
fresh rerun reports are under `anchor/output/sdk-emulator-startup-fix-20261028`.
The synthetic WAV mounts in Linux but is absent from the firmware's USB source
view; loaded control acceptance is not established by that fixture. A physical
rerun on a previous test timed out over strict SSH before any remote writes.
