# Generated MP3 decoder witnesses

These files contain generated two-second audio at 44,100 Hz stereo and 48,000 Hz mono. They contain no commercial recordings. FFmpeg 9.0.1 with libmp3lame produced three encodings for each source:

| File prefix | Encoder options |
| --- | --- |
| `cbr-info` | `-c:a libmp3lame -b:a 128k -write_xing 1` |
| `vbr-xing` | `-c:a libmp3lame -q:a 2 -write_xing 1` |
| `no-info` | `-c:a libmp3lame -b:a 128k -write_xing 0` |

`PROVENANCE.json` locks every MP3 and expected PCM prefix by SHA-256. Sample counts and recognized header intervals were obtained by executing the unchanged original frame analyzer. The unchanged original file reader and its decoder produced each first 2,048-frame prefix. Prefix files are signed little-endian Int16 stereo; mono is duplicated into both channels by that reader. The original executable and decoder remain private reference material.

The stereo metadata frames use a different channel mode from their following audio frames, so the original three-frame consistency check excludes their interval. The mono Info/Xing headers pass that check and contribute 1,152 leading frames. This distinguishes the source rule from unconditional header padding.

The independently licensed decoder must preserve the exact timeline and prefix alignment. Its arithmetic may differ from the historical decoder by one Int16 unit, which is the amplitude tolerance used by the test. Encoded frame counts, header intervals and startup offsets have no tolerance. Tests run from these committed files and need neither FFmpeg nor the original application.

`mixed-format.mp3` joins generated sources with a format change after four initial frames. The adapter rejects that stream explicitly because the original reader's partial-recovery behavior is outside the supported decoding contract.
