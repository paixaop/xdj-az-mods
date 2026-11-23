# Native library owner and physical-file capture

`sdk_native_sources.c` supplies a private source-observation boundary. It assigns a
non-reused core epoch to an observed native MountPoint/SqliteDataBase owner pair, keeps
genuine GNU weak owners, and can capture the native connection's open-file identity.
It does not establish that a loaded track or loop callback belongs to that epoch.
No public source/load capability follows from a matching media number.

## Integration and lifetime contract

Compile this translation unit as GNU C11 for 64-bit Linux, with unwind tables and
C cleanup support (`-fexceptions -fasynchronous-unwind-tables`). Native ownership is
represented by explicit C layouts and reference-count operations. The production
physical adapter is enabled only for little-endian AArch64. The private declarations
are in `src/common/azsources.h`; no native pointers or file descriptors appear in its output.

All mutable state, including the weak owners, lives in a statically initialized,
process-lifetime `SourceState`. Its mutex and once controls are ready before the
`azmod_init` ELF constructor runs. Early configuration/capture cannot be overwritten
by delayed initialization or invalidated by an automatic shutdown destructor.
Configuration disables and enables capture under the same mutex, so an earlier
configurator cannot overwrite a disable waiting for that mutex. The physical-ready
query uses atomic publication. The constructor-order negative control explicitly
resets captured state later and must fail the early-state assertion.

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

## Loaded-source and committed file ownership

Status receipts tie the observed native connection to the current load and
physical source. Each successful SQLite open has its own incarnation. A close
revokes that connection at entry and leaves other owned connections valid.
A replacement mount needs a new native origin. A cached source key does not
extend a borrowed native row, holder or result object past its native lifetime.

Native track structure publishes a matched phrase/grid holder pair. The
capture checks it against the active pack and load before handing off counted
references. Stored PQTZ and PSSI arrays have separate file origins. File
presence alone cannot establish that their revisions belong together.
Committed cue and grid receipts must match the physical read or save,
component, source generation and load. A grid-only save can carry a valid
cue origin across publication when the bridge has witnessed that component
boundary.

Binding identities and runtime selection use the reviewed
[firmware profiles](FIRMWARE-SUPPORT.md). Test source retirement, independent
USB connections and native file association at the relevant producer.

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
C ownership/worker model remains synthetic; passing it validates the actual SQLCipher
profile and open-file adapter, not production hook installation or loaded-source binding.
