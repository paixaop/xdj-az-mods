/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azslipobserver.h"
#include "../../src/common/azsha256.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "native_abi_fixture.h"
#ifdef __aarch64__
#include <sys/mman.h>
#include <unistd.h>
#endif
extern uintptr_t azsdk_slip_getter_returns[3];
#ifndef __aarch64__
void azsdk_slip_getter_entry_0() {0} void azsdk_slip_getter_entry_1() {0} void azsdk_slip_getter_entry_2() {0}
#else
int slip_getter_test_patch(uintptr_t, void *, void **, void *);
uint64_t slip_getter_test_native(void *, void **, void *, unsigned);
void slip_getter_test_begin(), slip_getter_test_end(), slip_getter_test_site0(), slip_getter_test_site1(), slip_getter_test_site2();
uint64_t slip_getter_test_target(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, double, double, double, double, double, double, double, double);
uintptr_t slip_getter_test_sp, slip_getter_test_entry_sp, slip_getter_test_flags;
#endif
static unsigned installed,
    fail_at = 99, records, record_site;
static uintptr_t record_source, record_target;
static struct azsdk_slip_native_profile profile;
static int fake_install(uintptr_t target, void *unused, void **out, void *unused2) {
    (void) unused;
    (void) unused2;
    assert(installed < 3 && target == profile.getter_sites[installed]);
    assert(azsdk_slip_getter_returns[installed] == target + 4);
    if (installed++ == fail_at) return 0;
    *out = ((void *) (uintptr_t) (0x130000 + installed * 16));
    return 1;
}
#ifdef __aarch64__
static void *table_page;
static unsigned invoked, throws;
uint64_t slip_getter_test_body(void *self, uint64_t target, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                               double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7) {
    assert(self == ((void *) (0x130200)) && target == ((uintptr_t) (slip_getter_test_target)));
    assert(a2 == 2 && a3 == 3 && a4 == 4 && a5 == 5 && a6 == 6 && a7 == 7);
    assert(d0 == 1 && d1 == 2 && d2 == 3 && d3 == 4 && d4 == 5 && d5 == 6 && d6 == 7 && d7 == 8);
    assert(slip_getter_test_sp == slip_getter_test_entry_sp && slip_getter_test_flags == UINT64_C(0x60000000));
    assert(((uintptr_t) (__builtin_return_address(0))) == profile.getter_sites[record_site] + 8);
    assert(errno == EINTR);
    ++invoked;
    errno = EDOM;
    if (throws) fixture_raise();
    return UINT64_C(0x200130fefefefe);
}
#endif
void azsdk_slip_capture_getter(unsigned site, uintptr_t source, uintptr_t target) {
    const int old = errno;
    ++records;
    record_site = site;
    record_source = source;
    record_target = target;
#ifdef __aarch64__
    // A second displaced LDR after this point would fault, exposing accidental
    // trampoline execution. The native getter itself never reads this table.
    assert(!mprotect(table_page, 4096, PROT_NONE));
    asm volatile("mov x8,#88; movi v0.16b,#0x55; movi v1.16b,#0x66; cmp x8,#0" ::: "x8", "v0", "v1", "cc");
#endif
    errno = old;
}
#ifdef __aarch64__
struct GetterCall {
    void **table;
    unsigned site;
    uint64_t result;
};
static void getter_invoke(void *context) {
    struct GetterCall *call = context;
    call->result = slip_getter_test_native((void *) 0x130200, call->table, (void *) (uintptr_t) (0x130300 + call->site * 16), call->site);
}
#endif
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *mode = argv[1];
    _Alignas(16) uint32_t code[64];
    for (unsigned j = 0; j < 64; ++j) code[j] = 0xd503201f;
    const uint32_t ldr[] = {0xf9401021, 0xf9403421, 0xf9403821};
    for (unsigned i = 0; i < 3; i++) {
        code[8 + i * 8] = ldr[i];
        code[9 + i * 8] = 0xd63f0020;
        profile.getter_sites[i] = ((uintptr_t) (&code[8 + i * 8]));
    }
    profile.producer = ((uintptr_t) (code));
    profile.producer_bytes = sizeof code;
    azsha256_hex(code, sizeof code, profile.producer_sha256);
    bool expected = !strcmp(mode, "success");
    if (!strcmp(mode, "body")) code[60] ^= 1;
    else if (!strcmp(mode, "site")) {
        profile.getter_sites[0] += 4;
    } else if (!strcmp(mode, "partial")) {
        fail_at = 1;
    } else assert(expected || !strcmp(mode, "arm"));
#ifdef __aarch64__
    if (!strcmp(mode, "arm")) {
        profile.producer = ((uintptr_t) (slip_getter_test_begin));
        profile.producer_bytes = ((uintptr_t) (slip_getter_test_end)) - profile.producer;
        const uintptr_t sites[] = {((uintptr_t) (slip_getter_test_site0)), ((uintptr_t) (slip_getter_test_site1)), ((uintptr_t) (slip_getter_test_site2))};
        uint32_t before[3][4];
        for (unsigned i = 0; i < 3; i++) {
            profile.getter_sites[i] = sites[i];
            memcpy(before[i], ((void *) (sites[i])), 16);
        }
        azsha256_hex(((void *) (profile.producer)), profile.producer_bytes, profile.producer_sha256);
        table_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(table_page != MAP_FAILED);
        __auto_type table = ((void **) (table_page));
        table[4] = table[13] = table[14] = ((void *) (slip_getter_test_target));
        assert(azsdk_slip_getters_install(&profile, slip_getter_test_patch, NULL) && azsdk_slip_getter_hooks_v1 == 7);
        for (unsigned i = 0; i < 3; i++) {
            __auto_type words = ((uint32_t *) (sites[i]));
            assert((words[0] & 0xfc000000) == 0x14000000);
            assert(!memcmp(words + 1, before[i] + 1, 12) && words[1] == 0xd63f0020); // BLR and native return PC remain untouched.
            for (unsigned throwing = 0; throwing < 2; throwing++) {
                throws = throwing;
                record_site = i;
                errno = EINTR;
                bool caught = false;
                const unsigned prior = invoked, old_records = records;
                struct GetterCall call = {table, i, 0};
                caught = !az_native_try(getter_invoke, &call);
                if (!caught) assert(!throwing && call.result == UINT64_C(0x200130fefefefe));
                assert(caught == (bool) throwing && errno == EDOM && invoked == prior + 1 && records == old_records + 1);
                assert(record_site == i && record_source == 0x130300 + i * 16 && record_target == ((uintptr_t) (slip_getter_test_target)));
                assert(!mprotect(table_page, 4096, PROT_READ | PROT_WRITE));
            }
        }
        assert(!munmap(table_page, 4096));
        puts("PASS actual island LDR-once/BLR/LR/SP/GPR/FP/flags/results/errno/native exception");
        return 0;
    }
#endif
    assert((bool) azsdk_slip_getters_install(&profile, fake_install, NULL) == expected);
    assert(!azsdk_slip_getters_install(&profile, fake_install, NULL));
    assert(azsdk_slip_getter_hooks_v1 == (expected ? 7u : !strcmp(mode, "partial") ? 1u
                                                                                   : 0u));
    assert(installed == (expected ? 3u : !strcmp(mode, "partial") ? 2u
                                                                  : 0u));
    printf("PASS checked getter install %s\n", mode);
}
