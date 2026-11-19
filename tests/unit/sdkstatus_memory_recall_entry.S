/* SPDX-License-Identifier: GPL-3.0-only */
/* Reproduce the caller-held row/index ABI and expose exact return sites. */
.text
.p2align 2
.global fixture_recall_accept
.type fixture_recall_accept,%function
fixture_recall_accept:
.cfi_startproc
stp x29,x30,[sp,#-0x100]!
.cfi_def_cfa_offset 0x100
.cfi_offset x29,-0x100
.cfi_offset x30,-0xf8
str x22,[sp,#0x10]
.cfi_offset x22,-0xf0
mov x29,sp
mov x22,x5
str x6,[sp,#0x78]
str x6,[sp,#0x90]
mov x5,x4
add x4,sp,#0xa0
mov x9,#0xabc
str x9,[x4]
blr x7
.global fixture_recall_accept_return
fixture_recall_accept_return:
ldr x22,[sp,#0x10]
ldp x29,x30,[sp],#0x100
.cfi_def_cfa_offset 0
ret
.cfi_endproc
.size fixture_recall_accept,.-fixture_recall_accept
.macro RECALL_CALL name,target
.global \name
.type \name,%function
\name:
.cfi_startproc
stp x29,x30,[sp,#-16]!
.cfi_def_cfa_offset 16
.cfi_offset x29,-16
.cfi_offset x30,-8
mov x29,sp
blr x2
.global \name\()_return
\name\()_return:
ldp x29,x30,[sp],#16
.cfi_def_cfa_offset 0
ret
.cfi_endproc
.size \name,.-\name
.endm
/* Pass the actual production function as x2, retaining normal x0/x1. */
RECALL_CALL fixture_recall_ctor0,recall_ctor
RECALL_CALL fixture_recall_ctor1,recall_ctor
RECALL_CALL fixture_recall_ctor2,recall_ctor
.global fixture_memory_call
.type fixture_memory_call,%function
fixture_memory_call:
.cfi_startproc
stp x29,x30,[sp,#-16]!
.cfi_def_cfa_offset 16
.cfi_offset x29,-16
.cfi_offset x30,-8
mov x29,sp
blr x3
.global fixture_memory_call_return
fixture_memory_call_return:
ldp x29,x30,[sp],#16
.cfi_def_cfa_offset 0
ret
.cfi_endproc
.size fixture_memory_call,.-fixture_memory_call
.section .note.GNU-stack,"",%progbits
