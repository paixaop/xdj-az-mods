# Track Analysis service

`sdk_analysis_service.c` and `sdk_analysis_loaded.c` implement the public worker-only
analysis leases appended to `az_library_v1`: `analysis_open`, `analysis_open_loaded`,
`analysis_commit` and `analysis_close`. The public contract is documented in the SDK's
`docs/18-track-analysis.md`. Keep the core and SDK public headers identical. Mods check
each used entry with `AZ_LIB_HAS`; the core owns all private native calls and hooks.

`analysis_open` retains the file-only USB OneLibrary path. `analysis_open_loaded`
accepts a deck and an accepted LOAD generation, requires a ready native grid, and
pins the exact native map edition. Its native publication and Beatport source
providers are qualified only for firmware 2.00. Entry-point instruction checks and
the required map-writer instrumentation gate the path; a matching public ABI alone
does not enable it on another firmware profile.

The loaded USB path combines the reducer's qualified current track reference with
existing physical USB qualification, read-only library resolution and file signatures.
The Beatport provider in `sdk_analysis_source.cpp` observes native completed/decrypted
downloads, retains the genuine native Track resource and an open file descriptor,
and checks load identity, source revision, file identity and completion state. It does
not guess filenames or infer USB metadata for streamed tracks. Native resources remain
private; the public record contains locators and an opaque owner-bound job ID.

## Native grid and result contract

A loaded lease creates a private mode-0400 PMAI/PQTZ snapshot. It preserves native
cell indexes, applies the holder's signed sample origin, converts using its actual
sample rate, and rounds coordinates to whole milliseconds. `analysis_path` is this
read-only snapshot; `audio_path` is a read-only input locator. Both are valid only for
the retained job. The mod must return `TA01` with zero PQTZ length and plain PSSI.
The core can save its own snapshot when disk PQTZ is absent; it never installs or
retimes a native grid through this API.

Existing saved/cached PQTZ must match the captured grid's count, beat positions,
tempos and timestamps before cached PSSI is refreshed or new PSSI is persisted.
Timestamp checks use `compare_timestamps()` and a sample-rate-aware allowance for
PQTZ rounding. Matching counts alone are insufficient. PSSI structure is validated
before access, and a populated DAT PSSI takes precedence over an empty EXT placeholder.
Conflicting populated DAT/EXT maps are refused.

`sdk_analysis_write.h` supplies the bounded, format-only insertion/empty-tag replacement.
The core validates the `TA01` sizes, allowed PCM rates, PQTZ/PSSI layouts, counts and
phrase boundaries before staging. `sdk_status_map_analysis.inc` uses the firmware's
native phrase parser and counted result, then verifies the current holder and installs
it only into the unchanged empty phrase holder. Normal native notification publishes
the new map edition; populated native phrases are never overwritten. The native reader
converts the firmware's internal mood values 0/1/2 into the public PSSI values 1/2/3;
unknown native values become 0. Publication tests verify this conversion as well as
phrase counts, so default mood-specific Cues rules work on newly published maps.

The Track Analysis worker preserves encoded AAC timing. It restores only the first
1024-frame slot suppressed by FAAD's overlap initialization; it retains remaining
encoder priming and does not apply MP4 edit lists or iTunSMPB as implicit trimming.
Container edit metadata remains explicit. Generated FFmpeg/Apple witnesses verify
1024/2112-frame priming and exact seek replay. Source and native-grid clocks must be
qualified together; decoder tests alone do not prove every native resampling path.

## Sources, caching and return values

Successful loaded opens set `AZ_ANALYSIS_LOADED` and request only `AZ_DATA_PHRASES`.
Beatport leases also set `AZ_ANALYSIS_STREAMING` and leave `track` empty. At most four
jobs per mod and eight globally are retained; competing loaded leases prevent reuse
of a deck's Beatport cache while a job owns it.

Streaming analysis occupies four fixed settings caches, one per deck, under
`CM_STATE/analysis/beatport-0` through `beatport-3`. They contain DAT, EXT and an
identity composed from the full native TrackID and audio stat fingerprint. A changed
identity replaces that deck's old analysis. Container size bounds still apply. Audio
and credentials are never copied into these settings caches.

`AZ_NOT_FOUND` from the public loaded open means phrases are already available: native
phrases are populated, or matching saved phrases have just been installed. It returns
`job=0` and a versioned record. Native download-source `AZ_NOT_FOUND` is translated to
`AZ_BUSY`, because source capture can arrive later. Empty/incomplete native grids also
return `AZ_BUSY`. Mods retry busy/stale arrival conditions while the load remains;
unsupported profiles or invalid grid layouts are not successful leases. Initial
phrase-map read failures are propagated.

The community mod owns the algorithm and portable worker. The core owns source
qualification, validation, persistence and native publication. Existing populated
saved maps are preserved. Directory descriptors and no-follow component walks constrain
creation and rename to the selected USB or settings root. Malformed/unsupported target
tags fail closed; unrelated tags and container bytes survive verbatim.

Rows without ANLZ locators use a deterministic source-local path and
`AZ_ANALYSIS_NEEDS_LINK`. Commit or successful saved-map recovery stages a conditional
native library transaction; the mod retries `lib_spool_kick` on its worker. The update
matches content ID and audio path and fills only an empty locator, preserving later
DJ edits. Existing playback gates remain in force.

## Races and resource release

Commit rechecks load/map identity and source/file fingerprints before staging is
published. DAT and EXT are individually synced and renamed, with DAT first when
needed. They are not an atomic pair. Durable file publication precedes native refresh;
a subsequent load/map change can therefore leave valid saved analysis while native
publication returns stale. A later lease can reuse it only after the grid comparison.
Stat fingerprints are freshness witnesses, not locks against every native editor.

Native publication revalidates the exact load and holder edition under the map
mutation guard and compare/exchanges only the expected empty phrase content. Retimed
grids, replacement loads and populated phrase holders are refused. Successful commit
consumes the job. Other retained jobs must be closed; close releases both map pins and
the Beatport source token, deletes the temporary grid snapshot, and frees its retained
grid. Commit/close serialize under the service mutex. Close may return busy and must
be retried; a consumed or already closed ID is stale. Close cannot undo completed
publication. Native Track destruction runs outside the source registry mutex.

Run `tests/sdk-analysis.sh` and `tests/sdk-analysis-source.sh` in their supported plain
and sanitizer modes, plus the native map-analysis tests. Firmware 2.00 USB emulator acceptance exercised two AAC-LC M4A tracks with empty
phrase banks: both received 17 native phrases, native phrase bars refreshed during
the same load, and CUEs and Anchor reacted automatically. Original disk PQTZ tags
remained byte-identical. Controlled Beatport source tests cover completion, resource
lifetime and identity fences.

Authenticated native Beatport catalog acceptance also passed on firmware 2.00.
Two catalog loads generated 13 and 10 phrases during the same load; native grids
retained their 448 and 401 rows and map versions. CUEs applied automatically, and
Anchor used the refreshed phrase map for its countdown. Reload restored the saved
ten-phrase map without launching another worker. Native CloudDirectPlay and Beatport
login required matching private `cabinet/encryption/csdk.dat` and `bpck.dat` provisioning
in the emulator; these files are not included in packages.

Physical acceptance on 2026-11-16 used the same qualified EP147 image on a deck
reporting firmware 2.01. Six Beatport loads automatically generated native phrase
maps and phrase-based Cues plans. The two fresh final-build witnesses generated
15/13 phrases and 8/8 hot plus 10/5 memory cues without reloading; native grid
counts 388/424 and revision 1 were preserved. Two USB loads recognized existing
analysis and preserved their cue banks. SDK Play activated Anchor on both sources.
Physical USB generation on an empty bank was not repeated in this run. Retained
evidence is in the parent workspace's `temp/physical-auto-20261116/` directory.
