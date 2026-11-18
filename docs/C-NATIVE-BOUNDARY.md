# C implementation and native hooks

Core services, SDK examples, mods, and ordinary test fixtures use C. The native
boundary uses GNU C11, including cleanup attributes, thread-local storage and the
platform unwinder. No C++ compiler or C++ source is required.

## Error handling

Core and SDK operations continue to report their existing status codes. A guarded
firmware call uses `az_native_try`: one means normal return, zero means the native
call failed. Unguarded firmware calls retain their original propagation behavior.

`src/common/aznative_guard.c` implements this boundary in C. Its hidden C
personality recognizes the nearest active guard and delegates ordinary cleanup
to the platform C personality. The unwinder runs intervening native destructors
and C cleanup handlers before the guard cleanup uses `longjmp` to return zero.
Jumping directly from a throw site would skip those cleanups and is not supported.
Guards are thread-local and nestable. Forced unwinds, including glibc thread exit,
continue unwinding instead of becoming ordinary error returns.

The firmware still uses its native exception runtime. The boundary calls that
runtime's C ABI functions `__cxa_begin_catch` and `__cxa_end_catch` to preserve
exception bookkeeping and payload destruction. The core therefore still links
that runtime. This is firmware interoperability, not C++ source or language-level
exception handling in our implementation. Removing it would change native error
behavior.

## Native return values

Some firmware functions return an owned pointer or pointer pair indirectly even
though ordinary C returns such small structs in registers. `az_native_result_word`
and `az_native_result_pair` use a 24-byte C return carrier to select the indirect
ABI. Only the initialized 8- or 16-byte prefix is copied to the caller; the remaining
carrier bytes are never inspected. These adapters support AAPCS64 and SysV x86-64
and accept one pointer-sized argument. They are not general signature adapters.

The carrier is only for calling firmware. It must never be returned from a hook
to a smaller native destination. Hooks that must forward the original result
address, caller address or captured registers retain their native entry shims.

## Retained contributed assembly

| Location | Hook contract |
| --- | --- |
| `src/azmod/sdk_cache_entry.S` | Forward integer/SIMD arguments, hidden result and original caller state through cache hooks. |
| `src/azmod/sdk_slip_entry.S` | Reuse the same entry contract for Slip callbacks. |
| `src/azmod/sdk_slip_getter_entry.S` | Preserve mid-function registers, flags and stack while observing the getter call. |
| `src/azstem/sdk_status_memory_entry.S` | Preserve the native cache hook's incoming result destination and forward it to the original. |
| Inline entries in `waveform.c`, `sdk_status_file_cues.inc`, `sdk_status_file_maps.inc`, and `sdk_status_memory_recall.inc` | Preserve hook caller/stack identity, native result destinations or captured firmware registers. |

The corresponding cache-entry, cache-birth, status-memory, Slip-getter and waveform
hook fixtures retain assembly to exercise those actual contracts. The SDK's Slip
probe retains its cache entry shim and hook fixture. Existing upstream assembly
is unchanged. Ordinary result fixtures, the error boundary, compiler fences and
wave-report probe labels are C.

## Verification

`bash tests/native-c.sh` checks normal and failing calls, real native-runtime and
foreign exceptions raised from C, nested cleanup, payload destruction, `errno`,
thread isolation and forced unwind. On Linux it also compiles an independent GCC
`-fpcc-struct-return` oracle that returns exactly 8 or 16 bytes, checking the C
adapters and destination canaries. `SAN=asan` and `SAN=tsan` enable the respective
sanitizer passes. The suite is included in `tests/sdk-completion.sh`.

The existing service and hook suites remain required. Host and compiler tests do
not replace physical-deck acceptance of the resulting libraries.
