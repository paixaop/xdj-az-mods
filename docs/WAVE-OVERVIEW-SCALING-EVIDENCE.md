# Native overview height scaling correction

The overview stem scaler used byte 10 as the second draw height in a 12-byte native
`OverviewWaveform_1200Pt::WavePoint`. The native renderer consumes bytes 4 and 5. Byte 10
is a retained wire byte, so the old code left the second height unattenuated and changed
an unrelated retained value. `OV_HGT_B` is corrected from 10 to 5; styles, settings and
tint spans retain their existing calculations.

The old hook also borrowed Content without retaining it and restored its temporary array
only after a normal native return. The repair uses a bounded private lease acquired from
the verified native updater's already-held Content. Scoped C cleanup restores the array
on both normal and exception exits while that ownership is live. Other entry contexts,
including manual mute refreshes and older helpers, render stock. Those manual refreshes
are not accepted as attenuated native overview rendering by this change.

## Same-object proof in both builds

The explicit original ELF identities are:

| Build | SHA-256 | Native overview method, end exclusive |
|---|---|---|
| 1.30 | `736bdc9322c00e5770af459c92cace33d8680825c07f00f909f74dfc473a77a6` | `0x1dfb328..0x1dfc70c` |
| 2.00 | `6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925` | `0x1f675c8..0x1f689ac` |

`ep147addr.h` already maps the 1.30 alias `EP_1dfb328` to the second entry on 2.00.
`azmod.c` installs the hidden `azm_ov_entry` shim at that entry and stores its original
trampoline in `g_orig_ov`. The dispatcher calls that trampoline with the same `self`.

Both native entries have the same first 96 instruction bytes, SHA-256
`d124316dd4927fcd27b7cc0e5c2357c8f0863cbc857848833d7c39ba28bb28d4`.
They follow `self + 0x80` to the shared object and `shared + 0x28` to Content. Native code
then uses `Content + 0x28` for the array and `Content + 0x30` for its count. The source
original hook followed exactly this chain. The repaired dispatcher takes its Content from
the native caller's already-counted holder instead of acquiring a newly loaded UPO slot.
The lease supplies the same array/count representation, installs the scaled copy into
that Content's `+0x28`, then restores only if the field still points to its copy. When the
native renderer still selects that Content, it consumes this exact substituted array
without another format conversion. If native publication selected a different Content,
the captured old object remains alive and the native renderer uses its own current data.

The native per-record instructions at `0x1dfb79c` / `0x1f67a3c` load the array pointer,
multiply the point index by 12, load bytes 4 and 5, and multiply both heights by the same
vertical factor. Both reads precede the color-mode branch. RGB comes from bytes 0, 1 and
2; blue mode uses the palette index at byte 3. The alternate resampling path also reads
height byte 4 at `0x1dfc260` / `0x1f68500` and byte 5 at `0x1dfc27c` / `0x1f6851c`.
These observations come from original ELF bytes and Capstone, corroborated by the full
native-method pseudocode in each build.

The matching PWV4 reply-payload conversion at `0x17a8e20` / `0x1904240` establishes this
native layout. Its matched point-conversion branch has identical bytes in both builds;
a narrow fail-closed instruction model checks 1,375 synthetic vectors and 67 reached
instructions. The layout is:

| Native byte | Meaning in this path |
|---:|---|
| 0..2 | Derived RGB, each spectral channel multiplied by 255 and divided by their maximum |
| 3 | Derived blue palette index |
| 4 | Maximum of PWV4 wire bytes 3, 4 and 5 |
| 5 | PWV4 wire byte 5, the second scalar draw height |
| 6..11 | Retained original six PWV4 wire bytes |

Thus native byte 10 retains PWV4 wire byte 4 and is not a second native draw height.
The existing tint mirror into bytes 6..8 is left as it was; the correction changes only
which second height receives the established `height * attenuation / 256` calculation.

This payload-to-Content proof establishes representation. It does not establish native
loaded-source preference, native file freshness or unsaved edit behavior.

## Regression fixture

`tests/wave-overview.sh` extracts the actual production scaler, dispatcher, cleanup and
entry shim. Its independent consumer follows the proven
native object offsets and reads the literal height offsets 4 and 5. It uses synthetic
native records; no firmware, audio or real track fixture is required.

The fixture checks both height bytes at zero, fractional and identity attenuation, record
counts 1, 3, 17 and 2048, the existing tint behavior, unchanged source bytes, cached-copy
refresh when only the second height changes, unavailable-deck/settings fallbacks,
concurrent atomic curve updates and pointer restoration. Its C++ driver throws through
the actual extracted C dispatcher compiled with release `-O2 -fexceptions
-fasynchronous-unwind-tables`. ARM Linux runs also traverse the actual entry assembly and
check the captured register/SP values. Cases cover original exception propagation, native
array replacement before a throw, real allocation retirement, failed acquisition, helper
absence/incompatibility, and unavailable profiles. The latter fall back without touching
even an invalid widget address.

The broker tests cover duplicate/wrong/stale tokens, wrong thread/caller, capacity eight,
invalid counts, count saturation, malformed guard, output-size preservation and retirement
while a lease remains live. Native capture validation separately exercises the actual
stack/PC/vptr guard with invalid, truncated, misaligned and mismatched inputs. The synthetic
owning boundary uses a real lock; its owner callback actually frees retired Content.
These fixtures verify lease mechanics, not execution of native firmware UI methods.

Startup tests issue 10,000 foreign-thread queries and a same-thread query immediately
before publication. Thread/profile/stack setup is release-published only when complete;
query/caller guards acquire that state. Forced contention must stop after eight retain
attempts without changing output or count. Saturation tests reserve room for the native
renderer's own temporary retain and reject counts at or above `INT_MAX-1`.

Before the production correction the fixture fails its complete-record comparison. After
the correction a private control restores the old index 10 and must fail a specific
`FAIL overview:` assertion. Two further controls restore normal-return-only cleanup and
remove the genuine retain. Each must fail a fixture assertion. Compiler failure or an
unrelated sanitizer diagnostic does not count as catching any control.

```sh
SAN=plain tests/wave-overview.sh
SAN=asan tests/wave-overview.sh
SAN=tsan tests/wave-overview.sh
```

The ASan mode also enables UBSan. These are standalone tests of the actual mod source and
a modeled native consumer, not execution of the firmware GUI method.

The expanded suite reports 96,279 scalar/hook/startup checks, plus the separate C++ unwind and
broker assertions, and catches all three controls. Host plain, ASan/UBSan and TSan and
Ubuntu/Linaro GCC 7.5 ARM Linux plain runs are recorded. The TSan run includes a concurrent
atomic attenuation-curve writer and cross-thread broker rejection. Compile-only native
bundles can be prepared with `WAVE_OVERVIEW_BUILD_ONLY=/absolute/output/path`; they include
source/executable hashes and a standalone runner. Native kernel sanitizer execution is
a separate validation step from qemu-user compilation.

The existing `tests/unit/run.sh` overview-related `waveatt` extraction covers curve
construction and refresh cadence, and ends before this scaler. It has no offset-10
assumption to update. Its unchanged narrow fixture passes 36 checks and catches all
eight existing mutations after the height correction; no shared-runner edit was needed.

## Already-owned native entry and unwind lifetime

The complete native setter preserves `x24` and calls the renderer with `BL`. Its caller
has already retained a separate, explicitly typed `OverviewWaveform_1200Pt::Content`
local. The following original-entry facts are verified in both ELF builds:

| Capture | 1.30 | 2.00 |
|---|---:|---:|
| Native updater | `0x1c16aa0` | `0x1d80220` |
| Setter | `0x1dfc710` | `0x1f689b0` |
| Incoming renderer LR | `0x1dfc7f0` | `0x1f68a90` |
| Setter's saved caller LR at entry SP+8 | `0x1c174fc` | `0x1d80c7c` |
| Retained Content local | entry SP+`0xd0`, also `x24` | entry SP+`0xd0`, also `x24` |
| Content object vptr | `0x25cd0e0` | `0x2906700` |

The shim captures `x24`, LR and SP before a C prologue. The private azstem broker checks
the caller azmod image, configured main thread, known build/release mapping, entire native
setter/updater code fingerprints, native return PCs, bounded live stack location, matching
holder and Content vptr. Only then does it retain the already-owned Content. It never
loads an unprotected UPO Content slot to acquire its first reference.

The existing inline-hook transfer uses a direct `B` to an island and `LDR X17; BR X17`,
or the same absolute jump during constructor installation. It preserves incoming x24,
LR and SP before the shim. The shim tail-branches into the C dispatcher without adding a
stack frame. The GNU fixture object confirms all four instructions and uses `BLR` for
authenticated cleanup calls, including the unwind landing path.

Native FDE/LSDA rows cover the setter call and place Content cleanup after rendering
unwinds through the setter. Thus the native caller's holder remains live through the
hook's cleanup. The extra lease cannot be the final reference in this supported context.
Content's destructor owns and frees `array-12`, which is why restoration must precede
release even when the original renderer throws.

`finish` accepts a bounded non-reused token, consumes it once, restores only if the current
array is still its own installation, and releases through azstem's existing verified
owner path (`0x22043f0` / `0x2371890`). It accepts no caller-supplied object to release.
The core's cleanup call has a post-call compiler barrier so caller authentication still
sees an azmod return PC after optimization. The original exception continues to unwind.

This establishes a narrow already-owned borrow point. It does not certify arbitrary
raw-slot acquisition or a long-lived native Content cache. Manual refreshes intentionally
use stock rendering because they do not carry this native holder. Refresh no longer
preflights array/count through an unowned Content chain.

## Report-thread diagnostic

The old `wave_prov_report` followed cached `g_ovi[i].self` widget identities from the
overlay report thread and also read mutable UI cache fields and plain hot-path counters.
Those reads had no native object lifetime or atomic publication. The report now emits
only the existing atomic counters for overview, painting, construction, update and
two-deck paint activity. It does not follow widget/provider pointers or inspect UI cache
records. Counter snapshots are independent; they are not an atomic whole-render sample.

The report also prints owned atomic lease diagnostics, for example
`overview lease: binding=ready(5) acquired=12`. Binding codes are 0 unbound, 1 missing
helper, 2 rejected query, 3 incompatible helper, 4 unavailable native profile and 5 ready.
Ready confirms helper compatibility. `acquired` counts successful guarded begin calls,
including unity/settings stock draws and original exceptions. It is independent of
`applied`; compare its process-cumulative value before and after a controlled native
update to establish that an entry was accepted. It does not prove completed rendering
or visual attenuation. No native pointer is read for these diagnostics.

`tests/wave-report.sh` extracts the actual function. It verifies output with a `PROT_NONE`
cached widget and a retired/unmapped key, concurrently mutates the fake UI cache and
atomic counters, and checks the logging-disabled path. The two modes pass 440 and 439
checks. Its restored old-probe control must fault at the exact retired/poisoned widget
address and expected probe instruction context; an unrelated fault is rejected. Host plain, ASan/UBSan and TSan pass both real
modes and this control. This is a separate control from the three overview controls.
The fixtures additionally assert every binding status/report label, rejected and
off-thread acquisition counts, and successful acquisition when no scaled copy is applied.

Native static ASan initially passed both real report cases but handled the old probe's
SEGV itself, so the fixture's custom signal tag was absent. The runner correctly rejected
that incomplete control proof. The corrected unsanitized parent forks/execs the fully
sanitized child and validates either the exact signal address/PC proof or ASan's SEGV
address, probe PC range, READ access, top function and source line. Capture is bounded to
1 MiB and 30 seconds. Wrong-context/error/exit cases are rejected by validator self-tests.
Only the injected broken control uses `-O0` to keep its machine labels ordered; the real
production report remains `-O2`, and both retain their selected sanitizer instrumentation.
No sanitizer options are disabled or suppressed. GNU7.5 shared and static ASan host runs
exercise and pass the ASan-diagnostic proof path; native ARM requalification is separate.

## Evidence and acceptance boundary

The bounded private research is under the Anchor task's
`output/sdk-completion-wave-calibration/`. It retains the two original-source identities,
FDE extents and source signatures in `selected-function-ranges.json`; native owner-chain
and height-read assembly; complete Kuna native-method records; `preview-point-vectors.json`;
and the native holder, destructor and LSDA evidence in `lifetime-notes.md` and its manifest.
The source contribution contains no firmware bytes or audio.
Ordinary Capstone/Kuna output is identified separately from actual REA Evidence records.

The old `waveform.c` source SHA-256 was
`7f31b0d0596e9496d4207d6198e6fee52185bb80cabbfb6bac0a56732d8e86c2`;
the height-only correction was
`7dfcd93be7c831b71b9f5f0a22898036a9351e5aebe950877d967e71ccc0ea90`.
The subsequent scoped-lease repair records `waveform.c` as
`b13a988e662c0223a019d95b6662a963e1d0e186e8edbed0e6bcfa086d64b4a6`.

Physical overview acceptance remains pending: verify the guarded native update entry in
RGB and blue modes on both supported firmware builds, including zero/partial attenuation,
restoring all stems, layout changes and reload. Manual mute refresh attenuation remains
unsupported by the guarded entry and must not be counted as passed. No hardware control
or native GUI call was performed by this researcher.
For the controlled check, record and preserve the original `azmod-logs.on` flag state,
temporarily enable the existing capped logs, and compare `acquired` before/after the
native update in `/tmp/azmod-waveprov`. Restore the flag's original presence afterward.
The native height formulas do not establish calibrated PCM amplitude, complete native
waveform rendering or a meaning for PWVC.
