/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file inline_hooks.c
 * @brief AArch64 inline-hook and vtable-patch primitives used by every azmod hook
 *
 * azm_install_hook replaces the first instruction of an EP147 function with a B to a
 * 16-byte jump in a nearby executable island (one atomic store, so a thread already in
 * the function sees the old or the new code, never half of each), and builds a trampoline
 * holding the relocated instruction followed by a jump back. Without an island in B range
 * it falls back to overwriting four instructions with an absolute jump (LDR X17 / BR X17).
 * The fallback is allowed only while azmod's constructor runs (EP147's main has not started,
 * so no EP147 code can be executing); after azm_hooks_go_late() a hook without an island in
 * reach is refused (AZM_REFUSED_LATE) instead of being written non-atomically under a running
 * EP147. Every call is recorded with the path it took and azm_hooks_dump() writes the table.
 * The trampoline is returned through *orig. A function whose first four instructions
 * contain a PC-relative branch, literal load or adr is declined (returns -1) because it
 * cannot be relocated safely; a null or misaligned address is also declined. An adrp is
 * re-encoded when its target still reaches from the trampoline, otherwise its absolute
 * result is materialized with movz/movk.
 *
 * azm_install_island_hook_at is an explicit one-instruction alternative for a
 * separately reviewed entry. It validates only that displaced instruction,
 * requires a reachable island, and never uses the four-instruction fallback.
 * The general installer's stricter four-instruction policy is unchanged.
 *
 * azm_patch_vtable swaps one vtable slot and returns the
 * original, which avoids prologue relocation entirely but only intercepts virtual calls
 * through that vtable. Callers must check the return value and treat a decline as "hook
 * not installed".
 *
 * Entry points: azm_install_hook, azm_patch_vtable, azm_hooks_go_late, azm_hooks_dump.
 */
#include "../common/azprot.h" /* azprot_unlock / azprot_relock */
/* --- AArch64 inline-hook installer --- */
/* 2 = adrp (relocatable), 1 = PC-relative and not relocatable, 0 = position-independent. */
static int azm_pcrel_kind(uint32_t in) {
    if ((in & 0x9F000000u) == 0x90000000u)
        return 2; /* adrp (relocatable) */
    if ((in & 0x9F000000u) == 0x10000000u)
        return 1; /* adr  */
    if ((in & 0xFC000000u) == 0x14000000u)
        return 1; /* b */
    if ((in & 0xFC000000u) == 0x94000000u)
        return 1; /* bl */
    if ((in & 0xFF000010u) == 0x54000000u)
        return 1; /* b.cond */
    if ((in & 0x7E000000u) == 0x34000000u)
        return 1; /* cbz/cbnz */
    if ((in & 0x7E000000u) == 0x36000000u)
        return 1; /* tbz/tbnz */
    if ((in & 0x3B000000u) == 0x18000000u)
        return 1; /* ldr(literal) */
    return 0;
}
/* Absolute page address an adrp at `pc` computes. */
static uint64_t azm_adrp_target(uint32_t in, uintptr_t pc) {
    uint32_t immlo = (in >> 29) & 0x3u, immhi = (in >> 5) & 0x7FFFFu;
    int64_t imm = (int64_t) ((immhi << 2) | immlo);
    if (imm & (1LL << 20))
        imm |= ~((1LL << 21) - 1);
    return (uint64_t) ((pc & ~0xFFFUL) + ((uintptr_t) imm << 12));
}
/* Materialize a full 64-bit constant into Xd: movz + 3x movk. Used for an adrp whose
 * target is out of range from the trampoline: the absolute value it would have produced
 * is position-independent, so it works wherever mmap put the trampoline.
 *   MOVZ Xd,#imm16,LSL#s = 0xD2800000 | hw<<21 | imm16<<5 | Rd
 *   MOVK Xd,#imm16,LSL#s = 0xF2800000 | hw<<21 | imm16<<5 | Rd   (hw = s/16) */
static void azm_emit_abs_addr(uint32_t *out, int rd, uint64_t val) {
    out[0] = 0xD2800000u | (0u << 21) | ((uint32_t) (val & 0xFFFFu) << 5) | (uint32_t) rd;
    out[1] = 0xF2800000u | (1u << 21) | ((uint32_t) ((val >> 16) & 0xFFFFu) << 5) | (uint32_t) rd;
    out[2] = 0xF2800000u | (2u << 21) | ((uint32_t) ((val >> 32) & 0xFFFFu) << 5) | (uint32_t) rd;
    out[3] = 0xF2800000u | (3u << 21) | ((uint32_t) ((val >> 48) & 0xFFFFu) << 5) | (uint32_t) rd;
}
/* Can this adrp be re-encoded at new_pc? adrp reaches +-4 GB, and an mmap'd trampoline
 * usually lands around 0x7f........ while EP147's text is at 0x1......, far out of range.
 * azm_reloc_adrp truncates to 21 bits, so it must only be used when this returns true;
 * otherwise a garbage page address is loaded and the next instruction dereferences it.
 * When it is false, azm_install_hook materializes the absolute address instead. */
static int azm_adrp_reloc_ok(uint32_t in, uintptr_t old_pc, uintptr_t new_pc) {
    uint32_t immlo = (in >> 29) & 0x3u, immhi = (in >> 5) & 0x7FFFFu;
    int64_t imm = (int64_t) ((immhi << 2) | immlo);
    if (imm & (1LL << 20))
        imm |= ~((1LL << 21) - 1);
    uintptr_t target = (old_pc & ~0xFFFUL) + ((uintptr_t) imm << 12);
    int64_t nimm = ((int64_t) (target & ~0xFFFUL) - (int64_t) (new_pc & ~0xFFFUL)) >> 12;
    return nimm >= -(1LL << 20) && nimm < (1LL << 20); /* fits the signed 21-bit field */
}
static uint32_t azm_reloc_adrp(uint32_t in, uintptr_t old_pc, uintptr_t new_pc) {
    uint32_t immlo = (in >> 29) & 0x3u, immhi = (in >> 5) & 0x7FFFFu;
    int64_t imm = (int64_t) ((immhi << 2) | immlo);
    if (imm & (1LL << 20))
        imm |= ~((1LL << 21) - 1);
    uintptr_t target = (old_pc & ~0xFFFUL) + ((uintptr_t) imm << 12);
    int64_t nimm = ((int64_t) (target & ~0xFFFUL) - (int64_t) (new_pc & ~0xFFFUL)) >> 12;
    uint32_t nlo = (uint32_t) (nimm & 0x3), nhi = (uint32_t) ((nimm >> 2) & 0x7FFFF);
    return (in & ~((0x3u << 29) | (0x7FFFFu << 5))) | (nlo << 29) | (nhi << 5);
}
static void azm_abs_jump(uint32_t *dst, uint64_t target) {
    dst[0] = 0x58000051u;
    dst[1] = 0xD61F0220u;
    memcpy(&dst[2], &target, 8); /* LDR X17,#8 ; BR X17 */
}
/* Every address azmod patches, and who patched it: community mods claim hooks through
 * community.c, which refuses an address already in here (one address, one owner). */
#define AZM_HOOKED_MAX 512
static struct {
    uintptr_t addr;
    const char *who;
} g_hooked[AZM_HOOKED_MAX];
static int g_hooked_n = 0;
static const char *g_hook_owner = "AZ-MODS";
/* Islands: small blocks of executable memory within a B instruction's reach (+-128 MB) of
 * EP147's code. Each hook gets a 16-byte jump there (LDR X17,#8; BR X17; .quad hook), so the
 * function itself needs only one new instruction, a B to its island: one aligned 4-byte
 * store, which other threads see either before or after, never half done. That makes it
 * safe to hook a function another thread may be running (late hooks, community mods). */
#define AZM_ISLAND_BYTES (64 * 1024)
#define AZM_B_REACH ((intptr_t) 128 * 1024 * 1024 - AZM_ISLAND_BYTES)
static struct {
    uint32_t *base;
    int used; /* 4-instruction slots handed out */
} g_azm_island[4];
static int azm_reaches(uintptr_t from, uintptr_t to) {
    intptr_t d = (intptr_t) to - (intptr_t) from;
    return d > -AZM_B_REACH && d < AZM_B_REACH;
}
static uint32_t *azm_island_slot(uintptr_t FUNC) {
    for (int k = 0; k < 4; k++) {
        if (!g_azm_island[k].base) {
            static const intptr_t tries[] = {-(1 << 20), -(4 << 20), -(16 << 20), -(64 << 20), 64 << 20, 100 << 20};
            for (size_t t = 0; t < sizeof tries / sizeof tries[0]; t++) {
                uintptr_t hint = (FUNC + (uintptr_t) tries[t]) & ~(uintptr_t) 0xFFFF;
                if (hint < 0x100000)
                    continue;
                /* r-x: each slot is written through azprot_unlock, never left writable */
                void *m = mmap((void *) hint, AZM_ISLAND_BYTES, PROT_READ | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (m == MAP_FAILED)
                    continue;
                if (azm_reaches(FUNC, (uintptr_t) m)) {
                    g_azm_island[k].base = m;
                    break;
                }
                munmap(m, AZM_ISLAND_BYTES);
            }
            if (!g_azm_island[k].base)
                return NULL;
        }
        if (azm_reaches(FUNC, (uintptr_t) g_azm_island[k].base) &&
            g_azm_island[k].used < AZM_ISLAND_BYTES / 16)
            return g_azm_island[k].base + 4 * g_azm_island[k].used++;
    }
    return NULL;
}
static void azm_note_hooked(uintptr_t FUNC) {
    if (g_hooked_n < AZM_HOOKED_MAX) {
        g_hooked[g_hooked_n].addr = FUNC;
        g_hooked[g_hooked_n].who = g_hook_owner;
        g_hooked_n++;
    }
}
/* Does this instruction end the function (ret / br, any form)? When one sits in the first
 * three instructions the function is shorter than the 16-byte jump, so that jump would
 * overwrite whatever follows it, and its "original" would run that code too. */
static int azm_ends_fn(uint32_t in) {
    return (in & 0xFFFFF000u) == 0xD65F0000u || /* ret, retaa, retab */
           (in & 0xFFFFF000u) == 0xD61F0000u || /* br, braaz, brabz */
           (in & 0xFFFFF800u) == 0xD71F0800u || /* braa, brab */
           in == 0xD69F03E0u;                   /* eret */
}
/* May the first ncover instructions of a function be replaced? Refused: a PC-relative
 * instruction other than adrp in the first four (the rule for both ways of patching), and,
 * when more than one instruction is covered, a function that ends inside the covered ones. */
static int azm_patch_ok(const uint32_t *f, int ncover) {
    for (int i = 0; i < 4; i++)
        if (azm_pcrel_kind(f[i]) == 1)
            return 0;
    for (int i = 0; i < ncover - 1; i++)
        if (azm_ends_fn(f[i]))
            return 0;
    return 1;
}
/* A trampoline page: written while private and writable, then made r-x before anyone can
 * call it. NULL (and nothing mapped) when the page cannot be made executable. */
static uint32_t *azm_tramp_seal(uint32_t *tramp, int nwords) {
    __builtin___clear_cache((char *) tramp, (char *) tramp + nwords * 4);
    if (mprotect(tramp, 4096, PROT_READ | PROT_EXEC) != 0) {
        munmap(tramp, 4096);
        return NULL;
    }
    return tramp;
}
/* Hook phases: while azmod's constructor runs, EP147's main has not been entered: the only
 * threads are the loader's (running constructors) and azmod's own, so no EP147 function can be
 * executing and the 16-byte jump cannot be seen half written. azm_hooks_go_late() marks the end
 * of that window (the constructor's last step). From then on EP147 starts its threads, and an
 * inline hook must be the one-store B to an island; without an island in reach it is refused. */
static _Atomic int g_azm_late;
static void azm_hooks_go_late(void) {
    __atomic_store_n(&g_azm_late, 1, __ATOMIC_RELEASE);
}
/* The install table: every azm_install_hook call, in order, with the path it took. Fixed size,
 * filled lock-free (late callers may race), written out by azm_hooks_dump. */
enum {
    AZM_ISLAND = 1,          /* one aligned B to a 16-byte island jump */
    AZM_FALLBACK = 2,        /* the 16-byte absolute jump (constructor phase only) */
    AZM_DECLINED = -1,       /* not patchable, or a resource failed: nothing written */
    AZM_REFUSED_LATE = -3,   /* no island in reach after the constructor: nothing written */
};
#define AZM_REC_MAX 256
static struct {
    uintptr_t addr;
    const char *hook; /* the hook argument as written at the call site */
    const char *who;  /* AZ-MODS, or the community mod's id */
    signed char path;
    unsigned char late;
} g_azm_rec[AZM_REC_MAX];
static _Atomic int g_azm_rec_n, g_azm_rec_lost;
static int azm_rec(uintptr_t addr, const char *hook, int path) {
    int i = __atomic_fetch_add(&g_azm_rec_n, 1, __ATOMIC_RELAXED);
    if (i >= AZM_REC_MAX) {
        __atomic_add_fetch(&g_azm_rec_lost, 1, __ATOMIC_RELAXED);
        return path;
    }
    g_azm_rec[i].addr = addr;
    g_azm_rec[i].hook = hook;
    g_azm_rec[i].who = g_hook_owner;
    g_azm_rec[i].late = (unsigned char) __atomic_load_n(&g_azm_late, __ATOMIC_ACQUIRE);
    __atomic_store_n(&g_azm_rec[i].path, (signed char) path, __ATOMIC_RELEASE);
    return path;
}
static const char *azm_path_name(int p) {
    return p == AZM_ISLAND ? "island" : p == AZM_FALLBACK   ? "16byte"
                                    : p == AZM_REFUSED_LATE ? "REFUSED-LATE"
                                    : p == AZM_DECLINED     ? "declined"
                                                            : "?";
}
/* The table as a file, overwritten whole on every call (so it never grows): one summary line,
 * then one line per install. At most AZM_REC_MAX + 2 lines; written in release builds too,
 * since which path each hook took is otherwise invisible on the deck. */
static void azm_hooks_dump(const char *path) {
    int n = __atomic_load_n(&g_azm_rec_n, __ATOMIC_ACQUIRE);
    if (n > AZM_REC_MAX)
        n = AZM_REC_MAX;
    int c[4] = {0, 0, 0, 0}; /* island, 16byte, declined, refused */
    for (int i = 0; i < n; i++) {
        int p = __atomic_load_n(&g_azm_rec[i].path, __ATOMIC_ACQUIRE);
        c[p == AZM_ISLAND ? 0 : p == AZM_FALLBACK   ? 1
                            : p == AZM_REFUSED_LATE ? 3
                                                    : 2]++;
    }
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f)
        return;
    fprintf(f, "hooks %d: island %d, 16byte %d, declined %d, refused-late %d, unrecorded %d\n", n, c[0],
            c[1], c[2], c[3], __atomic_load_n(&g_azm_rec_lost, __ATOMIC_RELAXED));
    for (int i = 0; i < n; i++) {
        const char *h = g_azm_rec[i].hook ? g_azm_rec[i].hook : "?";
        if (!strncmp(h, "(void *)", 8))
            h += 8;
        while (*h == ' ')
            h++;
        fprintf(f, "0x%07lx %-12s %-5s %-10s %s\n", (unsigned long) g_azm_rec[i].addr,
                azm_path_name(__atomic_load_n(&g_azm_rec[i].path, __ATOMIC_ACQUIRE)),
                g_azm_rec[i].late ? "late" : "ctor", g_azm_rec[i].who ? g_azm_rec[i].who : "?", h);
    }
    if (fclose(f) == 0)
        rename(tmp, path);
    else
        unlink(tmp);
}
/* azm_install_hook(addr, hook, &orig): the call site's hook expression is recorded with it
 * (azmod_internal.h defines the same macro for the files included before this one). */
#ifndef azm_install_hook
#define azm_install_hook(f, h, o) azm_install_hook_at((f), (h), (o), #h)
#endif
static int azm_install_entry_hook_at(uintptr_t FUNC, void *hook, void **orig, const char *name,
                                     int island_only) {
    if (!FUNC || (FUNC & 3))
        return azm_rec(FUNC, name, AZM_DECLINED); /* an unresolved EP_ entry is 0: never hook it */
    if (island_only && (!hook || ((uintptr_t) hook & 3) || !orig ||
                        ((uintptr_t) orig & (sizeof(void *) - 1))))
        return azm_rec(FUNC, name, AZM_DECLINED);
    uint32_t *func = (uint32_t *) FUNC;
    if (island_only ? azm_pcrel_kind(func[0]) == 1 : !azm_patch_ok(func, 1))
        return azm_rec(FUNC, name, AZM_DECLINED);
    uint32_t *isl = azm_island_slot(FUNC);
    if (!isl && island_only)
        return azm_rec(FUNC, name, __atomic_load_n(&g_azm_late, __ATOMIC_ACQUIRE)
                                       ? AZM_REFUSED_LATE : AZM_DECLINED);
    int ncopy = isl ? 1 : 4, out = 0;
    if (!isl && __atomic_load_n(&g_azm_late, __ATOMIC_ACQUIRE)) {
        dbg("hook %lx: refused, no island in reach and EP147 is running (a 16-byte write is not atomic)\n",
            (unsigned long) FUNC);
        return azm_rec(FUNC, name, AZM_REFUSED_LATE);
    }
    if (!isl && !azm_patch_ok(func, 4)) {
        dbg("hook %lx: declined, the function ends inside the 16 bytes a jump would cover\n",
            (unsigned long) FUNC);
        return azm_rec(FUNC, name, AZM_DECLINED);
    }
    uint32_t *tramp = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED)
        return azm_rec(FUNC, name, AZM_DECLINED);
    /* Copy the instructions the patch will cover into the trampoline (1 with an island, 4
     * without), expanding where needed: an adrp is re-encoded if it still reaches, otherwise
     * its absolute result is materialized with movz/movk. */
    for (int i = 0; i < ncopy; i++) {
        uint32_t in = func[i];
        if (azm_pcrel_kind(in) == 2) {
            uintptr_t old_pc = FUNC + (uintptr_t) i * 4;
            uintptr_t new_pc = (uintptr_t) &tramp[out];
            if (azm_adrp_reloc_ok(in, old_pc, new_pc)) {
                tramp[out++] = azm_reloc_adrp(in, old_pc, new_pc);
            } else {
                azm_emit_abs_addr(&tramp[out], (int) (in & 0x1F), azm_adrp_target(in, old_pc));
                out += 4;
                dbg("hook %lx: adrp at +%d materialised (target %lx out of range)\n",
                    (unsigned long) FUNC, i * 4, (unsigned long) azm_adrp_target(in, old_pc));
            }
        } else {
            tramp[out++] = in;
        }
    }
    azm_abs_jump(&tramp[out], FUNC + 4 * (uintptr_t) ncopy);
    if (!azm_tramp_seal(tramp, out + 4))
        return azm_rec(FUNC, name, AZM_DECLINED);
    azprot_t pt;
    if (isl) {
        /* the island is ready before anyone can reach it */
        if (azprot_unlock((uintptr_t) isl, 16, &pt) != 0) {
            munmap(tramp, 4096);
            return azm_rec(FUNC, name, AZM_DECLINED);
        }
        azm_abs_jump(isl, (uint64_t) (uintptr_t) hook);
        __builtin___clear_cache((char *) isl, (char *) isl + 16);
        if (azprot_relock(&pt) != 0)
            dbg("hook %lx: island page left writable (mprotect failed)\n", (unsigned long) FUNC);
        uint32_t b = 0x14000000u | (uint32_t) ((((intptr_t) isl - (intptr_t) FUNC) >> 2) & 0x03FFFFFF);
        if (azprot_unlock(FUNC, 4, &pt) != 0) {
            munmap(tramp, 4096);
            return azm_rec(FUNC, name, AZM_DECLINED);
        }
        /* the original before the store that makes it reachable */
        __atomic_store_n(orig, (void *) tramp, __ATOMIC_RELAXED);
        __atomic_store_n(func, b, __ATOMIC_RELEASE); /* the one store that redirects it */
        __builtin___clear_cache((char *) func, (char *) func + 4);
        if (azprot_relock(&pt) != 0)
            dbg("hook %lx: code page left writable (mprotect failed)\n", (unsigned long) FUNC);
        azm_note_hooked(FUNC);
        azm_rec(FUNC, name, AZM_ISLAND);
        return 1;
    }
    /* no island in reach, constructor phase: the 16-byte absolute jump (safe only while no
     * other thread can be running the function, which the phase check above guarantees) */
    if (azprot_unlock(FUNC, 16, &pt) != 0) {
        munmap(tramp, 4096);
        return azm_rec(FUNC, name, AZM_DECLINED);
    }
    /* the original before the jump: a release fence, so no core can see the jump while
     * *orig still reads empty */
    __atomic_store_n(orig, (void *) tramp, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    azm_abs_jump(func, (uint64_t) (uintptr_t) hook);
    __builtin___clear_cache((char *) func, (char *) func + 16);
    if (azprot_relock(&pt) != 0)
        dbg("hook %lx: code page left writable (mprotect failed)\n", (unsigned long) FUNC);
    azm_note_hooked(FUNC);
    azm_rec(FUNC, name, AZM_FALLBACK);
    return 1;
}
static int azm_install_hook_at(uintptr_t FUNC, void *hook, void **orig, const char *name) {
    return azm_install_entry_hook_at(FUNC, hook, orig, name, 0);
}
/* Deliberately opt-in and initially unused: this installs no native target by
 * itself. Its caller still supplies a reviewed live entry/output reference. */
static int azm_install_island_hook_at(uintptr_t FUNC, void *hook, void **orig, const char *name)
    __attribute__((unused));
static int azm_install_island_hook_at(uintptr_t FUNC, void *hook, void **orig, const char *name) {
    return azm_install_entry_hook_at(FUNC, hook, orig, name, 1);
}
/* Swap one vtable slot and return the original through *orig. Use it for a function whose
 * prologue azm_install_hook would decline (a PC-relative branch in the first four
 * instructions): a slot is a data pointer, so no trampoline or relocation is needed.
 * The cost is that it only catches calls made virtually through this exact vtable. */
static int azm_patch_vtable(uintptr_t slot_addr, void *hook, void **orig) {
    if (!slot_addr || (slot_addr & 7))
        return -1;
    void **slot = (void **) slot_addr;
    azprot_t pt;
    if (azprot_unlock(slot_addr, sizeof(void *), &pt) != 0)
        return -1;
    /* the original first, then the slot with release: a thread that reaches the hook through
     * the new slot finds *orig already set */
    __atomic_store_n(orig, *slot, __ATOMIC_RELAXED);
    __atomic_store_n(slot, hook, __ATOMIC_RELEASE);
    if (azprot_relock(&pt) != 0)
        dbg("vtable slot %lx: page left writable (mprotect failed)\n", (unsigned long) slot_addr);
    return 1;
}
