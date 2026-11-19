/* SPDX-License-Identifier: GPL-3.0-only */
/* Native single-word holder result uses x8, even though its storage is one word. */
.text
.p2align 2
#if defined(__APPLE__)
.globl _silent_cache
_silent_cache:
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x8
    b _silent_cache_body
#else
.globl silent_cache
.type silent_cache, %function
silent_cache:
#if defined(__aarch64__)
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x8
    b silent_cache_body
#else
    jmp silent_cache_body
#endif
.size silent_cache, .-silent_cache
.section .note.GNU-stack,"",%progbits
#endif
