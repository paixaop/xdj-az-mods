# Stored analysis and waveform delivery evidence

This report records the stored-data work added during the delivery-stage review. The
portable parser and local-file waveform provider now cover additional documented variants
and reject the regressions listed below. Completion of these parser capabilities does not
establish native loaded-analysis preference, native edit observation or calibrated rendering.
The broader support matrix remains in [CORE-SDK-SUPPORT.md](CORE-SDK-SUPPORT.md).

## Implemented behavior

### Saved cues and loops

* Canonical short PCP2 entries can end at byte 40. A comment-length field is read only
  when all four bytes at offset `0x28` exist. A present malformed length still fails the
  component read. Entry header sizes must match PCP2's 16 bytes or PCPT's 28 bytes.
* A PCP2 memory entry exposes its raw one-byte color ID from offset `0x1c`, including
  zero, under `AZ_CUE_COLOR_ID`. This is distinct from `AZ_CUE_COLOR`, which indicates
  authoritative RGB in the optional hot-cue trailer. Memory trailer bytes are not treated
  as an RGB color. The public `reserved`/`color_id` union retains the cue-entry stride.
  No EP147 RGB palette is inferred from these IDs.
* PCPT loop status values 0 and 1 expose saved `auto_active = 0`; value 4 exposes saved
  `auto_active = 1`. Unknown values leave that property unavailable. Point entries do not
  acquire loop activation state. This is the saved Active Loop property, not current loop
  engagement or a native recall command.
* PCP2 has no equivalent activation field. A selected extended loop can receive that one
  saved property from an exact, unique legacy counterpart. Classification, type, original
  start/end milliseconds and, for hot cues, slot must agree. Duplicate counterparts,
  conflicting statuses, unknown statuses or mismatches leave the property unavailable.
  Memory lists retain DAT precedence, and distinct saved entries are not coalesced by
  their positions. A malformed discarded legacy annotation does not invalidate otherwise
  usable authoritative extended cues.

The field layouts and status enum are documented by the
[ANLZ format research](https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/anlz.html)
and its [primary Kaitai schema](https://github.com/Deep-Symmetry/crate-digger/blob/main/src/main/kaitai/rekordbox_anlz.ksy).

### Raw phrase variants

PSSI offsets `+12` and `+14` are separate 16-bit version and entry-stride fields. Versions
0 and 1 use the bounded existing raw kind/mood parser; the existing XOR variant remains
supported. An unknown version marks only phrases `AZ_AVAIL_UNSUPPORTED`, allowing other
valid requested components to publish. Invalid container/tag lengths remain failures, and
known versions retain their stride/count/payload checks. Normalized phrase semantics remain
unavailable. The split fields are described in
[pyrekordbox's primary format definitions](https://github.com/dylanljones/pyrekordbox/blob/master/pyrekordbox/anlz/structs.py).

Version 0 has a natural local export. Version 1 is currently format-backed and synthetic:
tests cover an explicit version-1 fixture and an in-memory version-header change of the
natural export. The latter must not be cited as a naturally produced version-1 file.

### Additional raw preview encodings

| Public encoding | Tag | Canonical header | Bytes per column | Source-time axis |
|---|---|---:|---:|---|
| `AZ_WAVE_PWV4_PREVIEW = 4` | PWV4 | 24 | 6 | Approximate duration / column count |
| `AZ_WAVE_PWV6_PREVIEW = 5` | PWV6 | 20 | 3 | Approximate duration / column count |

The names above denote the raw tag encodings. The provider returns lossless packed bytes,
bounded to 4096 columns per read. PWV6's 20-byte header is accepted without reading a
nonexistent 24th byte. PWV4's canonical extra header word is zero. Empty tags, clipped tail
windows, malformed payloads and duplicate target tags follow the existing bounded provider
contract. The cache's per-record column buffer is now `4096 * 6` bytes; eight records still
fit below 8 MiB in the tested ABI.

Unlike detail tags PWV3/PWV5/PWV7, these previews cover the whole track and do not use a
150 Hz clock. The provider requires valid positive finite library duration and uses
`duration_s / sample_count`. Without that metadata, the request returns unsupported. The
library's integer-second duration makes this axis approximate. Raw bytes and this axis do
not establish native color, amplitude, pixel heights, playback alignment or calibration.
The distinction is supported by the
[primary preview implementation](https://github.com/Deep-Symmetry/beat-link/blob/main/src/main/java/org/deepsymmetry/beatlink/data/WaveformPreview.java)
and the [primary detail implementation](https://github.com/Deep-Symmetry/beat-link/blob/main/src/main/java/org/deepsymmetry/beatlink/data/WaveformDetail.java).

Internal entry points added for this support are `sww_is_preview(encoding)`,
`sww_header_bytes(encoding)`, `sww_plan_window_axis(index, first, count, step, out)` and
`sww_plan_preview_window(index, first, count, duration_s, out)`. Existing index structures
retain their layout. `sww_format_for` recognizes encoding IDs 4 and 5; `sww_index_detail`
and `sww_find_detail` accept their canonical layouts. `sww_plan_window` retains the fixed
150 Hz detail contract and explicitly rejects previews. The public service selects EXT
for PWV4 and 2EX for PWV6. All existing file bounds, source freshness, owned snapshots and
short-record prefix validation remain applicable.

## Local file comparison

The user-owned original `ANLZ0000.DAT`, `.EXT` and `.2EX` files are identified in
[CORE-SDK-ANLZ-EVIDENCE.md](CORE-SDK-ANLZ-EVIDENCE.md). No fixture/audio bytes are copied into
the source contribution. Independent bounded tag walking finds:

| File / tag | Section offset | Header / total bytes | Columns / stride |
|---|---:|---:|---:|
| EXT PWV4 | 169049 | 24 / 7224 | 1200 / 6 |
| 2EX PWV6 | 165399 | 20 / 3620 | 1200 / 3 |
| 2EX PWV7 | 270 | 24 / 165129 | 55035 / 3 |
| 2EX PWVC | 169019 | 14 / 20 | Three uninterpreted 16-bit payload values |

The new real-file test compares three streamed PWV4 and PWV6 windows against raw file
bytes: start, midpoint and clipped tail. Its deliberately supplied 400-second reference
axis tests the calculation; it is not a measured duration for the track. The original
pair contains no saved memory loops, so that pair cannot satisfy the real Active Loop
acceptance condition. The test can require real active/inactive saved memory loops using
the environment variables below when such analysis files are available.

PWVC's payload in this file is 80, 100, 152. Its framing is established; its interpretation
is not. The primary [Dysentery protocol research](https://github.com/Deep-Symmetry/dysentery/blob/main/doc/modules/ROOT/pages/track_metadata.adoc#requesting-vocal-detection-configuration)
describes vocal configuration, while [Baken's analysis-generation research](https://github.com/M-Igashi/baken/issues/147)
describes per-track gain. No calibration is derived from those three numbers or from a
comparison of preview/detail maxima. Native rendering evidence must resolve that meaning
before the SDK promises native-equivalent colors or heights.

## Regression checks and controls

`tests/sdk-stored.sh` compiles the production parser/provider headers and runs the new
asset-independent C fixture. Optional real files stay in the user's external sample
directory:

```sh
SAN=plain tests/sdk-stored.sh
SAN=asan tests/sdk-stored.sh
SAN=tsan tests/sdk-stored.sh
ANLZ_SAMPLE_DIR=/path/to/analysis-only-samples SAN=asan tests/sdk-stored.sh
REQUIRE_ACTIVE_MEMORY_LOOP=1 REQUIRE_INACTIVE_MEMORY_LOOP=1 \
  ANLZ_SAMPLE_DIR=/path/to/acceptance-pair tests/sdk-stored.sh
```

The ASan mode also enables UBSan and stops on undefined behavior. TSan exercises four
concurrent consumers with private parser records. Tests cover zero-time saved loops,
short/optional tails, raw color IDs, hot RGB, malformed lengths, recognized and unknown
statuses, identity mismatches, duplicate counterparts, both PSSI versions, unknown-version
component availability, exact 20-byte preview headers, duration validity, stride/capacity,
range clipping, empty tags and maximum-size windows.

The recorded host runs pass 356 checks without external assets and 557 with the original
local DAT/EXT/2EX sample. The latter passes plain, ASan/UBSan and TSan, including all thirteen
mutation controls in every mode. An Ubuntu/Linaro GCC 7.5 ARM Linux build also passes the
real-file fixture and controls with `-Wall -Wextra -Werror`. These are standalone processes; they do not
establish native in-process acceptance on the deck.

Thirteen deliberate source mutations must fail a specific `FAIL stored:` assertion:
rejecting short PCP2 entries, skipping canonical entry headers, suppressing active status,
accepting ambiguous counterparts, misreading memory RGB, substituting 150 Hz for preview
time, reducing PWV4 stride, reading a 24-byte PWV6 header, ignoring duration validity,
reducing the column buffer, restoring the old combined PSSI version/stride word,
accepting an unknown phrase version, and rejecting preview encodings in the core request
validator. A compiler failure or unrelated sanitizer diagnostic
does not count as catching a control.

The actual disk-provider fixture adds ten checks through the public request/acquire path:
PWV4 from EXT, PWV6 from 2EX, raw column stride and clipped tail, the duration/count axis,
out-of-range first columns including `UINT32_MAX`, unavailable duration and missing 2EX
without EXT fallback. Its `preview-extension` control removes PWV6's 2EX selection and
must fail that same fixture. It is separate from the thirteen stored-suite controls.

## Remaining delivery evidence

* Real saved memory loops with inactive/active file status and native UI observation of
  that same saved property, including persistence after save/reload. The parser's saved
  flag must remain separate from audible/current loop engagement.
* Native/file comparison and loaded-analysis preference for the same source identity and
  generation, plus native edits, additional source types and both firmware builds.
* A natural PSSI version-1 export and any further version/layout variants before broader
  format coverage can be claimed.
* Native waveform rendering paths, gain/gamma/channel interpretation, frequency cutoffs
  and precise time alignment. PWVC is not interpreted by this implementation.

These requirements remain open until the corresponding parent hardware/native evidence
is recorded. No native control command is enabled by these stored-data changes.
