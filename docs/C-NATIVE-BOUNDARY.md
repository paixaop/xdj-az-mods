# C implementation and native hooks

Core services, SDK examples, mods, and ordinary test fixtures use C. The native
boundary uses GNU C11, including cleanup attributes, thread-local storage and the
platform unwinder. No C++ compiler or C++ source is required.

## Error handling

Follow upstream's C conventions: validate inputs and native identity, return the
existing status or null result for an ordinary failure, and release acquired
resources on every exit. Refused hooks leave the feature off. Fatal-signal
handlers report the fault and chain to EP147; they are not recovery handlers.

Firmware exceptions propagate through core hooks to EP147's own handlers. Keep
upstream's `-fexceptions -fasynchronous-unwind-tables` flags: C frames and their
cleanup handlers must be unwindable. GNU C cleanup attributes release locks,
temporary native owners and provisional registrations on normal return and unwind,
as upstream uses them for scoped state restoration.

Production code must not define an exception personality, catch native exceptions,
translate an exception into an SDK error, or jump over native cleanup. There is no
`az_native_try`, `setjmp`/`longjmp` exception boundary or direct `__cxa_*` dependency
in the core. The production link does not add `libstdc++`. EP147 still owns its
native runtime and decides how its exceptions are handled.

The shared libraries use `-shared-libgcc`. This differs from upstream's static
GCC link: the target-toolchain DSO regression aborts in the private unwinder with
`-static-libgcc`, and passes with the shared unwinder. It follows
[GCC's shared-library exception guidance](https://gcc.gnu.org/onlinedocs/gcc/Link-Options.html).
`libgcc_s` is compiler support for C unwinding; it does not add C++ source or a
C++ exception handler.

`tests/unit/native_unwind.c` is a test-only stand-in for that outer firmware handler.
It lets C fixtures verify propagation and cleanup; it is never linked into either
production library. The SDK has the same stand-in only under `tests/fixtures/`.

## Upstream conventions

The baseline is upstream 1.2.9, commit `f851c610fd2c68adcc950c1032d97bceeae1bb02`:
[`src/Makefile`](https://github.com/Kyle-Hosman/xdj-az-mods/blob/f851c610fd2c68adcc950c1032d97bceeae1bb02/src/Makefile),
[`ARCHITECTURE.md`](https://github.com/Kyle-Hosman/xdj-az-mods/blob/f851c610fd2c68adcc950c1032d97bceeae1bb02/ARCHITECTURE.md#9-contributing-notes),
and [`.clang-format`](https://github.com/Kyle-Hosman/xdj-az-mods/blob/f851c610fd2c68adcc950c1032d97bceeae1bb02/.clang-format).

Keep helpers static, expose only deliberate library interfaces, and keep native
object ownership inside the core. Use per-deck state, exact supported-build
profiles, constructor-time hook installation and existing feature gates. Keep
blocking work off playback and paint paths. Public SDK calls remain the only mod
boundary. Format C with the repository's unchanged clang-format 21.1.8 settings.

## Native return values

Some firmware functions return an owned pointer or pointer pair indirectly even
though ordinary C returns such small structs in registers. `aznative_calls.h`
provides separate typed calls for strings, players, locations, track state,
editor text and phrase content. Each native function type uses a 24-byte C return
carrier to select the indirect ABI. The wrapper copies only the initialized 8- or
16-byte prefix into its typed result; the remaining carrier bytes are never
inspected. These adapters support AAPCS64 and SysV x86-64. Keep the native size or
object argument type on each operation instead of passing everything as `void *`.

The carrier is only for calling firmware. It must never be returned from a hook
to a smaller native destination. Hooks that must forward the original result
address, caller address or captured registers retain their native entry shims.

## Typed native bindings

Convert qualified firmware addresses to function pointers at the binding boundary.
Use the operation's prototype thereafter. Notification listeners and their control
blocks have typed vtables; Slip retains its separate scalar callback contract.
Their native layout and ownership rules remain unchanged.

Connection installation has separate open and close callbacks and typed original
function slots. The low-level upstream island installer still takes erased
pointers; the local adapters contain that conversion. It must publish the original
into the supplied slot before exposing the replacement entry. Close is installed
first, and both originals must be valid before connection births are admitted.

Generic callback contexts, serialized bytes and heterogeneous owner payloads retain
their opaque pointer types. Public SDK layout and upstream SQL binding tables are
unchanged by these local interface types.

## Retained contributed assembly

| Location | Hook contract |
| --- | --- |
| `src/azmod/sdk_cache_entry.S` | Forward integer/SIMD arguments, hidden result and original caller state through cache hooks. |
| `src/azmod/sdk_slip_entry.S` | Reuse the same entry contract for Slip callbacks. |
| `src/azmod/sdk_slip_getter_entry.S` | Preserve mid-function registers, flags and stack while observing the getter call. |
| `src/azstem/sdk_status_memory_entry.S` | Preserve the native cache hook's incoming result destination and forward it to the original. |
| Inline entries in `waveform.c`, `sdk_status_file_cues.inc`, `sdk_status_file_maps.inc`, and `sdk_status_memory_recall.inc` | Preserve hook caller/stack identity, native result destinations or captured firmware registers. |

The corresponding cache-entry, cache-birth, status-memory, Slip-getter and waveform
hook fixtures retain assembly to exercise those actual contracts. Core-owned Slip diagnostics retain their entry shim and hook fixture. The SDK
Slip example consumes only public status observations. Existing upstream assembly
is unchanged. Ordinary result fixtures, the error boundary, compiler fences and
wave-report probe labels are C.

## Verification

`bash tests/native-c.sh` checks normal calls and propagation to a test outer handler, with real native-runtime and
foreign exceptions raised from C, nested cleanup, payload destruction, `errno`,
thread isolation and forced unwind. On Linux it also compiles an independent GCC
`-fpcc-struct-return` oracle that returns exactly 8 or 16 bytes, checking the C
adapters and destination canaries. `SAN=asan` and `SAN=tsan` enable the respective
sanitizer passes. The suite is included in `tests/sdk-completion.sh`.
It also runs `tests/native-types.sh`, which checks that valid bindings compile and
incorrect callable signatures or object types are rejected by the compiler.

The existing service and hook suites remain required. Host and compiler tests do
not replace physical-deck acceptance of the resulting libraries.


## Independent core services

Analysis computation/runtime/source publication, stored track data, status
reduction/storage/native/GUI projection, command ownership, and Beatport login
compile as private production objects. Their headers specify typed operations;
they do not import the upstream host's internal header or mutable global state.
The complete ownership and startup map is in
[ARCHITECTURE.md](../ARCHITECTURE.md#10-core-service-ownership).

Native adapters preserve the existing firmware receiver, source identity,
exception forwarding and reference lifetime. A copied status projection cannot
replace a native ownership proof. Host bridges reuse the existing master mutex,
BpmState weak registry, descriptor registry and hook installer rather than
creating a second registry or installing duplicate hooks. All analysis workers,
including phrase/vocal child threads, use the existing signal-stack trampoline.
