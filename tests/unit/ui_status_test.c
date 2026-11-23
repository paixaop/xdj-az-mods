/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "sdk_status_engine.c"
void az_status_host_gui_interest(uint64_t topics) { (void) topics; }
void az_status_host_wake(void) {}
void az_status_host_deadline(uint64_t ns) { (void) ns; }
void az_status_host_native_interest(unsigned deck, uint64_t topics) {
    (void) deck;
    (void) topics;
}
void az_status_host_store_interest(unsigned deck, uint64_t topics) {
    (void) deck;
    (void) topics;
}
void az_status_host_store_binding(unsigned deck, uint64_t epoch, const az_track_ref *ref, uint64_t cut) {
    (void) deck;
    (void) epoch;
    (void) ref;
    (void) cut;
}
int az_status_host_live(int owner) { return owner >= 0; }

static void rejected(unsigned deck, uint64_t epoch, int expected) {
    az_track_ref out, sentinel;
    memset(&sentinel, 0xa5, sizeof sentinel);
    out = sentinel;
    assert(az_status_loaded_reference(deck, epoch, &out) == expected);
    assert(!memcmp(&out, &sentinel, sizeof out));
    /* Every return path releases the reducer lock. */
    assert(!pthread_mutex_trylock(&ds.mu));
    assert(!pthread_mutex_unlock(&ds.mu));
}

int main(void) {
    ds.enabled = 1;
    rejected(4, 1, AZ_INVALID);
    rejected(0, 0, AZ_INVALID);
    assert(az_status_loaded_reference(0, 1, NULL) == AZ_INVALID);
    rejected(0, 1, AZ_STALE);
    assert(ds_publish(0, 1, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    rejected(0, 1, AZ_BUSY);
    az_track_ref ref = {.source_id = 7, .track_id = 88, .source_generation = 9};
    struct ds_field field = {
        AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK, {.info = {AZ_AVAIL_READY, AZ_PROV_NATIVE, 1, ds_now()}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = 1, .count = 1, .provider_id = 7}, &ref, NULL};
    assert(ds_publish(0, 1, 0, ds_now(), &field, 1, NULL, NULL, 0, 0) == AZ_OK);
    az_track_ref out;
    assert(az_status_loaded_reference(0, 1, &out) == AZ_OK && !memcmp(&out, &ref, sizeof out));
    /* The selector must reject malformed or transitional reducer cells, even when the
     * current image is still on the requested epoch. No rejection writes the output. */
    struct ds_cell *cell = ds.deck[0].current.field[AZ_STATUS_LOAD][AZ_STATUS_LOAD_TRACK];
    assert(cell && sa_load_cell_ready(cell, 1));
    cell->info.info.availability = AZ_AVAIL_STALE;
    rejected(0, 1, AZ_BUSY);
    cell->info.info.availability = AZ_AVAIL_READY;
    unsigned scope = cell->info.scope;
    cell->info.scope = 0;
    rejected(0, 1, AZ_BUSY);
    cell->info.scope = scope;
    cell->info.scope_id = 2;
    rejected(0, 1, AZ_BUSY);
    cell->info.scope_id = 1;
    cell->bytes--;
    rejected(0, 1, AZ_BUSY);
    cell->bytes++;
    assert(az_status_loaded_reference(0, 1, &out) == AZ_OK && !memcmp(&out, &ref, sizeof out));
    assert(ds_publish(0, 2, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    rejected(0, 1, AZ_STALE);
    rejected(0, 2, AZ_BUSY);
    puts("PASS loaded reference: reducer qualification, epoch fences and untouched failures");
    return 0;
}
