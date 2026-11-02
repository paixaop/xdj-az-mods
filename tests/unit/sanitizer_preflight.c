/* SPDX-License-Identifier: GPL-3.0-only */
/* Isolated sanitizer runtime checks. Never reads native firmware or controls a deck. */
#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int start;
static volatile int value;
static int race_mode;

static void *increment(void *unused) {
    (void) unused;
    while (!atomic_load_explicit(&start, memory_order_acquire)) sched_yield();
    for (int i = 0; i < 10000; i++) {
        if (!race_mode) pthread_mutex_lock(&mutex);
        value++;
        if (!race_mode) pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

__attribute__((noinline)) static void overflow(volatile char *bytes, size_t count) {
    bytes[count + 8] = 1;
}

__attribute__((noinline)) static void leak(size_t count) {
    volatile char *bytes = malloc(count);
    if (!bytes) exit(3);
    bytes[0] = 1;
    value = bytes[0];
    /* Deliberately leave an unreachable allocation for LeakSanitizer. */
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "clean";
    size_t count = argc > 2 ? (size_t) strtoul(argv[2], NULL, 10) : 64;
    if (count < 16 || count > 4096) return 2;
    if (!strcmp(mode, "expected-failure")) {
        fputs("preflight expected failure\n", stderr);
        return 73;
    }
    if (!strcmp(mode, "heap")) {
        volatile char *bytes = malloc(count);
        if (!bytes) return 3;
        overflow(bytes, count);
        free((void *) bytes);
        return 0;
    }
    if (!strcmp(mode, "signed")) {
        volatile int maximum = INT_MAX;
        maximum += 1;
        value = maximum;
        return 0;
    }
    if (!strcmp(mode, "leak")) {
        leak(count);
        return 0;
    }
    race_mode = !strcmp(mode, "race");
    if (!race_mode && strcmp(mode, "clean")) return 2;
    char *bytes = malloc(count);
    if (!bytes) return 3;
    memset(bytes, 0x5a, count);
    pthread_t a, b;
    if (pthread_create(&a, NULL, increment, NULL) || pthread_create(&b, NULL, increment, NULL)) return 3;
    atomic_store_explicit(&start, 1, memory_order_release);
    if (pthread_join(a, NULL) || pthread_join(b, NULL)) return 3;
    int ok = race_mode || value == 20000;
    ok = ok && bytes[count - 1] == 0x5a;
    free(bytes);
    if (!ok) return 1;
    puts("preflight clean-ready");
    return 0;
}
