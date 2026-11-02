/* SPDX-License-Identifier: GPL-3.0-only */
.text
.p2align 2
.global slip_getter_test_native,slip_getter_test_begin,slip_getter_test_end
.global slip_getter_test_site0,slip_getter_test_site1,slip_getter_test_site2
.global slip_getter_test_target
.type slip_getter_test_native,%function
slip_getter_test_native:
slip_getter_test_begin:
.cfi_startproc
    stp x29,x30,[sp,#-32]!
.cfi_def_cfa_offset 32
.cfi_offset x29,-32
.cfi_offset x30,-24
    stp x19,x20,[sp,#16]
.cfi_offset x19,-16
.cfi_offset x20,-8
    mov x29,sp
    mov x19,x2
    mov w20,w3
    mov x9,sp
    adrp x10,slip_getter_test_sp
    str x9,[x10,:lo12:slip_getter_test_sp]
    mov x2,#2
    mov x3,#3
    mov x4,#4
    mov x5,#5
    mov x6,#6
    mov x7,#7
    mov x8,#8
    fmov d0,#1.0
    fmov d1,#2.0
    fmov d2,#3.0
    fmov d3,#4.0
    fmov d4,#5.0
    fmov d5,#6.0
    fmov d6,#7.0
    fmov d7,#8.0
    cmp w20,#1
    b.eq 1f
    cmp w20,#2
    b.eq 2f
    cmp x8,x8
slip_getter_test_site0:
    ldr x1,[x1,#0x20]
    blr x1
    b 3f
1:  cmp x8,x8
slip_getter_test_site1:
    ldr x1,[x1,#0x68]
    blr x1
    b 3f
2:  cmp x8,x8
slip_getter_test_site2:
    ldr x1,[x1,#0x70]
    blr x1
3:  ldp x19,x20,[sp,#16]
    ldp x29,x30,[sp],#32
.cfi_def_cfa_offset 0
    ret
.cfi_endproc
slip_getter_test_end:
.size slip_getter_test_native,.-slip_getter_test_native
.type slip_getter_test_target,%function
slip_getter_test_target:
    mov x9,sp
    mrs x11,nzcv
    adrp x10,slip_getter_test_entry_sp
    str x9,[x10,:lo12:slip_getter_test_entry_sp]
    adrp x10,slip_getter_test_flags
    str x11,[x10,:lo12:slip_getter_test_flags]
    b slip_getter_test_body
.size slip_getter_test_target,.-slip_getter_test_target
.section .note.GNU-stack,"",%progbits
