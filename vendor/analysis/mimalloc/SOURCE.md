# mimalloc 2.2.4

Source: https://github.com/microsoft/mimalloc/tree/v2.2.4
Archive SHA-256: `754a98de5e2912fddbeaf24830f982b4540992f1bab4a0a8796ee118e0752bda`.
The retained worker used this exact release, rebuilt for ARMv8.0.

The C mod retains it through explicit private `mi_*` calls for analysis and
codec allocations. `MI_MALLOC_OVERRIDE` is disabled: no process-wide allocator
interposition, exported allocator symbols, or Swift runtime is involved.
`src/static.c` compiles the required C sources. Linux uses dynamic TLS suitable
for a loaded mod. AddressSanitizer builds enable mimalloc's ASan tracking.
Upstream source and headers are unchanged. See LICENSE (MIT).
