/* SPDX-License-Identifier: GPL-3.0-only */
/* Production probe logger: separate revisions, copied records, age and file bounds. */
#include "../common/azdeckinfo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include LIVE_PROBE_INC
int main(void) {
    FILE *f = tmpfile();
    assert(f);
    struct azstem_live_observation_v1 value = {
        .size = sizeof value, .version = 1, .sample = {.unit_generation = 9, .observed_fields = 7, .valid_fields = 7, .mode_is_2 = 1, .play_pause = 0, .play_aux = 1, .quantize = 0, .key_lock = 1, .revisions = {10, 20, 30}, .observed_ns = {100, 200, 300}}};
    struct azsdk_live_sample previous = {0};
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == 3);
    long end = ftell(f);
    assert(end > 0 && live_probe_log(f, 2, 4, &value, &previous, 1000) == 0 && ftell(f) == end);
    value.sample.revisions[1]++;
    value.sample.observed_ns[1] = 400;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == 1);
    value.sample.observed_fields = value.sample.valid_fields = 2;
    value.sample.revisions[1]++;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == 1);
    assert(previous.revisions[0] == 10 && previous.revisions[2] == 30);
    value.sample.observed_fields = value.sample.valid_fields = 7;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == 0);
    struct azsdk_live_sample before = previous;
    end = ftell(f);
    value.sample.observed_ns[1] = 1001;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == -1 && ftell(f) == end && !memcmp(&previous, &before, sizeof before));
    value.sample.observed_ns[1] = 400;
    value.sample.mode_is_2 = 2;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == -1);
    value.sample.mode_is_2 = 1;
    value.sample.valid_fields = 6;
    value.sample.mode_is_2 = value.sample.play_pause = value.sample.play_aux = -1;
    value.sample.revisions[0]++;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == 1);
    value.sample.unit_generation++;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == 3);
    assert(live_probe_log(f, -1, 4, &value, &previous, 1000) == -1);
    assert(live_probe_log(f, 4, 4, &value, &previous, 1000) == -1);
    assert(live_probe_log(f, 2, 16, &value, &previous, 1000) == -1);
    value.version = 2;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == -1);
    value.version = 1;
    value.size--;
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == -1);
    value.size++;
    for (int deck = 0; deck < 4; ++deck) {
        for (size_t bytes = sizeof(uint32_t); bytes < sizeof value; bytes += sizeof(uint32_t)) {
            void *prefix = malloc(bytes);
            assert(prefix);
            memset(prefix, 0, bytes);
            uint32_t size = (uint32_t) bytes;
            memcpy(prefix, &size, sizeof size);
            struct azsdk_live_sample saved = previous;
            end = ftell(f);
            assert(live_probe_log(f, deck, 4, prefix, &previous, 1000) == -1);
            assert(ftell(f) == end && !memcmp(&previous, &saved, sizeof saved));
            free(prefix);
        }
    }
    rewind(f);
    char text[4096] = {0};
    assert(fread(text, 1, sizeof text - 1, f) > 0);
    assert(strstr(text, "stream=play revision=10 received_ns=100 age_ns=900 valid=1"));
    assert(strstr(text, "stream=quantize revision=21 received_ns=400 age_ns=600 valid=1"));
    assert(strstr(text, "stream=key_lock revision=30 received_ns=300 age_ns=700 valid=1"));
    assert(strstr(text, "stream=play revision=11 received_ns=100 age_ns=900 valid=0"));
    assert(!fseek(f, 256L * 1024 - 150, SEEK_SET));
    value.sample.revisions[1]++;
    end = ftell(f);
    assert(live_probe_log(f, 2, 4, &value, &previous, 1000) == 0 && ftell(f) == end);
    fclose(f);
    puts("sdk live probe: independent revisions, bounded output, invalid receipts and monotonic ages passed");
}
