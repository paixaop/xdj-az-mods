/* SPDX-License-Identifier: GPL-3.0-only */
/* The real azstem installer and azprot header, with deterministic syscall faults.
 * Synthetic mmap'd code only; no EP147 binary, library or hardware is accessed. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(HOOK_REQUIRE_AARCH64) && !defined(__aarch64__)
#error "native hook fixture requires AArch64"
#endif
enum fault {
    CLEAN,
    MMAP_FAIL,
    RX_FAIL,
    UNLOCK_FAIL,
    RESTORE_ONCE,
    RESTORE_ALWAYS,
    ROLLBACK_UNLOCK_FAIL,
    CLEANUP_FAIL
};
struct scenario {
    const char *name;
    enum fault fault;
    int protection, cross_page, late, bad, null_previous, fatal;
};
static const struct scenario scenarios[] = {
    {"success-rx", CLEAN, PROT_READ | PROT_EXEC, 0, 0, 0, 0, 0},
    {"success-cross-page", CLEAN, PROT_READ | PROT_EXEC, 1, 0, 0, 0, 0},
    {"mmap-failure", MMAP_FAIL, PROT_READ | PROT_EXEC, 0, 0, 0, 0, 0},
    {"trampoline-rx-failure", RX_FAIL, PROT_READ | PROT_EXEC, 0, 0, 0, 0, 0},
    {"initial-unlock-failure", UNLOCK_FAIL, PROT_READ | PROT_EXEC, 0, 0, 0, 0, 0},
    {"restore-once", RESTORE_ONCE, PROT_READ | PROT_EXEC, 0, 0, 0, 0, 0},
    {"restore-once-readonly", RESTORE_ONCE, PROT_READ, 0, 0, 0, 0, 0},
    {"restore-once-cross-page", RESTORE_ONCE, PROT_READ | PROT_EXEC, 1, 0, 0, 0, 0},
    {"restore-once-null-previous", RESTORE_ONCE, PROT_READ | PROT_EXEC, 0, 0, 0, 1, 0},
    {"restore-persistent", RESTORE_ALWAYS, PROT_READ | PROT_EXEC, 0, 0, 0, 0, 1},
    {"rollback-unlock-failure", ROLLBACK_UNLOCK_FAIL, PROT_READ | PROT_EXEC, 0, 0, 0, 0, 1},
    {"rollback-cleanup-failure", CLEANUP_FAIL, PROT_READ | PROT_EXEC, 0, 0, 0, 0, 1},
    {"late", CLEAN, PROT_READ | PROT_EXEC, 0, 1, 0, 0, 0},
    {"null-hook", CLEAN, PROT_READ | PROT_EXEC, 0, 0, 1, 0, 0},
    {"null-output", CLEAN, PROT_READ | PROT_EXEC, 0, 0, 2, 0, 0},
    {"zero-target", CLEAN, PROT_READ | PROT_EXEC, 0, 0, 3, 0, 0},
    {"misaligned-target", CLEAN, PROT_READ | PROT_EXEC, 0, 0, 4, 0, 0},
    {"nonrelocatable", CLEAN, PROT_READ | PROT_EXEC, 0, 0, 5, 0, 0},
};
static const struct scenario *current;
static void *code_mapping, *original, *previous, *trampoline, *result;
static size_t code_size;
static uint32_t *entry;
static uint32_t saved[4];
static int instrument, in_constructor, main_seen, map_calls, allocations, frees, unmap_calls;
static int code_unlocks, code_restores, trampoline_rx, cache_calls, success_logs;
static void *cache_begin[4], *cache_end[4];

__attribute__((noreturn)) static void fail(const char *message) {
    dprintf(STDERR_FILENO, "FAIL hookinstall: case=%s %s\n", current ? current->name : "parent", message);
    _exit(90);
}
#define CHECK(value, message)        \
    do {                             \
        if (!(value)) fail(message); \
    } while (0)

static void *test_mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    if (instrument) {
        map_calls++;
        CHECK(size == 4096 && protection == (PROT_READ | PROT_WRITE), "bounded writable trampoline allocation");
        if (current->fault == MMAP_FAIL) {
            errno = ENOMEM;
            return MAP_FAILED;
        }
    }
    void *p = mmap(address, size, protection, flags, fd, offset);
    if (instrument && p != MAP_FAILED) {
        trampoline = p;
        allocations++;
    }
    return p;
}
static int test_mprotect(void *address, size_t size, int protection) {
    if (instrument && address == trampoline) {
        trampoline_rx++;
        CHECK(size == 4096 && protection == (PROT_READ | PROT_EXEC), "trampoline becomes RX");
        if (current->fault == RX_FAIL) {
            errno = EACCES;
            return -1;
        }
    } else if (instrument) {
        CHECK(address == code_mapping && size == code_size, "exact one/two-page code span");
        if (protection & PROT_WRITE) {
            code_unlocks++;
            if (current->fault == UNLOCK_FAIL || (current->fault == ROLLBACK_UNLOCK_FAIL && code_unlocks == 2)) {
                errno = EACCES;
                return -1;
            }
        } else {
            code_restores++;
            if (current->fault == RESTORE_ALWAYS ||
                ((current->fault == RESTORE_ONCE || current->fault == ROLLBACK_UNLOCK_FAIL || current->fault == CLEANUP_FAIL) && code_restores == 1)) {
                errno = EACCES;
                return -1;
            }
        }
    }
    return mprotect(address, size, protection);
}
static int test_munmap(void *address, size_t size) {
    if (instrument) {
        CHECK(address == trampoline && size == 4096, "cleanup only unused trampoline");
        unmap_calls++;
        if (current->fault == CLEANUP_FAIL) {
            errno = EINVAL;
            return -1;
        }
    }
    int rc = munmap(address, size);
    if (instrument && !rc) frees++;
    return rc;
}
static void test_clear_cache(char *begin, char *end) {
    if (instrument) {
        CHECK(cache_calls < 4, "bounded cache clears");
        cache_begin[cache_calls] = begin;
        cache_end[cache_calls++] = end;
    }
    __builtin___clear_cache(begin, end);
}
static void test_log(const char *fmt, ...) {
    if (!strncmp(fmt, "install: hooked @", 16)) success_logs++;
}
static void test_exit(int status) __attribute__((noreturn));
#define mmap test_mmap
#define mprotect test_mprotect
#define munmap test_munmap
#define __builtin___clear_cache test_clear_cache
#define logln test_log
#include AZPROT_INC
#define _exit test_exit
#include HOOK_INSTALL_INC
#undef _exit
#undef mmap
#undef mprotect
#undef munmap
#undef __builtin___clear_cache

static int old_original(void) { return 77; }
static int hook(void) { return ((int (*)(void)) original)() + 100; }
static int protection_now(void) {
    return azprot_query((uintptr_t) code_mapping, (uintptr_t) code_mapping + code_size);
}
static void check_lock_free(void) {
    CHECK(pthread_mutex_trylock(&g_azprot_mu) == 0, "protection mutex released");
    pthread_mutex_unlock(&g_azprot_mu);
}
static int bytes_original(void) { return !memcmp(entry, saved, sizeof saved); }
static void cache_entry(int index) {
    CHECK(cache_calls > index && cache_begin[index] == entry && cache_end[index] == (char *) entry + 16, "entry cache-clear range");
}
static void test_exit(int status) {
    CHECK(current && current->fatal && status == 126 && in_constructor && !main_seen, "fatal before main with exit126");
    CHECK(allocations == 1 && frees == 0 && !success_logs, "fatal never reports success or frees a live trampoline");
    check_lock_free();
    if (current->fault == ROLLBACK_UNLOCK_FAIL) {
        CHECK(!bytes_original() && original == trampoline && code_unlocks == 2 && code_restores == 1, "unrecoverable unlock stops with explicit live state");
        CHECK(protection_now() == (current->protection | PROT_WRITE), "failed unlock retains transient protection until process exit");
        CHECK(cache_calls == 2, "no rollback write after unlock refusal");
    } else {
        CHECK(bytes_original() && original == previous && code_unlocks == 2 && code_restores == 2, "rollback bytes and prior original precede fatal protection/cleanup failure");
        CHECK(cache_calls == 3, "rollback instruction cache cleared before fatal");
        cache_entry(2);
        CHECK(protection_now() == (current->fault == CLEANUP_FAIL ? current->protection : current->protection | PROT_WRITE), "fatal protection state is observed exactly");
        CHECK(unmap_calls == (current->fault == CLEANUP_FAIL), "cleanup follows restored protection");
    }
    dprintf(STDERR_FILENO, "FATAL-CHECK hookinstall: case=%s phase=constructor exit=126 bytes=%s original=%s cache=%d frees=%d\n",
            current->name, bytes_original() ? "restored" : "patched", original == previous ? "prior" : "trampoline", cache_calls, frees);
    _exit(status);
}

__attribute__((constructor)) static void exercise_constructor(void) {
    const char *name = getenv("AZ_HOOK_INSTALL_CASE");
    if (!name) return;
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
        if (!strcmp(name, scenarios[i].name)) current = &scenarios[i];
    CHECK(current, "known scenario");
    in_constructor = 1;
    long page = sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096, "valid page size");
    code_size = (size_t) page * (current->cross_page ? 2u : 1u);
    code_mapping = mmap(NULL, code_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(code_mapping != MAP_FAILED, "synthetic code allocation");
    entry = (uint32_t *) ((char *) code_mapping + (current->cross_page ? page - 8 : 0));
    for (int i = 0; i < 4; i++) entry[i] = 0xd503201fu; /* NOP */
    entry[4] = 0x528000e0u;
    entry[5] = 0xd65f03c0u; /* MOV W0,#7 ; RET */
    if (current->bad == 5) entry[0] = 0x94000010u; /* nonrelocatable BL */
    memcpy(saved, entry, sizeof saved);
    __builtin___clear_cache((char *) entry, (char *) entry + 24);
    CHECK(mprotect(code_mapping, code_size, current->protection) == 0, "initial native protections");
    previous = current->null_previous ? NULL : (void *) old_original;
    original = previous;
    atomic_store(&g_hooks_late, current->late);
    instrument = 1;
    uintptr_t target = current->bad == 3 ? 0 : (uintptr_t) entry + (current->bad == 4);
    result = install_hook_at(target, current->bad == 1 ? NULL : (void *) hook, current->bad == 2 ? NULL : &original);
    CHECK(!current->fatal, "fatal scenario must not return from installer");
    in_constructor = 0;
}

static int child_main(void) {
    main_seen = 1;
    CHECK(current && !in_constructor, "normal main after constructor");
    check_lock_free();
    int success = current->fault == CLEAN && !current->bad && !current->late;
    if (success) {
        CHECK(result == trampoline && original == trampoline && !bytes_original(), "successful hook publication");
        CHECK(code_unlocks == 1 && code_restores == 1 && trampoline_rx == 1 && allocations == 1 && !frees, "successful syscall counts");
        CHECK(success_logs == 1 && cache_calls == 2, "successful cache and publication evidence");
        cache_entry(1);
        CHECK(protection_now() == current->protection, "success restores exact protection");
        CHECK(azprot_query((uintptr_t) trampoline, (uintptr_t) trampoline + 4096) == (PROT_READ | PROT_EXEC), "successful trampoline remains RX");
#if defined(__aarch64__)
        CHECK(((int (*)(void)) entry)() == 107 && ((int (*)(void)) original)() == 7, "native hook and trampoline execute");
#endif
    } else {
        CHECK(result == NULL && bytes_original() && original == previous, "decline restores entry bytes and prior original");
        CHECK(protection_now() == current->protection && !success_logs, "decline restores original protection and never reports success");
        CHECK(allocations == frees, "all declined trampolines freed");
        if (current->fault == RESTORE_ONCE) {
            CHECK(code_unlocks == 2 && code_restores == 2 && cache_calls == 3 && unmap_calls == 1, "restore failure rollback syscall/cache counts");
            cache_entry(1);
            cache_entry(2);
        }
        if (current->late || current->bad) CHECK(!map_calls && !code_unlocks && !code_restores && !cache_calls, "early decline before any syscall/write");
        if (current->fault == UNLOCK_FAIL) CHECK(code_unlocks == 1 && !code_restores && frees == 1 && cache_calls == 1, "initial unlock failure is unchanged");
        if (current->fault == RX_FAIL) CHECK(trampoline_rx == 1 && !code_unlocks && frees == 1, "trampoline RX failure never touches target");
        if (current->fault == MMAP_FAIL) CHECK(map_calls == 1 && !allocations && !code_unlocks, "allocation failure never touches target");
    }
    instrument = 0;
    if (success) CHECK(munmap(trampoline, 4096) == 0, "fixture releases successful trampoline");
    CHECK(munmap(code_mapping, code_size) == 0, "fixture releases synthetic code");
    printf("CHILD-MAIN hookinstall: case=%s passed\n", current->name);
    return 0;
}

int main(void) {
    if (current) return child_main();
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    CHECK(n > 0 && (size_t) n < sizeof self, "self executable path");
    self[n] = 0;
    unsigned passed = 0;
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        int pipefd[2];
        CHECK(pipe(pipefd) == 0, "child output pipe");
        pid_t child = fork();
        CHECK(child >= 0, "fork constructor case");
        if (!child) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            if (setenv("AZ_HOOK_INSTALL_CASE", scenarios[i].name, 1)) _exit(92);
            execl(self, self, (char *) NULL);
            _exit(93);
        }
        close(pipefd[1]);
        char output[16384];
        size_t used = 0;
        for (;;) {
            ssize_t got = read(pipefd[0], output + used, sizeof output - used - 1);
            if (got < 0 && errno == EINTR) continue;
            CHECK(got >= 0, "read child result");
            if (!got) break;
            used += (size_t) got;
            CHECK(used < sizeof output - 1, "bounded child diagnostics");
        }
        close(pipefd[0]);
        output[used] = 0;
        int status;
        CHECK(waitpid(child, &status, 0) == child, "join child");
        int expected = scenarios[i].fatal ? 126 : 0;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != expected || strstr(output, "FAIL hookinstall:") ||
            (scenarios[i].fatal && (!strstr(output, "azstem: fatal hook install") || !strstr(output, "FATAL-CHECK hookinstall:") || strstr(output, "CHILD-MAIN")))) {
            fprintf(stderr, "FAIL hookinstall: parent case=%s expected_exit=%d wait_status=%d\n%s", scenarios[i].name, expected, status, output);
            return 1;
        }
        if (scenarios[i].fatal) fputs(output, stdout);
        passed++;
    }
    printf("PASS hookinstall: %u constructor cases, real protections/rollback/cleanup, fatal126 before main, native_exec=%d\n", passed,
#if defined(__aarch64__)
           1
#else
           0
#endif
    );
    return 0;
}
