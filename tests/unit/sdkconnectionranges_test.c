/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the production ELF range collector on malformed metadata. No mapped
 * native object or module is dereferenced by these validation cases. */
#include "../../src/azmod/sdk_connection_hooks.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    ElfW(Phdr) segments[] = {
        {.p_type = PT_NOTE, .p_flags = PF_R, .p_vaddr = 1, .p_memsz = 4},
        {.p_type = PT_LOAD, .p_flags = PF_W, .p_vaddr = 2, .p_memsz = 4},
        {.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = 0x100, .p_memsz = 0x80},
        {.p_type = PT_LOAD, .p_flags = PF_R, .p_vaddr = 0x200, .p_memsz = 0x40}};
    struct az_sql_profile profile = {.base_ = 0x1000};
    struct dl_phdr_info image = {.dlpi_addr = 0x2000, .dlpi_phdr = segments, .dlpi_phnum = 4};
    assert(!ranges(&image, sizeof image, &profile) && !profile.count_);
    image.dlpi_addr = profile.base_;
    assert(ranges(&image, sizeof image, &profile) && profile.count_ == 2);
    assert(az_sql_profile_contains(&profile, 0x1100, 0x80, PF_R | PF_X));
    assert(!az_sql_profile_contains(&profile, 0x117f, 2, PF_R | PF_X));
    assert(!az_sql_profile_contains(&profile, 0x1200, 1, PF_X));
    assert(az_sql_profile_contains(&profile, 0x1200, 0x40, PF_R));
    profile.count_ = 32;
    assert(ranges(&image, sizeof image, &profile) && !profile.count_);
    image.dlpi_phnum = 1;
    segments[0] = (ElfW(Phdr)) {.p_type = PT_LOAD, .p_flags = PF_R, .p_vaddr = UINTPTR_MAX};
    assert(ranges(&image, sizeof image, &profile) && !profile.count_);
    segments[0].p_vaddr = 0;
    segments[0].p_memsz = UINTPTR_MAX;
    assert(ranges(&image, sizeof image, &profile) && !profile.count_);
    puts("PASS connection ELF ranges: exact image, readable loads, permissions, capacity and address-overflow refusal");
    return 0;
}
