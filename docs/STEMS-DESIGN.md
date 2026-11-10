# Stems implementation and design lessons

Stems extends EP147 at three boundaries: panel input, decoded source audio and native
display data. It demonstrates how a feature can share the deck's transport, DSP and UI
while supplying its own audio and control state. This document records the implementation
lessons for core and public SDK development. The service recommendations below are design
directions, not implemented APIs or additions to the SDK completion scope.

The library structure and hook mechanisms are described in
[Architecture](../ARCHITECTURE.md). Stem files and computer-side separation are covered by
[Stems format](STEMS-FORMAT.md) and [Remote Stems](REMOTE-STEMS.md).

## Current integration points

Stems is a built-in core feature. The UI library and `azstem.so` run inside EP147 through
`LD_PRELOAD`; the firmware-specific hooks belong to core.

| Boundary | Current behavior | Source |
|---|---|---|
| Source audio | `pbread_hook` replaces decoded source PCM with the enabled stems at their gains, before native tempo, key and scratch processing | [key_shift.c](../src/azstem/key_shift.c), [stem_playback.c](../src/azstem/stem_playback.c) |
| Pad input | On the stems page, pads 1–4 toggle stems and pads 5–8 solo them; the consumed pad bits are removed so Beat Loop does not also fire | [bus_interpose.c](../src/azmod/bus_interpose.c), [mod_menu_overlay.c](../src/azmod/mod_menu_overlay.c) |
| Pad feedback | Outgoing LED frames receive stem colors; native pad tiles receive each deck's displayed state | [pad_leds.c](../src/azmod/pad_leds.c), [stems_display.c](../src/azmod/stems_display.c) |
| EQ knobs | CH3 controls deck 1 and CH4 controls deck 2: HI is vocals, MID is melody plus bass, LOW is drums | [bus_interpose.c](../src/azmod/bus_interpose.c), [stem_envelopes.c](../src/azstem/stem_envelopes.c) |
| Main waveform | Per-stem peak envelopes and gains scale a private copy of native waveform points for rendering | [waveform.c](../src/azmod/waveform.c), [wave_att.h](../src/azmod/wave_att.h) |

## Preserve native behavior through the integration boundary

Replacing source samples lets EP147 apply its existing playback processing to the stem
mix. The feature does not need a second transport or an independent implementation of
scratching and time stretching. Future audio features should first establish where their
input can enter the existing engine and which downstream behaviors that preserves.

With all four stems present, enabled and at unity gain, the normal path uses the original
recording. Separated stems do not reconstruct it exactly. The implementation crossfades
changes between the original and the stem sum, and changes of the enabled-stem mask.
Preserving the untouched listening state is part of the feature's behavior.

Source alignment also belongs at this boundary. Codec delay, resampling and the tool that
created the stems can change the relationship between native source samples and stem
samples. [ofsload.c](../src/azstem/ofsload.c) measures alignment and applies only its verified
codec/rate rules; an unverified case keeps offset zero. A successful case for one format
does not establish alignment for every source.

## Reuse native widgets with their dependencies understood

EP147 contains a `PadStemWidget` and stems mode `0x0b`, which the AZ's normal pad-mode
selector does not expose. `modeget_hook` supplies that mode specifically to the pad-grid
and title consumers. `stemref_hook` temporarily supplies the mode and host-link state
needed for the native refresh, then restores the original values and pointers. Each tile's
image state is updated per deck.

This supplies native layout and artwork, but the widget's presence does not prove that a
complete standalone stems engine exists. AZ-MODS still supplies playback, controls and
state. Reusing another native component requires evidence for its owner, lifetime,
calling thread and required state. Temporary substitutions must remain scoped to the
verified call; global state would affect unrelated decks and callers.

## Treat input and feedback as one ownership decision

A pad remap must agree across the consumed input event, the logical deck receiving it,
the LED frame and the screen. Otherwise one action can trigger both the mod and the stock
function, or the pad can display a different action from the one it performs.

Stems makes this decision using the active pad page and that deck's stem availability.
Stem Volume separately borrows the CH3/CH4 EQ readings only while its ownership checks
allow it. It stands down if internal deck 3 or 4 has a track, PRO DJ LINK player 3 or 4 is
present, or the required ownership information is unavailable. Those checks are defined
in [link_members.h](../src/azmod/link_members.h); the knob readings become software stem
gains, not replacement EQ filters.

A reusable control service should bind event routing and feedback to the same ownership
state. It needs explicit conflict handling and release behavior when the mod is disabled,
the mode or deck changes, or its state becomes unavailable. Existing frame filters are a
useful transport mechanism; they do not by themselves express ownership of a complete
control group.

## Establish identity and lifetime before using state

Two physical control surfaces operate four logical decks. A surface side, a deck number,
a native object address and a loaded track are different identities.
[deck_track_map.c](../src/azstem/deck_track_map.c) captures deck identity from constructor
arguments and invalidates mappings on destruction. `stem_playback.c` changes its track
generation when a load changes, invalidating cached associations that could otherwise
survive reuse of a native object's address.

New asynchronous services should carry the relevant source, deck and load generation
through requests and results. Reject stale work before applying it to a new load. A valid
pointer or plausible title alone does not establish track identity, and an unknown deck
must not silently become deck 1.

Timing needs the same discipline: identify the clock domain, units and actual source
sample rate before comparing positions. SDK and mod timestamp equality/inequality checks
must use `compare_timestamps()`, with the correct `units_per_second`, source sample rate
and `window_samples`. Compare its result with `AZ_TIME_EQUAL` and handle `AZ_TIME_INVALID`
explicitly. Convert clocks and units before comparison; a fixed epsilon or assumed track
sample rate is not a substitute.

## Keep expensive work outside playback and account for its cost

Separation runs on a computer. The deck loads and decodes stems on background threads,
builds peak envelopes there, and exposes prepared buffers and atomic controls to the
audio path. File access, allocation and locks must stay outside the real-time callbacks.

Precomputation still consumes memory. Four six-minute stereo stems decoded as float32 at
44.1 kHz require about 508 MB of PCM alone, before other buffers. This is a calculation
from that format, not a measured process footprint. The loader checks available memory
and reserves a floor for the rest of EP147. Future services need bounded buffers, explicit
readiness and a defined behavior when work cannot be admitted.

Behavior must also be checked on hardware. `pad_leds.c` records that earlier hooks inside
the native LED serializer/color path froze that path on the deck; the implementation now
copies published colors into outgoing frames. A plausible hook location and passing host
tests do not establish real-time behavior. See [Testing](TESTING.md).

## Describe waveform data according to what it measures

The main waveform height factor is the enabled, gain-weighted sum of stem envelope peaks
divided by the sum of all stem peaks at that position. It is capped at unity, so boosting a
stem cannot make the waveform taller than the stock waveform. One factor scales all
frequency bands. Separate mini waveforms use the individual stem envelopes.

This is an approximation of audible stem balance, not a fresh analysis of the exact output
mix. It does not account for phase cancellation or produce new per-band stem analysis.
Public data should distinguish native observations, saved analysis and derived values,
and specify their timeline, resolution, validity and revision. A useful visual estimate
must not be presented as precise audio evidence.

## Replace file polling for performance controls

The current mute-mask path is `write_stems()` in [azmod.c](../src/azmod/azmod.c), followed by
`read_stems()` in [azstem.c](../src/azstem/azstem.c). The UI atomically replaces one small
control file per deck. A dedicated thread polls the files every 50 ms and publishes the
values to the audio path. EQ gains already use an in-process function and atomics.

File polling adds filesystem work and polling delay to a performance control even though
both libraries inhabit the same process. The 50 ms interval is not an end-to-end response
guarantee: scheduling and native pre-rendered audio can add delay.

A replacement should use a versioned in-process command/state interface with bounded
publication, explicit thread ownership and a revision identifying the applied state.
Distinguish command acceptance, application to source reads and audible output; an
acknowledgment alone does not prove that previously buffered audio has been replaced.

## Implications for the public SDK

The following are service recommendations, not current API guarantees:

| Service direction | Contract it should provide |
|---|---|
| Shared control ownership | Claim a logical control group, receive semantic events, publish matching LEDs/display state and release it predictably; resolve conflicts in core |
| Stem control and observation | Availability and readiness, mute/solo, gains and envelopes, with deck/load identity, revisions and explicit unavailable states |
| Native waveform presentation | Supported changes to native display data with scoped ownership and restoration, separate from APIs that read saved waveform analysis |

These capabilities belong behind the public SDK. Core should own the firmware addresses,
native objects, hooks and adaptation between supported builds. Community mods must not
copy the private stems hooks or call private exports directly; a missing capability must
first be implemented in core and exposed through the SDK.

The existing public `audio_tap` is read-only and delivers a deck's output before the
hardware mixer on a core thread. It does not provide source replacement. Its contract is
in [azmod-mod.h](../src/azmod/azmod-mod.h), with the implementation in
[sdk_audio.c](../src/azmod/sdk_audio.c). Any future audio mutation service needs a separate
contract for timing, buffer ownership, lifecycle and failure behavior. These recommendations
do not change the scope recorded in [Core SDK support](CORE-SDK-SUPPORT.md).
