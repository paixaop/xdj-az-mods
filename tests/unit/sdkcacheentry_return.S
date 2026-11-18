/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent native result ABI to explicit C output parameters. */
#if defined(__aarch64__) && defined(__linux__)
.text
.p2align 2
.macro result_entry name, destination
.global \name
.type \name,%function
\name:
.cfi_startproc
    adrp x9,\destination
    add x9,x9,:lo12:\destination
    str x8,[x9]
    stp x8,x30,[sp,#-16]!
.cfi_def_cfa_offset 16
.cfi_offset 30,-8
    bl \name\()_impl
    ldp x0,x30,[sp],#16
.cfi_def_cfa_offset 0
.cfi_restore 30
    ret
.cfi_endproc
.size \name,.-\name
.endm
result_entry cache_entry_create,cache_entry_owner_destination
result_entry cache_entry_query,cache_entry_rows_destination
.global cache_call_indirect
.type cache_call_indirect,%function
cache_call_indirect:
.cfi_startproc
    stp x29,x30,[sp,#-16]!
.cfi_def_cfa_offset 16
.cfi_offset 29,-16
.cfi_offset 30,-8
    mov x9,x0
    mov x8,x1
    mov x10,x2
    ldp x0,x1,[x10]
    ldp x2,x3,[x10,#16]
    ldp x4,x5,[x10,#32]
    ldp x6,x7,[x10,#48]
    blr x9
    ldp x29,x30,[sp],#16
.cfi_def_cfa_offset 0
.cfi_restore 29
.cfi_restore 30
    ret
.cfi_endproc
.size cache_call_indirect,.-cache_call_indirect
.section .note.GNU-stack,"",%progbits
#endif
