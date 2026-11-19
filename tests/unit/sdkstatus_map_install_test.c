/* SPDX-License-Identifier: GPL-3.0-only */
#define main maps_baseline_main
#include "sdkstatus_maps_test.c"
#undef main
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static uintptr_t sites[4];
static unsigned patches;
static int failure, mode;
static void original(void) {}
static int patch(uintptr_t address, void *replacement, void **retained, const char *label) {
    assert(replacement && retained && !strcmp(label, "status-map-writer"));
    unsigned index;
    for (index = 0; index < 4; ++index)
        if (sites[index] == address) break;
    assert(index < 4 && !*retained);
    if ((int) index == failure) return mode == 1 ? 1 : 0;
    *retained = (void *) original;
    *(uint32_t *) address = 0x14000000u;
    ++patches;
    return 1;
}
int main(int argc, char **argv) {
    assert(argc == 5);
    int build = atoi(argv[1]);
    bool analysis = !strcmp(argv[2], "analysis");
    failure = atoi(argv[3]);
    mode = atoi(argv[4]);
    uintptr_t bias = 0;
    size_t bytes = 0x2200000;
    void *memory;
    if (build == 2) memory = mmap((void *) 0x600000, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    else {
        memory = mmap(NULL, 0x3000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        bias = (uintptr_t) memory;
        bytes = 0x3000000;
    }
    assert(memory != MAP_FAILED && (build != 2 || memory == (void *) 0x600000));
    const uintptr_t offsets[][4] = {{0xd295b0, 0xd29b20, 0x176e748, 0x176fbe0}, {0xdb34d0, 0xdb3a40, 0x18c9d30, 0x18cb1e8}};
    const uint32_t words[] = {0xc8dffc03, 0xf9400802, 0xa9b17bfd, 0xa9b67bfd};
    for (unsigned i = 0; i < 4; ++i) {
        sites[i] = bias + offsets[build - 1][i];
        *(uint32_t *) sites[i] = words[i];
    }
    struct Site {
        uintptr_t address;
        uint32_t words[4];
    } const analysis_sites[] = {
        {0xdafa38, {0xa9a47bfd, 0x910003fd, 0xa90363f7, 0xf9400017}},
        {0x2477df0, {0xa900081f, 0xb5000042, 0xd65f03c0, 0xa9bd7bfd}},
        {0x2477e70, {0xf9400000, 0x177eb92b, 0xd503201f, 0xd503201f}},
        {0x6f2728, {0xa9bb7bfd, 0x5289eb03, 0x91003002, 0x910003fd}},
        {0x6f2848, {0xa9bc7bfd, 0x5289eb01, 0x72aa4861, 0x910003fd}},
        {0x2371890, {0xa9b77bfd, 0x910003fd, 0x910203e8, 0xa90153f3}}};
    if (build == 2)
        for (unsigned i = 0; i < 6; ++i) memcpy((void *) analysis_sites[i].address, analysis_sites[i].words, 16);
    assert(!azn_maps_install(0, bias, patch) && !azn_maps_install(3, bias, patch) && !azn_maps_install(build, bias, NULL));
    int failed = failure;
    if (analysis) {
        assert(build == 2 && failed >= 0 && failed < 6);
        *(uint32_t *) analysis_sites[failed].address ^= 1;
        failure = -1;
    } else if (failed >= 0 && mode == 2) *(uint32_t *) sites[failed] ^= 1;
    unsigned expected = analysis || failed < 0 ? 15u : 15u ^ (1u << failed);
    assert(azn_maps_install(build, bias, patch) == expected);
    assert(patches == (expected == 15 ? 4u : 3u));
    if (analysis) {
        assert(!atomic_load(&analysis_ready));
        memcpy((void *) analysis_sites[failed].address, analysis_sites[failed].words, 16);
    } else if (failed >= 0) *(uint32_t *) sites[failed] = words[failed];
    failure = -1;
    assert(azn_maps_install(build, bias, patch) == 15 && patches == 4);
    assert(atomic_load(&analysis_ready) == (build == 2));
    assert(azn_maps_install(build, bias, patch) == 15 && patches == 4);
    assert(!munmap(memory, bytes));
    return 0;
}
