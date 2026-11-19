/* SPDX-License-Identifier: GPL-3.0-only */
/* Real ARM64 entry patches on private executable pages; no firmware addresses. */
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
#define dbg(...) ((void) 0)
#include "../../src/azmod/inline_hooks.c"

static void *original;
static unsigned calls;
static int replacement(void) {
    ++calls;
    return ((int (*)(void)) original)() + 100;
}
static uint32_t *native_code(int short_function) {
    uint32_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    for (unsigned i = 0; i < 16; ++i) code[i] = 0xd503201fu;
    code[0] = 0x528000e0u; /* mov w0,#7 */
    code[short_function ? 1 : 4] = 0xd65f03c0u; /* ret */
    __builtin___clear_cache((char *) code, (char *) code + 64);
    assert(mprotect(code, 4096, PROT_READ | PROT_EXEC) == 0);
    assert(((int (*)(void)) code)() == 7);
    /* Exhaust actual island capacity to exercise the constructor fallback. */
    for (unsigned i = 0; i < 4; ++i) {
        g_azm_island[i].base = code;
        g_azm_island[i].used = AZM_ISLAND_BYTES / 16;
    }
    return code;
}
static void direct_patch(void) {
    uint32_t *code = native_code(0), saved[16];
    memcpy(saved, code, sizeof saved);
    original = NULL;
    assert(azm_install_hook_at((uintptr_t) code, (void *) replacement, &original, "direct-test") == 1);
    assert(original && original != code && g_hooked_n == 1 && g_hooked[0].addr == (uintptr_t) code);
    assert(g_azm_rec_n == 1 && g_azm_rec[0].path == AZM_FALLBACK);
    assert(memcmp(code, saved, 16) != 0 && memcmp(code + 4, saved + 4, sizeof saved - 16) == 0);
    assert(azprot_query((uintptr_t) code, (uintptr_t) code + 64) == (PROT_READ | PROT_EXEC));
    assert(azprot_query((uintptr_t) original, (uintptr_t) original + 32) == (PROT_READ | PROT_EXEC));
    assert(((int (*)(void)) original)() == 7 && calls == 0);
    assert(((int (*)(void)) code)() == 107 && calls == 1);
}
static void refused_patch(unsigned scenario) {
    uint32_t *code = native_code(scenario == 1), saved[16];
    memcpy(saved, code, sizeof saved);
    if (scenario == 0) azm_hooks_go_late();
    void *sentinel = (void *) (uintptr_t) 0x1000;
    original = sentinel;
    int rc = scenario == 2 ? azm_install_island_hook_at((uintptr_t) code, (void *) replacement, &original, "island-only-test")
                           : azm_install_hook_at((uintptr_t) code, (void *) replacement, &original, "refusal-test");
    assert(rc == (scenario == 0 ? AZM_REFUSED_LATE : AZM_DECLINED));
    assert(original == sentinel && memcmp(code, saved, sizeof saved) == 0 && g_hooked_n == 0);
    assert(g_azm_rec_n == 1 && g_azm_rec[0].path == rc);
    assert(((int (*)(void)) code)() == 7);
}
int main(void) {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            if (!scenario) direct_patch();
            else refused_patch(scenario - 1);
            /* Normal termination also retains counters for these isolated scenarios. */
            exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("PASS inline entries: native fallback, original trampoline, RX restoration, late/short/island-only refusal");
    return 0;
}
