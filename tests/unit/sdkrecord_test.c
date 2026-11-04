/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile the same owning provider boundary as the loader's deck services. */
#define main cue_boundary_main
#include "sdkcueevents_test.c"
#undef main
#include <math.h>
static struct azstem_loop_observation_v2 loop_receipt;
static struct azstem_load_observation_v1 load_receipt;
static int loop_status, load_status;
static struct azstem_deck_observation_v1 loaded_receipt;
static int loaded_read(int deck, struct azstem_deck_observation_v1 *out) {
    (void) deck;
    *out = loaded_receipt;
    return 0;
}

static int loop_read(int deck, struct azstem_loop_observation_v2 *out) {
    (void) deck;
    *out = loop_receipt;
    return loop_status;
}
static int load_read(int deck, struct azstem_load_observation_v1 *out) {
    (void) deck;
    *out = load_receipt;
    return load_status;
}
static az_deck_snapshot read_snapshot(void) {
    az_deck_snapshot out = {.size = sizeof out};
    assert(sd_deck_snapshot_v1(0, &out) == AZ_OK);
    return out;
}
int main(void) {
    g_st_enabled = 1;
    atomic_store(&g_sd_observation, loaded_read);
    loaded_receipt = (struct azstem_deck_observation_v1) {sizeof loaded_receipt, 1, 0, 0, 1, 100};
    az_deck_snapshot loaded = read_snapshot();
    assert((loaded.valid & AZ_LIVE_LOADED) && loaded.loaded == 0);
    for (unsigned fault = 0; fault < 6; ++fault) {
        struct azstem_deck_observation_v1 saved = loaded_receipt;
        switch (fault) {
        case 0: loaded_receipt.size--; break;
        case 1: loaded_receipt.version++; break;
        case 2: loaded_receipt.reserved = 1; break;
        case 3: loaded_receipt.revision = 0; break;
        case 4: loaded_receipt.observed_ns = 0; break;
        case 5: loaded_receipt.loaded = 2; break;
        }
        loaded = read_snapshot();
        assert(!(loaded.valid & AZ_LIVE_LOADED) && loaded.loaded == -1);
        loaded_receipt = saved;
    }

    atomic_store(&g_sd_loop_observation, loop_read);
    atomic_store(&g_sd_load_observation, load_read);
    loop_receipt = (struct azstem_loop_observation_v2) {sizeof loop_receipt, 2, 1, 1, 100, 0, 1, 0, 4};
    load_receipt = (struct azstem_load_observation_v1) {sizeof load_receipt, 1, {0}};
    load_receipt.sample.unit_generation = 3;
    load_receipt.sample.load_generation = 4;
    load_receipt.sample.invalidation_epoch = 5;
    load_receipt.sample.observed_ns = 100;
    az_deck_snapshot out = read_snapshot();
    assert((out.valid & AZ_LIVE_LOOP_ENGAGED) && out.loop.engaged == 0);
    assert((out.valid & AZ_LIVE_LOOP_BOUNDS) && out.loop.in_s == 0 && out.loop.out_s == 4);
    assert((out.valid & AZ_LIVE_NATIVE_LOAD) && out.native_load_generation == 4 && !(out.valid & AZ_LIVE_TRACK));
    loop_receipt.bounds_valid = 0;
    out = read_snapshot();
    assert((out.valid & AZ_LIVE_LOOP_ENGAGED) && !(out.valid & AZ_LIVE_LOOP_BOUNDS));
    loop_receipt.bounds_valid = 1;
    loop_receipt.engaged = -1;
    out = read_snapshot();
    assert(!(out.valid & AZ_LIVE_LOOP_ENGAGED) && (out.valid & AZ_LIVE_LOOP_BOUNDS));
    loop_receipt.engaged = 1;
    for (unsigned fault = 0; fault < 8; ++fault) {
        struct azstem_loop_observation_v2 saved = loop_receipt;
        switch (fault) {
        case 0: loop_receipt.size--; break;
        case 1: loop_receipt.version++; break;
        case 2: loop_receipt.unit_generation = 0; break;
        case 3: loop_receipt.revision = 0; break;
        case 4: loop_receipt.observed_ns = 0; break;
        case 5: loop_receipt.in_s = NAN; break;
        case 6: loop_receipt.out_s = INFINITY; break;
        case 7: loop_status = AZ_BUSY; break;
        }
        out = read_snapshot();
        assert(!(out.valid & AZ_LIVE_LOOP_BOUNDS));
        if (fault != 5 && fault != 6) assert(!(out.valid & AZ_LIVE_LOOP_ENGAGED));
        loop_receipt = saved;
        loop_status = 0;
    }
    for (unsigned fault = 0; fault < 5; ++fault) {
        struct azstem_load_observation_v1 saved = load_receipt;
        switch (fault) {
        case 0: load_receipt.size--; break;
        case 1: load_receipt.version++; break;
        case 2: load_receipt.sample.observed_ns = 0; break;
        case 3: load_receipt.sample.unit_generation = 0; break;
        case 4: load_status = AZ_BUSY; break;
        }
        out = read_snapshot();
        assert(!(out.valid & AZ_LIVE_NATIVE_LOAD));
        load_receipt = saved;
        load_status = 0;
    }
    /* Retirement reports unavailable; a replacement cannot borrow old evidence. */
    loop_status = load_status = AZ_STALE;
    out = read_snapshot();
    assert(!(out.valid & (AZ_LIVE_NATIVE_LOAD | AZ_LIVE_LOOP_ENGAGED | AZ_LIVE_LOOP_BOUNDS)));
    loop_status = load_status = 0;
    loop_receipt.unit_generation++;
    loop_receipt.observed_ns = 0;
    load_receipt.sample.unit_generation++;
    load_receipt.sample.observed_ns = 0;
    out = read_snapshot();
    assert(!(out.valid & (AZ_LIVE_NATIVE_LOAD | AZ_LIVE_LOOP_ENGAGED | AZ_LIVE_LOOP_BOUNDS)));
    const uint32_t capacities[] = {4, offsetof(az_deck_snapshot, loop_unit_generation) - 1,
                                   offsetof(az_deck_snapshot, loop_unit_generation), sizeof out, sizeof out + 16};
    for (unsigned n = 0; n < sizeof capacities / sizeof capacities[0]; ++n) {
        _Alignas(az_deck_snapshot) unsigned char bytes[sizeof out + 48], before[sizeof bytes];
        memset(bytes, 0xa5, sizeof bytes);
        uint32_t size = capacities[n];
        memcpy(bytes + 16, &size, 4);
        memcpy(before, bytes, sizeof bytes);
        int rc = sd_deck_snapshot_v1(0, (az_deck_snapshot *) (bytes + 16));
        if (n < 2) {
            assert(rc == AZ_INVALID);
            assert(!memcmp(bytes, before, sizeof bytes));
        } else {
            assert(rc == AZ_OK);
            assert(!memcmp(bytes, before, 16));
            uint32_t got;
            memcpy(&got, bytes + 16, 4);
            assert(got == size);
            size_t written = size < sizeof out ? size : sizeof out;
            assert(!memcmp(bytes + 16 + written, before + 16 + written, sizeof bytes - 16 - written));
        }
    }
    puts("PASS SDK record boundary: independent validity, zero coordinates, receipt envelopes and bounded writes");
    return 0;
}
