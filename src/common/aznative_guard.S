/* SPDX-License-Identifier: GPL-3.0-only */
/* Itanium ABI catch-all boundary for foreign firmware exceptions.
 * int az_native_try(void (*call)(void *), void *context): 1 on return, 0 on throw.
 * Keep the LSDA and unwind frame together; no exception may skip C cleanup. */
#if defined(__APPLE__) && defined(__aarch64__)
	.section	__TEXT,__text,regular,pure_instructions
	.globl	_az_native_try                  ; -- Begin function az_native_try
	.p2align	2
_az_native_try:                         ; @az_native_try
Lfunc_begin0:
	.cfi_startproc
	.cfi_personality 155, ___gxx_personality_v0
	.cfi_lsda 16, Lexception0
; %bb.0:
	stp	x29, x30, [sp, #-16]!           ; 16-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	mov	x8, x0
Ltmp0:
	mov	x0, x1
	blr	x8
Ltmp1:
; %bb.1:
	mov	w0, #1                          ; =0x1
	ldp	x29, x30, [sp], #16             ; 16-byte Folded Reload
	ret
LBB0_2:
Ltmp2:
	bl	___cxa_begin_catch
Ltmp3:
	bl	___cxa_end_catch
Ltmp4:
; %bb.3:
	mov	w0, #0                          ; =0x0
	ldp	x29, x30, [sp], #16             ; 16-byte Folded Reload
	ret
LBB0_4:
Ltmp5:
	bl	___clang_call_terminate
Lfunc_end0:
	.cfi_endproc
	.section	__TEXT,__gcc_except_tab
	.p2align	2, 0x0
GCC_except_table0:
Lexception0:
	.byte	255                             ; @LPStart Encoding = omit
	.byte	155                             ; @TType Encoding = indirect pcrel sdata4
	.uleb128 Lttbase0-Lttbaseref0
Lttbaseref0:
	.byte	1                               ; Call site Encoding = uleb128
	.uleb128 Lcst_end0-Lcst_begin0
Lcst_begin0:
	.uleb128 Ltmp0-Lfunc_begin0             ; >> Call Site 1 <<
	.uleb128 Ltmp1-Ltmp0                    ;   Call between Ltmp0 and Ltmp1
	.uleb128 Ltmp2-Lfunc_begin0             ;     jumps to Ltmp2
	.byte	1                               ;   On action: 1
	.uleb128 Ltmp1-Lfunc_begin0             ; >> Call Site 2 <<
	.uleb128 Ltmp3-Ltmp1                    ;   Call between Ltmp1 and Ltmp3
	.byte	0                               ;     has no landing pad
	.byte	0                               ;   On action: cleanup
	.uleb128 Ltmp3-Lfunc_begin0             ; >> Call Site 3 <<
	.uleb128 Ltmp4-Ltmp3                    ;   Call between Ltmp3 and Ltmp4
	.uleb128 Ltmp5-Lfunc_begin0             ;     jumps to Ltmp5
	.byte	1                               ;   On action: 1
Lcst_end0:
	.byte	1                               ; >> Action Record 1 <<
                                        ;   Catch TypeInfo 1
	.byte	0                               ;   No further actions
	.p2align	2, 0x0
                                        ; >> Catch TypeInfos <<
	.long	0                               ; TypeInfo 1
Lttbase0:
	.p2align	2, 0x0
                                        ; -- End function
	.section	__TEXT,__text,regular,pure_instructions
	.private_extern	___clang_call_terminate ; -- Begin function __clang_call_terminate
	.globl	___clang_call_terminate
	.weak_def_can_be_hidden	___clang_call_terminate
	.p2align	2
___clang_call_terminate:                ; @__clang_call_terminate
	.cfi_startproc
; %bb.0:
	stp	x29, x30, [sp, #-16]!           ; 16-byte Folded Spill
	mov	x29, sp
	.cfi_def_cfa w29, 16
	.cfi_offset w30, -8
	.cfi_offset w29, -16
	bl	___cxa_begin_catch
	bl	__ZSt9terminatev
	.cfi_endproc
                                        ; -- End function
.subsections_via_symbols
#elif defined(__linux__) && defined(__aarch64__)
	.arch armv8-a
	.text
	.align	2
	.p2align 3,,7
	.global	az_native_try
	.type	az_native_try, %function
az_native_try:
.LFB0:
	.cfi_startproc
	.cfi_personality 0x9b,DW.ref.__gxx_personality_v0
	.cfi_lsda 0x1b,.LLSDA0
	stp	x29, x30, [sp, -16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x2, x0
	mov	x0, x1
	add	x29, sp, 0
	.cfi_def_cfa_register 29
.LEHB0:
	blr	x2
.LEHE0:
	mov	w0, 1
.L1:
	ldp	x29, x30, [sp], 16
	.cfi_remember_state
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa 31, 0
	ret
.L4:
	.cfi_restore_state
	bl	__cxa_begin_catch
	bl	__cxa_end_catch
	mov	w0, 0
	b	.L1
	.cfi_endproc
.LFE0:
	.global	__gxx_personality_v0
	.section	.gcc_except_table,"a",@progbits
	.align	2
.LLSDA0:
	.byte	0xff
	.byte	0x9b
	.uleb128 .LLSDATT0-.LLSDATTD0
.LLSDATTD0:
	.byte	0x1
	.uleb128 .LLSDACSE0-.LLSDACSB0
.LLSDACSB0:
	.uleb128 .LEHB0-.LFB0
	.uleb128 .LEHE0-.LEHB0
	.uleb128 .L4-.LFB0
	.uleb128 0x1
.LLSDACSE0:
	.byte	0x1
	.byte	0
	.align	2
	.4byte	0

.LLSDATT0:
	.text
	.size	az_native_try, .-az_native_try
	.hidden	DW.ref.__gxx_personality_v0
	.weak	DW.ref.__gxx_personality_v0
	.section	.data.rel.local.DW.ref.__gxx_personality_v0,"awG",@progbits,DW.ref.__gxx_personality_v0,comdat
	.align	3
	.type	DW.ref.__gxx_personality_v0, %object
	.size	DW.ref.__gxx_personality_v0, 8
DW.ref.__gxx_personality_v0:
	.xword	__gxx_personality_v0
	.section	.note.GNU-stack,"",@progbits
#elif defined(__linux__) && defined(__x86_64__)
	.text
	.globl	az_native_try                   # -- Begin function az_native_try
	.p2align	4
	.type	az_native_try,@function
az_native_try:                          # @az_native_try
.Lfunc_begin0:
	.cfi_startproc
	.cfi_personality 155, DW.ref.__gxx_personality_v0
	.cfi_lsda 27, .Lexception0
# %bb.0:
	pushq	%rbx
	.cfi_def_cfa_offset 16
	.cfi_offset %rbx, -16
	movq	%rdi, %rax
	movl	$1, %ebx
.Ltmp0:
	movq	%rsi, %rdi
	callq	*%rax
.Ltmp1:
.LBB0_2:
	movl	%ebx, %eax
	popq	%rbx
	.cfi_def_cfa_offset 8
	retq
.LBB0_1:
	.cfi_def_cfa_offset 16
.Ltmp2:
	movq	%rax, %rdi
	callq	__cxa_begin_catch@PLT
	xorl	%ebx, %ebx
.Ltmp3:
	callq	__cxa_end_catch@PLT
.Ltmp4:
	jmp	.LBB0_2
.LBB0_3:
.Ltmp5:
	movq	%rax, %rdi
	callq	__clang_call_terminate
.Lfunc_end0:
	.size	az_native_try, .Lfunc_end0-az_native_try
	.cfi_endproc
	.section	.gcc_except_table,"a",@progbits
	.p2align	2, 0x0
GCC_except_table0:
.Lexception0:
	.byte	255                             # @LPStart Encoding = omit
	.byte	155                             # @TType Encoding = indirect pcrel sdata4
	.uleb128 .Lttbase0-.Lttbaseref0
.Lttbaseref0:
	.byte	1                               # Call site Encoding = uleb128
	.uleb128 .Lcst_end0-.Lcst_begin0
.Lcst_begin0:
	.uleb128 .Ltmp0-.Lfunc_begin0           # >> Call Site 1 <<
	.uleb128 .Ltmp1-.Ltmp0                  #   Call between .Ltmp0 and .Ltmp1
	.uleb128 .Ltmp2-.Lfunc_begin0           #     jumps to .Ltmp2
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp1-.Lfunc_begin0           # >> Call Site 2 <<
	.uleb128 .Ltmp3-.Ltmp1                  #   Call between .Ltmp1 and .Ltmp3
	.byte	0                               #     has no landing pad
	.byte	0                               #   On action: cleanup
	.uleb128 .Ltmp3-.Lfunc_begin0           # >> Call Site 3 <<
	.uleb128 .Ltmp4-.Ltmp3                  #   Call between .Ltmp3 and .Ltmp4
	.uleb128 .Ltmp5-.Lfunc_begin0           #     jumps to .Ltmp5
	.byte	1                               #   On action: 1
.Lcst_end0:
	.byte	1                               # >> Action Record 1 <<
                                        #   Catch TypeInfo 1
	.byte	0                               #   No further actions
	.p2align	2, 0x0
                                        # >> Catch TypeInfos <<
	.long	0                               # TypeInfo 1
.Lttbase0:
	.p2align	2, 0x0
                                        # -- End function
	.section	.text.__clang_call_terminate,"axG",@progbits,__clang_call_terminate,comdat
	.hidden	__clang_call_terminate          # -- Begin function __clang_call_terminate
	.weak	__clang_call_terminate
	.p2align	4
	.type	__clang_call_terminate,@function
__clang_call_terminate:                 # @__clang_call_terminate
	.cfi_startproc
# %bb.0:
	pushq	%rax
	.cfi_def_cfa_offset 16
	callq	__cxa_begin_catch@PLT
	callq	_ZSt9terminatev@PLT
.Lfunc_end1:
	.size	__clang_call_terminate, .Lfunc_end1-__clang_call_terminate
	.cfi_endproc
                                        # -- End function
	.hidden	DW.ref.__gxx_personality_v0
	.weak	DW.ref.__gxx_personality_v0
	.section	.data.DW.ref.__gxx_personality_v0,"awG",@progbits,DW.ref.__gxx_personality_v0,comdat
	.p2align	3, 0x0
	.type	DW.ref.__gxx_personality_v0,@object
	.size	DW.ref.__gxx_personality_v0, 8
DW.ref.__gxx_personality_v0:
	.quad	__gxx_personality_v0
	.section	".note.GNU-stack","",@progbits
#else
#error "Unsupported native exception ABI"
#endif

/* Nontrivial native result carrier. Tail forwarding retains the caller's CFI. */
.text
.p2align 2
#if defined(__APPLE__)
.globl _az_native_result
.private_extern _az_native_result
_az_native_result:
    mov x3, x0
    mov x8, x1
    mov x0, x2
    br x3
#else
.globl az_native_result
.hidden az_native_result
.type az_native_result, %function
az_native_result:
#if defined(__aarch64__)
    mov x3, x0
    mov x8, x1
    mov x0, x2
    br x3
#else
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    jmp *%rax
#endif
.size az_native_result, .-az_native_result
#endif
