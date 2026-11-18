/* SPDX-License-Identifier: GPL-3.0-only */
/* Model the firmware's hidden result independently of the C caller adapter. */
.text
.p2align 2
#if defined(__APPLE__)
.globl _test_make
_test_make:
    mov x1, x0
    mov x0, x8
    b _test_make_body
#else
.globl test_make
.type test_make, %function
test_make:
#if defined(__aarch64__)
    mov x1, x0
    mov x0, x8
#else
    jmp test_make_body
#endif
#if defined(__aarch64__)
    b test_make_body
#endif
.size test_make, .-test_make
.section .note.GNU-stack,"",%progbits
#endif
