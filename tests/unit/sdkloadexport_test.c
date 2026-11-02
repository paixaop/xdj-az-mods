/* SPDX-License-Identifier: GPL-3.0-only */
#include "azloadexport.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL load export: %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

static int ready = 1, reads;
static struct azsdk_load_sample receipt = {11, 29, 37, 999999, 0, 0};
int azsdk_load_read(int deck, struct azsdk_load_sample *out) {
    CHECK(deck >= 0 && deck < 4 && out);
    ++reads;
    if (!ready) return 0;
    *out = receipt;
    return 1;
}
static void unchanged(int deck, uint32_t size, uint32_t version) {
    _Alignas(struct azstem_load_observation_v1) unsigned char bytes[128], before[128];
    memset(bytes, 0xa5, sizeof bytes);
    memcpy(bytes, &size, sizeof size);
    memcpy(bytes + sizeof size, &version, sizeof version);
    memcpy(before, bytes, sizeof bytes);
    CHECK(azstem_load_observation_v1(deck, (void *) bytes) == -1);
    CHECK(memcmp(bytes, before, sizeof bytes) == 0);
}
int main(void) {
    CHECK(sizeof(struct azstem_load_observation_v1) == 48);
    uint32_t *prefix = malloc(sizeof *prefix);
    CHECK(prefix);
    *prefix = sizeof *prefix;
    CHECK(azstem_load_observation_v1(0, (void *) prefix) == -1 && *prefix == sizeof *prefix);
    free(prefix);
    CHECK(reads == 0);
    CHECK(azstem_load_observation_v1(0, NULL) == -1);
    for (uint32_t size = 0; size < 48; ++size) unchanged(0, size, 1);
    CHECK(reads == 0);
    for (uint32_t version = 0; version < 5; ++version) if (version != 1) unchanged(0, 48, version);
    unchanged(0, 128, UINT32_MAX); /* newer schema, despite a larger allocation */
    unchanged(-1, 48, 1); unchanged(4, 48, 1);
    CHECK(reads == 0);
    for (int deck = 0; deck < 4; ++deck) {
        for (uint32_t size = 48; size <= 128; ++size) {
            _Alignas(struct azstem_load_observation_v1) unsigned char bytes[144], before[144];
            memset(bytes, 0xa5, sizeof bytes);
            uint32_t header[2] = {size, 1};
            memcpy(bytes, header, sizeof header);
            memcpy(before, bytes, sizeof bytes);
            CHECK(azstem_load_observation_v1(deck, (void *) bytes) == 0);
            struct azstem_load_observation_v1 value;
            memcpy(&value, bytes, sizeof value);
            CHECK(value.size == size && value.version == 1);
            CHECK(memcmp(&value.sample, &receipt, sizeof receipt) == 0);
            CHECK(memcmp(bytes + sizeof value, before + sizeof value, sizeof bytes - sizeof value) == 0);
        }
    }
    receipt.native_request_id = UINT32_MAX;
    struct azstem_load_observation_v1 value = {sizeof value, 1, {0, 0, 0, 0, 0, 0}};
    azstem_load_observation_v1_fn function = azstem_load_observation_v1;
    CHECK(function(3, &value) == 0 && value.sample.native_request_id == UINT32_MAX);
    struct { struct azstem_load_observation_v1 current; unsigned char tail[16]; } future;
    memset(&future, 0xa5, sizeof future);
    future.current.size = UINT32_MAX; future.current.version = 1;
    CHECK(function(0, &future.current) == 0 && future.current.size == UINT32_MAX);
    for (unsigned n = 0; n < sizeof future.tail; ++n) CHECK(future.tail[n] == 0xa5);
    ready = 0;
    unchanged(0, 48, 1); unchanged(3, 128, 1);
    puts("PASS load export: prefix, size/version/deck, absent receipt, sample copy and tail guards");
}
