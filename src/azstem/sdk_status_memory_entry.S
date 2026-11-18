/* SPDX-License-Identifier: GPL-3.0-only */
/* Bridge only the native hidden-result convention. Ownership remains in C. */
.text
.p2align 2
#if defined(__APPLE__)
.globl _azn_cache_update_entry
.private_extern _azn_cache_update_entry
_azn_cache_update_entry:
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x8
    b _azn_cache_update
.globl _azn_invoke_cache_update
.private_extern _azn_invoke_cache_update
_azn_invoke_cache_update:
    mov x5, x0
    mov x8, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    br x5
#else
.globl azn_cache_update_entry
.hidden azn_cache_update_entry
.type azn_cache_update_entry, %function
azn_cache_update_entry:
#if defined(__aarch64__)
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, x8
    b azn_cache_update
#else
    jmp azn_cache_update
#endif
.size azn_cache_update_entry, .-azn_cache_update_entry
.globl azn_invoke_cache_update
.hidden azn_invoke_cache_update
.type azn_invoke_cache_update, %function
azn_invoke_cache_update:
#if defined(__aarch64__)
    mov x5, x0
    mov x8, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    br x5
#else
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %rcx
    jmp *%rax
#endif
.size azn_invoke_cache_update, .-azn_invoke_cache_update
.section .note.GNU-stack,"",%progbits
#endif
