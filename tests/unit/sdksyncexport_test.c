/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azdeckinfo.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_sdk_live_on = 1, ready = 1;
static struct azsdk_sync_sample receipt = {31, 70, 700, 2, 1};
int azsdk_tempo_lock_read(int deck, struct azsdk_tempo_lock_sample *out) {
    assert(deck >= 0 && deck < 4);
    if (!ready) return 0;
    *out = (struct azsdk_tempo_lock_sample){100u + deck, 200u + deck, 300u + deck, deck & 1, 1};
    return 1;
}
int azsdk_sync_read(int deck, struct azsdk_sync_sample *out) {
    assert(deck >= 0 && deck < 4);
    if (!ready) return 0;
    *out = receipt;
    return 1;
}
#define NDECK 4
#include SYNC_EXPORT_INC
int main(void) {
    assert(sizeof(struct azstem_sync_observation_v1) == 40);
    uint32_t *tiny = malloc(sizeof *tiny);
    assert(tiny);
    *tiny = sizeof *tiny;
    assert(azstem_sync_observation_v1(0, (struct azstem_sync_observation_v1 *) tiny) == -1 && *tiny == sizeof *tiny);
    free(tiny);
    for (int deck = 0; deck < 4; ++deck) {
        struct { struct azstem_tempo_lock_observation_v1 value; uint64_t tail; } tempo = {{sizeof tempo, 1, 0, 0, 0, -1, 0}, UINT64_MAX};
        assert(azstem_tempo_lock_observation_v1(deck, &tempo.value) == 0);
        assert(tempo.value.unit_generation == 100u + deck && tempo.value.raw_locked == (deck & 1));
        assert(tempo.value.revision == 200u + deck && tempo.value.observed_ns == 300u + deck && tempo.tail == UINT64_MAX);
        for (unsigned reason = 0; reason < 4; ++reason) {
            struct azstem_tempo_lock_observation_v1 value = tempo.value;
            if (reason == 0) value.size = sizeof value - 1;
            if (reason == 1) value.version = 2;
            if (reason == 2) g_sdk_live_on = 0;
            if (reason == 3) ready = 0;
            struct azstem_tempo_lock_observation_v1 before = value;
            assert(azstem_tempo_lock_observation_v1(deck, &value) == -1 && !memcmp(&before, &value, sizeof value));
            g_sdk_live_on = ready = 1;
        }
        struct {
            struct azstem_sync_observation_v1 value;
            unsigned char tail[16];
        } out;
        memset(&out, 0xa5, sizeof out);
        out.value.size = sizeof out;
        out.value.version = 1;
        assert(azstem_sync_observation_v1(deck, &out.value) == 0);
        assert(out.value.size == sizeof out && out.value.version == 1 && out.value.unit_generation == 31);
        assert(out.value.revision == 70 && out.value.observed_ns == 700 && out.value.raw_mode == 2 && out.value.valid == 1);
        for (unsigned i = 0; i < sizeof out.tail; ++i) assert(out.tail[i] == 0xa5);
        receipt.raw_mode = 1;
        receipt.valid = 0;
        assert(azstem_sync_observation_v1(deck, &out.value) == 0 && out.value.raw_mode == 1 && !out.value.valid && out.value.observed_ns == 700);
        receipt.raw_mode = 2;
        receipt.valid = 1;
        for (unsigned reason = 0; reason < 5; ++reason) {
            struct azstem_sync_observation_v1 value = out.value;
            int query_deck = deck;
            if (reason == 0) value.size = sizeof value - 1;
            if (reason == 1) value.version = 2;
            if (reason == 2) query_deck = 4;
            if (reason == 3) g_sdk_live_on = 0;
            if (reason == 4) ready = 0;
            struct azstem_sync_observation_v1 before = value;
            assert(azstem_sync_observation_v1(query_deck, &value) == -1 && !memcmp(&before, &value, sizeof value));
            g_sdk_live_on = ready = 1;
        }
    }
    assert(azstem_sync_observation_v1(-1, NULL) == -1);
    puts("sync export: four decks, invalid receipts, exact size/version guards and unchanged failure outputs passed");
    return 0;
}
