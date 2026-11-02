# SDK automated acceptance

Run the layers in this order: unit tests, isolated firmware emulators, then the
physical deck. Each result describes its own scope. A skipped prerequisite or
missing observation is incomplete coverage; it cannot establish a pass.

## Reuse a loaded track for command tests

Prepare the test track once per logical deck and keep the same firmware process
and native load for ordinary command cases. Use public SDK requests and fresh
source/storage observations to set up and restore each fixture. Browser navigation,
track reloads and screenshots are not part of ordinary command-group boundaries.
Capture the GUI for initial loading/approval or failure diagnosis only.

In the current retained emulator, the existing runner supports:

```sh
# All 26 commands on the already loaded track on logical Deck 2.
python3 anchor/output/sdk-loop-emulator/runtime/current-cue-case.py all-commands 2

# Focused behavior checks keep that track loaded and restore their fixture.
python3 anchor/output/sdk-loop-emulator/runtime/current-cue-case.py memory-color 2

# Request this separately when verifying persistence across a track reload.
python3 anchor/output/sdk-loop-emulator/runtime/current-cue-case.py memory-color-reload 2
```

Run these commands from the workspace parent. The retained runner requires the
existing owned VM, approved probe and loaded test track; it does not start a VM
or prepare a missing load. `all-commands` composes the existing twelve loop and
fourteen cue behavioral checks. It pins process birth, native load/unit, title
and source rate, streams progress, and stops at the first failed or incomplete
group. A group must return its actual complete per-deck operation coverage.
The original target-side ten-second command deadlines remain in force.

Ordinary `memory-color` reports reload persistence as `NOT_RUN`; it deletes only
its new disposable point and verifies the original complete stored cue contents.
Use `memory-color-reload` explicitly for the reload regression and test retained
disk reboot persistence separately. Rebuilds of loaded native code still require
the established cold-start procedure. Use a focused selector while fixing one
command; reserve `all-commands` for the relevant integration pass.

## Public loop-command experiment

`bash tests/sdk-loop-emulator.sh --sdk-root ../sdk` runs the native async fixtures,
the command protocol/controller and normal-loader runtime unit tests, and the
SDK's actual `examples/loop-command-probe` client fixture. `SAN=asan` or
`SAN=tsan` selects the corresponding native fixture mode. These are unit gates;
their synthetic results do not establish firmware execution.

Build and package `sdk/examples/loop-command-probe` with the SDK tools. Supply
its `dist/probe-binding.json` and `.azmod` package to
`sdk_emulator_loop_runtime.run_loop_runtime`, along with a source-bound runtime
package, pinned panel driver/receiver, owned loopback guest context, actual UI
Allow/reload callback, and per-deck track-selection callback. The adapter stages
the client on the guest's real writable USB overlay, verifies all package hashes,
and uses the normal loader and actual UI trust flow. It enables and separately
records the three command flags before startup. Original disks and the physical
deck are outside this test's write scope.

`sdk_emulator_loop.run_refusals` first submits valid command envelopes with a
stale load generation, checking actual SDK rejection on all four units. Local
probe refusals or expired test deadlines cannot establish this coverage. Its
verdict is archived separately from native application.

`sdk_emulator_loop.run_commands` tests all twelve transient loop IDs on logical
decks 0–3. Each request uses the client's public native-load snapshot and a
bounded native-clock deadline. It requires matching APPLIED receipts, explicit
result release and, where the public API exposes it, a fresh independent loop
observation. It never treats ACCEPTED or a missing receipt as success. Retained
cue contents and beat rationals lack independent public readers; their exact
confirmation remains the backend's correlated native receipt. Cancellation,
stale execution and MOVE partial failures have separate native-shaped unit
coverage; the happy-path emulator matrix alone does not certify those races.

The probe acts only on explicit, process-bound requests atomically renamed into
its own `data/request`. Full receipts go into bounded `data/results.<pid>.log`;
the core log carries only short readiness records because its lines are capped
at 400 bytes. Archive both streams with the mapped binary hash and PID/birth
evidence. IN/OUT require actual loaded playback; ENTER seeks to retained IN, and
MOVE is non-atomic. Firmware execution, quantization modes, physical controls,
audio timing and saved persistence retain separate reported acceptance scopes.

## 1. Unit tests

```sh
SAN=plain bash tests/sdk-completion.sh --output /tmp/sdk-unit-plain
SAN=asan bash tests/sdk-completion.sh --output /tmp/sdk-unit-asan
SAN=tsan bash tests/sdk-completion.sh --output /tmp/sdk-unit-tsan
```

The aggregate requires the foundation and fifteen extensions. `sdk-automation.sh`
tests the automation itself using temporary files and fake SSH, without contacting
a target. It also runs the all-deck control, loaded-runtime, acknowledged panel
transport and Linux position-publisher fixtures. Its regressions cover corrupt packets, changed source inputs, unsafe
archive paths, timeouts, log recovery, process loss, emulator ownership, black
screens and missing evidence. An observed failure remains a failure when a later
check cannot complete.

The timing tests use every deck, both native-shaped firmware profiles, multiple
sample rates and long track coordinates. The acceptance bound is one source
sample plus half a microsecond of cache rounding. Reported coordinates remain
sample-derived. Separate controls restore the old strict comparison on each deck
and must fail. Clock failures, backward clocks, frequency conversion, malformed
beat grids and playback interval underflow also have regressions.

The command matrix checks every operation on all four decks, all quantization
modes and both persistence choices. Production native capabilities must remain
unsupported. Synthetic successful dispatch checks per-deck result isolation.
Reconnect tests retain old snapshots across repeated source epochs, reject stale
and foreign handles, preserve peer consumers and exercise journal overflow.

Load/live probe tests reject every short size prefix on all four decks without
reading the version or changing history/output. The initial GNU7 ASan run caught
an eight-byte load from a four-byte input; the size guard now uses a bounded
volatile prefix read. The concurrent command fixture keeps 400 applied commands
and its 45-second watchdog, while yielding between pending polls so it can run
under full-system emulation without starving its owner thread.

GNU ELF and Linux process-map fixtures require Linux. macOS results alone cannot
close the aggregate. `bash tests/run-linux.sh` also requires a successful
unsuppressed sanitizer preflight before the complete existing Linux unit suite.

## 2. Emulator

Build one immutable ARM packet without contacting a target:

```sh
bash tests/sdk-data-deck.sh --compile-only \
  HOST SSH_KEY PINNED_KNOWN_HOSTS /tmp/sdk-arm-packet
```

Run both previously prepared firmware builds sequentially:

```sh
python3 tests/integration/sdk_emulator_smoke.py \
  /path/to/fw130 /tmp/sdk-arm-packet/bundle.tar.gz /tmp/sdk-emu130 \
  --profile 130 --port 2360 --qmp-port 4460
python3 tests/integration/sdk_emulator_smoke.py \
  /path/to/fw200 /tmp/sdk-arm-packet/bundle.tar.gz /tmp/sdk-emu200 \
  --profile 200 --port 2361 --qmp-port 4461
```

Each input folder needs its existing kernel, patched initramfs, raw system/USB
images and client key. The driver uses the already-built local QEMU image. Base
images are mounted read-only behind fresh qcow2 overlays. SSH and QMP bind only
to loopback; the original EP147 service is masked. Cleanup checks a fresh
ownership label before removing a VM. Logs and overlays remain local.

The packet validates its complete checksum inventory and current compiled source
closure before upload. The qualified v2 packet contains 75 clean extension
runs and 116 diagnostic controls, plus the foundation and separate static/shared
sanitizer preflights (99 successful runs and 145 controls observed per kernel).
It covers command, stored, live, sources, native-command policy, time, mono
preview and public MT/Sync setting families. The other aggregate extensions retain their own unit
entry points and separately qualified native packets; this packet does not
implicitly include them.

After the standalone tests, a shim-only stock firmware baseline checks process
health and a nonblank display. The firmware SHA must match the selected profile.
Baseline audio initialization failure blocks in-process SDK acceptance. Passing
standalone tests in that guest does not establish native callbacks, commands,
source association, controls or persistence.

### Optional in-process scalar observations

After a corrected emulator passes its shim-only baseline, the smoke runner can
start a second, temporary SDK process in that same owned VM. Prepare a package
from a reviewed full core pair, its source manifest, and matching diagnostic probes:

```sh
python3 tests/integration/sdk_emulator_sdk.py /tmp/sdk-runtime-package \
  /path/to/release-source/out /path/to/release-source-manifest.json \
  /path/to/runtime-probes
python3 tests/integration/sdk_emulator_smoke.py \
  /path/to/fw200 /tmp/sdk-arm-packet/bundle.tar.gz /tmp/sdk-emu200-runtime \
  --profile 200 --port 2361 --qmp-port 4461 \
  --runtime-package /tmp/sdk-runtime-package --runtime-seconds 480
```

The package preparation command expects the existing probe `manifest.json`,
`operator-loop-probe.so` and `source-probe.so`. It aliases the operator probe to
`loop-probe.so` and writes schema 2 `runtime.json`, binding exactly four AArch64
libraries to their SHA-256 values and current source closure. The host package
also retains `azmod.so.full` and `azstem.so.full`; these companions are not uploaded.
Validation derives the loop reader and source candidate deltas from their exact
full-library function symbols, checks the exported anchors and target code against
the stripped providers, and compares the result with the probe's read-only
`.azsdk.binding` compiler-input section. A manifest-only delta correction cannot
repair an already compiled probe. Legacy packages without this proof must rebuild
their probes before current acceptance. Changed, missing or symlinked inputs fail
before a VM is started. Other prepared packages may use the same schema with an
explicitly declared timed loop probe. All inputs are rechecked before staging.

When deriving diagnostic helpers from a frozen SDK release, keep the native
packet's compiled source closure separate. The Python smoke runner's optional
`native_source_root` selects the frozen source tree used to validate and execute
that packet. Runtime artifacts still validate against the runner's own source
tree. Both roots are recorded, and neither manifest's hashes are relabeled.

SDK activation is attempted only after baseline `PASS`. The controller requires
the original EP147 service to remain masked, accepting the systemd states
`masked` and `masked-runtime`. Every other state refuses activation with a
diagnostic before stopping the baseline. Failed remote operations retain a
controller error and the operation even when SSH supplies no stderr.
It copies libraries and a startup
script into a unique guest `/tmp` directory, enables only the seven SDK diagnostic
flags on the disposable overlay, and creates a separate RAM systemd unit with
`Restart=no`. It preserves the tested emulator startup exports. It verifies the
uploaded hashes and reads mapped-file hashes, firmware identity, feature flags,
fresh deck heartbeat and the pinned process birth token. Base disk images remain
read-only and the normal smoke ownership checks still govern VM cleanup. This
module has no external-target execution CLI and refuses non-loopback SSH.

The controller retains both probe logs and requires completion footers, at least
120 seconds of stable-PID observations, all four registration slots and loop,
play, quantize and key-lock receipts. Source scans must reach their footer with
no invalid records. The source probe uses one absolute 300-second monotonic
budget, including query work and bounded interrupted sleeps. Clock failures,
backward time and exhausted retry budgets produce explicit refusal footers.
The 480-second capture bound includes startup and emulation margin. Once the source footer
arrives, the operator loop probe is stopped through its own validated, PID-bound
stop file. A shorter observation can legitimately return `INCOMPLETE`. Missing
callbacks, refused experimental ABIs and unavailable mapped-file evidence are
also incomplete; probe errors or changed identities/hashes are failures.
If collection is interrupted, bounded read-only recovery retains available
logs, journal and mapping evidence from the same birth-pinned process. Recovery
never retries activation or controls, and secondary failures cannot replace the
original controller error.

Results are stored separately in `native-runtime/report.json`. Requesting this
phase adds its status to the smoke result: a passing standalone packet and baseline
cannot hide a failed or incomplete SDK runtime observation. The scope is scalar
registration/receipt consistency on observed empty decks. Loaded-track identity,
source binding, native commands, persistence and audio behavior retain explicit
incomplete acceptance fields and require their own observations. No physical deck
is changed by this workflow.

### Loaded emulator controls

The Python smoke API additionally accepts `loaded_context` with a digest-bound
panel driver, exact expected title aliases, an independently verified GUI
selection callback, position-provider binding and bounded panel transport.
The runner supplies the live VM nonce and endpoints itself. It retains the
baseline gate and validates the loaded result separately under `LLLL`.

The first selection comes from `prepare_track(controller)`. For decks 2–4,
`prepare_deck_load(controller, deck, selected)` must independently verify the
current highlighted track and return its exact expected title plus a new GUI
evidence path. It can restore the browser after a prior LOAD changes the view.
Missing, refused or reused selection evidence stops before that deck's LOAD.
Reports retain each selection before exactly one LOAD; the runner never retries
a stateful input to recover unknown GUI context.

Native live registration does not replay an initial Play or KeyLock value.
An optional `prepare_controls(controller)` callback can explicitly establish
observations after all four decks have loaded. `establish_live_receipt(deck,
stream)` accepts only Play or KeyLock, obtains a matching unit witness and a
fresh bound native clock, and sends at most one setup input for a missing stream.
It records the first genuine receipt with no assumed initial value. Setup is
logged separately from the subsequent known-state roundtrip; a failed setup is
never retried. Without this callback, missing initial receipts still refuse
before input. Neither setup nor a raw Play receipt establishes paused safety.

`sdk_emulator_loaded.py` requires the actual main-dispatch readiness message,
all four registration slots and a fresh, identity-bound position publisher
before panel input. `sdk_emulator_controls.py` selects each logical deck's
physical control pair and observes play, loop, key-lock and global quantize
transitions. Raw play notification bytes are never treated as paused state.
The position witness uses the deck engine's verified 44.1 kHz coordinates.
An unchanged phase within an engaged loop needs an independently justified
maximum source rate and a sampling interval that excludes loop aliasing;
missing evidence remains incomplete.
Waits discard transient stale heartbeats and native `U` (unknown) deck samples,
resetting their consecutive-observation count without replaying controls. `U`
means an unvalidated sample or a first zero TrackID during a transition; it never
proves either loaded or empty. Malformed records and changed identities still
refuse immediately. The five-second freshness limit remains fixed.
Persistent stale data is incomplete, while wrong identity and future clocks
refuse immediately. Reads and transport time count toward the wait deadline;
a late positive cannot satisfy it or become a partial result.
A play input uses one combined receipt/motion wait. Each of two consecutive
observations must contain a newer same-unit Play receipt and qualified motion
after its capture time and the pre-input native clock fence. This consumes
qualifying windows that arrive while the receipt is being observed. The same
30-second default includes read time; exact-deadline results fail. Position
freshness, provider identity, five post-receipt samples spanning 400 ms and the
250 ms sampling-gap bound remain required. The copied position ABI identifies
a logical deck and provider/process but carries no native unit token, so these
checks do not establish native-unit object correspondence or source/load identity.
When neither a direct transport witness nor a source-rate bound is available,
the manual-loop check refuses before sending any input. Other deck checks still
run. The position publisher retains only twelve samples and stops after a
bounded 30-minute session, covering the runner's supported long capture window.
The readiness window is 180 seconds, capped by the existing overall deadline,
so it can observe the emulator shim's full 120-second grace after preceding
initialization. Grace expiry does not qualify readiness.

`sdk_panel_transport.py` uses one strict pinned loopback SSH channel and an
acknowledged native receiver. Each frame is exactly 153 bytes, checked against
the pinned process birth before a device write. Ownership checks, finite I/O
deadlines and neutral cleanup remain required. The read-only position witness
is opt-in and pins the provider build ID, header hash and copied-record ABI.
These diagnostic clients do not implement native SDK commands, source-qualified
load identity, saved-loop persistence, beat-grid acceptance or audio validation.

The emulator's native packet uses a 120-second bound for each sanitizer
preflight process; the standalone runner defaults to 15 seconds and exposes
`--preflight-timeout` for integer values from 1 through 300. The selected bound
is recorded in its report. The first 2.00 attempt on a previous test timed out in the
shared ASan leak control at 15 seconds, while the same control passed at that
bound on 1.30. The larger emulator bound accommodates the observed intermittent
TCG delay; it does not establish the cause of the leak-scanner delay. Timeouts
remain failures, genuine expected diagnostics remain required, and reruns must
retain the failed attempt and run the complete packet. Individual passing
controls cannot replace a failed packet result.

### Read-only runtime scheduling evidence

Use `sdk_emulator_diagnostics.py` when an owned runtime has delayed heartbeats,
positions or native controls. Its `scheduler_snapshot_script(pid, birth)` reads
Linux per-thread stat, schedstat and wait-channel records. It checks the pinned
principal before and after collection, limits the thread inventory to 512 and
caps the capture at 512 KiB. The caller must retain the existing live VM nonce,
strict loopback SSH and bounded command checks; this helper selects no target,
starts no target process and writes nothing to the guest. The schema-2 collector
reads each enumerated task in one AWK process and compares its birth, policy and
RT priority before and after the counters/channel reads. An incomplete task is
marked retired only after its task directory is absent. A final census requires
every remaining task to have complete records; a new or reused task, unreadable
live task, changed policy, or missing principal refuses the capture. The caller
retains stdout and stderr on failure. This avoids the old bulk-AWK/head pipeline
masking an open failure and dropping all later wait-channel records. Collection
remains sequential and makes no claim of an atomic process-wide snapshot.

Parse each retained raw capture with `parse_scheduler_snapshot(raw, pid, birth)`.
`scheduler_delta(before, after)` reports CPU runtime and runqueue wait for threads
with unchanged birth identities. It refuses backward clocks, decreased counters,
changed identities, duplicate records and partial captures. Keep transport start
and end times beside the raw records because thread reads are sequential. A wait
channel is a point-in-time observation; neither it nor high runqueue delay proves
which native operation caused a stall. These diagnostics do not relax heartbeat
freshness, position binding, control deadlines or feature acceptance.

### Controlled CPU/RAM startup comparison

```sh
python3 tests/integration/sdk_emulator_resources.py /tmp/sdk-resource-comparison \
  --base /path/to/fw200 --profile 200 --port 2370 --qmp-port 4470
# Analyze retained evidence without Docker or starting a VM:
python3 tests/integration/sdk_emulator_resources.py /tmp/sdk-resource-comparison \
  --analyze-only
```

The new output freezes the runner and QEMU image identity. Six sequential boots
cover four/six CPUs and 2 GiB/4,038,197,248 bytes of guest RAM, repeating the low
and normal endpoints. Each boot uses fresh overlays, identical firmware/driver/
shim/ALSA inputs and the same startup procedure, without a preceding SDK test
workload. Observation lasts up to 120 seconds and ends early after confirmed
service failure. PCM state, guest memory/load, cgroup OOM and CPU quota counters,
startup logs and a display capture are retained. Guest boot logs must confirm the
requested CPU count and memory; missing metrics, an unloaded audio driver, changed
inputs or failed cleanup invalidate a resource-only conclusion.

The a previous test firmware 2.00 comparison reproduced the identical preview-mixer
assertion in all six boots, with no OOM or quota-throttling events. Six CPUs
removed the CPU-5 affinity warning. The embedded driver SHA was
`83e452fb18a506a5bbff8f1b0c84b44e0a18c07d6133424ebcaec93cfcf0648d`
in every case. Normal resource allocation did not clear the failure; TCG timing,
driver logic and other guest configuration remain possible causes. This is not
acceptance of the repaired-driver artifact or of firmware 1.30.

SSH boot discovery ignores banner-only keyscan output and pins only structurally
valid keys for the owned loopback endpoint. Strict checking remains enabled, and
a valid pinned key is never replaced during that boot.

### Public settings under an owned emulator reload adapter

The public-setting harness stages the bound SDK probe using normal community-mod
filenames and the enabled list. It requires the actual native Allow UI to write
the exact probe binary hash. It never writes trust records. The subsequent UI
Reload action must create a real `apply` request. In this fixture the deployment
watcher is inactive, so the owned adapter consumes that request once and stops
only the temporary SDK service. It verifies absence of the old native process,
then captures a different native PID and birth before creating a new receiver.
The normal mod entry/library service provides all MT/Sync observations.

This scope proves public Master Tempo and Sync enabled settings on empty units,
including explicit first-receipt setup and two known-state transitions per
setting/deck. It does not qualify the production deployment watcher or USB copy
pipeline. Fresh API query clocks, independent native receipt clocks, the mapped
probe hash, five-second heartbeat/query freshness and the unchanged source/binary
bindings are required. Unknown settings remain unknown. No playing/paused,
track/source association or native-command inference is admitted.

### Receiver expiry and native identity

The native panel receiver keeps its 900-second absolute lifetime and 300-second
idle limit. A longer owned runtime can use at most six separate receiver leases,
renewed at explicit phase boundaries after both control buses have acknowledged
their neutral frames. Close the previous lease, independently check the same
native PID/birth, then create a sequence-zero receiver. Never replay a frame or
renew after a failed stateful write. Neutral here describes panel holds only;
it supplies no playback or paused-state evidence.

A receiver CHECK failure does not establish that EP147 exited. The loaded
runtime falls back to a bounded, read-only `/proc/PID/stat` check on the same
owned guest. A matching identity permits diagnostic collection while the failed
sender remains closed. Identity refusal blocks further access; an unavailable
check remains unavailable evidence. Retain the original transport failure and
the bounded `process-identity-fallback.jsonl` record separately.

Track-selection callbacks may use the existing 180-second LOAD wait. Once all
four loaded titles qualify, the controller restores its constructor-configured control wait
(30 seconds by default) before receipt setup and scalar/motion checks. It also
restores that value after a failed preparation. `wait_budgets` records the actual
per-deck LOAD waits and configured control wait. A longer navigation/LOAD wait
must not silently extend every later control wait.

## 3. Physical deck automation

Reuse the exact packet after the emulator attempts:

```sh
python3 tests/integration/sdk_native_run.py \
  /tmp/sdk-arm-packet/bundle.tar.gz HOST SSH_KEY PINNED_KNOWN_HOSTS \
  /tmp/sdk-real-kernel --timeout 1800
python3 tests/integration/sdk_runtime_check.py health \
  HOST SSH_KEY PINNED_KNOWN_HOSTS --seconds 600 --output /tmp/sdk-real-health
python3 tests/integration/sdk_runtime_check.py log /path/to/capture.log \
  --required-decks 1 2 3 4 --output /tmp/sdk-capture-check
```

The standalone runner writes only a unique target `/tmp` directory. It uses
strict pinned SSH, low-priority bounded child workloads, checksum verification,
exact diagnostic controls and an unchanged EP147 PID guard. It collects logs
before cleanup and retains scratch after timeouts or collection failures.
The health sampler only reads process/service state. Neither tool changes
settings, preload, services, USB contents or playback.

The capture validator checks independent stream revisions, owner generations,
receipt clocks, scalar validity and source-derived loop coordinates. It detects
the old quantization-consistent range rejection on every deck. Missing deck
observations remain incomplete; a quiet log cannot establish callback freshness.

Automated health checks describe the currently loaded runtime. Accepting a new
core inside EP147 still requires a controlled activation with matching probes
and fresh paused-state confirmation for loaded decks. Physical button sequences,
a real saved memory loop/Active Loop export, native command semantics and audio
behavior remain separate acceptance work. Back up analysis files before any
authorized persistence experiment.

All output directories must be new. JSON reports distinguish `PASS`, `FAIL` and
`INCOMPLETE`; their exit codes are 0, 1 and 2 respectively. Preserve failed and
incomplete attempts alongside successful reruns.


### Public stored-waveform admission

`tests/integration/sdk_emulator_wave_layer.py` checks the sealed
`sdk-wave-layer-probe` v1 grammar, exact package and owned normal-loader context.
It performs no target I/O. Use `wave_probe_archive` before RAM staging and
`wave_approved` only on approval records created by the actual Allow UI. The
future adapter must retain one apply consumption, absence of the old process,
a distinct pinned new PID/birth and exact mapped core/probe hashes before
passing a capture to `validate_wave_capture`. Its context is separate from the
untrusted mod log. The existing settings probe is a different mod and result.

A PASS requires encoding3/8 middle and clipped-tail requests, exact public
reference/geometry/150Hz axis, FILE/DERIVED provenance, all refusal statuses and
complete owned-ticket cleanup. Each emitted sample is independently inverted,
including its reserved zero byte, symbolic membership and unused fields. Full
FNV hashes can be recomputed only for windows entirely contained in the sample.
The complete larger-window inverse is performed by the exact bound probe; its
unlogged bytes cannot be reconstructed from the prefix. No pixel/calibration or
native source/load claim follows. Missing source/track/format or an incomplete
worker remains INCOMPLETE, with no alternate-track search or replay.

`check_startup_prerequisites` records only opt-in/opt-out RAM configuration and
identity requirements. It deliberately cannot certify installation from flags,
health, hook-table rows or symbol presence. Installation still needs separately
qualified read-only Binding layout/lifetime, entry/island/replacement, distinct
original trampolines and armed-state evidence. Never call a private function to
manufacture that witness. All paused/rate, source/load, command and hardware
guards remain independent.
