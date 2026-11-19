/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual ELF ranges, executable bytes, thread stack and shared-library identity. */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/azstem/overview_lease.c"
__asm__(".text\n.balign 16\n.global overview_code\n.hidden overview_code\noverview_code:\n.zero 11000\n");
extern const unsigned char overview_code[];
static void release_owner(void *owner, void *object) {
    (void) owner;
    (void) object;
    assert(0);
}
static void ranges(void) {
    ElfW(Phdr) segments[4] = {
        {.p_type = PT_NOTE, .p_flags = PF_R | PF_X, .p_vaddr = 0, .p_memsz = 20000},
        {.p_type = PT_LOAD, .p_flags = PF_R, .p_vaddr = 0, .p_memsz = 20000},
        {.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = 0, .p_memsz = 1000},
        {.p_type = PT_LOAD, .p_flags = PF_R | PF_X, .p_vaddr = 2000, .p_memsz = 10000},
    };
    struct dl_phdr_info image = {.dlpi_addr = 0x10000, .dlpi_name = "", .dlpi_phdr = segments, .dlpi_phnum = 4};
    struct azov_native_profile profile = {.setter = 0x10000, .updater = 0x10000 + 2000};
    assert(azov_main_range(&image, sizeof image, &profile) == 1);
    image.dlpi_name = "other.so";
    assert(!azov_main_range(&image, sizeof image, &profile));
    image.dlpi_name = "";
    profile.setter += 301;
    assert(azov_main_range(&image, sizeof image, &profile) == 2);
    profile.setter -= 301;
    profile.updater += 469;
    assert(azov_main_range(&image, sizeof image, &profile) == 2);
    segments[2].p_vaddr = UINTPTR_MAX;
    assert(!azov_executable_segment(&image, &segments[2]));
    segments[2].p_vaddr = 0;
    segments[2].p_memsz = UINTPTR_MAX;
    assert(!azov_executable_segment(&image, &segments[2]));
    assert(!azov_range_contains(10, 20, 9, 1) && !azov_range_contains(10, 20, 21, 0));
    assert(azov_range_contains(10, 20, 20, 0) && !azov_range_contains(10, 20, 20, 1));
    assert(azov_range_contains(10, 20, 10, 10));
}
static void profile_and_stack(void) {
    struct azov_native_profile profile = {
        .setter = (uintptr_t) overview_code,
        .updater = (uintptr_t) overview_code + 1000,
        .owner_release = (uintptr_t) release_owner,
    };
    profile.setter_hash = azov_hash(profile.setter, 700);
    profile.updater_hash = azov_hash(profile.updater, 9532);
    assert(azov_profile_verified(&profile, release_owner));
    assert(!azov_profile_verified(&profile, NULL));
    profile.setter_hash ^= 1;
    assert(!azov_profile_verified(&profile, release_owner));
    profile.setter_hash ^= 1;
    profile.updater_hash ^= 1;
    assert(!azov_profile_verified(&profile, release_owner));
    profile.updater_hash ^= 1;
    profile.setter = (uintptr_t) &profile;
    assert(!azov_profile_verified(&profile, release_owner));
    assert(azov_prepare_stack());
    uintptr_t here = (uintptr_t) &profile;
    assert(azov_stack_lo < here && here < azov_stack_hi && azov_stack_hi - azov_stack_lo >= 0xd8);
    for (int build = 1; build <= 2; ++build) {
        azov_profile = NULL;
        azov_prepare_profile(build, release_owner);
        assert(!azov_profile);
    }
}
int main(int argc, char **argv) {
    assert(argc == 4);
    ranges();
    profile_and_stack();
    void *first = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    void *other_name = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    void *other_image = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
    assert(first && other_name && other_image);
    void *entry = dlsym(first, "overview_client");
    assert(entry && !azov_accept_client(NULL) && !azov_accept_client((void *) main));
    assert(!azov_accept_client(dlsym(other_name, "overview_client")));
    assert(azov_accept_client(entry) && azov_accept_client(entry));
    assert(!azov_accept_client(dlsym(other_image, "overview_client")));
    azov_lease_configure(1, release_owner);
    assert(!azov_profile && azov_configured && azov_caller_ok(entry) && !azov_caller_ok((void *) main));
    int (*client)(int (*)(uint32_t, struct azov_lease_api *, size_t), struct azov_lease_api *) = NULL;
    memcpy(&client, &entry, sizeof client);
    struct azov_lease_api api = {0};
    assert(client(azstem_overview_lease_v1, &api));
    assert(api.abi_version == AZOV_LEASE_ABI && (api.flags & AZOV_LEASE_PROFILE_UNAVAILABLE));
    dlclose(other_image);
    dlclose(other_name);
    dlclose(first);
    puts("PASS native overview: executable bounds, real ELF byte hashes, stack and client-image admission");
}
