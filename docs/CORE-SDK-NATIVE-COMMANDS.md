# SDK native command boundary

Transient loop operations 1–12 use the shared asynchronous native backend for
both firmware profiles and start by default. Explicit `azmod.track-data.off` or
`azmod.native-load.off` disables the command backend at startup.
`azmod.native-loop-commands.off` disables the ten control/adjustment operations;
EXIT and RELOOP have separate `azmod.native-exit.off` and
`azmod.native-reloop.off` diagnostic switches. Check each operation's capability bit.

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
cancellation and correlated Slip/BeatSync/Quantize descendant tracking.
Use `command_submit_async`, acknowledge the validated terminal callback, then await
`RELEASED`; legacy `command_poll`/`command_release` remain compatibility-only. `ACCEPTED` can span
processing frames. `APPLIED` requires settled descendants and the operation's
checked postcondition on the same load; clamped/no-op or partial adjustments
cannot count as successful exact edits. Its revision is the shared transient
confirmation counter, independent of deck, loop and database revisions.
Cancellation after dispatch cannot undo an audible operation. Native command
behavior is exercised through the public SDK in the firmware emulator; see the
[SDK integration tests](CORE-SDK-AUTOMATION.md). Qualify physical behavior separately.

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

Cue adapters and their required native-load/snapshot services start by default on
supported firmware. `azmod.native-cue-commands.off`, checked before startup,
disables cue commands even if the old `.on` flag remains. Firmware identity,
constructor/destructor admission and per-operation capability checks still apply.
MAIN_SET, MAIN_RECALL and HOT_TRIGGER use the native
control facade. HOT_SET/CLEAR, MEMORY_SET/DELETE, SAVED_SAVE/DELETE/AUTO and cue
comment/color updates use `sdk_cue_commands.c` and `sdk_cue_persist.c`;
saved/memory recall uses `sdk_cue_recall.c`. The public bridge dispatches these
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

### Explicit-position cue creation

Commands 27/28 use the same actor and persistence queues. A fresh stored-Cue
value carries source-rate sample coordinates, cached microseconds, empty seek
variants and bundled color/comment. The existing native converter constructs
source-qualified properties and compressed-format seek accessors. The rate
comes from the owned PlayerState source-position clock at +0x18. Native
getPositionTime at 1.30 `0x1493688` and 2.00 `0x154ebc8` uses that value to convert
its selected sample position into microseconds. UnitStateServer publishes it
through the same Core sampling-rate getter used by stock point construction:
`0x135e1d0` on 1.30 and `0x1419538` on 2.00 return Core+0x68. No constant rate,
file metadata or musical key word supplies this clock. No playhead or working
cue supplies the target position.
The builder selects the nearest actual source frame, with half-frame ties
rounded forward. Its microsecond cache uses integer ceiling of that frame's
time because the native serializer floors cached microseconds back to samples;
truncating both representations can move a target more than one frame early.
The optional native-cue snapshot tail publishes this same creator clock,
including on an empty cue bank. Original-prefix callers remain supported;
bounded copies preserve unknown tails. A changed source clock refreshes a
cached snapshot even when its cue collection revision is unchanged. The
POSITION engine clock is reported separately; equality is never assumed.

The native point serializer can leave an arbitrary unused OUT sample cache
when OUT microseconds carries the absence marker. Typed comparison first
checks endpoint presence and ignores that absent endpoint's sample cache;
valid loop endpoints still require their declared rate and timestamp checks.

Creation requires the exact native cue revision. Both owner admission and the
repository helper reject occupied Hot slots, duplicate Memory positions and
the ten-Memory limit. Confirmation checks the correlated receipt and a typed
comparison of both pinned baseline collections, allowing exactly the added cue.
The stock Property-to-Cue serializer drops labels. Creation restores the request's
UTF-8 comment and RGB on its fresh native handler value before the single PUT,
using the existing native metadata constructors. Confirmation checks the request
metadata explicitly and accepts only the native Memory palette index for that
RGB. Inactive tag-zero seek storage is not a value; active seek variants and all
other baseline cue fields retain their typed checks. Live Hot hydration keeps
the original labelled Property after checking its canonical source coordinates.

Hot creation then uses the existing owned-property DSP handoff to update its
playable slot. It invokes the native property-update method, vtable slot 7, of
the validated CueProperty / CuePropertyWithBufferLock types. The two entries
are byte-verified by the cue profile: 1.30 `0x13e6340` / `0x13edcd8`, 2.00
`0x14a1780` / `0x14a9118`. Cue profiles are validated and cached before passive
status installs its observation hook on the same update entry. This copies the property, updates its revision/dirty
state and refreshes buffer locks. It does not call CueLoadFacade::call, which
also cancels unrelated control state. DSP completion requires the requested
slot and color, unchanged main/working cue bounds, loop engagement and
transport, with no correlated seek. Storage success followed by failed Hot
hydration is UNCERTAIN rather than a full application result.

New Hot preparation retains the manager-scoped owned property without borrowing
CueLoader's optional lazy recall cache. Dispatch remains inside the registered
unit's DSP lifetime lease: the unit/Core/ControlFacade bindings, native load
generation, property source ID, owner affinity and typed target method must
match. MEMORY_RECALL and SAVED_RECALL retain their separate CueLoadFacade checks.

HOT_CLEAR accepts an optional exact native-cue-ID revision fence before its
first DSP stage. Existing callers without that optional fence remain valid.

## Owner lifetime lease

`az_owner_gate` supplies the owner-thread lifetime lease used by the native backend:

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

The backend validates the thread predicate, native queue lifetime, load identity,
argument encodings and operation completion separately from the lease.

## Binding maintenance

Regenerate the cue and EXIT/RELOOP verification tables against the exact
supported stock ELFs with `tools/sdk-cue-profile.py` and
`tools/sdk-exit-profile.py`. Validate lifetime, owning-thread dispatch and
the resulting native postcondition for each operation. A queued task number
or a generic state notification cannot establish application.

## Regression fixtures

Run these checks from the core repository.

```sh
bash tests/sdk-native-commands.sh
SAN=asan bash tests/sdk-native-commands.sh
SAN=tsan bash tests/sdk-native-commands.sh
```

The runner also supports `SAN=ubsan` for a standalone UBSan run.

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
