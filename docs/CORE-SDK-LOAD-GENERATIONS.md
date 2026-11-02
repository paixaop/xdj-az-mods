# Native load-generation observer

`sdk_load_observer.cpp` is an opt-in private observer of native load attempts and
matching terminal receipts. It does not start a load, decode TrackInfo/LTI,
qualify a library/source/media session, or enable SDK commands. No public
capability is claimed by this module. Device acceptance is still required.

## Verified boundary

The bounded evidence and full ID-flow report are retained at
`anchor/output/sdk-completion-native-commands/LOAD-TAP-REPORT.md` and the adjacent
SHA-256-checked ELF RTTI/assembly. Supported artifacts are EP147 1.30
(`736bdc9322c00e5770af459c92cace33d8680825c07f00f909f74dfc473a77a6`)
and 2.00 (`6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925`).

A fully constructed owned `DjPlayerUnit` owns its facade bundle at `+0x190`;
bundle[0] is the complete TrackLoadFacade. Its primary slot 2 is load, primary
slot 3 is unload, and its ITrackUnloadable secondary subobject at+8 has a distinct
slot-2 adjusting unload implementation. All three are intercepted. The native
load entry passes TrackLoadOption by reference in X2, listener in X5.

The real completion listener is `LoadController+0x150`, slot 2. The `+0x120`
listener is onTrackInfoStockCreated(WherefromRef) and has a different signature.
Both completion task RTTIs canonically name `const dj_player::LoadRequestID&`;
`LoadID` in the pretty-function string is that same type. The native option's
polymorphic 16-byte prefix contains an atomic u32 ID at+8. Explicit native option
copies preserve that ID through unload/start, handler work and queued facade
completion. The actual terminal callback receives it before the controller
discards it for the later GUI event.

| Entry compared against the owned vtable | 1.30 ELF VA | 2.00 ELF VA |
| --- | --- | --- |
| Primary load | `0x13b9790` | `0x1474e10` |
| Primary unload | `0x13b9720` | `0x1474da0` |
| Secondary unload | `0x13b9a38` | `0x14750c0` |
| Completion thunk | `0xa35690` | `0xa7b4a0` |

Production compares these exact addresses plus the main image's load bias to
entries obtained from genuinely owned objects. A different main-image RX entry
declines. These constants are verification data; native calls forward through
the checked original vtable entries. GNU fixtures inject their own owned virtual
entry profile only under `AZSDK_LOAD_TESTING`.

## Installation and lifetime

The C lifecycle is wired in `deck_track_map.c` and `azstem.c`. Startup enables the
private tap only when the normal DjPlayerUnit constructor hook and destructor
hook both installed, the constructor address is the verified default, and both
`/home/root/settings/azmod.track-data.on` and
`/home/root/settings/azmod.native-load.on` exist. Configuration receives the same
identified EP147 build ID and `sdk_patch_slot` from `sdk_vtable_patch.h`.

The atomic `g_sdk_load_on` flag gates registration from the existing verified
post-original-constructor context. Both deck copies must agree, the unit must
own that deck's slot, and its vtable must equal the known build's primary unit
vtable. The destructor retires the matching logical deck before calling native
teardown. Loop/live observer guards remain intact. `sdk_export.c` includes the
private `sdk_load_export.c` implementation; it must not be added as a second core
translation unit. The standalone C fixture still compiles it separately.

Configure once on the management path before construction, supplying the core's
ordered vtable installer. It must compare the current slot with the prefilled
verified original, preserve that original, then release-store the hook. A changed
slot declines. Every original is atomically published before calling this
installer. Every installed hook forwards the exact original arguments, including
after disable, refusal or a partial installation. Capture requires a successful
acknowledgement for every relevant slot; a hook installed without acknowledgement
remains forward-only. No native code bytes or native containers are modified.

Registration validates the complete nominal unit/facade RTTI and offset-to-top;
the parent additionally checks its existing build-specific unit vtable. The
caller owns the complete object graph for registration and owns the deck/unit
association. Retire before native destruction/reuse. Stored unit/facade/listener
values are opaque comparison identities, never followed outside a genuine owned
registration or callback. The module and original-function storage outlive every
intercepted native call; these are process-lifetime interceptors.

The completion listener is discovered from a genuine native load argument and
validated as the complete LoadController's `+0x150` secondary table. Its slot is
installed before forwarding that load. First discovery invokes the supplied
installer and can perform setup work; the bounded capture path itself uses no
allocation, waiting, native task or persistence operation.

## Correlation and publication

Every new observed attempt/unload/retirement atomically invalidates the current
epoch before trying the bounded capture flag. Pending identity is recorded before
the original load call; inline completion and execution-before-post-return are
allowed. Nothing rewrites pending identity merely because that call returned.
Only result zero with the same native ID, unit registration and invalidation
epoch can publish a separately assigned nonzero core u64 load generation.
Duplicate or late A/B/A receipts cannot consume a different pending ID.

Native ID0 is a valid first value. A persistent per-logical-deck maximum rejects
reuse, backward IDs and wrap even across unit retirement/configuration. An owned
attempt whose ID cannot be read permanently blocks that deck's uniqueness
history. Epoch/counter exhaustion also refuses availability; no counter wraps.

An immutable 16-entry listener reservation table survives retirement and
configuration. Each bounded scan uses a strong compare/exchange on its first
empty entry and inspects that same entry on failure before advancing. Entries
never change, so two contenders for one listener cannot reserve different slots.
A cross-deck alias invalidates and permanently blocks both logical decks before
the second original can emit a same-ID callback. Capacity exhaustion blocks the
unrecordable association. Pointer reuse cannot silently transfer an old listener
to another logical deck.

Snapshots use atomic scalar publication and sequence checks. Acquiring any newer
sample field also orders its preceding odd writer sequence before the reader's
final check. Readers recheck the invalidation epoch, current unit generation,
validity and configuration after copying. A lost capture or a concurrent unload
cannot let an old completion republish a usable receipt. Disable/invalidation is
repeated under the serialized management lock so a waiting disable wins over an
earlier enabling configurator.

Unit-generation values are this observer's registration identities, not another
observer's counter. A successful read records the native API receipt, not a
source-qualified metadata join or physical-media causality. Missing callbacks,
NULL/wrong listeners, suppressed native result4, stale native stage guards,
allocation/post failure and queued-work replacement leave no usable generation.
Coverage of unobserved native entry modes and runtime acceptance remains a
separate obligation before publishing a capability.

## Regression fixtures

The private `azstem_load_observation_v1` C export has a 48-byte size/version/sample
record in `azloadexport.h`. It reads the raw four-byte size prefix before any
version/full-record access, returns0 on a copied receipt or-1 without changing
output, and preserves caller size and an unknown tail. It calls the real hidden
`azsdk_load_read`; there is no second enable flag. Its C fixture checks an actual
four-byte allocation, all shorter sizes, larger/newer buffers, versions/decks,
absence, exact sample copy and tail guards. The missing-size-guard control must
produce an ASan heap-buffer-overflow. This C portion runs before the platform skip.

`bash tests/sdk-load-lifecycle.sh` extracts the current constructor/destructor and
startup configuration code. It exercises original-first construction, wrong
unit/deck and occupied-slot rejection, pre-teardown retirement, disable gating,
both build IDs and all settings/hook-success combinations. Eleven controls remove
those registration, ownership, ordering and configuration guards. `SAN=asan` and
`SAN=tsan` instrument the same focused C fixtures; the LoadState snapshot stress
checks remain in that pass with the new load stubs. Retired fake objects stay
mapped through all checks and joined threads, then are released. A GNU/Linux LSan
control deliberately omits that cleanup and must report leaked allocations.

Run the parent installer's checks separately with `bash tests/sdk-vtable.sh`
(and `SAN=asan` / `SAN=tsan`). They exercise
`src/azstem/sdk_vtable_patch.h`'s expected-original comparison, ordered replacement,
protection restoration and forward-only rollback, plus redirect-lint coverage.

Run `bash tests/sdk-load.sh` in GNU/Linux with g++ and the core's native C++ map;
`SAN=asan` adds ASan/UBSan and `SAN=tsan` requests TSan. macOS skips the ELF fixture.
The private header also receives a C11 compilation check. These fixtures perform
real virtual dispatch through synthetic native classes; they never load EP147.

The fixture exercises both build profiles, before-return completion, missing and
failed receipts, same opaque track/reload, late/duplicate A/B/A, primary and
secondary unload, wrong listener/interface, retired/reused identities, persistent
ID wrap/reuse, contention, deterministic publication-vs-unload/retirement races,
1000 writes under three concurrent snapshot readers, counter/epoch exhaustion,
all four installer decline/partial/mismatch stages, changed main-image RX
entries, unknown-ID barriers, concurrent configure ordering, and sequential and
concurrent same-listener/same-ID ambiguity with late callbacks after refusal.

Opposite controls remove the completion offset/ID/result guards, native-ID
monotonicity, completion consumption, unload/retire invalidation, installation
acknowledgement/original ordering, exact native-entry check, serialized disable,
unknown-ID barriers, shared-listener invalidation and bounded RTTI comparison.
Every control must fail its actual fixture assertion; the RTTI memcmp control
must produce an ASan global-buffer-overflow.

Development GNU ARM64 tests use the pinned `azmod-test:18.04` image. Under its
QEMU environment ASan must use `detect_leaks=0`; those results include ASan/UBSan
and explicitly exclude LSan. TSan compilation succeeds but execution reports
`unsupported VMA range` (47; supported39/42/48), so this environment supplies no
TSan runtime coverage for the ELF observer. Logs are retained under
`anchor/output/sdk-completion-native-commands/`. In-process/deck validation is
owned by the integration task and is not represented by these synthetic tests.
