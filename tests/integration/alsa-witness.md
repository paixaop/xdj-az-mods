# ALSA return observer for emulator comparisons

This test-only preload counts completed ALSA calls and error returns without changing successful forwarding. It is separate from the production core and MOD API. Place the same helper after `azmod.so` and `azstem.so`, before system ALSA, in both comparison arms. Do not load either stub library into firmware.

The constructor resolves all seven original functions. Wrappers call each resolved original once with unchanged arguments and preserve its return and `errno`. The audio path uses only lock-free scalar counters: no allocation, file output, resolution, waiting or timestamp reads. The helper does add counter overhead; use identical instrumentation for both arms.

`azsdk_alsa_witness_v1` is a fixed 352-byte counter block. `initialized=1`, `ready=127`, `unresolved=0` and `overflow=0` are required. If an original is absent, or a call arrives before its constructor resolution, that lane is unsupported and returns `-ENOSYS` without resolving on the audio thread. Such a process cannot qualify an acceptance run. A zero error count in an unsupported lane is not evidence of zero errors. Counter exhaustion is sticky. Counters have process lifetime and are never reset.

Each API has separate entered, returned, negative-result and EPIPE-result counters. `recover_epipe_inputs` counts calls asking ALSA to recover EPIPE, even when recovery succeeds. Do not add originating EPIPE returns and recovery attempts to produce a unique xrun count. A capture is a bounded read of independent atomic fields, not an atomic cross-API event snapshot; calls in progress may finish after the observation. Positive completed playback and capture counts are required in each measured arm.

The companion read-only soak witness pins the helper hash, verifies the symbol and layout, checks preload order and that every resolved original falls in mapped system libasound, and verifies the process incarnation. These counters measure observed ALSA incidents. They do not measure all host-side silence, device losses, or audible dropouts.

Build on the ARM build image using `bash tests/integration/build-alsa-witness.sh OUTPUT_DIRECTORY`. This command only compiles. In a fresh private emulator directory, execute these independent controls:

```sh
LD_PRELOAD="$PWD/alsa-witness.so:$PWD/alsa-witness-stub.so" ./alsa-witness-test forward
LD_PRELOAD="$PWD/alsa-witness.so:$PWD/alsa-witness-stub.so" ./alsa-witness-test overflow
LD_PRELOAD="$PWD/alsa-witness.so:$PWD/alsa-witness-partial.so" ./alsa-witness-test unsupported
```

The stub records exact arguments, call count and entry errno independently. The forwarding control exercises every preload lane with full/short/zero success, EPIPE and another error; it verifies successful EPIPE recovery is counted separately. The other controls prove exhausted and unresolved lanes cannot silently qualify a zero-error result. These fixtures do not substitute for positive coverage in the actual firmware process.
