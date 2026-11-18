/* SPDX-License-Identifier: GPL-3.0-only */
/* Native single-word holder result uses x8, even though its storage is one word. */
.text
.p2align 2
#if defined(__APPLE__)
.globl _inline_cache
_inline_cache:
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x8
    b _inline_cache_body
#else
.globl inline_cache
.type inline_cache, %function
inline_cache:
#if defined(__aarch64__)
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x8
    b inline_cache_body
#else
    jmp inline_cache_body
#endif
.size inline_cache, .-inline_cache
.section .note.GNU-stack,"",%progbits
#endif
