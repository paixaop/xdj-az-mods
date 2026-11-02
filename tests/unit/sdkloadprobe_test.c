/* SPDX-License-Identifier: GPL-3.0-only */
/* The private probe logger is tested from its actual source, without native objects. */
#define _GNU_SOURCE
#include "azloadexport.h"
#include "aztimestamp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include LOAD_PROBE_INC
static void rejected(FILE *f, int deck, int result, const struct azstem_load_observation_v1 *value,
                     struct azsdk_load_sample previous, int availability, unsigned long long now) {
    struct azsdk_load_sample before = previous;
    int status = availability;
    long offset = ftell(f);
    assert(load_probe_log(f, deck, result, value, &previous, &availability, now) == -1);
    assert(ftell(f) == offset && availability == status && !memcmp(&previous, &before, sizeof before));
}
int main(void) {
    FILE *f = tmpfile();
    assert(f);
    struct azstem_load_observation_v1 value = {sizeof value, 1, {10, 20, 30, 100, 0, 0}};
    struct azsdk_load_sample zero = {0}, previous = {0};
    int availability = -2;
    assert(load_probe_log(f, 0, -1, NULL, &previous, &availability, 1000) == 1);
    assert(availability == -1 && !memcmp(&previous, &zero, sizeof zero));
    long offset = ftell(f);
    assert(load_probe_log(f, 0, -1, NULL, &previous, &availability, 1000) == 0 && ftell(f) == offset);
    assert(load_probe_log(f, 0, 0, &value, &previous, &availability, 1000) == 1);
    assert(!memcmp(&previous, &value.sample, sizeof previous) && !availability);
    offset = ftell(f);
    assert(load_probe_log(f, 0, 0, &value, &previous, &availability, 1000) == 0 && ftell(f) == offset);
    struct azsdk_load_sample committed = previous;
    assert(load_probe_log(f, 0, -1, NULL, &previous, &availability, 1000) == 1);
    assert(availability == -1 && !memcmp(&previous, &committed, sizeof previous));
    /* A failed read's output is not interpreted, even if it contains no receipt. */
    struct azstem_load_observation_v1 poison;
    memset(&poison, 0xa5, sizeof poison);
    assert(load_probe_log(f, 0, -1, &poison, &previous, &availability, 1000) == 0);
    assert(!memcmp(&previous, &committed, sizeof previous));
    assert(load_probe_log(f, 0, 0, &value, &previous, &availability, 1000) == 1);
    for (int which = 0; which < 9; ++which) {
        struct azstem_load_observation_v1 bad = value;
        if (which == 0) bad.size--;
        else if (which == 1) bad.size++;
        else if (which == 2) bad.version++;
        else if (which == 3) bad.sample.unit_generation = 0;
        else if (which == 4) bad.sample.load_generation = 0;
        else if (which == 5) bad.sample.invalidation_epoch = 0;
        else if (which == 6) bad.sample.observed_ns = 0;
        else if (which == 7) bad.sample.observed_ns = 1001;
        else bad.sample.reserved = 1;
        rejected(f, 0, 0, &bad, zero, -2, 1000);
    }
    rejected(f, -1, 0, &value, zero, -2, 1000);
    rejected(f, 4, 0, &value, zero, -2, 1000);
    rejected(f, 0, 1, &value, zero, -2, 1000);
    rejected(f, 0, -2, &value, zero, -2, 1000);
    rejected(f, 0, 0, NULL, zero, -2, 1000);
    rejected(f, 0, -1, NULL, zero, -2, 0);
    assert(load_probe_log(NULL, 0, 0, &value, &previous, &availability, 1000) == -1);
    assert(load_probe_log(f, 0, 0, &value, NULL, &availability, 1000) == -1);
    assert(load_probe_log(f, 0, 0, &value, &previous, NULL, 1000) == -1);
    for (int deck = 0; deck < 4; ++deck) {
        for (size_t bytes = sizeof(uint32_t); bytes < sizeof value; bytes += sizeof(uint32_t)) {
            void *prefix = malloc(bytes);
            assert(prefix);
            memset(prefix, 0, bytes);
            uint32_t size = (uint32_t) bytes;
            memcpy(prefix, &size, sizeof size);
            rejected(f, deck, 0, prefix, zero, -2, 1000);
            free(prefix);
        }
    }
    struct azstem_load_observation_v1 next = value;
    next.sample.load_generation++;
    next.sample.invalidation_epoch++;
    next.sample.observed_ns++;
    next.sample.native_request_id = 1;
    for (int which = 0; which < 6; ++which) {
        struct azstem_load_observation_v1 bad = next;
        if (which == 0) bad.sample.unit_generation = value.sample.unit_generation - 1;
        else if (which == 1) bad.sample.load_generation = value.sample.load_generation - 1;
        else if (which == 2) bad.sample.invalidation_epoch = value.sample.invalidation_epoch - 1;
        else if (which == 3) bad.sample.observed_ns = value.sample.observed_ns - 1;
        else if (which == 4) bad.sample.invalidation_epoch = value.sample.invalidation_epoch;
        else bad.sample.native_request_id = 0;
        rejected(f, 0, 0, &bad, committed, 0, 1000);
    }
    /* A generation identifies one immutable receipt, including its timestamp/ID. */
    for (int which = 0; which < 4; ++which) {
        struct azstem_load_observation_v1 bad = value;
        if (which == 0) bad.sample.unit_generation++;
        else if (which == 1) bad.sample.invalidation_epoch++;
        else if (which == 2) bad.sample.observed_ns++;
        else bad.sample.native_request_id++;
        rejected(f, 0, 0, &bad, committed, 0, 1000);
    }
    assert(load_probe_log(f, 0, 0, &next, &previous, &availability, 1000) == 1);
    next.sample.unit_generation++;
    next.sample.load_generation++;
    next.sample.invalidation_epoch++;
    next.sample.native_request_id = UINT32_MAX;
    /* Equal timestamps remain valid on a coarse-resolution monotonic clock. */
    assert(load_probe_log(f, 0, 0, &next, &previous, &availability, 1000) == 1);
    struct azstem_load_observation_v1 wrapped = next;
    wrapped.sample.load_generation++;
    wrapped.sample.invalidation_epoch++;
    wrapped.sample.native_request_id = 0;
    rejected(f, 0, 0, &wrapped, previous, 0, 1000);
    for (int deck = 0; deck < 4; ++deck) {
        struct azsdk_load_sample history = {0};
        int status = -2;
        struct azstem_load_observation_v1 v = value;
        v.sample.unit_generation += (uint64_t) deck;
        assert(load_probe_log(f, deck, 0, &v, &history, &status, 1000) == 1);
    }
    rewind(f);
    char text[8192] = {0};
    assert(fread(text, 1, sizeof text - 1, f) > 0);
    assert(strstr(text, "availability=unavailable read_rc=-1 loaded=unknown"));
    assert(strstr(text, "native_request_id=0 received_ns=100 age_ns=900"));
    assert(!strstr(text, "loaded=0") && !strstr(text, "availability=unloaded"));
    for (int deck = 0; deck < 4; ++deck) {
        char key[64];
        snprintf(key, sizeof key, "load_deck=%d availability=receipt", deck);
        assert(strstr(text, key));
    }
    assert(!fseek(f, 256L * 1024 - 150, SEEK_SET));
    offset = ftell(f);
    availability = -1;
    struct azsdk_load_sample before = previous;
    assert(load_probe_log(f, 0, 0, &next, &previous, &availability, 1000) == 0);
    assert(ftell(f) == offset && availability == -1 && !memcmp(&previous, &before, sizeof before));
    fclose(f);
    int pipes[2];
    assert(!pipe(pipes));
    FILE *stream = fdopen(pipes[1], "w");
    assert(stream);
    availability = -2;
    previous = zero;
    assert(load_probe_log(stream, 0, 0, &value, &previous, &availability, 1000) == -1);
    assert(availability == -2 && !memcmp(&previous, &zero, sizeof zero));
    fclose(stream);
    close(pipes[0]);
    FILE *readonly = fopen("/dev/null", "r");
    assert(readonly);
    assert(load_probe_log(readonly, 0, 0, &value, &previous, &availability, 1000) == -1);
    assert(availability == -2 && !memcmp(&previous, &zero, sizeof zero));
    fclose(readonly);
    puts("PASS load probe: private receipt/unavailable states, nativeID0, monotonic scalars, input/output bounds and all4 decks");
}
