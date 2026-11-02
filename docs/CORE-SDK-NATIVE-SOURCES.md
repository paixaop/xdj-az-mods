# Native library owner and physical-file capture

`sdk_native_sources.cpp` supplies a private source-observation boundary. It assigns a
non-reused core epoch to an observed native MountPoint/SqliteDataBase owner pair, keeps
genuine GNU weak owners, and can capture the native connection's open-file identity.
It does not establish that a loaded track or loop callback belongs to that epoch.
No public source/load capability follows from a matching media number.

## Integration and lifetime contract

Compile this translation unit with GNU libstdc++, C++11 or later, 64-bit Linux, RTTI and
exceptions. The production physical adapter is enabled only for little-endian AArch64.
The private C declarations are in `src/common/azsources.h`; no native pointers or file
descriptors are present in its output structure.

All mutable state, including the weak owners, lives in a guarded function-local
`SourceState`. This matters because the C `azmod_init` ELF constructor can run before
default-priority C++ global constructors. An early configuration/capture cannot later
be overwritten by delayed global initialization. Configuration disables and enables
capture under the same mutex, so an earlier configurator cannot overwrite a disable
that is still waiting for that mutex. The physical-ready query uses atomic publication.
GNU7 constant-initialized the current aggregate in a negative-control experiment, so
an actual current global-state reset was not reproduced. The function-local object is
constructor-order hardening for future state growth. Its synthetic negative control
adds an explicitly dynamic test-only member and must fail the early-state assertion.

Before installing hooks, call:

```c
azsdk_sources_configure(track_data_enabled, build_id,
    AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER);
```

Build IDs are `EPB_V130=1` and `EPB_V200=2`. The first successful build selection stays
fixed for the process. An unknown build, changed build, or unknown policy bit disables
capture and retires current bindings. Configure can wait, resolve symbols, and hash
481,192 mapped code bytes. It belongs on the startup/configuration worker. Physical
profile preparation is attempted once, so the native SQLCipher module must already
be loaded. Failure leaves owner observation available with physical status PROFILE.

The integration point is inside the existing legitimate `mlserver_onmsg_hook`,
immediately **after** `g_orig_mlserver_onmsg` returns and before leaving the native
message invocation:

```c
g_orig_mlserver_onmsg(server, message, argument);
azsdk_sources_observe_owned_message(server, NULL, 0);
```

The caller owns the complete native receiver/dispatcher chain for the entire call.
It must not call this through a timer, cached raw server, playback/draw callback, or
another worker. The bridge checks nominal GNU RTTI, the native invocation byte at
server+0xd2, a non-null canonical server+0x28 worker pointer, and the actual native
slot-5 `needsToExecuteAsync` method against its exact build RVA before calling it.
That method must return false. The explicit worker-pointer check matters: its native
null-worker error branch also returns false after logging.

These checks constrain an already owned invocation. They cannot make stale or
arbitrary pointers safe, acquire native teardown synchronization, or prove an
arbitrary external direct call to a native virtual method is a normal queued task.

The chain is read afresh after the original handler:

| Link | Verified offset/layout |
|---|---|
| server to dispatcher | 1.30 +0x1c0; 2.00 +0x1c8 |
| dispatcher to handler | +0x20 |
| handler to InformationUpdater | +0x30 |
| updater native shared MountPoint | +8 raw / +0x10 control |
| SqliteMountPoint native shared SqliteDataBase | +0x30 raw / +0x38 control |
| DB MediaID / sqlite3 connection | +8 u16 / +0x38 pointer |

The native MountPoint and DB media words must agree and be nonzero. A NoDBMountPoint,
missing connection, empty shared owner, wrong nominal type, unsupported worker, or
invalid chain is refused. In the valid worker scope, an unsupported/empty chain retires
that server's previously observed binding. An untrusted/wrong-worker invocation does
not change the registry.

No shared owner is fabricated from raw pointers or copied by `memcpy`. The bridge
borrows the stable native `const std::shared_ptr<music_library::MountPoint>&` and
`const std::shared_ptr<music_library::SqliteDataBase>&`, then constructs ordinary
`std::weak_ptr` objects from those references. It never weak-locks or follows an old
native pointer from a reader. Native strong counts are unchanged by core retention.

## Bounded registry and conservative diagnostics

There are 32 weak-owner epoch entries and 32 server bindings. Capture uses a try-lock;
contention returns BUSY. It does not allocate entries on the heap, evict epochs, release
retired weak control blocks, or wait for a registry reader. `azsdk_sources_sweep()`
releases retired/expired weak entries on a background/configuration worker. Call it
periodically there, and after disabling capture. Exhaustion returns LIMIT and marks
the observed media number ambiguous rather than choosing an old or newest entry.

The owner key includes both actual GNU control-block identities, both raw alias
addresses, the connection token and native media word. Equal allocator addresses with
different owners receive different epochs. A replaced DB owner or connection receives
a new epoch even when the MountPoint stays the same. Retired epochs are never revived;
epoch and revision exhaustion refuses publication rather than wrapping.

Two 8 KiB bitmaps retain seen/ambiguous media history for the process lifetime. After a
second distinct observed epoch for a native media word, `azsdk_sources_candidate()`
permanently returns AMBIGUOUS with owner_epoch zero, including after sweep, disable,
or quota pressure. It never chooses the newest mount. The native discriminator cycles
1..255; it is not a core generation.

Every successful sample carries HISTORY_INCOMPLETE. CURRENT means that the latest
observed server binding still names live weak owners; it does not certify physical
presence, a complete mount/unmount stream, initial history, freshness or a track-load
association. An entire lifecycle interval can occur between message observations.
Candidate lookup is diagnostic only, even before ambiguity has been observed.
Its success status is derived from the same copied scalar snapshot returned to
the caller. If a weak owner expires before that snapshot is sampled, candidate
lookup returns UNKNOWN without changing the output. The registry mutex does not
prevent native owners from expiring.

Scalar reads perform no native calls, weak locks or filesystem work. An expired or
retired epoch returns STALE with CURRENT/PHYSICAL and physical values cleared. Once
swept it returns UNKNOWN. Guard/limit/busy/unknown/invalid failures leave the caller's
output unchanged. STALE and AMBIGUOUS return inspectable scalar records. Prefix copies
accept an unaligned output of at least eight bytes and never overwrite unknown tails.

## Optional physical metadata adapter

With `AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER`, the verified normal library worker may
capture small metadata while the native callback still owns the actual DB and file.
It does no ANLZ reads, DB content queries, pathname-based database selection, descriptor
duplication or deferred raw-FD work. It never closes SQLite's descriptor. Only its own
`/proc/self/fdinfo` descriptor is closed.

Preparation resolves public APIs by name from an explicit already-loaded
`libsqlcipher.so.0` handle, validates their mapped module/RVAs, and hashes the actual
live text. Both shipped libraries have `.text` at RVA0xf980, length481192, SHA256
`243c4e20f552c97ee0aa8de4527501d7a3dd5a223e8e01273574017f7afc3944`.
Version-number/text must be 3033000 / `3.33.0`. Hashing uses public unkeyed OpenSSL
exports from that native module's dependency scope. A successful process-lifetime
module reference keeps code/profile pointers valid; it does not keep a DB alive.

In the owned callback, public `sqlite3_file_control(db,"main",7,&file)` and opcode27
return the actual file and VFS. The adapter requires:

- Mapped native VFS version3, szOsFile120, maximum pathname512, one of `unix`,
  `unix-excl`, `unix-none`, `unix-dotfile`, and checked xOpen RVA0x53968.
- Native IO table RVA0xb3d98/version3, 0xb3e30/version3, or 0xb3ec8/version1;
  checked xRead0x1513c, xFileSize0x1bdf4 and xFileControl0x536b0.
- The actual Unix file's embedded VFS at+8 equals opcode27's result before reading
  its signed descriptor at+0x18.
- fstat of that descriptor is a regular file with nonnegative size; bounded fdinfo
  parsing yields a nonzero, nonduplicated, overflow-checked mount ID.
- Native xFileSize and a second fstat agree with the first dev/inode/size; the
  descriptor, methods and embedded VFS have not changed during the capture.

Per attempt, the bridge invokes at most two public file-control calls, one native
xFileSize, two direct fstats, and one fdinfo open/close with at most eight reads and
4096 total bytes. The native xFileSize itself may perform filesystem work. Syscall and
byte counts are bounded; **storage and SQLite-mutex latency are not hard bounded**.
This opt-in policy is specific to the normal serialized library background worker,
not a claim that metadata IO is safe on playback/draw hooks.

Failed physical capture publishes owner/media diagnostics with a reason and no
physical validity. In particular, marker, affinity, missing policy and missing profile
guards make zero SQLite/filesystem calls; VFS/method refusals occur after the required
public file-control calls but before fstat/fdinfo/xFileSize.

The resulting dev/inode/mount-ID scalars identify the native connection's **open
file**, including after its pathname has been replaced. They must be joined to the
existing physical source inventory by this captured identity. An old mount that is
absent from that inventory stays old/stale; a reused path/media number cannot rebind
it. Binding the owner epoch to the actual accepted/committed loaded transaction remains
a separate integration proof. This module never sets loaded-track source validity.

## Evidence and limits

The native contracts were derived from the exact executable SHA256 values:

- 1.30: `736bdc9322c00e5770af459c92cace33d8680825c07f00f909f74dfc473a77a6`.
- 2.00: `6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925`.

Private research is retained outside this source tree in `loaded-source-contract.md`
and `loaded-source-physical-contract.md`, with their exact `.c`, `.asm` and JSON
artifacts. Material worker evidence includes `loaded-source-physical-affinity-130.asm`,
`loaded-source-physical-server-thread-200.c`, the message/thread and mount/unmount task
artifacts. Physical layout/profile values are recorded independently for both builds
in `loaded-source-physical-layout.json` and `loaded-source-physical-artifacts.json`.
They are static ELF/Ghidra evidence, not REA runtime observations or fabricated REA
Evidence IDs. The original Ghidra project was not changed by this implementation.

The core graph project was unavailable (`project_not_found`); exact source and the
cached native evidence were read directly. No exhaustive global native teardown or
write-path coverage is claimed. Normal receive, mount and unmount task serialization
is established; unrelated direct native calls and teardown still require the stated
owned-callback contract.

## Regression verification

Run `bash tests/sdk-sources.sh` on Linux with GNU libstdc++. `SAN=asan` enables ASan/UBSan;
`SAN=tsan` enables TSan. A runtime that cannot start the sanitizer is a failed/unavailable
validation, never a pass or suppression. The script also compiles the production TU
independently of the fixtures.

The fixtures use real shared/weak control blocks and synthetic native-shaped GNU RTTI
classes for both dispatcher offsets. They cover weak expiry and unchanged strong
counts, equal raw addresses/different owners, replacement DB owners/connections,
aliasing owners, multi-server bindings, permanent ambiguity, quotas, nonwrapping
identities, malformed/unsupported guards, prefix outputs, concurrent capture/reads,
and zero native/filesystem calls at guard refusal. Physical fixtures use only a newly
created scratch file; they exercise all accepted IO tables, rejected VFS/method
profiles, bounded fdinfo failures, changed descriptors/methods and renamed path reuse.
Planted controls must fail for marker/affinity bypass, merging raw addresses, ignoring
weak expiry, forgetting ambiguity, bypassing the physical profile, exceeding fdinfo
read bounds and accepting a changed descriptor. A real POSIX record lock is held across
capture and a child must still conflict with it. A planted duplicate-and-close of the
native descriptor must fail this check even though the original FD remains readable.
Control failures require an assertion or sanitizer fixture diagnostic; arbitrary runtime
startup failures do not count as caught controls.
The priority101 constructor regression publishes an actual weak-owner record before
ordinary global initialization and verifies it survives until main. Its planted
forced-dynamic-global-State control must fail its intended early-state assertion. The
suite also extracts the actual production
hook tail and checks that source capture sees the original handler's new owner state;
capturing before the original handler is a planted failure.

An explicitly requested native test can be built from the same fixture and run as:

```sh
sdknativesources-test --native-sqlcipher /usr/lib/libsqlcipher.so.0 2
```

It opens only unique unkeyed `/tmp/az-source-native.*` scratch databases through public
SQLite APIs, then runs the production module-profile/physical adapter before and after
replacing a pathname. It removes and closes its own scratch artifacts on success. Its
C++ ownership/worker model remains synthetic; passing it validates the actual SQLCipher
profile and open-file adapter, not production hook installation or loaded-source binding.

## Private cache ownership work

The current checkout also contains uninstalled cache-provenance helpers and an
register-only AArch64 entry bridge. They are dependencies for native source/load
qualification. They do not advertise that capability or install native targets.
Frozen SDK v2 does not contain this later cache work.

Cache records reuse `SourceState` and retain genuine weak MountPoint, DB,
Collector and cache owners. Scoped query pins release cache before Collector,
so returning reserved pages cannot follow an already freed Collector pool.
Queries copy a scalar while the native caller still owns its actual row result;
the core never copies, moves, frees or retains the owning row carrier. Mutation
fences retire generations before forwarding and stay active through native
exceptions. Source connection/media tuples and real control identities are
checked again after reentrant copy callbacks. Cold cache hits cannot invent a
birth or renew a retired one.

Both-profile synthetic GNU owner fixtures pass plain, ASan/UBSan and TSan. The
final fixture and register bridge separately pass AArch64 checks for eight GPR
arguments, eight FP/SIMD arguments, four FP/SIMD results, two GPR results,
original X8 destinations, native exceptions and
caller-only cleanup of nontrivial 16/24-byte results. These are synthetic
bookkeeping and ABI results. They do not establish native hook coverage,
selected-holder storage lifetime, immutable client assembly causality or a
loaded source association.

Native review found cleanup/disposal calls after factory writes and before
shared-owner publication. Capturing the latest generation at the whole Collector
return is insufficient. The helper now retains a scoped construction receipt:
primary construction requires an unchanged stamp throughout; standard fallback
mode0 requires one successful setup and sixteen successful setters in order,
then an unchanged final-write stamp through native cleanup and publication.
Unknown or nested/foreign sequences decline. A real failing ARM fixture precedes
this fix; both-profile GNU owner, ARM ABI and sanitizer tests now pass, with four
isolated factory fault controls caught. This receipt is not yet connected to
installed native factory entries.

The private connection-incarnation helper separately passes four-context tests,
ARM checks, sanitizers and five fault controls. It admits only observed successful
nonnull opens, retires before close regardless of BUSY/error/zombie outcome, and
refuses overlapping births, stale pointer reuse and counter overflow. Its state
is embedded once in the existing, process-lifetime SourceState. A sealed original
SQLCipher profile is shared by physical capture and lifecycle forwarding. Exact
native modules for both firmware versions pass real memory-only open/open16/
open_v2, BUSY/zombie close and partial-install tests with the actual island
installer. The canonical SourceState bridge additionally passes with physical
preparation both before and after redirects and with diagnostics disabled during
close. The working source now calls the canonical installer once at startup
when track data and native source capture are enabled and
`/home/root/settings/azmod.native-connections.on` exists. Installation remains
off by default; refusal cannot qualify a connection. The accepted723-input core
predates that caller. The new caller and its single-gate refactor pass32 cases
using the actual startup configuration fragment, canonical State, real island
installer and both exact native modules. These tests open only `:memory:`;
EP147 startup acceptance of the new caller is still pending. No library file or
key is used by these tests.

A cache pin does not preserve a `shared_ptr` holder inside a reallocatable native
vector. Native wrappers must resolve fresh bounds and owners inside an actually
owned ListServer scope, use stable local borrows, and perform fresh validation
plus positive selection/lifecycle fencing. The private helper does not acquire
native liveness from pointer checks or epochs. The remaining request/reply and
client-to-load causal bridge remains separate from these cache records.

Connection generation now joins the immutable source key with the actual weak
owners, aliases, connection address and media number. A zero generation remains
an incomplete diagnostic and cannot qualify cache provenance. Sample, weak
identity, binding and counters publish only after native physical work and fresh
owner/tuple/generation validation. A temporary unavailable receipt refuses
qualification without retiring an unrelated source or reclaiming its cache
binding. Real close/reopen at the same address requires a fresh source epoch.
Deterministic actual-State races cover publication, sweep, same-key reuse and
cache reclamation; host plain/ASan+UBSan/TSan and ARM fixtures pass.

The existing onMessage tail now enters a C++ forwarding bridge when source
capture is enabled. It copies a bounded whitelist of scalar arguments before
the original can destroy the native message, calls the original once, and
restores scoped context on normal return or native exception. Existing source
post-observation and playlist post-processing keep their order. Tests extract
the actual hook tail and exercise four contexts on both profiles. Message
sequence, route and selector remain copied diagnostics; they establish no
request/reply or load identity. This new core integration passes a coherent723-input ARM core link, export/strip
checks and binary memory-access lint. A fresh native packet and EP147 runtime
acceptance remain separate requirements.

Focused suites are `sdk-cache-birth.sh`, `sdk-source-reuse.sh`,
`sdk-cache-reclamation.sh`, `sdk-cache-message-bridge.sh`,
`sdk-cache-message-hook.sh` and `sdk-source-connection-native.sh` under `tests/`.
The last suite requires explicitly bound original modules, the real installer
object and a coherent runtime loader/dependency set; it uses only `:memory:`.
None of these dependencies enables native source/load qualification or commands.

The later raw-row helper in `azcacherow.h` is unused by that723-input core target.
Both supplied executables prove a24-byte result with eight-byte owning pointer
slots, separately allocated0x98-byte rows, byte tag at+4 and rawu32 at+0. Only a
unique tag0x23 is copied; first-row/fixed-index assumptions, null/duplicate rows,
missing partial windows and spans beyond the16-row observation quota decline.
Zero is copied as a raw value; TrackID validity remains unproved. Tests preserve
caller-only row/string destruction and four independent contexts in both fixture
profiles. Meaningful ARM RED precedes hostplain/ASan+UBSan/TSan+ARM GREEN and the
same checks after refactoring. The helper neither proves nominal native C++ types
nor acquires carrier/row liveness; source, connection, query, request and load
association remain separate. No production observer invokes it.
