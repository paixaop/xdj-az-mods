# FAAD2 AAC-LC decoder

Unmodified `libfaad/` and `include/neaacdec.h` from FAAD2 2.11.4:
https://github.com/knik0/faad2/tree/2.11.4

Archive: https://codeload.github.com/knik0/faad2/tar.gz/refs/tags/2.11.4
SHA-256: `ee479ccbae4a8387ab696e6f21a481bd83fe3881471cafa81b4ae59d7d3aed43`.

The source headers license the library under GPL version 2 or later; `COPYING`
contains GPL version 2. The mod is distributed under GPL version 3.
Code from FAAD2 is copyright (c) Nero AG, www.nero.com.

`Makefile` uses `LC_ONLY_DECODER`, with floating-point synthesis and signed
16-bit PCM output. SBR parsing remains compiled so implicit HE-AAC is detected
and rejected by the wrapper; only AAC-LC output is accepted. No dynamic library,
process invocation, audio device, network API or firmware API is required.
The decoder's first-frame suppression is in `libfaad/decoder.c` immediately
after `hDecoder->frame++`. The wrapper restores that encoded 1024-frame slot
as silence. This is an explicit encoded clock, not a claim of gapless/native
firmware timing equivalence. See `PortableDSP/SOURCE.md` for container handling.

The C mod routes allocations in common.c, decoder.c and ssr_ipqf.c through
its private mimalloc wrappers; decoder algorithms are unchanged.
