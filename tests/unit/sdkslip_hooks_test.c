/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azslipobserver.h"
#include "../../src/common/azsha256.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "native_abi_fixture.h"
void azsdk_slip_dispatch_call(struct azce_call *);
extern void *azsdk_slip_entry_originals[AZCE_KIND_COUNT];
#ifdef AZSDK_SLIP_ARM_ENTRY
void azsdk_slip_entry_3(void);
#else
#define ENTRY(n) void azsdk_slip_entry_##n(void) {0}
ENTRY(0)
ENTRY(1)
ENTRY(2) ENTRY(3)
#undef ENTRY
    void azsdk_slip_invoke_original(struct azce_call *c) {
    c->result[0] = 0x130200;
    errno = ERANGE;
}
#endif
_Alignas(16) static unsigned char code[4][32], owner[32], registration[32];
static struct azsdk_slip_native_profile profile = {0};
static unsigned calls, fail_at = 99;
static bool duplicate, unaligned;
static int install(uintptr_t target, void *unused, void **out, void *unused2) {
    (void) unused;
    (void) unused2;
    const unsigned order[] = {2, 3, 0, 1};
    assert(calls < 4 && target == profile.targets[order[calls]]);
    assert(!azsdk_slip_ready());
#ifndef AZSDK_SLIP_ARM_ENTRY
    struct azce_call frame = {0};
    frame.kind = 2;
    errno = EINTR;
    azsdk_slip_dispatch_call(&frame);
    assert(frame.result[0] == 0x130200 && errno == ERANGE);
#endif
    if (calls++ == fail_at) return 0;
    *out = ((void *) (uintptr_t) (unaligned ? 0x100001 : duplicate ? 0x100000
                                                                   : 0x100000 + calls * 16));
    return 1;
}
#ifdef AZSDK_SLIP_ARM_ENTRY
struct Result {
    unsigned long long a, b;
};
static bool throwing;
struct Result native_original(unsigned long long a, unsigned long long b, unsigned long long c, unsigned long long d,
                              unsigned long long e, unsigned long long f, unsigned long long g, unsigned long long h) {
    assert(a == 1 && b == UINT32_MAX && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 && h == 8 && errno == EINTR);
    errno = EDOM;
    if (throwing) fixture_raise();
    return (struct Result) {0x130200, 0x87654321};
}
static void arm_throw(void *unused) {
    (void) unused;
    typedef struct Result (*Fn)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);
    Fn entry = (Fn) azsdk_slip_entry_3;
    entry(1, UINT32_MAX, 3, 4, 5, 6, 7, 8);
}
static void arm_entry() {
    azsdk_slip_entry_originals[3] = ((void *) (native_original));
    typedef struct Result (*Fn)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);
    Fn entry = (Fn) azsdk_slip_entry_3;
    errno = EINTR;
    struct Result result = entry(1, UINT32_MAX, 3, 4, 5, 6, 7, 8);
    assert(result.a == 0x130200 && result.b == 0x87654321 && errno == EDOM);
    throwing = true;
    bool caught = false;
    errno = EINTR;
    caught = !az_native_try(arm_throw, NULL);
    assert(caught && errno == EDOM);
    puts("PASS actual Slip entry3 integer/result/errno/CFI exception forwarding");
}
#endif
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *mode = argv[1];
    for (unsigned i = 0; i < 4; i++) {
        for (unsigned n = 0; n < 32; n++) code[i][n] = ((unsigned char) (n + i * 13));
        profile.targets[i] = ((uintptr_t) (code[i]));
        memcpy(profile.expected[i], code[i], 16);
        profile.code_bytes[i] = 32;
        azsha256_hex(code[i], 32, profile.code_sha256[i]);
    }
    profile.owner_scope = ((uintptr_t) (owner));
    profile.owner_scope_bytes = 32;
    azsha256_hex(owner, 32, profile.owner_scope_sha256);
    profile.construct_return = 0x130200;
    profile.registration = ((uintptr_t) (registration));
    profile.registration_bytes = 32;
    azsha256_hex(registration, 32, profile.registration_sha256);
    for (unsigned i = 0; i < 4; i++) profile.parent_returns[i] = 0x130208 + i * 4;
    profile.source_control_vtable = 0x130300;
    profile.parent_control_vtable = 0x130308;
    bool expected = false;
    if (!strcmp(mode, "success")) expected = true;
    else if (!strcmp(mode, "prefix")) code[0][0] ^= 1;
    else if (!strcmp(mode, "body")) code[2][20] ^= 1;
    else if (!strcmp(mode, "owner")) owner[20] ^= 1;
    else if (!strcmp(mode, "partial")) fail_at = 3;
    else if (!strcmp(mode, "duplicate")) duplicate = true;
    else if (!strcmp(mode, "unaligned")) unaligned = true;
    else assert(false);
    assert(!!azsdk_slip_install(&profile, install, NULL) == expected);
    assert(!!azsdk_slip_ready() == expected);
    assert(!azsdk_slip_install(&profile, install, NULL));
    if (!strcmp(mode, "prefix") || !strcmp(mode, "body") || !strcmp(mode, "owner")) assert(calls == 0);
    if (expected) {
        assert(calls == 4);
#ifdef AZSDK_SLIP_ARM_ENTRY
        arm_entry();
#endif
    }
    printf("PASS Slip checked startup %s\n", mode);
}
