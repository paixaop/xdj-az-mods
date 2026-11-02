# SDK native command boundary

Current work follows [the command plan](../../anchor/CORE_SDK_IMPLEMENTATION_PLAN.md).
Use the existing native adapters for all 26 IDs and prove actual command behavior.
No evidence ledger/dossier is a deliverable. Historical research below records
how paths were discovered; it does not reopen superseded implementation tasks.

Transient loop operations 1–12 use the shared asynchronous native backend for
both firmware profiles. Enable `azmod.track-data.on` and `azmod.native-load.on`
before startup. `azmod.native-loop-commands.on` enables the ten control/adjustment
operations below; EXIT and RELOOP retain their separate `azmod.native-exit.on`
and `azmod.native-reloop.on` flags. Check each operation's capability bit.

Use `AZ_COMMAND_NATIVE_LOAD`, a valid snapshot `native_load_generation`, zero
`expected_revision`, and `AZ_QUANTIZE_NATIVE`. Unused numbers, cue selectors,
color and comment must be zero/empty. No public structure layout changes.

| Operation | Arguments and behavior |
| --- | --- |
| Loop IN | All numeric arguments zero. Capture current playhead into native default cue/loop IN; use device-configured quantization. Arbitrary absolute IN is unsupported. |
| Loop OUT | All numeric arguments zero. Capture current playhead and enter native loop; requires inactive working loop and valid retained IN. Arbitrary absolute OUT is unsupported. |
| Loop ENTER | All numeric arguments zero. Native RELOOP alias, including seek to retained IN; already-engaged requests finish uncertain. |
| Loop EXIT | All numeric arguments zero; native exit, with inactive postcondition. |
| RELOOP | All numeric arguments zero; native reloop, requiring inactive-to-engaged transition for confirmation. |
| Clear retained points | All numeric arguments zero. Clear native kind0 retained cue, including the temporary main cue; this does not directly clear the separate working loop. |
| Double / halve | All numeric arguments zero. Scale existing engaged working loop; confirmation checks unchanged IN and the scaled beat ratio (beat mode) or sample length (time mode). |
| Beat-based length | Positive `beats` exactly representable as a nonzero unsigned16 numerator/denominator rational; other numeric arguments zero. Change existing engaged loop, preserving IN. |
| Move loop | Signed nonzero `position_s` delta in source seconds; other numeric arguments zero. Two native boundary adjustments, outward boundary first. Non-atomic; partial application is uncertain, with no rollback. |
| Adjust IN | Signed nonzero `position_s` delta in source seconds; other numeric arguments zero. |
| Adjust OUT | Signed nonzero `end_s` delta in source seconds; other numeric arguments zero. |

Adjustments require an engaged loop with coherent source points. IN adjustment
and MOVE additionally require native default-cue selection (kind0) and mode380
equal to zero; other selections/modes are rejected before dispatch. Native minimum
policy0 enforces approximately13.33ms. OUT/length use native LoopType1 to skip
the selected-retained-cue copy branch. Beat-length application can still
synchronize the temporary default cue with the working loop; callers must not
assume that temporary cue stays unchanged. IN has no LoopType argument: its native
route can copy the working cue into the selected retained cue; MOVE inherits
this side effect. These are transient native edits, not verified saved-track
persistence. Native operations can reposition playback.

All operations share per-deck FIFO ordering, load/lifetime checks, deadlines,
cancellation and correlated Slip/BeatSync/Quantize descendant tracking. Poll
`command_poll` until terminal, then `command_release`. `ACCEPTED` can span
processing frames. `APPLIED` requires settled descendants and the operation's
checked postcondition on the same load; clamped/no-op or partial adjustments
cannot count as successful exact edits. Its revision is the shared transient
confirmation counter, independent of deck, loop and database revisions.
Cancellation after dispatch cannot undo an audible operation. Native command
behavior is exercised through the public SDK in the firmware emulator; see the
current coverage in [the support matrix](CORE-SDK-SUPPORT.md#commands). Physical
hardware acceptance is a separate stage.

The backend exchanges copied scalars through four bounded mailboxes. Dispatch
runs after successful native preProcess thread binding. A separate hook holds
the unit lifetime gate across the complete PlayerCore processing frame, including
rendering, command execution and postprocessing. Unit destruction closes admission
and waits for that frame; dormant command references never hold the gate open.

Slip, BeatSync and pending Quantize states use the original native asynchronous
route. The backend captures actual native command references at Quantize dispatch,
JumpController requests and command-manager submission. Verified command execute
slots restore the originating SDK request scope, so later descendants retain its
token and load identity. Complete and deleting destructor slots retire each
command incarnation before pool reuse. Patching the verified slots also routes
native guarded destructor optimizations through their existing indirect fallback.
The actual native Quantize cancel helper adopts owner-matched pending work and
callbacks affected by EXIT. A retained QuantizePending must execute; replacement
is reported as uncertainty. A later native request can take over the same
infrastructure object without inheriting the SDK token. Other decks and unrelated
native command calls retain their original behavior.

`ACCEPTED` persists across native frames. `APPLIED` requires the same native load,
an inactive working loop for EXIT or an inactive-to-engaged transition for RELOOP,
disposal of the correlated command references and empty native manager/Quantize
holders. Its revision is a shared per-deck transient-command confirmation counter,
independent of deck/loop/database revisions. RELOOP submitted while already engaged
finishes uncertain: engagement cannot confirm the requested seek, and there is no
request-correlated position receipt. Missing retained points also cannot become
success. Command replacement without execution, exceptions and native no-ops do
not become success. Unknown native handoffs cannot acknowledge settlement and keep the deck fenced
for the rest of that process.

Cancellation before native dispatch prevents the call. After dispatch it returns
pending and allows native bookkeeping to finish; cancellation cannot undo an
already-started operation. Each deferred execute rechecks native load, unit generation,
affinity and deadline. Stale, retired or expired work is suppressed through retained
tombstones, even after public result release. Such requests finish uncertain;
unknown handoffs cannot acknowledge settlement. Pool address reuse gets a fresh
incarnation after native destruction.

RELOOP can replace an existing Quantize callback without invoking EXIT's cancel
helper. Its submission hook adopts that owner's pending infrastructure before
the native call and requires retained QuantizePending execution after acceptance.
The private service adds size-checked `commands`, `submit_command` and an
append-only `submit_loop` entry carrying copied operation/delta/beat scalars.
Exact older service allocations are checked before reading new fields; the
original prefix and legacy submit remain EXIT-only for older core pairs.
MOVE arms its second edit after first-stage descendants settle, then invokes it
inside the next successful preProcess binding. No facade mutation runs after
postProcess. Set-subobject task-box, availability, Quantize and context bindings
must match the owned control facade before invoking any new route.

Polling collects copied receipts and performs SDK deadline cleanup; consumers
release terminal results. The inspector's Control page demonstrates this protocol.
Host fixtures exercise deferred native-shaped command chains. Public SDK tests
in an owned emulator additionally check the requested firmware effects. Physical
hardware acceptance requires separate deployment and testing on a deck.

## Native cue operations

Enable `azmod.native-cue-commands.on` with track data and native-load support for
the development cue adapters. MAIN_SET, MAIN_RECALL and HOT_TRIGGER use the native
control facade. HOT_SET/CLEAR, MEMORY_SET/DELETE, SAVED_SAVE/DELETE/AUTO and cue
comment/color updates use `sdk_cue_commands.cpp` and `sdk_cue_persist.cpp`;
saved/memory recall uses `sdk_cue_recall.cpp`. The public bridge dispatches these
paths through `submit_cue`, not the obsolete general adapter.

When a public cue request is admitted, queued internal snapshots on all decks
yield their actor slots. The decks share the actor owner lock. New internal reads
wait until public native work settles, then resume even if the caller holds its
terminal result. Native snapshot tasks retain their actor tombstones until
destruction. A running owner frame remains protected by the actor lock;
contention uses the existing copied request queue with the original deadline
and load identity.

A busy target lookup leaves the copied public request queued. Fresh load and
revision checks run before dispatch, within the original deadline. Temporary
cue-cache contention must not appear as a stale collection or unsupported
command; genuine load/revision mismatches still refuse before native execution.

Memory point recall cancels native Active Loop setup without creating another
loop wrap. It can settle after the tagged callbacks drain and the native manager
is idle. Completion still requires the correlated seek to the same source-bound
point, a disengaged loop and paused transport. Loop setup operations retain their
replacement-chain requirement and the original request deadline.

Internal cue refreshes revalidate the native source, load and both collection
revisions. An unchanged publication renews the existing converted snapshot;
changed collections or loads require fresh native position conversion.

Comment and color completion requires the correlated native reply and a full
typed comparison of both stored cue collections against their pinned baseline.
Only the selected field may change; checks include complete comment strings and
native seek values. Memory RGB confirmation also checks its derived native
palette index. Public snapshot conversion refreshes separately, so callers
should wait for `native_cues` to expose the revision returned by completion.

Use `AZ_COMMAND_NATIVE_LOAD` and native quantization. Saved writes also require
`AZ_COMMAND_PERSIST`. Revision-selected operations take IDs from `native_cues`,
set `AZ_COMMAND_NATIVE_CUE_IDS` and pass that snapshot's exact revision. File-list
IDs are not native cue IDs. The public header documents operation arguments.
Native save/delete completion must be followed by the requested collection change;
reload/restart tests must check persistence. Active-loop switching may require
two writes; partial application cannot count as full success. Validate these paths
with runtime behavioral tests appropriate to each operation/profile.

## Retired generic adapter

`azsdk_native_commands_adapter` is an unsupported fixture stub, and its separate
capabilities function returns zero. It is not the production route and must not
be enabled or developed as a parallel command backend. The `OwnerGate` class in
the same source file is used by the production asynchronous backend and remains
required. Keep existing ownership and deferred-completion protections.

`src/common/aznativecommands.h` is a private C/C++ boundary. Mods use the versioned
public library service; firmware objects and entry points stay inside the core.

## Implemented admission primitive

`OwnerGate` supplies the owner-thread lifetime lease used by the native backend:

- Publication assigns a nonzero generation and refuses replacement while live.
  Generations are never reused; exhaustion refuses further publication.
- Admission uses one lock-free compare/exchange. It acquires the reader bit
  before reading the object or calling its owner-thread predicate. A contending
  caller returns immediately. An exception or failed predicate releases the bit.
- A lease exposes its object and generation only on its admitting thread.
  Ending on another thread refuses and leaves the lease held. Normal scope exit
  ends it automatically. Thread identities use non-reused process serials;
  reusing a TLS address or `pthread_t` after thread exit cannot inherit a lease.
  First-use token allocation makes one compare/exchange and returns busy on
  contention or exhaustion.
- Retirement serializes with publication, validates the exact generation, closes
  admission, and waits for an existing reader before forgetting the pointer.
  Retirement of an old generation cannot close a replacement.
- Reentrant retirement cannot wait for its own reader. It closes admission and
  returns `AZ_BUSY`; the caller must end the lease and retry. Only `AZ_OK` permits
  native destruction. Publication and retirement belong on management paths;
  they must not run in playback/draw callbacks.

This is conditional lifetime protection. The complete object must be owned when
published, its real destructor must call `retire` before freeing any member, and
the gate must outlive its leases. Without those integrations, this primitive
cannot turn a stored pointer into native ownership. Destruction of a held lease
on a foreign thread, or destruction of a gate inside its own lease, terminates
instead of silently losing the release handle or freeing active storage.

The primitive does not certify the supplied thread predicate, native queue
lifetime, load identity, native argument encodings, or application completion.
It is intentionally disconnected from the disabled production adapter.

## Native evidence and remaining obligations

The detailed historical research notes below record the earlier general-command
investigation. The loop/cue implementation above supersedes its candidate-only
status; firmware behavior and persistence results are summarized in
[the support matrix](CORE-SDK-SUPPORT.md#commands). Paired loop
slot/ABI evidence is retained in `anchor/output/sdk-completion-command-map/`,
with additional selected/working property and quantization disassembly in
`anchor/output/sdk-completion-command-owner/loop-*-research-*.asm`.


Evidence is scoped to these exact ELF artifacts:

| Build | SHA-256 |
| --- | --- |
| 1.30 | `736bdc9322c00e5770af459c92cace33d8680825c07f00f909f74dfc473a77a6` |
| 2.00 | `6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925` |

Cached read-only provider output in `/private/tmp/az-sdk-rea/` establishes the
unit-owned facade bundle at `DjPlayerUnit+0x190`, the composite cue facade at
bundle `+0x40`, and Set/Load subobjects at composite `+0xc8/+0x190`. Unit teardown
frees this bundle before the PlayerCore task boxes. An arbitrary-thread wrapper
can enqueue a task containing the raw facade. Keeping the facade alive only
until that wrapper returns is insufficient. Availability bytes and two epoch
checks do not pin queued work.

The control facade's `loopExit()` is slot 3: `0x136fe48` on 1.30 and `0x142b198`
on 2.00. It has no argument and returns void. Slot 6 (`0x136ae80` / `0x14261d0`)
is `jump(CueKind, HotCueBehavior, SlipBehavior)`. EXIT's task retains the raw
facade and does not repeat availability/load checks. A task number indicates
queue progress; `lastStarted` is written before execution and cannot acknowledge
application. Cancellation is a pre-execution skip, without a completion fence.

`reloop(SlipBehavior)` is slot 4: `0x1367b68` / `0x1422eb8`. It returns void
and forwards through the checked slot 6 `jump` with kind 0, behavior 0 and the
supplied 32-bit SlipBehavior. This backend selects native SlipBehavior 0; it
does not map SDK quantization values into that enum. Both facade entries and
their original nominal table are checked. The generated verification profiles
come from `tools/sdk-exit-profile.py` against the two exact stock ELFs.

An owner-thread route remains a research candidate. The verified PlayerCore
primary vtable slot 3 is `0x135b780` / `0x1416ae8`. It directly calls
`preProcess` at `0x135b7cc -> 0x13583e0` /
`0x1416b34 -> 0x1413750`. Cached `task-native-entry-200.c` shows `preProcess`
binding the native task-box thread identity and draining its queues. The full
slot-3 assembly is retained in
`anchor/output/sdk-completion-native-commands/player-core-slot3.asm` in the
workspace. An outer vtable interception does not prove that native direct calls
will pass through it. Resolving a code-hook target from a checked owned object
would still require safe installation before concurrent execution, exact build
and signature guards, and a verified borrowed owner lifetime.

Custom `IAsyncTask` construction also remains unsupported. Its base token is a
lazy process value; its diagnostic method returns a nontrivial native String
through AArch64 X8. Native allocation, destruction without execution, posting
failure ownership, and execution-before-post-return must all be matched. The
retained absolute addresses are evidence anchors, not newly callable entries.

Even an inline facade wrapper can submit lower-level engine work. The loop
backend now correlates the verified descendants; this does not establish a
general acknowledgement path for other operations. Working-property notifications cannot
prove which SDK request caused a matching value. Saving cues also needs the
separate native persistence request/reply path; setters do not prove saving.
Slip/quantization/loop/minimum-length enums and RGB channel packing require
individual validation. No command is enabled by this document.

The graph project was not indexed for the relevant core scopes; graph lookups
and coverage returned `project_not_found`. These are bounded source/binary
findings, without an exhaustive graph or a runtime certification claim. The
original Ghidra project was not opened or changed for this work.

## Regression fixtures

Run from the core repository:

```sh
bash tests/sdk-native-commands.sh
SAN=asan bash tests/sdk-native-commands.sh
SAN=tsan bash tests/sdk-native-commands.sh
```

All three passed on the development host. ASan includes UBSan. The runner also
supports `SAN=ubsan` for a standalone UBSan run. ASan/UBSan and TSan transcripts
are retained under `anchor/output/sdk-completion-native-commands/`.

Each run checks both the production compilation and a compilation with private
fixture inspection enabled. The fixture covers all 26 operation IDs with and
without persistence intent against the disabled adapter, unchanged output on
unsupported callbacks, generation/replacement/exhaustion, wrong-thread
admission and release, failed/throwing predicates, scope release, self-retirement,
retirement waiting for an active reader, 200 repeated heap-object lifetimes
under four competing reader threads, 128 successive distinct thread identities,
and thread-token exhaustion. A synthetic adapter exercises the real
portable engine's begin/end pairing and revalidation after load replacement or
retirement. It never loads or invokes firmware.

Nine opposite-outcome controls must fail the real assertions: false capability
advertisement, missing owner-thread check, foreign-thread release, missing stale
generation guards, missing retirement wait, missing reentrant close, missing
predicate-failure release, generation wrap, and thread-identity reuse. These validate the fixture's
ability to catch the lifetime and capability regressions; they cannot prove a
native backend exists.
