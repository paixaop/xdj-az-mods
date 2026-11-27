# Core Track Analysis

Track Analysis is a built-in AZ-MODS feature. It fills independently missing
phrases and vocal data for loaded USB OneLibrary and completed Beatport tracks.
In **Extras**, **Phrase Extraction** and **Vocal Extraction** independently enable
automatic processing. Both default to on and appear just above **Vocal Indicator**.
There is no master switch. The services run when community mods are disabled.
Migration applies the previous Track Analysis choice to both switches unless a
new choice already exists. These switches do not restrict explicit SDK requests.

One core coordinator owns automatic and SDK-requested computation. It calls the
private source, engine and publication interfaces directly. External mods use
`az_analysis_v1` requests and caller-owned result copies, without shipping a model
or decoder. The old Track Analysis mod is obsolete and is rejected by the new mod
ABI before its entry point runs.

Jobs execute one at a time with up to three analysis workers, all at nice 19 on
A53 CPUs 1–3. Child threads inherit that scheduling policy. If applying it fails,
computation returns unsupported. The deadline is ten minutes and the aggregate
tracked scratch budget remains 512 MiB. Codec state and returned arrays are
additional. A load replacement or disable cancels automatic work; retiring an SDK
owner cancels only that owner's work. Started children are joined before input
release. Busy/stale automatic work retries; permanent failures wait for another
load or an off/on toggle. Status distinguishes saved analysis awaiting a locator
transaction from successfully linked completion.

Temporary native contention keeps the same request pending. Core retains the
original encoded publication and resumes native refresh without rewriting
successfully saved files. Each retry still validates the owner, source and load;
changed results or saved files are refused. Retained publication bytes are
released on completion or retirement.

Phrase work requires and preserves the loaded native grid. Vocal-only work does
not need a grid. Both extraction stages read the private analysis model.
Populated phrase/vocal maps are preserved, including valid
all-zero instrumental timelines. Errors remain visible in status and core logs.
With normal core logging enabled, startup and request failures are written to
`/tmp/azmod-analysis.log` under the existing core log size cap; debug tracing is
not required.

## Vocal Indicator

The upstream 1.2.9 Vocal Indicator paints the preview and scrolling waveform.
Enable it in Extras and choose Above Preview and/or Above Waveform. The preview
uses upstream's dashed intensity strip where the layout has room. The separate
consolidated Vocal Lines renderer, merge-gap setting and below-phrases placement
have been removed. Saved Vocal Lines display choices migrate to the upstream
switches; Below Phrases becomes Above Preview.

The worker reads raw PVDI through the shared, current-load USB/Beatport resolver
and upstream's parser, including masked rekordbox exports. A valid existing PVDI
map always wins, including an all-zero instrumental map. With no usable PVDI:

- Vocal Extraction enabled: wait while analysis is queued, running or awaiting
  publication. Successful publication wakes the indicator to read the new PVDI.
- Vocal Extraction disabled, unavailable or unsuccessful: use upstream's vocal
  stem-envelope detection when stems are available.

Changing tracks hides the old display immediately. Pending or stale reads do not
select another load's data. The worker retries missing data and notices stems
that appear later. File access and extraction never run in the paint callback.

## Analysis completion events

The core publishes one copied success record after final publication and library
link completion, for automatic and SDK-requested work. The record identifies the
request, source, track and loaded-deck generation. `generated_components` reports
what was computed; `available_components` reports the requested components now
available. Validated reuse has a zero generated mask. Failed, cancelled and
saved-but-unlinked requests never emit successful completion.

Core consumers register with `analysis_events_core_listen`; community mods use
`az_analysis_v1.on_complete`. Delivery is on the UI thread, outside provider
locks, with independent cursors and at most 16 events per listener per turn. The
64-entry journal reports overflow using `missed`; consumers must reacquire state.
Listeners must validate load identity before acting on queued events. SDK owner
retirement suppresses delivery to that lifetime. See the SDK service contract.

## Execution and optimizations

| Control | Values | Default |
| --- | --- | --- |
| `TRACK_ANALYSIS_STAGE_MODE` | `sequential`, `concurrent` | `concurrent` when both components are requested, `sequential` for a single component |
| `TRACK_ANALYSIS_PHRASE_THREADS` | `1`, `2`, `3` | `1` |
| `TRACK_ANALYSIS_PVDI_THREADS` | `1`, `2`, `3` | `2` for combined concurrent work, `1` for sequential work |

Concurrent combined work accepts phrase/vocal splits 1/1, 1/2 and 2/1.
Sequential stages each accept up to three threads. Invalid counts or an
oversubscribed split fail before decoding. The C engine also accepts these
settings through the typed `ta_execution` record in `ta_request`.

Phrase workers divide independent decimator bands, CQT pitch bins and FFT bars.
Filter state stays with its band across PCM blocks. Vocal workers divide
eight-window groups, sharing read-only weights and features with private scratch.
Output slots are disjoint; overlap scores merge in the original source order.
Concurrent stages use separate decoders and join before returning. Cancellation,
allocation failure or either-stage failure yields no partial published result.

The port retains 8 KiB decimator tiles, 16,384-sample envelope chunks with
in-place section passes and ARMv8 NEON FMA kernels. Vocal matrices pack eight
output columns and share each weight load across up to eight windows, keeping
sixteen vector accumulators in registers. Bounded prefetching reads upcoming
weight rows without leaving the packed block. The vocal FFT reuses an immutable
per-job permutation/twiddle plan, windows directly into bit-reversed order, and
uses four-lane NEON butterflies and power conversion. Its original multiply,
explicit FMA and addition order is preserved, including scalar tails.
The CQT uses the original eight independent double-precision FMA chains and
reduction order. DSP/codec kernels retain `-O3`; ARM64 builds target ARMv8.0 and
tune for Cortex-A53 without fast-math. The original mimalloc 2.2.4 allocator is
linked privately for analysis and codec allocations, with dynamic TLS and no
process-wide allocator override or exported allocator symbols. Vendored libraries retain
their C implementations; unused C++ compatibility branches have been removed.

`TRACK_ANALYSIS_PHRASE_PROFILE=1` retains phrase stage timings. Test drivers can
inspect typed feature/probability arrays and stage timings through
`ta_diagnostics`; the original `TRACK_ANALYSIS_PVDI_FEATURES` and
`TRACK_ANALYSIS_PVDI_PROBABILITIES` float dumps remain opt-in diagnostics.

## Audio and vocal model

Supported audio is WAV/FLAC at 44.1 or 48 kHz, MPEG-1 Layer III MP3 at those rates,
and mono/stereo AAC-LC in unencrypted, nonfragmented M4A/MP4 or ADTS. Maximum
source duration is twenty minutes, subject to the memory bound. HE-AAC, AIFF,
encrypted media, folder-only tracks and complex MP4 edit lists are unsupported.
AAC files are limited to 128 MiB and MP4 parser allocations to 16 MiB per decoder.
MP3/AAC decoding preserves the encoded source clock and encoder priming. MP4 edit
metadata is reported without silently trimming the audio.

Vocal analysis uses the recovered 29-feature, bidirectional two-layer LSTM with
256 units per layer and two dense layers. Its 1,669,314 weights remain a local
model asset prepared with the [model importer](#private-model-provisioning).
Inference uses C and ARMv8 NEON with deterministic window merging. Linux retains
the original musl activation rounding through private C math functions. The frontend
runs at 22050 Hz with 2048-sample FFT windows and a 1024-sample hop. The model
supplies the strict thresholds for activity values 0–4. At least nine frames
are required.
The public SDK records `AZ_VOCAL_LSTM` provenance. Existing vocal maps, including
older heuristic maps, are preserved.

## Build and test

`src/Makefile` links the separately compiled engine archive and runtime into
`azmod.so`; it needs no SDK checkout or separate mod package. Private libraries
retain their licenses under `vendor/analysis` and `assets/analysis`. Release
payloads include those notices under `/usr/share/licenses/azmod-analysis`.

From the core repository, put all generated output outside the checkout:

```sh
make -f tests/analysis.mk CC=clang OUT=/path/to/temp/analysis \
  ANALYSIS_ASSETS=/path/to/private/assets SANITIZE=address,undefined test test-runtime
python3 tests/analysis/test_provision.py
```

The engine tests preserve the numerical witnesses, all twelve execution modes,
codec clocks/seeks, cancellation, bounded allocation and joined child behavior.
The runtime tests use the production engine with controlled source dependencies.
Native source and publication tests remain separate. Host and container checks
do not establish physical playback or four-deck interference acceptance.

The display workflow and callback regressions run against production C:

```sh
SAN=asan bash tests/vocal-workflow.sh
bash tests/sdk-data-callbacks.sh
SAN=asan bash tests/analysis-host.sh
SAN=asan bash tests/sdk-analysis.sh
```

## Private model provisioning

The model is private local data and is excluded from source, SDK examples and
release packages. The macOS extractor accepts both macOS app bundles and Windows
installation files in a single executable. Build it from the core checkout with
the Xcode command-line tools and OpenSSL development library. The makefile uses
`pkg-config` to find libcrypto when available.

```sh
make -f tools/analysis/create-pvdi-model.mk
```

The default command creates `~/pvdi-model.bin` from a local rekordbox app
bundle. The supported macOS builds are 7.2.19.0342 and 7.2.20.0354.
Set `REKORDBOX_APP` to use another copy of the app, or set `OUTPUT_DIR` to a
private directory outside the checkout. The importer selects a profile
by the app's full `CFBundleVersion` and requires the exact size and SHA-256 of
the ARM64 executable slice and model graph for that build. The importer rejects
a version or file mismatch before reading any profile offsets. The `AZPVDI60`
file contains weights, vocal normalization, frontend and activity thresholds,
tensor shapes, recurrent connections and gate order, initial states and biases,
the vocal output class, and extracted phrase parameters for PCM, band filters,
mood, tempo, compression, beat activity, rhythm, fades, boundaries, opening spectra,
snare detection, vocal entry windows, fills, and recurrence. The loader uses the
graph input width and native motion window width to locate the coefficient tables
and tensors. The profile
records each build's source instructions and table locations in the
[macOS offset table](../src/analysis/host/pvdi-macos-offsets.inc).
Both supported builds ship the same GraphDef.

Run `make -f tools/analysis/create-pvdi-model.mk test` with the supported app to
check the complete generated model against the verified reference digest.
The test also checks that unsupported builds, modified graphs and modified
executables are rejected and that a rejected import preserves the previously
generated file.

For Windows x64 builds 7.2.17.0, 7.2.18.0, 7.2.19.0 and 7.2.20.0, pass
`rekordbox.exe` and the `vd_param.pb` file from the same installation directory.
On macOS, `windows-model` runs the same extractor executable as the default
target.

```sh
make -f tools/analysis/create-pvdi-model.mk windows-model \
  WINDOWS_REKORDBOX_EXE="/path/to/rekordbox.exe" \
  WINDOWS_VD_PARAM_PB="/path/to/vd_param.pb" \
  OUTPUT_DIR="/path/to/private/assets"
```

After building, invoke the macOS extractor directly with either input form.
Create the output directory before running these commands.

```sh
src/out/host/create-pvdi-model "/path/to/rekordbox.app" "/path/to/private/assets"
src/out/host/create-pvdi-model "/path/to/rekordbox.exe" "/path/to/vd_param.pb" "/path/to/private/assets"
```

A Windows-host build is available through the `src/out/host/create-pvdi-model-windows`
target. It takes the same three positional arguments for Windows input.
The extractor verifies the PE size, SHA-256 and fixed file version, then
authenticates and decodes the packaged graph. All four supported Windows builds
produce the same `AZPVDIW1` file. It includes Windows-derived phrase and vocal
parameters and the native arithmetic recipes for vocal motion and phrase
reductions. The four supported builds contain the same decoded network weights as
the supported macOS builds.

Run `test-windows` in the same makefile with both Windows input paths and the
full `WINDOWS_REKORDBOX_VERSION` to check all mapped fields, the complete file
digest, and rejection of unsupported or damaged inputs. On macOS, `test-unified`
uses one compiled executable for both the macOS and Windows extraction tests.
Run `test-windows-model` in `tests/analysis.mk` with `ANALYSIS_ASSETS` set to the
generated directory to exercise both analysis stages and check the model loader.

The [core analysis policy table](../src/common/analysis-policy.def) supplies the
portable decoder and SDK compatibility rules. Its audio input limits and
floating-offset rounding come from core commit `9128e8d`. Grid and phrase
capacities come from `21e693d`. Saved-vocal activation and interval clipping
come from `026ab712`. The storage units follow the SDK's ANLZ wire format. The
core includes that table at build time. SDK clients can still read previously
saved analysis with the private model absent.

To create and atomically install the file on the mounted AZ-MODS USB, use its
`MODS/analysis/assets` directory as the output directory:

```sh
make -f tools/analysis/create-pvdi-model.mk OUTPUT_DIR=/Volumes/USB/MODS/analysis/assets
```

Regenerate existing `AZPVDI01` through `AZPVDI59` assets before using this build.
For an older core that still consumes `AZPVDI01`, use the importer and
provisioning tools from its matching core release. After provisioning, remove
the obsolete Track Analysis mod through the usual mod manager. Missing or corrupt assets produce a visible
error when fresh extraction is requested. Valid saved maps remain available.

See [the core service contract](CORE-SDK-TRACK-ANALYSIS.md) and the public SDK's
`docs/18-track-analysis.md` for ownership, sources and publication outcomes.
