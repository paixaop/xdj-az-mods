/* SPDX-License-Identifier: GPL-3.0-only */
/* Validate actual observer admission against synthetic ELF metadata only. */
#include "../../src/azstem/sdk_loop_observer.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    ElfW(Phdr) segments[] = {
        {.p_type = PT_NOTE, .p_flags = PF_R, .p_vaddr = 1, .p_memsz = 4},
        {.p_type = PT_LOAD, .p_flags = PF_W, .p_vaddr = 2, .p_memsz = 4},
        {.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = 0x100, .p_memsz = 0x80}};
    struct dl_phdr_info candidate = {.dlpi_name = "foreign.so", .dlpi_addr = 0x1000, .dlpi_phdr = segments, .dlpi_phnum = 3};
    assert(!main_image(&candidate, sizeof candidate, NULL) && !image_count);
    candidate.dlpi_name = "";
    assert(main_image(&candidate, sizeof candidate, NULL) && image_count == 1);
    assert(image_contains(0x1100, 0x80, PF_R | PF_X));
    assert(!image_contains(0x117f, 2, PF_R | PF_X));
    image_count = sizeof image / sizeof *image;
    assert(main_image(&candidate, sizeof candidate, NULL) && !image_count);
    candidate.dlpi_phnum = 1;
    segments[0] = (ElfW(Phdr)) {.p_type = PT_LOAD, .p_flags = PF_R, .p_vaddr = UINTPTR_MAX};
    assert(main_image(&candidate, sizeof candidate, NULL) && !image_count);
    segments[0].p_vaddr = 0;
    segments[0].p_memsz = UINTPTR_MAX;
    assert(main_image(&candidate, sizeof candidate, NULL) && !image_count);
    puts("PASS loop observer ELF guards: main-image ownership, readable ranges, capacity and overflow refusal");
    return 0;
}
