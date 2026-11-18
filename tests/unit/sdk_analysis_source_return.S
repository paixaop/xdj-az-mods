/* SPDX-License-Identifier: GPL-3.0-only */
/* Firmware-model nontrivial result: the actual native result address is x8. */
.text
.p2align 2
.global fixture_get_state
.type fixture_get_state, %function
fixture_get_state:
    mov x0, x8
    b fixture_get_state_body
.size fixture_get_state, .-fixture_get_state
.section .note.GNU-stack,"",%progbits
