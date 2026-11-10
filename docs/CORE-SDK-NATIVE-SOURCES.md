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
association remain separate. The current selected-cache adapter now invokes the helper inside its owned query scope.

### Loaded USB track qualification

A loaded-track status subscription starts the storage worker independently of explicit tickets.
It requires an observed, complete `LOAD.track` for the active deck epoch before opening a
database. The worker shares parsing for decks at the same qualified reference and captured
file revision. The separate observer compares SQLite `data_version` on a persistent read-only
connection, including when committed data is in the WAL. A changed token proves committed
activity. The intervening row values and number of commits remain unknown, so affected watches
receive capture loss before the new baseline. Linux directory notifications detect replacement
and overflow while parsing is delayed. An in-place write with no authoritative commit witness
withdraws the affected analysis until a completed replacement can be established.

Each status reader verifies its own SQLCipher main file descriptor against the
source's mount, device and inode. It registers that descriptor with the built-in
library workers for the lifetime of the connection, so their eject detectors
count the firmware's readers correctly. A confirmed deleted or ambiguous row
withdraws the track reference. The existing pack cannot bind a replacement
occupant of that content ID.

The deterministic and autonomous cases in `tests/sdk-status-store.sh` supply qualified test
bindings. They cover duplicate IDs on two sticks, two decks sharing a source, delayed older
binding cuts, rollback, live WAL commits, source retirement, DAT/EXT isolation and saved PWAV
bytes. The Linux cases additionally cover two atomic renames back to identical contents and
notifier overflow. The worker publishes the saved records. Current projections and associations between
separately replaced DAT and EXT revisions still require independent evidence.

Focused Ghidra work on the exact 2.00 image above recovered these relationships. Addresses are
ELF virtual addresses. The matched 1.30 entries for the builder, handler, stocker and load
controller were located using their diagnostic strings and symbol-bounded ARM64 disassembly.

| Owner | 1.30 | 2.00 | Observation |
|---|---:|---:|---|
| PlayMusicPackBuilder::requestLoadInfo | 0x1839108 | 0x1994208 | The 2.00 builder copies pack+0x30 into the receiver's pack ID and passes the track scalars from pack+0x38/+0x40 to the request handler. |
| LoadInfoRequestHandler::doRequest | 0xa26fd8 | 0xa6c418 | The handler invokes its linked music-library provider with the copied track and the receiver's weak owner. |
| LoadInfoRequestTicket request construction | 0x10b2dd0 | 0x1142fd8 | The 2.00 local-list path creates 0x2102 with argument 1 taken from the ticket's TrackID+8. The following 0x3000 list query obtains its own atomic message sequence at 0x1142a88. |
| TrackInfoStocker::onDataArrived, LoadTrackInfo overload | 0x184c050 | 0x19a7150 | The stocker can enqueue an owned copy. The 2.00 consumer at 0x19a6db0 locates a pack by the supplied pack ID, then selects a builder. The shared Reception can also dispatch to a LoadableState receiver. |
| PlayMusicPackBuilder::setData, LoadTrackInfo overload | 0x1851390 | 0x19ac490 | On 2.00, 0x199b258 dynamically casts the owned Wherefrom to the PlayMusicPack. The builder checks the load-info validity bit before copying into that pack. |
| LoadController::doLoadTrack | 0xa39fc8 | 0xa7f688 | On 2.00 the controller resolves the pack using its current Wherefrom and copies pack+0x38/+0x40 into the TrackInfoToLoad argument. The facade receives that argument, the separate load option, Wherefrom and controller+0x150 listener. |

On 2.00 the request dispatcher at 0x1368430 sends 0x2102 to 0x135d5c0. It passes argument 1
to a LoadTrackInfoListCacheCreator at 0x1307fc0. The subsequent 0x3000 call at 0x135ff08
queries the selected cache at 0x1303bb0. The server emits its row and end records with the
specific incoming sequence and player route. ReplySender at 0x1357330 forwards to
IMessageSender. DataStream's task at 0xf00098 records duplicate request ID and player pairs.
Its sender at 0xeff9b0 selects a communicator by player number. The UDSCommunicator branch
at 0xef5010 serializes a message for the connection.

The status source observer follows the local member-zero UDS requests, their serialized
wire sends, and the separate decoded packet identities at the server and client receivers.
The live library server must match the actual receiving packet. Its positive source epoch
includes the observed SQLCipher connection incarnation. The server sends its reply under
that request's scope. The client receiver must forward its decoded packet to the exact
LoadInfo ticket. Its returned 0x3000 continuation must be the request that reaches the
sender. Both requests must use the same source, sender and wire communicators. The native
weak-receiver guard checks the selected ticket, checked promotion return PC and actual X21
strong owner at the Reception invocation.

On the stocker worker the commit is observed after LoadTrackInfo is copied and before
the builder advances. A matched native player request carries the core-issued pack token
only if its captured track identity and Wherefrom are still current. The worker checks
the actual native connection's device, inode and mount ID against both physical USB ports,
resolves the unique row in a read-only transaction, and rechecks the source before
publishing LOAD.track. Retirement, capture contention or replacement revokes a claim.
The saved worker revalidates each result against the same deck epoch and qualified reference.

In both emulator profiles the first load of the local track with row ID 36 used initial
sequence 23 and continuation 24. On 1.30, native loads 53, 55, 61 and 59 shared one prepared
pack across a same-track reload and overlapping decks. On 2.00, three loads selected rows
36, 88 and 36 in order. The second row used a different pack and the return to the first
row reused its original pack. These traces establish the native route. The host regression
in `tests/sdk-status-source.sh` rejects a different source, client packet or continuation,
an unqualified peer/receiver, a retired pack and a lost receipt. Physical sign-off still
requires a disposable two-USB fixture with duplicate content IDs on both firmware profiles.

The normally loaded no-tick status mod on candidate 03.3 subsequently received the
qualified USB1 reference on both emulator profiles. After its load cut it received row
36's file-backed values, including duration 206 seconds (one-second precision), 418 saved
beats, 19 saved phrases and a 400-sample PWAV preview on 2.00. The 1.30 trace also
qualified eight saved hot and eight saved memory entries. On both emulators,
all four logical decks loaded row 36 from the same physical USB1 incarnation.
Each kept its own load epoch and received the 418-cell current and saved grids
and both 19-entry phrase lists. The 1.30 attempts were 53, 56, 59 and 62.
The attached physical unit ran the 2.00 EP147 executable
(`6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925`).
On candidate 03.3 its normal USB1 load of Abarájame qualified row 2303
at sequence 34 and published its 260-second duration at sequence 35.
The reference and its file fields became stale at sequence 38 while the
same native load remained mounted. Recovery after the observer fix is
verified on emulated firmware. Physical recovery remains unverified.

The format string follows the integral OneLibrary `content.fileType` in the qualified row.
MP3, MP4, AAC, FLAC, ALAC, WAV and AIFF have documented codes. An unknown code or an
export lacking the column makes only format unavailable. In the retained genuine export,
a read-only SQLCipher query and an independent audio-stream probe agreed for all 91
code-4 tracks (AAC, MP4 audio sample entry). [OneLibrary model](https://github.com/dylanljones/pyrekordbox/blob/master/pyrekordbox/onelibrary/models.py)

### Source retirement and two ports

On the 2.00 emulator with two separate exports, the same ID 36 resolved on USB1
and USB2 to different library IDs and source generations. Browsing USB2 left the
load from USB1 intact. Disconnecting the unrelated stick preserved the other
load. Disconnecting the active stick withdrew the saved components. After both
sticks were reinserted, the firmware still held its SQLite descriptors on the
old, detached mount IDs and reused its original pack for one load. Those
loads could not claim the newly mounted files. A replacement needs a fresh
native origin that matches the new mount.

The status observer records distinct successful SQLite open results even when
two USB library workers open their connections concurrently. A close preceding
the return of an overlapping open prevents a new status receipt. Closing one
connection revokes it at entry and preserves independently open connections.
Each deployed revision can recover only the original source incarnation after
temporary loss of a physical observation. The stricter legacy cache and explicit
ticket readers retain their original contracts.

### Current structure used by the firmware

On 1.30 and 2.00, the local player model calls a single setter with the actual
active pack's phrase member at +0x120 and grid member at +0xd8. The calls are
at 0x1f9ba64 and 0x2108614, respectively. The setter installs both counted
holders in `gui::TrackStructureData` at +0x60 and +0x68, and advances its
change counter. The corresponding clear calls are at 0x1f9b958 and 0x2108508.

`PhraseMeterWidget` methods 0x1bff1e0 and 0x1d68968 consume that pair.
They index the grid through the native phrase's start and end beat numbers.
They convert the offset grid sample positions using the grid holder's integral
rate at +0x40. The core observes the pair setter independently of widget
visibility, takes counted native content references for the handoff, then
checks the pair against the actual active pack and load before publication.

The 91 genuine export triples have PQTZ only in DAT and PSSI only in EXT.
The PMAI header is identical across all 273 files. The 2EX companions contain
waveform tags and device information. The read-only native SQLCipher query
found NULL `content.analysisDataUpdateCount` for every row, including 36 and
88. These files establish separate saved arrays. They do not record which grid
revision an independently replaced PSSI came from. The status provider leaves
the saved grid association unavailable on this source.

### Other source families

A normally approved no-tick mod loaded the same audio file through folder browse
on both emulator firmware versions with only the genuine legacy `export.pdb`
and `exportExt.pdb` in the library directory. The stock firmware showed the
folder browser and accepted the file. Both reported native ID 72, title,
artist, load completion, current tempo and live hot cues. Loading the same
audio from OneLibrary returned ID 36. The PDB-only case had no qualified
`LOAD.track`, FILE metadata, saved cues, saved grid, saved phrases or overview.
A MusicLibraryServer diagnostic on 2.00 reported that its mount point could
not be created. This test covers a playable folder file on a PDB-only stick,
not a successful legacy library-row lookup.

The matched load's native MediaID can identify an origin independently of any
file reference. The typed firmware formatter inside the Beatport download
owner is at 0xbdd590 on 1.30 and 0xc58cf0 on 2.00. It reads the link-member
kind at byte 0, the storage interface from the low five bits of byte 4 and
the slot from its next three bits. The signed-in source paths use distinct
cloud and streaming interface values. Target-specific code checks the Spotify
interface only on 2.00. The core publishes this classification only across a
matched load with the same contained Wherefrom at completion. This does not
qualify a database connection or a row.

| Origin | Native source evidence | File scope and validation |
| --- | --- | --- |
| Local USB1 and USB2 OneLibrary | Both firmware profiles have USB1 and USB2 panel loads in emulator traces. On 1.30, the stock TrackID independently identified USB2 before the listener obtained its qualified FILE reference. The same row ID on both 2.00 ports resolves to two different libraries. | Exact physically matched native connection, mount, source generation and committed row required. |
| Local USB PDB-only folder | Both emulator profiles completed the folder load with ID 72. The MediaID classifier has separate USB slot fixtures. | No OneLibrary row or saved projection. The 2.00 signed-out source screen offered Cloud Log In as a separate option. |
| Linked device / wired rekordbox library | Both stock firmware profiles completed a network USB load from a virtual player offering a genuine export. The native classifier also distinguishes all-in-one units, wired rekordbox PCs and network devices on a disk/library interface in profile fixtures. | The remote loads supplied a native LINKED source without a local file reference. Wired rekordbox classification has synthetic profile coverage. |
| Streaming | Beatport, TIDAL and Apple Music interfaces are present on both images. Spotify is decoded only on 2.00. The firmware's named download path can stage media to a local file. | The staged download is not an exportLibrary row. Signed-in catalog playback and per-field native coverage are untested. |
| rekordbox Cloud | Both profiles distinguish its storage interface from local USB and streaming. The signed-out 2.00 panel displayed a login prompt. | No qualified saved reference or signed-in cloud-load trace. |

On 1.30 a separate cold PDB-only run classified that folder load as USB1.
Its native current grid had 418 cells and its current phrase list had 19
entries bound to that grid. Its file reference and saved arrays remained
unavailable. The paired model assignment was followed by the firmware's
typed phrase content notification. A dedicated approved mod with only a
1-Hz PHRASE_TRANSITION watch observed forward traversal 0→1→2, reverse
traversal 2→1, a loop wrap 15→14, and hot-cue jumps 16→0 and 0→2.
The first build updated the active endpoint on Beat Jump + without a
transition payload. A subsequent 1.30 run observed the grid-aware native
commit and delivered Beat Jump transitions in both directions to a standalone
phrase listener.

A later 2.00 PDB-only folder run independently matched native content ID 71
and published 418 current beats, native duration and live cues without a
FILE reference. After load, the current phrase list supplied 19 entries bound
to the exact native grid provider and revision. Short forward playback crossed
entries 0→1 and 1→2 without a reported gap. Separate reads of the stock source
clock before playback and after pausing showed the same TrackID advancing
from 41,409 to 572,058 samples at 44,100 Hz. This run covers forward traversal.

### Linked USB load

Both firmware profiles used a private Ethernet segment and a virtual player
at device number 3. The stock PRO DJ LINK mode discovered its USB export.
Its stock browser displayed 91 tracks, requested their artwork and cue data
from the database server and read the selected audio over NFS. An approved
no-tick mod observed the successful load of The Bigger Picture on Deck 1
at attempt 57 on each profile. Each NFS capture returned 8,491,091 bytes
for its audio handle. The first MiB matched the SHA-256 of the original
file on the peer. The successful public load was sequence 25 on 1.30 and
28 on 2.00. The public source was LINKED. Native metadata carried content
ID 3, title and artist. The current grid had 580 beats. The local track
reference, saved arrays, original tempo, duration and overview were unavailable.
The virtual peer used a bounded argument-type vector and served the analysis
and extended-cue replies requested by the firmware. [Virtual player protocol implementation](https://github.com/usr-ein/prolink/tree/ae895adc0a97fe2c791ed3bec0d5bcfd446b3fed)

Stopping the peer withdrew LOAD and POSITION at sequence 9374 on 1.30 and
3301 on 2.00. The old title still appeared on the 1.30 display. On 1.30
a new peer incarnation then
offered a separately named volume at the same number 3 and address. Its
appearance left the old load withdrawn. Loading that volume's row 3 through
the panel produced a fresh successful attempt 62 and a new LINKED load at
sequence 9386. The file reference stayed unavailable through both loads.

### Saved cue commits

The firmware's `AnalyzedDataAccessor` handles a completed cue save on the
storage worker. Its owned file identity supplies the UTF-16 audio and analysis
paths. The accessor supplies the UTF-8 storage root. A zero return from the
scoped method follows its successful write and finalization. The normal MEMORY
button on both profiles exercised `updateCueData` and its successful return
for the same USB1 root, row audio path and DAT path. The 1.30 methods for the
old-spec update, update and delete are 0x180ddd8, 0x180e7a0 and 0x180eb20.
Their 2.00 counterparts are 0x1969148, 0x1969b10 and 0x1969e90. Update was
exercised live. The compatibility and delete methods have static ABI evidence.

The observer checks the existing subscription cut, two paths, physical
incarnation and final bytes read through the checked, open descriptor.
Up to 16 captured revisions share a four-MiB byte budget. Decks holding the
same qualified row and file pair share each captured revision. The worker
orders delivery independently for each deck and subscription interval. A completed
save consumes a single-use digest when the monitor sees its matching file
image. It can exclude an unchanged sibling only when the non-cue tags and
container framing match and the observed writer closures account for the
completed saves. A concurrent writer, unknown closure, overflow or oversized
snapshot retains the corresponding loss.

On 1.30 an already-qualified load of USB1 row 36 had eight live and saved
memory cues. The real MEMORY button installed the ninth live entry. After
the stock write returned success, the same public load received nine saved
memory entries, eight saved hot entries, 418 saved beats, 19 saved phrases
and all 400 preview samples. The older monitor left both analysis files
stale after the in-place write. On 2.00, another stock MEMORY press took
the live and saved counts from nine to ten. The source remained qualified,
and the file grid, phrases and overview reported zero loss. The host fixture
stops parsing across two commits to one track loaded on two decks. It verifies
one retained copy of each file pair, both cue positions in order, and continued
delivery to the other deck while one native source witness is busy.

### Saved cue import identity

The version2 private FILE origin callback requires the genuine physical read
under the owned analysis getter, before the native stream opens. A reused
parsed cache entry carries its original read's custody. A new observation of
equal file bytes cannot give a cache hit a new file edition. The callback
authenticates the UTF-8 storage root and UTF-16 audio and DAT paths against a
unique committed OneLibrary row on the same physical USB database descriptor.
The original physical read may precede the load-completion receipt. Its bounded read token
therefore captures the source incarnation and DAT/EXT file identities
independently of a public deck binding. A changed database transaction,
file replacement, notification gap or ambiguous locator rejects the read.

Native PcptAtomData and Pcp2AtomData retain their original encoded entry
bytes from offset 14 through the declared extent. The physical scanner
checks their nominal type, declared header and total lengths, SHA-256 digest,
collection class and slot. It rejects repeated physical atoms and repeated
imported atoms. The public parser records the actual physical entry chosen
at each array assignment, preserving its existing companion precedence and
hot-slot replacement semantics. Matching times or sorted row numbers do
not establish an origin.

The worker attaches that entry only to the current qualified source and
published saved collection edition on the same deck and load epoch. Its
reference names the FILE provider, revision, saved-hot or saved-memory field
and copied loop entry. A record read before the first public track binding
becomes eligible after that binding and saved collection are published.
The native working-loop copy witness remains independently required.
A positively witnessed grid-only write can carry the cue origin across
the unchanged cue bytes when both notification histories cover its exact
closures. Other edits and uncertainty retire that origin.

The bounded registry holds at most 64 native reads shared by cue and analysis-tag
origins. Genuine getters capture those reads before the first subscription.
Native immutable Content and copied working properties retain their original
tokens independently through bounded atomic custody. The audio callback
performs no allocation, mutex acquisition or file work. A generation change
or lost custody makes its token unresolvable. Completed reads used by the
current four deck associations are also retained. Opaque read and row
tokens are monotonic and never serve as cue entry IDs. Exhaustion with all
slots in use withholds the next association.

### Saved phrase/grid origin bridge

The storage side accepts bounded, typed original-tag receipts for PQTZ,
optional PQT2 and PSSI through `azstatus_analysis_origin.h`. Each receipt
covers the original encoded tag, including every semantic header and payload
byte. The scanner checks its nominal type, declared extents, physical
companion, source row, file incarnation and SHA-256 digest. Repeated nominal
tags are ambiguous even when their bytes match. Decrypted phrase values
alone cannot identify the original encoded PSSI.

The native consumer must supply the original tokens from its positively
paired immutable grid and phrase Contents at that load's paired publication.
The storage worker then checks those origins against the currently published
saved FILE collections before providing `PHRASES.saved_grid`. The reference
names the exact saved grid provider and revision. Subsequent native-only map
edits do not undo the historical saved pairing. A certified grid offset
write can carry that grid origin across the witnessed translation. A
certified cue-only write preserves unchanged map tags. An unknown tag edit
retires that tag's origin independently.

Host fixtures cover this bridge, including a pair received before
`LOAD.track`, source qualification, forged hashes, a covered translation,
an unrelated cue commit and a PSSI replacement followed by a newly witnessed
pair using the still-valid original grid. Native OneLibrary importer
qualification is being verified separately from the legacy ANLZ constructors.
Its runtime coverage remains pending until the actual typed input-to-Content
chain reaches the bridge.

### Saved grid offset

On 2.00, a real GRID ADJUST rotary edit called the storage worker's
`AnalyzedDataAccessor::updateQuantizeOffsetData` at 0x1966618. The
corresponding 1.30 entry is 0x180b2a8. The EP147 profiles give the method
the accessor, its owned UTF-16 file identity and a 16-bit offset. The
2.00 diagnostic recorded successive arguments and successful returns for
USB1 row 36. The same files yielded offsets +4, +6, −6 and zero at PQTZ
header bytes 18–19. All 418 raw beat cells and all other tags stayed equal.
The native holder kept its cell samples. Its signed adjustment at 44.1 kHz
was +177, +265, −265 and zero samples respectively.

The saved status grid applies that signed millisecond adjustment to each raw
cell time. Captured grid revisions share the same bounded queue as saved cue
revisions. A matching grid commit can exclude unrelated DAT fields only when
the entire frame outside those two PQTZ bytes matches the previous revision
and notification counts account for the writer closures. An EXT field remains
independent. A host fixture holds the worker through positive and negative
commits and verifies both callback values.
