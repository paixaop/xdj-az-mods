/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include "sdk_status_engine.c"
static void ds_wake(void) {}
static void ds_deadline(uint64_t ns) { (void) ns; }
static int ds_live(int owner) { return owner >= 0; }
static void ds_gui_interest(uint64_t topics) { (void) topics; }
static void ds_native_interest(unsigned deck, uint64_t topics) { (void) deck; (void) topics; }
static void sdk_status_store_interest(unsigned deck, uint64_t topics) { (void) deck; (void) topics; }
static void sdk_status_store_binding(unsigned d, uint64_t e, const az_track_ref *r, uint64_t c) {
    (void) d; (void) e; (void) r; (void) c;
}
#define CM_ROOT "/tmp/azmod-proof-fixture"
#ifndef DS_METRICS_PATH
#define DS_METRICS_PATH "/tmp/azmod-proof-fixture.stats"
#endif
#define az_thread_create pthread_create
#include "sdk_status_metrics.c"
static unsigned callbacks;
static void receive(const az_deck_status_event_v1 *event, void *user) {
    (void) user;
    if (event->kind == AZ_STATUS_RESYNC) {
        assert(!event->occurrence && !event->phrase_transition);
        callbacks++;
    }
}
static struct ds_field field(unsigned topic, unsigned member, unsigned provenance, uint64_t revision,
                             const void *value, unsigned count) {
    return (struct ds_field) {topic, member, {.info = {AZ_AVAIL_READY, provenance, revision, ds_now()},
        .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = 1, .provider_id = 11, .count = count}, value, NULL};
}
int main(void) {
    ds.enabled = 1;
    az_track_ref source = {sizeof source, AZ_DATA_VERSION, 1, 2, 3, 4, 0};
    struct ds_field track = field(AZ_STATUS_LOAD, AZ_STATUS_LOAD_TRACK, AZ_PROV_FILE, 1, &source, 1);
    assert(ds_publish(0, 1, DS_BOUNDARY, ds_now(), &track, 1, NULL, NULL, 0, 0) == AZ_OK);
    const uint64_t metadata = AZ_STATUS_TOPIC(AZ_STATUS_METADATA), key = AZ_STATUS_TOPIC(AZ_STATUS_KEY);
    struct ds_field title = field(AZ_STATUS_METADATA, AZ_STATUS_METADATA_TITLE, AZ_PROV_FILE, 1, "title", 5);
    struct ds_field key_field = field(AZ_STATUS_KEY, AZ_STATUS_KEY_ORIGINAL, AZ_PROV_FILE, 1, "8A", 2);
    assert(ds_publish_file(0, 1, &source, ds_now(), &title, 1, 0, 0) == AZ_OK);
    assert(ds_publish_file(0, 1, &source, ds_now(), &key_field, 1, 0, 0) == AZ_OK);
    assert(ds_proofs.count == 0); /* Disabled diagnostics are inert. */
    ds_proofs.enabled = 1;
    az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1, 30, AZ_STATUS_ALL_V1};
    az_deck_subscription handles[2];
    assert(ds_register(0, &watch, receive, NULL, &handles[0]) == AZ_OK);
    assert(ds_register(1, &watch, receive, NULL, &handles[1]) == AZ_OK);
    ds_drain(8);
    ds_proof_context = (struct ds_proof_context) {.kind = DS_PROOF_GAP, .cause = DS_PROOF_DATABASE,
        .binding_cut = 1, .binding_serial = 10, .connection = 100, .db_before = 2, .db_after = 3,
        .device = 9, .inode = 8, .mount_id = 7};
    assert(ds_publish_file(0, 1, &source, ds_now(), NULL, 0, metadata | key, AZ_STATUS_CAPTURE_GAP) == AZ_OK);
    assert(ds_proofs.count == 1 && ds_proofs.record[0].interval == 1);
    assert(ds_proofs.record[0].input_lost == (metadata | key) && ds_proofs.record[0].context.cause == DS_PROOF_DATABASE);
    uint64_t gap_sequence = ds.deck[0].current.sequence;
    memset(&ds_proof_context, 0, sizeof ds_proof_context);
    ds_drain(8);
    assert(callbacks == 2 && ds_proofs.count == 3 && ds.metrics[0].resyncs == 2);
    for (unsigned i = 1; i < 3; i++) {
        const struct ds_proof_record *r = &ds_proofs.record[i];
        assert(r->kind == DS_PROOF_RESYNC && r->interval == 1 && r->sequence == gap_sequence);
        assert(r->cursor_before < r->sequence && r->handle && r->topics == AZ_STATUS_ALL_V1);
    }
    ds_proof_context = (struct ds_proof_context) {.kind = DS_PROOF_BASELINE, .binding_cut = 1,
        .binding_serial = 11, .connection = 100, .db_before = 3, .db_after = 3, .topics = metadata};
    title.info.info.revision++;
    assert(ds_publish_file(0, 1, &source, ds_now(), &title, 1, 0, 0) == AZ_OK);
    assert(ds_proofs.record[3].ready == metadata && ds_proofs.record[3].pending == key);
    /* A rejected source cannot settle the old interval. */
    source.source_generation++;
    assert(ds_publish_file(0, 1, &source, ds_now(), &key_field, 1, 0, 0) == AZ_STALE);
    assert(ds_proofs.count == 4 && ds_proofs.active[0].pending == key);
    source.source_generation--;
    ds_proof_context.topics = key;
    key_field.info.info.revision++;
    key_field.info.info.availability = AZ_AVAIL_UNSUPPORTED;
    key_field.info.count = 0; key_field.value = NULL;
    assert(ds_publish_file(0, 1, &source, ds_now(), &key_field, 1, 0, 0) == AZ_OK);
    assert(ds_proofs.record[4].unavailable == key && !ds_proofs.record[4].pending);
    assert(!ds_proofs.active[0].interval);
    /* Native loss remains independently attributable even at the same mask. */
    assert(ds_publish(0, 1, DS_NATIVE_INPUT, ds_now(), NULL, 0, NULL, NULL, key, AZ_STATUS_CAPTURE_GAP) == AZ_OK);
    assert(ds_proofs.record[5].kind == DS_PROOF_GAP && !ds_proofs.record[5].file_only && !ds_proofs.record[5].interval);
    assert(!ds_proofs.record[5].context.kind && !ds_proofs.record[5].context.connection);
    ds_drain(8);
    assert(ds_proofs.record[6].kind == DS_PROOF_RESYNC && !ds_proofs.record[6].interval);
    /* A source boundary explicitly closes the previous source interval. */
    ds_proof_context = (struct ds_proof_context) {.kind = DS_PROOF_GAP, .cause = DS_PROOF_DATABASE};
    assert(ds_publish_file(0, 1, &source, ds_now(), NULL, 0, metadata, AZ_STATUS_CAPTURE_GAP) == AZ_OK);
    uint64_t interval = ds_proofs.active[0].interval;
    memset(&ds_proof_context, 0, sizeof ds_proof_context);
    assert(ds_publish(0, 2, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    const struct ds_proof_record *retired = &ds_proofs.record[ds_proofs.count - 1];
    assert(retired->kind == DS_PROOF_RETIRE && retired->interval == interval && retired->epoch == 2);
    assert(retired->source.track_id == source.track_id && !retired->pending && retired->unavailable == metadata);
    ds_drain(8);
    unsigned receipts = ds_proofs.count;
    uint64_t capture_gaps = ds.metrics[0].capture_gaps;
    for (unsigned i = 0; i <= DS_HISTORY; i++) {
        double bpm = 120 + (i & 1u);
        struct ds_field tempo = field(AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_CURRENT_BPM, AZ_PROV_NATIVE, i + 1, &bpm, 1);
        tempo.info.scope_id = 2;
        assert(ds_publish(0, 2, DS_NATIVE_INPUT, ds_now(), &tempo, 1, NULL, NULL, 0, 0) == AZ_OK);
    }
    assert(ds_proofs.count == receipts); /* Ordinary live publications allocate no evidence. */
    ds_drain(8);
    assert(ds_proofs.count == receipts + 2 && ds.metrics[0].capture_gaps == capture_gaps);
    for (unsigned i = receipts; i < ds_proofs.count; i++) {
        const struct ds_proof_record *r = &ds_proofs.record[i];
        assert(r->kind == DS_PROOF_RESYNC && !r->interval && (r->reason & AZ_STATUS_EVICTED));
    }
    ds_write_metrics();
    FILE *file = fopen(DS_METRICS_PATH, "r");
    assert(file);
    char line[4096]; unsigned exported = 0, header = 0;
    while (fgets(line, sizeof line, file)) {
        if (!strncmp(line, "proof=", 6)) exported++;
        if (strstr(line, "provenance_version=1") && strstr(line, "provenance_overflow=0")) header++;
    }
    assert(!ferror(file) && fclose(file) == 0 && exported == ds_proofs.count && header == 1);
    assert(unlink(DS_METRICS_PATH) == 0);
    /* Exhaustion cannot overwrite any earlier receipt or claim completeness. */
    struct ds_proof_record first = ds_proofs.record[0];
    while (ds_proofs.count < DS_PROOF_CAPACITY) assert(ds_proof_new(DS_PROOF_RESYNC, 0, 2, 0));
    assert(!ds_proof_new(DS_PROOF_RESYNC, 0, 2, 0) && ds_proofs.overflow);
    assert(!memcmp(&first, &ds_proofs.record[0], sizeof first));
    printf("PASS provenance: FILE/native/consumer-loss separation, exact subscriber cuts, fenced partial/unavailable recovery, retirement, export and sticky exhaustion (%zu bytes)\n", sizeof ds_proofs);
    return 0;
}
