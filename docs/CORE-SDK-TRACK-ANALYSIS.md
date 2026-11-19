# Core analysis service

The core owns decoding, phrase/vocal computation, scheduling, source qualification,
storage and native publication. Built-in work enters `analysis_runtime` through
private typed operations; it does not register as a community mod or call a public
service table. `az_analysis_v1`, discovered with `analysis_service`, is the external
SDK boundary. It accepts copied requests, exposes completion/cancellation and
copies results into caller-owned arrays. Producer leases and raw/typed public
commit functions have been removed. Core and SDK headers must match.

The SDK guide `docs/18-track-analysis.md` documents version checks, execution
settings and request limits. [ANALYSIS.md](ANALYSIS.md) describes the retained
engine, controls, build and private model provisioning.

## Source and clock qualification

The loaded USB path combines the reducer's qualified native load generation and
track reference with physical USB identity, read-only library resolution and file
signatures. Beatport sources are completed/decrypted downloads observed through
the native source provider. Native Track resources remain private. The owned input
uses an already opened read-only descriptor and an opaque owner generation;
retirement revokes publication without invalidating that locator or independent
open readers. The runtime releases the input descriptor only after every decoder
child has joined, so a delayed child cannot open a reused descriptor number.
Native providers and publication remain gated by their firmware instruction and
map-writer checks; a supported public ABI alone does not enable a new firmware.

Phrase work captures the exact native grid edition and owns the copied typed
rows until the joined computation finishes. The capture preserves cell indexes,
the signed sample origin, actual sample rate and millisecond PQTZ rounding.
There is no temporary grid file or public sizing/copy round trip. Vocal-only
requests do not require a grid. Loaded phrase computation cannot replace or retime
that grid. Existing saved/cached PQTZ must match its count, positions, tempos and
timestamps before cached PSSI reuse. Timestamp checks use `compare_timestamps()`
with the actual clock and PQTZ rounding allowance.

AAC decoding restores only FAAD's first suppressed 1024-frame overlap slot;
remaining encoder priming is retained. MP4 edit metadata is explicit and is not
silently applied as trimming. Clock/seek fixtures cover FFmpeg and Apple AAC and
supported MP3. They do not certify every native resampling path.

## Typed persistence and reuse

Typed result validation directly builds native PQTZ/PSSI/PVDI/AZVD tags where
storage requires them. No TA01/TA02/TA03 transport envelope remains. The merger
preserves populated and unrelated tags and container bytes. Conflicting populated
DAT/EXT maps, invalid layouts and malformed PSSI are refused. Populated DAT PSSI
wins over an empty EXT placeholder. Source clocks, duration bounds, phrase fields,
vocal provenance/counts and quantized activity are validated before staging.

DAT and EXT writes are individually synced and renamed, DAT first when required;
they are not an atomic pair. A later failure can leave durable reusable data.
Native publication rechecks the load and exact holder edition under its mutation
guard, installs only the unchanged empty phrase holder, and sends the normal
native notification. Retimed/replacement/populated maps are refused. Native mood
0/1/2 is converted to public PSSI mood 1/2/3. Generated LSTM data never replaces
populated PVDI, including all-zero instrumental maps. Vocal-only publication does
not write DAT/EXT or invoke a native phrase refresh.

Beatport uses four bounded per-deck settings caches, keyed by full native TrackID
and audio fingerprint. Replacement clears DAT, EXT and 2EX together. Audio,
credentials and the private model are not copied into those settings caches.
Stored vocal readers return caller-owned intervals, preserve old spectral versus
LSTM provenance, and use the PVDI clock and exact-duration metadata. Loaded readers
validate the current audio-keyed cache without creating or evicting it.

## Lifetimes and outcomes

Core automatic work and each mod lifetime have distinct owner identities. SDK
workers must retain the generation inherited from their tracked worker; an old or
unqualified worker cannot reopen admission after re-enable. Retirement revokes
admission/publication immediately, signals asynchronous cleanup, and never waits
for a mod tick. Inputs remain owned until all engine children join. Caller-owned
result/interval copies survive request release and owner retirement.

A qualified source with every requested component already populated yields
COMPLETE with `AZ_NOT_FOUND`, indicating no new result array. Missing input/grid,
source withdrawal and unavailable native services are failures or explicit retry
conditions. A generic `AZ_NOT_FOUND` is not successful completion. The private
source layer uses a separate completion outcome for validated reuse.

When a USB row has no analysis locator, core queues a bounded conditional library
transaction. The worker advances it without an SDK poll or mod spool kick. Playback
can delay the transaction. Status stays SAVED/PENDING until COMMIT and the intended
row outcome are verified. A zero-row update, conflicting locator, failed statement
or failed COMMIT is not success. A failed later stage may leave reusable saved
components; the existing later-open repair path remains supported. This is not a
new persistent restart-recovery journal.

## Validation

Run `tests/sdk-analysis.sh`, `tests/sdk-analysis-source.sh`, the native map tests,
and `tests/analysis.mk` with their supported sanitizer configurations. Production
engine tests retain all twelve execution configurations, complete typed output
and feature/probability/kernel oracles. Runtime checks cover copied results,
capacity, retirement, reordered loads, cancellation, visible automatic progress,
source reuse versus failure and busy/pending/failed locator outcomes.

Historical native USB/Beatport and physical results from the previous mod pipeline
do not certify the new core integration. Re-run applicable emulator cases and
separately record physical idle/playback acceptance before claiming those results.
