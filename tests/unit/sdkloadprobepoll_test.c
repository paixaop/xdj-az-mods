/* SPDX-License-Identifier: GPL-3.0-only */
/* Execute the actual bounded polling function against private scalar exports. */
#define _GNU_SOURCE
#include "azloadexport.h"
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifndef AZSDK_LOOP_PROBE_OPERATOR
#define AZSDK_LOOP_PROBE_OPERATOR 0
#endif
#define OPERATOR_STOP_TICK 6010u

static FILE *capture;
static unsigned mode, tick, calls[4], closed;
static int fake_load(int deck, struct azstem_load_observation_v1 *out) {
    assert(deck >= 0 && deck < 4 && out && out->size == sizeof *out && out->version == 1);
    calls[deck]++;
    if (mode == 1 && tick == 1) {
        memset(out, 0xa5, sizeof *out);
        return -1;
    }
    unsigned revision = mode == 3 ? tick : mode == 1 && tick >= 3;
    out->sample = (struct azsdk_load_sample) {100u + (unsigned) deck, 200u + revision,
                                              300u + revision, 1000u + revision, revision, 0};
    if (mode == 2 && deck == 0) out->size = 4;
    return 0;
}
static void *fake_dlsym(void *handle, const char *name) {
    assert(handle == RTLD_DEFAULT);
    if (!strcmp(name, "azstem_load_observation_v1") && mode) {
        azstem_load_observation_v1_fn fn = fake_load;
        void *result;
        _Static_assert(sizeof fn == sizeof result, "fixture function-pointer representation");
        memcpy(&result, &fn, sizeof result);
        return result;
    }
    assert(!strcmp(name, "azstem_loop_observation_v2") ||
           !strcmp(name, "azstem_loop_observer_mask_v1") ||
           !strcmp(name, "azstem_live_observation_v1") ||
           !strcmp(name, "azstem_live_observer_mask_v1") ||
           !strcmp(name, "azstem_load_observation_v1"));
    return NULL;
}
static FILE *fake_fopen(const char *name, const char *flags) {
    assert(!strncmp(name, "/tmp/az-sdk-loop-probe.", 23) && !strcmp(flags, "we"));
    return capture;
}
static int fake_fclose(FILE *f) {
    assert(f == capture);
    closed++;
    return 0;
}
static int fake_setpriority(int which, id_t who, int priority) {
    assert(which == PRIO_PROCESS && !who && priority == 10);
    return 0;
}
static int fake_clock_gettime(clockid_t clock, struct timespec *value) {
    assert(clock == CLOCK_MONOTONIC && value);
    *value = (struct timespec) {1, (long) tick};
    return 0;
}
static int fake_nanosleep(const struct timespec *value, struct timespec *left) {
    assert(value && !value->tv_sec && value->tv_nsec == 100000000 && !left);
    if (AZSDK_LOOP_PROBE_OPERATOR) assert(tick < OPERATOR_STOP_TICK);
    tick++;
    return 0;
}
static int __attribute__((unused)) fake_access(const char *path, int mode_value) {
    assert(AZSDK_LOOP_PROBE_OPERATOR && path && mode_value == F_OK);
    assert(!strncmp(path, "/tmp/az-sdk-loop-probe.", 23) && strstr(path, ".stop"));
    assert(!(tick % 10u));
    return tick >= OPERATOR_STOP_TICK ? 0 : -1;
}
static char *fake_getenv(const char *name) {
    assert(!strcmp(name, "AZSDK_LOOP_PROBE"));
    return NULL;
}
#define dlsym fake_dlsym
#define fopen fake_fopen
#define fclose fake_fclose
#define setpriority fake_setpriority
#define clock_gettime fake_clock_gettime
#define nanosleep fake_nanosleep
#define getenv fake_getenv
#define access fake_access
#include LOAD_POLL_INC
#undef dlsym
#undef fopen
#undef fclose
#undef setpriority
#undef clock_gettime
#undef nanosleep
#undef getenv
#undef access

int main(void) {
    const unsigned expected_ticks = AZSDK_LOOP_PROBE_OPERATOR ? OPERATOR_STOP_TICK : AZSDK_LOOP_PROBE_TICKS;
    for (mode = 0; mode < 4; mode++) {
        capture = tmpfile();
        assert(capture);
        tick = closed = 0;
        memset(calls, 0, sizeof calls);
        assert(!observe(NULL) && closed == 1);
        long bytes = ftell(capture);
        assert(bytes > 0 && bytes <= 256L * 1024);
        char *log = calloc((size_t) bytes + 1, 1);
        assert(log);
        rewind(capture);
        assert(fread(log, 1, (size_t) bytes, capture) == (size_t) bytes);
        assert(strstr(log, "complete seen=0 submitted=0 errors="));
        assert(!strstr(log, "loaded=0") && !strstr(log, "availability=unloaded"));
        if (!mode) {
            assert(tick == expected_ticks && strstr(log, "private_load_export=0\n"));
            assert(strstr(log, "load_seen=0 load_available=0\n"));
            for (int deck = 0; deck < 4; deck++) {
                char key[128];
                assert(!calls[deck]);
                snprintf(key, sizeof key, "load_deck=%d availability=unavailable read_rc=-1 loaded=unknown", deck);
                assert(strstr(log, key));
            }
        } else {
            assert(strstr(log, "private_load_export=1\n"));
            for (int deck = 0; deck < 4; deck++) assert(calls[deck] == (mode == 3 ? tick : expected_ticks));
            if (mode == 2) {
                char expected[64];
                snprintf(expected, sizeof expected, "errors=%u ", expected_ticks);
                assert(tick == expected_ticks && strstr(log, expected));
                assert(strstr(log, "load_seen=14 load_available=14\n"));
                assert(!strstr(log, "load_deck=0 availability=receipt"));
            } else {
                assert(strstr(log, "errors=0 ") && strstr(log, "load_seen=15 load_available=15\n"));
                assert(strstr(log, "native_request_id=0 received_ns=1000"));
                if (mode == 3) assert(tick > 0 && tick < expected_ticks);
                else {
                    assert(tick == expected_ticks);
                    for (int deck = 0; deck < 4; deck++) {
                        char key[256];
                        snprintf(key, sizeof key, "load_deck=%d availability=unavailable read_rc=-1 loaded=unknown last_unit=%u last_load_generation=200 last_epoch=300 last_received_ns=1000", deck, 100u + (unsigned) deck);
                        assert(strstr(log, key));
                        snprintf(key, sizeof key, "load_deck=%d availability=receipt unit=%u load_generation=201 invalidation_epoch=301 native_request_id=1 received_ns=1001", deck, 100u + (unsigned) deck);
                        assert(strstr(log, key));
                    }
                }
            }
        }
        if (AZSDK_LOOP_PROBE_OPERATOR) assert(strstr(log, "operator_mode=1 stop_file="));
        free(log);
        fclose(capture);
    }
    printf("PASS load polling: actual optional exports, all4 decks, unavailable/recovery, errors, %uticks/100ms, operator=%u and 256KiB cap\n", expected_ticks, (unsigned) AZSDK_LOOP_PROBE_OPERATOR);
}
