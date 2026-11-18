/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent native hidden-result entry points for the C persistence fixture. */
.text
#if defined(__APPLE__)
#define SYMBOL(s) _##s
#else
#define SYMBOL(s) s
#endif
.macro RESULT name, body
.p2align 2
.globl \name
\name:
#if defined(__aarch64__)
    mov x1, x0
    mov x0, x8
    b \body
#else
    jmp \body
#endif
.endm
RESULT SYMBOL(allocate), SYMBOL(allocate_body)
#if !defined(__APPLE__)
.section .note.GNU-stack,"",%progbits
#endif
