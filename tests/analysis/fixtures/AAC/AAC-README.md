# Generated AAC decoder witnesses

These files contain synthetic pulses, no commercial audio. `AAC-Provenance.json`
records the exact encoded-file hashes, packet counts, edit metadata, and peak
positions measured independently with FFmpeg's AAC decoder.

Source: one second of signed 16-bit PCM, 44,100 Hz stereo or 48,000 Hz mono.
All samples are zero except for this pulse beginning at `rate/4` and `3*rate/4`:

```
0, 0, 5000, 10000, 15000, 20000, 25000, 30000,
28000, 25000, 20000, 15000, 10000, 5000, 0, 0
```

The stereo right channel contains half the left-channel values. A WAV written
with Python's `wave` and `struct.pack('<h', sample)` was encoded with FFmpeg
9.0.1 using `-map_metadata -1 -c:a aac -b:a 128k -movflags +faststart`.
ADTS files contain the same compressed packets, remuxed using `-c:a copy -f adts`.
Reference PCM was obtained from each ADTS file using `-f s16le -acodec pcm_s16le`.
The maximum absolute left-channel sample in a 128-frame window around each
source pulse plus 1024 gives `referencePeakFrames`.

`apple-lc-44100-2` uses the same stereo source, encoded with macOS AudioToolbox
through `-c:a aac_at -profile:a 1 -b:a 128k -movflags +faststart`. Its source delay
is 2112 frames, and its edit list describes 45056 presentation frames including
encoder tail padding. The raw pulse test verifies that all 2112 frames remain;
the wrapper restores only FAAD's dropped 1024-frame slot. The remaining encoder
priming stays in the decoded audio. This distinguishes decoder delay from total
encoder priming and prevents accidentally adding or removing it twice.

The tests verify the encoded clock, exact output length, independent simultaneous
decoders, precise seek replay across packet boundaries, and pulse positions
against both the generated source and independent decoder. MP4 edit lists start
at encoded frame 1024; ADTS has no edit metadata. The wrapper does not apply
these edits. They do not establish the native deck's AAC clock convention.

`unsupported-he-aac.m4a` uses the same mono source, encoded by macOS AudioToolbox
through `-c:a aac_at -profile:a 4 -b:a 48k`. It verifies explicit HE-AAC rejection.
Other malformed cases are mutations made by tests: truncation, changed ADTS
profile/rate, impossible table count, unsupported edits and encrypted sample type.
