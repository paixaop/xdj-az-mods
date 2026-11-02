/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual azmod installer, synthetic AArch64 code and real RX mappings only. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#define dbg(...) ((void)0)
#ifndef INLINE_HOOKS_INC
#define INLINE_HOOKS_INC "../../src/azmod/inline_hooks.c"
#endif
#include INLINE_HOOKS_INC
#ifndef HAVE_ISLAND_ONLY_ENTRY
/* Transparent OLD-path baseline, without an alternate installer/model. */
#define azm_install_island_hook_at azm_install_hook_at
#endif
#define REQUIRE(x, why) do { if (!(x)) { fprintf(stderr, "FAIL cache-island: %s\n", why); abort(); } } while (0)
static void *original;
static unsigned hooks;
static int observer(void *value) { ++hooks; return ((int (*)(void *))original)(value) + 100; }
static uint32_t *make_code(void) {
    uint32_t *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(p != MAP_FAILED, "synthetic code allocation");
    for (unsigned i = 0; i < 256; ++i) p[i] = 0xd503201fu;
    /* Exact first four setter words in BOTH profiles. Conditional target lies
     * at word133; synthetic tails restore this prologue's80-byte stack frame. */
    const uint32_t prefix[] = {0xa9bb7bfdu, 0x910003fdu, 0xf9401804u, 0xb4001044u};
    memcpy(p, prefix, sizeof prefix);
    p[4] = 0x528000e0u; p[5] = 0xa8c57bfdu; p[6] = 0xd65f03c0u;
    p[133] = 0x52800060u; p[134] = 0xa8c57bfdu; p[135] = 0xd65f03c0u;
    __builtin___clear_cache((char *)p, (char *)p + 1024);
    REQUIRE(mprotect(p, 4096, PROT_READ | PROT_EXEC) == 0, "synthetic code RX");
    return p;
}
static void success(void) {
    uint32_t *code = make_code(), saved[256]; memcpy(saved, code, sizeof saved);
    uint64_t object[8] = {0}; object[6] = 1;
    int (*native)(void *) = (int (*)(void *))code;
    REQUIRE(native(object) == 7, "unmodified nonnull native branch"); object[6] = 0;
    REQUIRE(native(object) == 3, "unmodified conditional native branch");
    void *sentinel = (void *)(uintptr_t)0x1000; original = sentinel;
    REQUIRE(azm_install_hook_at((uintptr_t)code, (void *)observer, &original, "strict-baseline") == -1 &&
            original == sentinel && memcmp(code, saved, sizeof saved) == 0,
            "general installer retains strict four-instruction refusal");
    puts("NATIVE_ISLAND_BASELINE_OK original_branches=2 strict_refusal=1"); fflush(stdout);
    REQUIRE(azm_install_island_hook_at((uintptr_t)code, (void *)observer, &original, "cache-setter") == 1,
            "missing dedicated single-instruction island installer");
    REQUIRE(original && original != sentinel && code[0] != saved[0] &&
            memcmp(code + 1, saved + 1, sizeof saved - sizeof *saved) == 0,
            "only first instruction redirected; native conditional branch untouched");
    REQUIRE(g_azm_rec[g_azm_rec_n - 1].path == AZM_ISLAND, "dedicated path always uses island");
    const uint32_t *trampoline = original;
    uintptr_t back = 0; memcpy(&back, trampoline + 3, sizeof back);
    REQUIRE(trampoline[0] == saved[0] && trampoline[1] == 0x58000051u &&
            trampoline[2] == 0xd61f0220u && back == (uintptr_t)code + 4,
            "single displaced instruction and original-PC jump-back");
    object[6] = 1; REQUIRE(native(object) == 107 && hooks == 1, "nonnull native result through exact original once");
    object[6] = 0; REQUIRE(native(object) == 103 && hooks == 2, "native relative branch keeps original PC");
}
static void refusal(unsigned late, uint32_t first_branch) {
    uint32_t *code = make_code(), saved[256];
    azprot_t pt; REQUIRE(azprot_unlock((uintptr_t)code, 16, &pt) == 0, "prepare refusal fixture");
    if (first_branch) code[0] = first_branch;
    else { code[2] = 0xd503201fu; code[3] = 0xd503201fu; }
    REQUIRE(azprot_relock(&pt) == 0, "restore prepared page RX");
    if (!first_branch) for (unsigned i = 0; i < 4; ++i) {
        g_azm_island[i].base = code; g_azm_island[i].used = AZM_ISLAND_BYTES / 16;
    }
    memcpy(saved, code, sizeof saved); if (late) azm_hooks_go_late();
    void *sentinel = (void *)(uintptr_t)0x1000; original = sentinel;
    REQUIRE(azm_install_island_hook_at((uintptr_t)code, (void *)observer, &original, "refusal") < 0,
            "no island or nonrelocatable first instruction refuses");
    REQUIRE(original == sentinel && memcmp(code, saved, sizeof saved) == 0,
            "refused entry leaves original pointer and every instruction unchanged");
}
static void first_instruction_refusals(void) {
    const uint32_t branches[] = {
        0x14000001u, /* B */ 0x94000001u, /* BL */ 0x54000020u, /* B.eq */
        0xb4000040u, /* CBZ */ 0xb5000040u, /* CBNZ */
        0x36000040u, /* TBZ */ 0x37000040u, /* TBNZ */
        0x58000040u, /* LDR literal */ 0x10000000u /* ADR */
    };
    for (unsigned i = 0; i < sizeof branches / sizeof *branches; ++i) refusal(0, branches[i]);
}
static void input_guards(void) {
    uint32_t *code = make_code(), saved[256]; memcpy(saved, code, sizeof saved);
    void *sentinel = (void *)(uintptr_t)0x1000; original = sentinel;
    REQUIRE(azm_install_island_hook_at(0, (void *)observer, &original, "null-entry") < 0 &&
            azm_install_island_hook_at((uintptr_t)code + 1, (void *)observer, &original, "unaligned-entry") < 0 &&
            azm_install_island_hook_at((uintptr_t)code, NULL, &original, "null-hook") < 0 &&
            azm_install_island_hook_at((uintptr_t)code, (void *)((uintptr_t)observer + 1), &original, "unaligned-hook") < 0 &&
            azm_install_island_hook_at((uintptr_t)code, (void *)observer, NULL, "null-output") < 0,
            "invalid dedicated installer inputs refuse before redirect");
    REQUIRE(original == sentinel && memcmp(code, saved, sizeof saved) == 0 && !g_hooked_n,
            "input refusal leaves original output, code and hook ownership unchanged");
}
int main(void) {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        pid_t child = fork(); REQUIRE(child >= 0, "fixture subprocess");
        if (!child) {
            if (!scenario) success();
            else if (scenario < 3) refusal(scenario == 2, 0);
            else if (scenario == 3) first_instruction_refusals();
            else input_guards();
            _exit(0);
        }
        int status; REQUIRE(waitpid(child, &status, 0) == child, "fixture wait");
        REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0, "isolated fixture scenario failed");
    }
    puts("PASS cache-island: native branches, single original instruction, strict policy, no-island/relative/input refusal");
    return 0;
}
