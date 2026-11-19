/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/azstem/sdk_analysis_source.c"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
static uintptr_t image_bias;
static unsigned image_scans, patch_calls, patch_mode;
int __wrap_dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *opaque) {
    ++image_scans;
    struct dl_phdr_info shared = {.dlpi_addr = 17, .dlpi_name = "libforeign.so"};
    assert(!callback(&shared, sizeof shared, opaque));
    struct dl_phdr_info executable = {.dlpi_addr = image_bias, .dlpi_name = ""};
    return callback(&executable, sizeof executable, opaque);
}
int azsdk_load_read(int deck, struct azsdk_load_sample *sample) {
    (void) deck;
    (void) sample;
    return 0;
}
int azn_load_track_matches(unsigned deck, uint64_t epoch, uint64_t load, const unsigned char id[12]) {
    (void) deck;
    (void) epoch;
    (void) load;
    (void) id;
    return 0;
}
static int native_wait(uintptr_t owner, const WeakTrack *weak, int generation) {
    (void) owner;
    (void) weak;
    (void) generation;
    return 13;
}
static int patch(uintptr_t address, void *replacement, void **original, const char *name) {
    assert(address == image_bias + 0xd39610 && replacement == (void *) wait_decrypt);
    assert(!strcmp(name, "sdk-beatport-analysis"));
    ++patch_calls;
    if (!patch_mode) return 0;
    if (patch_mode == 1) return 1;
    *original = (void *) native_wait;
    *(uint32_t *) address = 0x14000000u;
    return 1;
}
int main(void) {
    size_t size = 0xd3a000;
    void *image = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    image_bias = (uintptr_t) image;
    const struct azn_analysis_source_api *api = azn_analysis_sources();
    assert(api->install(1, patch) == AZ_UNSUPPORTED && api->install(2, NULL) == AZ_UNSUPPORTED);
    assert(!image_scans && !patch_calls && !atomic_load(&installed));
    assert(api->install(2, patch) == AZ_UNSUPPORTED && image_scans == 1 && !patch_calls);
    const uint32_t prologue[] = {0xa9b57bfdu, 0x910003fdu, 0x6d0527e8u, 0x0f000408u};
    memcpy((void *) (image_bias + 0xd39610), prologue, sizeof prologue);
    assert(api->install(2, patch) == AZ_FAILED && patch_calls == 1 && !atomic_load(&installed));
    patch_mode = 1;
    assert(api->install(2, patch) == AZ_FAILED && patch_calls == 2 && !atomic_load(&installed));
    patch_mode = 2;
    assert(api->install(2, patch) == AZ_OK && patch_calls == 3 && atomic_load(&installed));
    assert(original_wait == (void *) native_wait);
    assert(api->install(2, patch) == AZ_OK && patch_calls == 3 && image_scans == 4);
    assert(api->install(1, patch) == AZ_UNSUPPORTED && patch_calls == 3);
    assert(!munmap(image, size));
    puts("PASS analysis installer: image selection, profile/signature refusal, failed or empty patch, retry and mutated-prologue idempotence");
    return 0;
}
