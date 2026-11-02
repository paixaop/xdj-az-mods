/* SPDX-License-Identifier: GPL-3.0-only */
/* At each checked native call site the original LDR is executed once, then its
 * target is recorded by value. Resume at the original BLR so getter LR, native
 * result and exception unwinding remain entirely native. X17 is the ABI IP
 * scratch already used by the checked jump-island mechanism. */
.text
.hidden azsdk_slip_capture_getter
.hidden azsdk_slip_getter_returns
.macro SLIP_GETTER n, offset
.p2align 2
.global azsdk_slip_getter_entry_\n
.hidden azsdk_slip_getter_entry_\n
.type azsdk_slip_getter_entry_\n,%function
azsdk_slip_getter_entry_\n:
.cfi_startproc
    ldr x1,[x1,#\offset]
    sub sp,sp,#800
.cfi_def_cfa_offset 800
    stp x29,x30,[sp,#0]
.cfi_offset x29,-800
.cfi_offset x30,-792
    stp x0,x1,[sp,#16]
    stp x2,x3,[sp,#32]
    stp x4,x5,[sp,#48]
    stp x6,x7,[sp,#64]
    stp x8,x9,[sp,#80]
    stp x10,x11,[sp,#96]
    stp x12,x13,[sp,#112]
    stp x14,x15,[sp,#128]
    stp x16,x17,[sp,#144]
    stp x18,x19,[sp,#160]
    stp x20,x21,[sp,#176]
    stp x22,x23,[sp,#192]
    stp x24,x25,[sp,#208]
    stp x26,x27,[sp,#224]
    str x28,[sp,#240]
    mrs x9,nzcv
    mrs x10,fpcr
    mrs x11,fpsr
    stp x9,x10,[sp,#256]
    str x11,[sp,#272]
    stp q0,q1,[sp,#288]
    stp q2,q3,[sp,#320]
    stp q4,q5,[sp,#352]
    stp q6,q7,[sp,#384]
    stp q8,q9,[sp,#416]
    stp q10,q11,[sp,#448]
    stp q12,q13,[sp,#480]
    stp q14,q15,[sp,#512]
    stp q16,q17,[sp,#544]
    stp q18,q19,[sp,#576]
    stp q20,q21,[sp,#608]
    stp q22,q23,[sp,#640]
    stp q24,q25,[sp,#672]
    stp q26,q27,[sp,#704]
    stp q28,q29,[sp,#736]
    stp q30,q31,[sp,#768]
    mov x2,x1
    mov x1,x19
    mov w0,#\n
    bl azsdk_slip_capture_getter
    ldp q0,q1,[sp,#288]
    ldp q2,q3,[sp,#320]
    ldp q4,q5,[sp,#352]
    ldp q6,q7,[sp,#384]
    ldp q8,q9,[sp,#416]
    ldp q10,q11,[sp,#448]
    ldp q12,q13,[sp,#480]
    ldp q14,q15,[sp,#512]
    ldp q16,q17,[sp,#544]
    ldp q18,q19,[sp,#576]
    ldp q20,q21,[sp,#608]
    ldp q22,q23,[sp,#640]
    ldp q24,q25,[sp,#672]
    ldp q26,q27,[sp,#704]
    ldp q28,q29,[sp,#736]
    ldp q30,q31,[sp,#768]
    ldp x9,x10,[sp,#256]
    ldr x11,[sp,#272]
    msr nzcv,x9
    msr fpcr,x10
    msr fpsr,x11
    ldp x0,x1,[sp,#16]
    ldp x2,x3,[sp,#32]
    ldp x4,x5,[sp,#48]
    ldp x6,x7,[sp,#64]
    ldp x8,x9,[sp,#80]
    ldp x10,x11,[sp,#96]
    ldp x12,x13,[sp,#112]
    ldp x14,x15,[sp,#128]
    ldp x16,x17,[sp,#144]
    ldp x18,x19,[sp,#160]
    ldp x20,x21,[sp,#176]
    ldp x22,x23,[sp,#192]
    ldp x24,x25,[sp,#208]
    ldp x26,x27,[sp,#224]
    ldr x28,[sp,#240]
    ldp x29,x30,[sp,#0]
    add sp,sp,#800
.cfi_restore x29
.cfi_restore x30
.cfi_def_cfa_offset 0
    adrp x17,azsdk_slip_getter_returns
    add x17,x17,:lo12:azsdk_slip_getter_returns
    ldr x17,[x17,#(8*\n)]
    br x17
.cfi_endproc
.size azsdk_slip_getter_entry_\n,.-azsdk_slip_getter_entry_\n
.endm
SLIP_GETTER 0,0x20
SLIP_GETTER 1,0x68
SLIP_GETTER 2,0x70
.section .note.GNU-stack,"",%progbits
