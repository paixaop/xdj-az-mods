# Portable audio decoding

`miniaudio.h` is the unmodified miniaudio 0.11.23 single-header distribution from
https://github.com/mackron/miniaudio/blob/0.11.23/miniaudio.h.

SHA-256: `7e4f3f13c8fe66df2080ac3dd12a89193e3c2463cb7f067c798abd7331cd8ee6`.

The upstream MIT-0/public-domain license and attribution are retained in the header.
Only its WAV, FLAC and MP3 decoders and PCM conversion are built. Audio-device access,
runtime library loading, threading and playback engines are disabled.

The adapter disables MP3 gapless trimming and preserves the encoded frame clock.
It scans contiguous MPEG-1 Layer III frames at 44.1/48 kHz, reproduces the original
conditional Info/Xing header silence, and rejects format changes. Six frozen
generated MP3 fixtures check exact frame counts, startup alignment, seek replay
and PCM prefixes within one Int16 unit. Other MPEG profiles are refused.

This replaces AVFoundation/mpg123 decoding in the original package. Codec and sample
conversion output can differ, so the original package's bit-exact corpus results
are not claimed for this port. The frozen downstream algorithm fixtures remain tests.

## AAC-LC and MP4

`aac.c` wraps the separately vendored FAAD2 2.11.4 decoder. It accepts AAC-LC,
mono/stereo, 44.1/48 kHz, in nonfragmented, unencrypted MP4/M4A or ADTS. The
complete packet index is validated before decoding. Unsupported profiles,
format changes, multiblock ADTS frames, complex edit lists and over-20-minute
streams fail closed. Files are limited to 128 MiB; MP4 parser allocations are
limited to 16 MiB per open. PCM is decoded incrementally. Backwards seeks replay
the decoder to preserve exact MDCT overlap state without PCM spooling.

The MP4 demuxer is the unmodified CC0 `minimp4.h` from:
https://github.com/lieff/minimp4/tree/65fab0a062c3cd5befd98ecbaa890108cd45c226
Header SHA-256: `1a21f99afa15815e0e51eba416272f60886f7a72ebf15c36f884b0d42e2c8580`.
`MINIMP4-LICENSE` contains the upstream CC0 dedication. Allocation wrappers in
`aac.c` impose a smaller aggregate bound than upstream's per-allocation limit.

AAC output uses the **encoded access-unit clock**, 1024 source frames per
packet. FAAD drops the first packet's output while initializing its overlap
state; the wrapper restores that first 1024-frame slot as silence. It does not
implicitly apply MP4 edits, iTunSMPB gapless tags or padding removal. A simple
forward, unit-rate MP4 `elst` is exposed as `ta_decode_clock.edit_start_frames`
and `presentation_frames`; its segment duration is rounded from movie timescale
to source frames. `has_edit` distinguishes absent metadata from an actual zero
edit. Complex edits are rejected. ADTS has no container edit information.

`decoder_leading_frames` records the restored first slot. Consumers must align
this encoded clock to the native source clock before reusing beat-grid times.
An edit is descriptive metadata, not proof that the native deck applied it.
The decoder tests use generated impulses to verify this offset and seek replay;
they do not establish native-firmware AAC timing equivalence.
