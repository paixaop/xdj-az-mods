/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <pthread.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

/* Clang fixtures use plain storage with the same GCC __atomic operations as the body. */
static long g_ov_calls, g_ov_applied, g_wpaint_calls, g_upd_calls, g_sw2d_paints;
static long g_ov_acquired;
static int g_ovi_n, g_wpmemo_n, g_wwbctor_calls, g_ctmap_n, g_upd_bound, g_ov_bind_status;
/* Deliberately mutable UI cache: production report must never access its fields. */
static struct {
    volatile long self;
    volatile int cnt, deck;
} g_ovi[2];
static char *report;
static size_t report_size;
static int fail_open;
static _Atomic uintptr_t forbidden, fault_begin, fault_end;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "signal proof requires lock-free address loads");
static unsigned checks;
#define CHECK(test)                                                        \
    do {                                                                   \
        ++checks;                                                          \
        if (!(test)) {                                                     \
            fprintf(stderr, "FAIL wave report:%d: %s\n", __LINE__, #test); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static FILE *azlog_open(const char *path, const char *mode) {
    CHECK(!strcmp(path, "/tmp/azmod-waveprov") && !strcmp(mode, "w"));
    if (fail_open) return NULL;
    free(report);
    report = NULL;
    report_size = 0;
    FILE *out = open_memstream(&report, &report_size);
    CHECK(out != NULL);
    return out;
}
#define WAVE_REPORT_STRING_INNER(value) #value
#define WAVE_REPORT_STRING(value) WAVE_REPORT_STRING_INNER(value)
#define WAVE_REPORT_LABEL(name) WAVE_REPORT_STRING(__USER_LABEL_PREFIX__) name
static void wave_report_expect_probe(uintptr_t, const char *, unsigned, const void *, const void *)
    __attribute__((unused));
#if defined(__GNUC__) && !defined(__clang__)
static void wave_prov_report(void) __attribute__((noinline, noclone));
#else
static void wave_prov_report(void) __attribute__((noinline));
#endif
#include WAVE_REPORT_INC

static void wave_report_expect_probe(uintptr_t address, const char *source, unsigned line,
                                     const void *begin, const void *end) {
    CHECK(address == atomic_load_explicit(&forbidden, memory_order_relaxed) + 0x80);
    uintptr_t lo = (uintptr_t) begin, hi = (uintptr_t) end;
    CHECK(lo && hi > lo && hi - lo <= 4096);
    atomic_store_explicit(&fault_begin, lo, memory_order_relaxed);
    atomic_store_explicit(&fault_end, hi, memory_order_relaxed);
    fprintf(stderr, "CONTROL_EXPECT wave report: address=0x%" PRIxPTR " pc_begin=0x%" PRIxPTR " pc_end=0x%" PRIxPTR " source=%s:%u\n", address, lo, hi, source, line);
    CHECK(!fflush(stderr));
}
static uintptr_t fault_pc(void *context) {
#if defined(__linux__) && defined(__aarch64__)
    return (uintptr_t) ((ucontext_t *) context)->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__)
    return (uintptr_t) ((ucontext_t *) context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__APPLE__) && defined(__x86_64__)
    return (uintptr_t) ((ucontext_t *) context)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(__aarch64__)
    return (uintptr_t) ((ucontext_t *) context)->uc_mcontext->__ss.__pc;
#else
    (void) context;
    return 0;
#endif
}
static char *signal_hex(char *out, uintptr_t value) {
    static const char digits[] = "0123456789abcdef";
    *out++ = '0';
    *out++ = 'x';
    for (int shift = (int) (sizeof value * 8) - 4; shift >= 0; shift -= 4)
        *out++ = digits[(value >> shift) & 15u];
    return out;
}

static void forbidden_read(int signal_number, siginfo_t *info, void *context) {
    uintptr_t address = (uintptr_t) info->si_addr, pc = fault_pc(context);
    uintptr_t lo = atomic_load_explicit(&fault_begin, memory_order_relaxed);
    uintptr_t hi = atomic_load_explicit(&fault_end, memory_order_relaxed);
    static const char unrelated[] = "FAIL wave report: unrelated memory fault\n";
    if ((signal_number == SIGSEGV || signal_number == SIGBUS) && info->si_code > 0 &&
        address == atomic_load_explicit(&forbidden, memory_order_relaxed) + 0x80 &&
        lo && hi > lo && pc >= lo && pc < hi) {
        char message[160], *p = message;
        static const char prefix[] = "CONTROL_FAULT wave report: address=";
        for (size_t i = 0; i < sizeof prefix - 1; ++i) *p++ = prefix[i];
        p = signal_hex(p, address);
        *p++ = ' ';
        *p++ = 'p';
        *p++ = 'c';
        *p++ = '=';
        p = signal_hex(p, pc);
        *p++ = '\n';
        size_t count = (size_t) (p - message);
        ssize_t sent = write(STDERR_FILENO, message, count);
        _exit(sent == (ssize_t) count ? 97 : 99);
    }
    ssize_t sent = write(STDERR_FILENO, unrelated, sizeof unrelated - 1);
    _exit(sent == (ssize_t) (sizeof unrelated - 1) ? 98 : 99);
}
static void *ui_writer(void *argument) {
    (void) argument;
    for (int i = 0; i < 20000; ++i) {
        g_ovi[0].self = (long) forbidden;
        g_ovi[0].cnt = i;
        g_ovi[0].deck = i & 3;
        __atomic_store_n(&g_ov_calls, i, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ov_applied, i / 2, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ovi_n, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ov_acquired, i / 3, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ov_bind_status, i % (OV_LEASE_READY + 1), __ATOMIC_RELEASE);
    }
    return NULL;
}
int main(int argc, char **argv) {
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0);
    void *page = mmap(NULL, (size_t) page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    forbidden = (uintptr_t) page;
    int retired = argc == 2 && !strcmp(argv[1], "--retired");
    if (retired) CHECK(!munmap(page, (size_t) page_size));
    else CHECK(!mprotect(page, (size_t) page_size, PROT_NONE));
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = forbidden_read;
    action.sa_flags = SA_SIGINFO;
    CHECK(!sigemptyset(&action.sa_mask));
    CHECK(!sigaction(SIGSEGV, &action, NULL));
    CHECK(!sigaction(SIGBUS, &action, NULL));
    g_ovi[0].self = (long) forbidden;
    __atomic_store_n(&g_ovi_n, 1, __ATOMIC_RELAXED);
    wave_prov_report(); /* Restored old probe must fail at its actual poisoned/retired read. */
    CHECK(report && report_size && !strstr(report, "probe[") && !strstr(report, "self="));
    CHECK(strstr(report, "overview lease: binding=unbound(0) acquired=0\n"));
    pthread_t writer;
    CHECK(!pthread_create(&writer, NULL, ui_writer, NULL));
    for (int i = 0; i < 200; ++i) wave_prov_report();
    CHECK(!pthread_join(writer, NULL));
    __atomic_store_n(&g_ov_calls, 120, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ov_applied, 60, __ATOMIC_RELAXED);
    wave_prov_report();
    CHECK(strstr(report, "overview: calls=120 applied=60 instances=1\n"));
    CHECK(strstr(report, "wpaint:") && strstr(report, "wwbctor:") && strstr(report, "upd:") &&
          strstr(report, "sw2d:"));
    const struct {
        int status;
        const char *name;
    } states[] = {
        {OV_LEASE_UNBOUND, "unbound"}, {OV_LEASE_MISSING, "missing"}, {OV_LEASE_QUERY_REJECTED, "query-rejected"}, {OV_LEASE_INCOMPATIBLE, "incompatible"}, {OV_LEASE_PROFILE_UNAVAILABLE, "profile-unavailable"}, {OV_LEASE_READY, "ready"}, {99, "unknown"}};
    __atomic_store_n(&g_ov_acquired, 42, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < sizeof states / sizeof *states; ++i) {
        __atomic_store_n(&g_ov_bind_status, states[i].status, __ATOMIC_RELEASE);
        wave_prov_report();
        char expected[100];
        snprintf(expected, sizeof expected, "overview lease: binding=%s(%d) acquired=42\n",
                 states[i].name, states[i].status);
        CHECK(strstr(report, expected));
    }
    char *old = report;
    fail_open = 1;
    wave_prov_report();
    CHECK(report == old);
    if (!retired) CHECK(!munmap(page, (size_t) page_size));
    free(report);
    printf("wave report: %u checks passed; retired/poisoned keys and mutable UI cache ignored\n", checks);
    return 0;
}
