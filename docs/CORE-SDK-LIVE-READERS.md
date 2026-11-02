# Native live-state reader evidence

The private `sdk_live_observer.cpp` bridge captures four native change notifications: raw
play-state flags, quantize, key lock and Sync mode. It is not a completed public live-state service.
The source uses the GNU C++ build and existing owned unit constructor/destructor hooks.
An independent `azmod.native-live.on` opt-in also requires `azmod.track-data.on`, the default
constructor hook address and a successfully installed destructor hook. No new firmware
address from this ledger is installed as a hook. Native commands remain separate.

## Target and analysis coverage

| Build | SHA-256 | Local immutable target |
| --- | --- | --- |
| 1.30 | `736bdc9322c00e5770af459c92cace33d8680825c07f00f909f74dfc473a77a6` | `/private/tmp/az-sdk-rea/EP147-1.30` |
| 2.00 | `6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925` | locally supplied firmware image |

The investigation uses the existing 2.00 Ghidra database, an owned APFS copy opened read-only
with automatic analysis disabled, targeted GNU objdump disassembly of 1.30, ELF/RTTI table
reads on both executables, and one REA Kuna decompilation. Private outputs are in
`anchor/output/sdk-completion-live/`; firmware bytes and reconstructed native implementations
are not copied into this contribution.

REA Evidence `ev_946a3096dea981196365c5b43c29e8681f64ec8948484803402297000ac7cc9c`
identifies the 2.00 checkPlayState procedure at `0x153abd0`, Kuna 1.833, and the exact target
hash. It is a single-function reconstruction while the project export was building, without
whole-program prototypes. Its inferred prototypes are not native declarations. The ordinary
Ghidra/objdump and ELF observations below are additional evidence, not REA Evidence records.
Indirect dispatch, asynchronous ordering, and semantic enum mappings remain limitations.

Core graph indexing was unavailable because the graph daemon could not coordinate its index.
This bounded investigation used the supplied source and binary evidence directly. It does not
claim graph completeness or exhaustive coverage of every playback path.

## Weak ownership and payload contracts

The already verified owning chain is a fully constructed native DjPlayerUnit's pointer at
`+0x1a0` to `dj_player::UnitStateServer`, then that object's pointer at `+0x1f28` to
`dj_player::StateNotificationServer`. The caller owns the unit for the whole registration
call, checks its existing build-specific vtable first, and retires the reader before native
unit destruction. This is an internal ownership precondition; the bridge is not an arbitrary
pointer validation API. Registration checks the two publisher RTTI names, the complete server
vtable range in the main ELF, and executable register-method entries.

| Stream | Server virtual slot | Register, 1.30 / 2.00 | Checker, 1.30 / 2.00 | Borrowed payload |
| --- | --- | --- | --- | --- |
| BeatSyncMode | 2 | `0x1480850` / `0x153bde0` | `0x14877b0` / `0x1542d40` | Four-byte native enum |
| PlayPauseWithState | 15 | `0x1483480` / `0x153ea10` | `0x147f640` / `0x153abd0` | Three consecutive bytes |
| Quantize | 19 | `0x1484120` / `0x153f6b0` | `0x147e7f0` / `0x1539d80` | Zero-extended byte in a 32-bit scalar |
| KeyLock | 21 | `0x1484770` / `0x153fd00` | `0x147eef0` / `0x153a480` | Zero-extended byte in a 32-bit scalar |

The server vtable address points are `0x2897240` / `0x2be9d10`. Both type names are
`N9dj_player23StateNotificationServerE`. The selected listeners have two destructor slots,
callback slot 2 taking a const payload reference, and callback slot 3 taking the same
reference plus an unsigned logical player number. Native dispatch invokes both. Only slot 3
has enough routing information to publish a receipt. The bridge ignores slot 2 and rejects
another player's notification.

Both native registration implementations append a GNU 16-byte `weak_ptr` observer.
Their queued append procedures are `0x14890a0` / `0x1544630` for play, `0x14896c8` /
`0x1544c58` for quantize, and `0x14899e8` / `0x1544f78` for key lock; each addresses
the corresponding observer vector at `+0x1738`, `+0x1878`, or `+0x1918`. They
perform no initial state replay. A submitted registration does not establish that its queued
append has run, nor that any state has been observed. The bridge retains only its own
`shared_ptr` listeners. Native unit, publisher, notification, and track pointers are never
retained. Each callback copies exactly 3 or 4 borrowed bytes, uses lock-free atomics and one
bounded writer claim, and performs no allocation, logging, file access, or mutex locking.
`clock_gettime(CLOCK_MONOTONIC)` records receipt time.

## Exact values and limits

The play payload contains, in order:

| Private field | Native source | Getter, 1.30 / 2.00 |
| --- | --- | --- |
| `mode_is_2` | PlayerState int at `+0x28` compared with 2 | `0x14928e8` / `0x154de60` |
| `play_pause` | PlayerState byte at `+0x2d` | `0x14929b0` / `0x154df28` |
| `play_aux` | PlayerState byte at `+0x2e` | `0x1492a50` / `0x154dfc0` |
| `quantize` | PlayerState byte at `+0x80` | `0x1492988` / `0x154df00` |
| `key_lock` | PlayerState byte at `+0xac` | `0x1492058` / `0x154d5e0` |

These offsets explain the notification construction; the bridge does not read PlayerState
or keep its address. checkPlayState sends this notification when `+0x2d` changes. The other
two play bytes are context copied at that event; their changes alone do not trigger this
checker. This listener cannot establish every cueing/mode transition even after its enum
mapping is understood. `mode_is_2` is deliberately not called cueing, playing, or paused.
The native three-way playback mapping still needs native use-site or controlled UI correlation.
The Master Tempo mapping now has the both-build native evidence described below. TempoSliderLocked, also found during
research, is a lock flag, not a verified playback-rate value. Sync's raw state needs its own
enum contract. Neither is projected into a different field.

Play, quantize and KeyLock scalars must be exactly0 or1 for validity. Sync admits
only the verified native enum0 or2; every other mode is an observed invalid receipt. A malformed
current receipt clears that stream's validity and returns `-1` for its values; it does not
retain the previous valid state as current. Other streams keep their own values and receipt
clocks. `observed_fields` distinguishes an invalid receipt from no receipt, and `valid_fields`
identifies which values were admitted. Revisions and timestamps are independent per stream;
the combined sample is not a single native capture. A collision can temporarily omit a
component from one bounded read.

The monotonic unit generation prevents retired callbacks from publishing into a replacement
unit. It is not a committed track-load generation or source-qualified track identity. Latest
receipts have no built-in expiry: a consumer must check their individual age and identity
association. A registration with no later change can remain unavailable indefinitely.

## Integration and remaining delivery work

The existing GNU C++ build localizes C++ symbols using the same map as the loop reader.
Registration runs from the owned post-constructor hook after unit-vtable and slot checks;
retirement runs before the native destructor. `azsdk_live_capabilities` and the versioned
`azstem_live_observer_mask_v1` report submitted deck bits, not functional acceptance.

`azstem_live_observation_v1` wraps the copied sample in an independently guarded 96-byte
size/version record. The public snapshot appends raw play flags and key-lock fields and
separate timestamps/revisions for play, quantize and key lock. `AZ_LIVE_PLAY_FLAGS`,
`AZ_LIVE_QUANTIZE`, and `AZ_LIVE_KEY_LOCK` each follow only their own stream validity.
Unavailable values are `-1`. Invalid receipts can retain their receipt clocks while their
validity bit is clear. Older snapshot prefixes remain supported. Normalized playback remains
unavailable. The same strict KeyLock receipt supplies `master_tempo` and
`AZ_LIVE_MASTER_TEMPO`, using the existing key-lock revision/time and live unit generation.
An absent, malformed or retired receipt leaves both fields unavailable and -1.
Unit lifetime/receipt values remain independent of committed-load and source identity.
Physical acceptance has not been performed for this new bridge by this worker.

The opt-in `tests/integration/sdk-loop-probe.c` also resolves the two versioned live exports.
It logs each stream when its own revision changes, with monotonic receipt age, including
invalid receipts. A contended sample that omits another stream preserves that stream's
history. The existing matched 96-byte loop diagnostic is unchanged. Each live row is bounded
by a 512-byte buffer, and the probe reserves 16KiB per final tick for all records and footer
under the existing 256KiB/6000-tick limits. It uses no EP147 entry points or new hooks.

The following delivery contracts remain unresolved by this bounded investigation: normalized
paused/playing/cueing; playback position, direction, rate and tempo; retained loop IN/OUT,
reloop availability and loop mode; current main and hot cues; slip, sync, master, and the
remaining key settings. The existing working-loop observer covers a different notification.
Stored analysis cues are not evidence for current native cue state. Motion and pad mode are
not used to infer playback or loop state.

## Regression checks

`tests/sdk-live.sh` compiles the actual private module with a synthetic GNU ELF server,
GNU weak/shared ownership, native vtable slots, and native-style indirect callback dispatch.
It runs without firmware or a device. `SAN=asan` selects AddressSanitizer plus UBSan;
`SAN=tsan` selects ThreadSanitizer. Other hosts explicitly skip the Linux ELF fixture.

The fixture covers early ELF-constructor registration before C++ initialization, all four
logical decks, both admitted build IDs, disabled/unknown builds,
wrong RTTI and nonexecutable methods, absent initial replay, callback 2, wrong players,
every valid play-bit combination, zero scalar values, invalid high bits, exact borrowed
payload extents, independent stream clocks and revisions, caller output guards, retirement,
replacement generations, stale strong leases, native registration exceptions before and after
weak capture, each listener allocation failure, and concurrent publication/read/retirement.
Six opposite-outcome source controls must fail the same fixture: wrong player admission,
retired callback admission, wrong register slot, invalid boolean admission, stale valid-value
retention, and partial owner publication on allocation failure. The ASan pass also requires
a fourth-byte play-payload read to produce a heap-buffer-overflow and a restored fixed-size
RTTI comparison to produce a global-buffer-overflow. A short mismatching RTTI name exposed
that comparison bug; the corrected guard stops at the first differing byte. Passing synthetic checks does
not establish hardware behavior or the unresolved semantic mappings.

Local GNU7/ARM64 checks pass plain live and loop fixtures and the six plain controls.
The corrected live and loop modules pass ASan/UBSan with `ASAN_OPTIONS=detect_leaks=0`;
LeakSanitizer is unavailable under this Docker emulation. TSan compilation succeeds, but
the runtime rejects the host's 47-bit VMA (`unsupported VMA range`), so race-sanitizer
acceptance is still required on a supported Linux runtime.

Portable C regression checks compile the actual versioned export, snapshot projection,
configuration block, unit hooks and live probe logger. Controls catch undersized/versioned
private buffers, snapshot prefix overwrites, shared clocks/validity, normalized playback
inference, missing registration/retirement, bypassed track-data gating, shared probe
revisions, future receipt timestamps and probe file-budget overruns.

The standalone lifecycle fixture releases all of its fake native objects only after the
last assertion and joined thread. The standalone live suite sets no LeakSanitizer
suppressions. Portable C tests and all twelve integration controls pass plain and
ASan/UBSan; GNU7.5 also compiles the opt-in probe with and without matched diagnostics.

Both native observer opt-in flags are atomic because the optional probe can query the
private exports during ELF initialization. A fifth C fixture extracts the actual two
declarations and all five loop/live exports, runs one configuration writer and separate
loop/live readers, and checks bounded copied outputs. TSan controls restore each flag to
a plain int and require a data-race report; these controls need a supported native runtime.

The GNU7 live fixture intentionally replaces global allocation functions to test each
listener allocation failure. Nothrow scalar/array forms share that same failure budget,
with a deliberate bypass control. GNU7 whole-archives its static sanitizer allocation
operators, which conflict with these valid replacements. Native sanitizer runs for this
fixture therefore use matching private shared GNU runtime DSOs in the unique test bundle,
with separate clean/fault preflight and exact runtime hashes. Production code is unchanged;
allocation failures and sanitizer diagnostics remain tested without suppressions.

## Verified Master Tempo setting mapping

Both exact firmware binaries contain an actually registered
`usecase::deck::StateController::MasterTempoSetter`. Its second base is
`IKeyLockNotificationListener` at +0x10; the native owning constructor registers
that base through StateNotificationServer slot21. The callback maps the borrowed
32-bit notification1 to MasterTempo1 and0 to0, then calls specifically named
`Setting::updateItem<application_setting::MasterTempo>` code. The inverse setting
callback compares MasterTempo with1 and invokes the verified
`KeyControlFacade::setMasterTempo(bool)` path.

The KeyLock callback is0x97a5d0 on1.30 and0x9bfc00 on2.00. The inverse setting
callback is0x9796f0 /0x9bed20; the signature-bound control implementation is
0x1380990 /0x143bbb8. These are evidence coordinates, not installed mappings or
new native calls. Full SHA-bound RTTI, registration, complete function assembly
and exact template/control strings are retained under
`anchor/output/sdk-feature-goal-20261028/live-research/MASTER-TEMPO-FINDING.md`.

The public projection adds no native read, hook, queue or lifetime mechanism.
`AZ_LIVE_MASTER_TEMPO` is advertised with the existing native-live field
capabilities and becomes valid per snapshot only after its strict0/1 KeyLock
receipt. Its receipt uses `key_lock_revision`, `key_lock_observed_ns` and
`live_unit_generation`. Registration has no initial replay. Static setting
semantics are established; actual controls/audible behavior, physical acceptance
and source/load coherence remain open. The four-deck C fixture checks on/off,
absent/malformed values, independent receipt clocks, old prefixes and disable.
Two opposite controls bypass strict validity or use quantize as the value.

## Independent Sync enabled setting

The fourth component reuses the same owned unit, GNU weak listener and generation
retirement as the original three streams. StateNotificationServer slot2 registers
`IBeatSyncModeChangeListener` on both admitted builds. A native registered
`SyncModeSetter` consumes that exact four-byte mode and updates the application
Sync setting; its inverse sends mode0 for off and2 for on through the signature-bound
`BeatSyncControlFacade::setSyncMode(BeatSyncMode)`. The SDK conservatively admits
only0/off and2/on. Mode1 and other bit patterns preserve their receipt clock/raw
value but clear validity. This field describes the enabled setting, not active
beat lock or master selection.

The existing88-byte private `azsdk_live_sample` and96-byte
`azstem_live_observation_v1` remain unchanged. `azsdk_sync_sample` is a separate
32-byte internal record; the new40-byte `azstem_sync_observation_v1` export adds
size/version guards and preserves caller tails. The public snapshot appends
`sync_unit_generation`, `sync_revision`, `sync_observed_ns` and `sync_raw_mode`.
`AZ_LIVE_SYNC` requires a valid mode receipt; otherwise `sync` is-1. Its own
clock and unit lifetime never become a source/load association. No initial replay
is inferred from submitted registration.

Exact both-build ownership, dispatch, payload, setting and enum evidence is in
`anchor/output/sdk-feature-goal-20261028/live-research/key-shift/FINDING.md`.
The GNU fixture covers both profiles/all four logical decks, native slot2,
wrong-player/callback2 rejection, strict mode admission/raw bits, no initial
replay, independent clock, malformed-value clearing, retirement/replacement,
allocation/registration exceptions, concurrent readers and the legacy ABI. The
ASan short-payload control requires a fourth-byte overread diagnostic.
Physical Sync controls, actual callback acceptance and source/load coherence
remain separate validation obligations.
