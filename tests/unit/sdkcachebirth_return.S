/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent native hidden-result ABI thunks to C output parameters. */
#if defined(__aarch64__) && defined(__linux__)
.text
.p2align 2
.global cb_native_create
.type cb_native_create,%function
cb_native_create:
.cfi_startproc
    adrp x9,cb_native_owner_destination
    add x9,x9,:lo12:cb_native_owner_destination
    str x8,[x9]
    mov x2,x8
    b cb_native_create_impl
.cfi_endproc
.size cb_native_create,.-cb_native_create
.global cb_native_query
.type cb_native_query,%function
cb_native_query:
.cfi_startproc
    adrp x9,cb_native_rows_destination
    add x9,x9,:lo12:cb_native_rows_destination
    str x8,[x9]
    mov x4,x8
    b cb_native_query_impl
.cfi_endproc
.size cb_native_query,.-cb_native_query
.macro forward_begin name
.global \name
.type \name,%function
\name:
.cfi_startproc
    stp x29,x30,[sp,#-16]!
.cfi_def_cfa_offset 16
.cfi_offset 29,-16
.cfi_offset 30,-8
    mov x8,x0
    mov x16,x1
    mov x0,x2
.endm
.macro forward_end name
    blr x16
    ldp x29,x30,[sp],#16
.cfi_def_cfa_offset 0
.cfi_restore 29
.cfi_restore 30
    ret
.cfi_endproc
.size \name,.-\name
.endm
forward_begin cb_forward_owner
    mov x1,x3
forward_end cb_forward_owner
forward_begin cb_forward_rows
    mov w1,w3
    mov w2,w4
    mov x3,x5
forward_end cb_forward_rows
#elif defined(__x86_64__) && defined(__linux__)
.text
.global cb_native_create
.type cb_native_create,@function
cb_native_create:
.cfi_startproc
    mov %rdi,cb_native_owner_destination(%rip)
    mov %rdi,%rax
    mov %rsi,%rdi
    mov %rdx,%rsi
    mov %rax,%rdx
    jmp cb_native_create_impl
.cfi_endproc
.size cb_native_create,.-cb_native_create
.global cb_native_query
.type cb_native_query,@function
cb_native_query:
.cfi_startproc
    mov %rdi,cb_native_rows_destination(%rip)
    mov %rdi,%r9
    mov %rsi,%rdi
    mov %rdx,%rsi
    mov %rcx,%rdx
    mov %r8,%rcx
    mov %r9,%r8
    jmp cb_native_query_impl
.cfi_endproc
.size cb_native_query,.-cb_native_query
.global cb_forward_owner
.type cb_forward_owner,@function
cb_forward_owner:
.cfi_startproc
    mov %rsi,%r11
    mov %rdx,%rsi
    mov %rcx,%rdx
    jmp *%r11
.cfi_endproc
.size cb_forward_owner,.-cb_forward_owner
.global cb_forward_rows
.type cb_forward_rows,@function
cb_forward_rows:
.cfi_startproc
    mov %rsi,%r11
    mov %rdx,%rsi
    mov %rcx,%rdx
    mov %r8,%rcx
    mov %r9,%r8
    jmp *%r11
.cfi_endproc
.size cb_forward_rows,.-cb_forward_rows
#endif
#if defined(__linux__)
.section .note.GNU-stack,"",%progbits
#endif
