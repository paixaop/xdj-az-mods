# Retained deck activation math

`expf`, `expm1f`, `tanhf`, their exp2 table and overflow/underflow helpers come
from musl 1.2.5, the runtime of the retained ARM64 worker. Source tag:
https://git.musl-libc.org/cgit/musl/tag/?h=v1.2.5
Retrieved through the https://github.com/ifduyue/musl mirror at that tag.
Archive SHA-256: `83ff394502d1c334b040ea9bc66ec48bba453585e25b05f4bde3741d8245d883`.
See COPYRIGHT for the MIT and Sun notices; function notices are retained.

The minimal private libm.h supplies only the helpers these functions require.
All symbols are prefixed `ta_musl_` and hidden. Explicit FMA calls preserve the
contractions observed in the retained ARM64 worker's expf/expm1f instructions;
the rest of the mod remains `-ffp-contract=off`. No fast-math is used.
Linux vocal inference uses these functions so changing from a static musl worker
to a glibc-loaded mod preserves the activation rounding. Host macOS builds keep
their original system math for comparison with the retained macOS worker.
