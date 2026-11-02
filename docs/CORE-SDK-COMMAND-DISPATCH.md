# Portable SDK command dispatcher

`src/azmod/sdk_command_engine.h` supplies bounded command policy and synthetic adapter
tests. It contains no EP147 addresses, native pointer registry, hook, worker, or advertised
native capability. The transient loop adapter in `sdk_exit_bridge.c` and `sdk_exit.cpp`
connects this engine to the native loop mailbox. Cue operations use the same public
queue and the native cue control/actor, collection and persistence adapters. All 26
public operation IDs have development implementation paths; functional correctness
is established by actual command tests, not capability bits or source presence.
Operation arguments and native side effects are described in
[the native contract](CORE-SDK-NATIVE-COMMANDS.md). Follow
[the current execution plan](../../anchor/CORE_SDK_IMPLEMENTATION_PLAN.md) for
scope and efficient test selection; no evidence ledger is required.

## Storage, ownership and retry contract

An engine retains at most 32 requests/results, with four records per owner and 32 owner
slots. One record holds its canonical request, copied result, unique internal dispatch
token and state. No operation allocates. Consumer operations use a try-lock and return
`AZ_BUSY` without waiting. Only owner-thread `scmd_step` briefly takes a blocking mutex
when returning from adapter code. Never drive it from playback, drawing, or notification
callbacks that cannot tolerate that synchronization.

Request IDs are scoped to the owner and must increase across new accepted submissions.
The caller must serialize ID allocation **and submission**; allocating IDs atomically and
then submitting concurrently can arrive out of order. An out-of-order fresh ID returns
`AZ_STALE`, a rejected result, and internal detail `SCMD_DETAIL_ID_ORDER` before acceptance.
Failed admission does not consume its ID. Core-issued reservations can replace this policy
later without changing the internal dispatch token.

An identical retained request returns its existing result even if the deadline, load or
capability has subsequently changed. Changed arguments with the same owner/ID return
`AZ_INVALID`, leave the result output untouched, and report `SCMD_DETAIL_ID_CONFLICT`.
Comparison covers known argument values rather than C padding, future record tails, signed
zero distinctions, or bytes after a comment's first NUL.

`scmd_release` explicitly acknowledges a terminal, settled record and frees its slot.
Released IDs remain below a per-owner high-water mark and can never execute again; a late
retry returns `AZ_STALE`. Terminal records are not silently evicted to make room. Another
owner cannot poll, cancel or release a record by guessing its request ID.

## Admission and execution

`scmd_submit` takes a copied `scmd_target` and an adapter. It validates the v1 envelope,
finite numeric arguments, flags, strings, capability, deadline, exact load generation,
applicable revision, persistence intent, adapter-specific arguments and resource limits.
The target identifies which commands require a nonzero expected revision and which always
write saved data and therefore require `AZ_COMMAND_PERSIST`. Operation-specific units,
quantization rules, cue identity and paused/playing behavior belong to the adapter; this
engine does not guess them from the operation number.

An accepted result means queued, with `AZ_COMMAND_ACCEPTED`, reason `AZ_PENDING` and no
resulting state revision. One request per deck can be preparing or dispatched. Requests
are selected by their unique acceptance token, so another owner cannot overtake an older
request on the same deck. Other decks can progress independently.

The adapter supplies four callbacks:

- `arguments(context, request, target)` is bounded, pure validation. It runs under the
  engine mutex during admission and again during owner-thread execution. It must perform
  no I/O, waiting, native mutation, engine reentry or access to unowned native objects.
- `begin(context, request, target)` proves the correct native owning thread and acquires
  its endpoint lease. An inline adapter also acquires the native lifetime/coherence
  lease there; a deferred adapter queues scalars only and acquires/rechecks native
  lifetime, thread and load at actual execution. `AZ_OK` must pair with one `end`.
  `AZ_BUSY`, `AZ_PENDING` or a wrong-thread `AZ_INVALID` leave the request queued. A failed
  `begin` must clean up its own partial acquisition.
- `dispatch(context, request, token, completion)` performs or queues the verified action.
  `AZ_PENDING` means that application has not been proved. `AZ_OK` supplies explicit
  completion evidence. Other returns cannot prove no action, so become uncertainty.
- `end(context, deck)` releases the successful `begin` lease. Callback exceptions must be
  contained by the native adapter; none may unwind through this C engine.

The lease must keep both the native object and its load/revision association coherent.
Holding an arbitrary native pointer, constructor token or `weak_ptr` alone does not prove
that contract. The engine revalidates after `begin` and immediately before its dispatch
claim. The native adapter must also check deadline, generation and applicable revision at
the actual action on the native queue; a queue-entry time is not an execution deadline.

`scmd_completion` distinguishes outcome from whether a native task is settled. Applied
completion requires a matching load generation, a nonzero observed resulting revision that does not precede the owner-thread observation,
reason `AZ_OK`, and `settled=1`. Rejected or cancelled completion must explicitly attest
that no action remains queued. Bad completion metadata becomes uncertain and retains the
deck barrier. `scmd_complete` is a trusted adapter entry and must not be exposed to mods.
A busy completion must be retried from core-owned work rather than lost or spun on an
audio callback.

## Cancellation, deadlines and uncertainty

Before the dispatch claim, `scmd_cancel` returns a cancelled terminal result and prevents
the adapter dispatch. Cancellation while `begin` is acquiring a lease still wins, but the
record cannot be released until `end` has returned. After the claim, cancellation returns
`AZ_PENDING`; it does not promise that playback was unaffected. Already applied results
return `AZ_STALE` together with the unchanged applied result. Repeated cancellation of a
cancelled record is idempotent.

`scmd_expire` cancels queued/preparing requests at their deadline. A deadline reached after
dispatch produces `AZ_COMMAND_UNCERTAIN`, reason `AZ_STALE`. That outcome does **not**
prove that native execution stopped. The engine keeps the deck fenced and refuses release
until a valid, explicit `settled=1` acknowledgement proves the token cannot run later.
Otherwise, a late first action could overtake a second command. The EXIT adapter can acknowledge uncertain settlement after the full owner frame
ends and every remaining correlated execute is permanently suppressed by its
retained token/incarnation guard. Dormant tombstones remain until native destruction;
unknown handoffs cannot acknowledge settlement and keep the barrier for the
remaining process lifetime.

Published terminal results remain immutable. A late acknowledgement can release the fence
but cannot rewrite earlier uncertainty as applied. Consumers refresh live state to assess
what happened. No rollback or sample-accurate scheduling is promised.

## Result publication and integration

`scmd_poll` copies either accepted or terminal state. No borrowed mutable result pointer is
returned. The owner-scoped 64-entry `scmd_event` journal is written under the same mutex
after its corresponding result becomes pollable. Events carry request ID, dispatch token,
revision, receipt timestamp and sequence. A missed-event marker requires polling outstanding
results; it is produced even when no retained event belongs to that owner. The event's
`settled` bit describes publication time; a later drained acknowledgement does not rewrite
the historic event. Receipt timestamps use the core monotonic clock, not native source time.

Consumer records require their complete v1 size; larger buffers preserve caller size and
unknown tails. Input sizes are read as bytes before any complete-record access. Tokens and
journal sequences never wrap; admission reserves enough terminal-event sequence values for
all outstanding records. Destroy only after consumers, native callbacks and owner-thread
work stop and all records have been released.

`sdk_command.c` holds one shared production engine behind authenticated
`sdk_command_poll`, `sdk_command_cancel` and `sdk_command_release` wrappers. The existing
submit wrapper validates the complete request and routes the 26 native-load
operation IDs through the available loop/cue endpoint capabilities. Transient loop,
main-cue and hot-trigger actions use their native control paths; saved cue/loop and
metadata edits use native selection and persistence. Unavailable operations return
a correlated unsupported result; no synthetic adapter is exposed. Unsupported submission publishes no
accepted record or event. Unknown request IDs return `AZ_STALE`. These wrappers retain the
core's module/caller and playback-callback authentication checks.

The appended public service table and SDK harness now expose authenticated polling,
cancellation and release with size checks. The harness accepts the current native-load
and native-cue selector envelopes but keeps synthetic command capabilities zero.
Runtime tests must call the public service and assert the actual native effect,
correct completion/cancellation and persistence where applicable. Result-event
projection and unrelated live-state work are separate from proving these polling
command paths. Do not enable a synthetic adapter as a native implementation.

## Regression tests

Run from the core root:

```sh
bash tests/sdk-command.sh
SAN=asan bash tests/sdk-command.sh
SAN=tsan bash tests/sdk-command.sh
```

The production header is compiled directly. Fourteen named suites cover ABI envelopes,
admission, persistence, owner-scoped retries and replay prevention, revalidation, deadlines,
FIFO ordering, independent decks, in-flight uncertainty, late completion, cancellation races,
lease lifetime, malformed native completion, clock/adapter failures, wrong-thread/transient
retries, quotas, event recovery and concurrent callers.
Seventeen deliberately broken production variants must fail their regression assertions.
A compile-time control appends a request argument and must be refused until validation and
retry comparison are explicitly updated; a C++11 check verifies the native adapter header.
The separate real-wrapper fixture also has eight controls for bypassed authentication,
legacy incomplete argument validation and disconnected poll/cancel/release state.
ASan, UBSan and TSan each have a separately verified planted fault; a compiler that omits
instrumentation fails the runner. These tests execute synthetic adapters only and establish
no firmware call, audible behavior, physical-control ordering or persistence-after-reload
acceptance.
