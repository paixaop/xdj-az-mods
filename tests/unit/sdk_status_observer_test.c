/* SPDX-License-Identifier: GPL-3.0-only */
#include "sdk_status_engine.h"
#include "sdk_status_diagnostics.h"
#include "sdk_status_host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned notifications, callbacks, bindings;
static uint64_t order, epoch, native_interest[4], store_interest[4], gui_interest;
static int loaded;
static az_track_ref track;
void az_status_host_wake(void) {}
void az_status_host_deadline(uint64_t deadline) { (void) deadline; }
int az_status_host_live(int owner) { return owner == 3; }
void az_status_host_store_binding(unsigned deck, uint64_t load, const az_track_ref *ref, uint64_t cut) {
    struct ds_deck_stamp stamp;
    ds_read_stamp(deck, &stamp); /* Host notification is outside the reducer lock. */
    assert(stamp.epoch == load && cut);
    (void) ref;
    bindings++;
}
void az_status_host_store_interest(unsigned deck, uint64_t topics) { store_interest[deck] = topics; }
void az_status_host_native_interest(unsigned deck, uint64_t topics) { native_interest[deck] = topics; }
void az_status_host_gui_interest(uint64_t topics) { gui_interest |= topics; }
static void observe(unsigned deck, uint64_t cut, uint64_t load, int ready, const az_track_ref *ref) {
    struct ds_deck_stamp stamp;
    ds_read_stamp(deck, &stamp); /* Also proves the callback can reenter safely. */
    assert(deck == 0 && stamp.epoch == load && cut > order);
    notifications++;
    order = cut;
    epoch = load;
    loaded = ready;
    track = *ref;
}
static struct ds_field field(unsigned key, uint64_t load, unsigned provenance, uint64_t revision, const void *value) {
    return (struct ds_field) {.topic = AZ_STATUS_LOAD, .field = key, .info = {
        .info = {AZ_AVAIL_READY, provenance, revision, ds_now()}, .scope = AZ_STATUS_LOAD_SCOPE,
        .scope_id = load, .provider_id = 9, .count = 1}, .value = value};
}
static void publish(unsigned key, uint64_t load, unsigned provenance, uint64_t revision, const void *value, unsigned flags) {
    struct ds_field f = field(key, load, provenance, revision, value);
    assert(ds_publish(0, load, flags, ds_now(), &f, 1, NULL, NULL, 0, 0) == AZ_OK);
}
static void receive(const az_deck_status_event_v1 *event, void *user) {
    (void) user;
    callbacks++;
    assert(event->deck == 0);
    ds_retire(3); /* Retiring inside delivery preserves its pinned view. */
    assert(event->view->load);
}
static void test_loaded_reference(void) {
    az_track_ref ref = {sizeof ref, AZ_DATA_VERSION, 1, 2, 3, 4, 0}, copy = {0};
    assert(az_status_loaded_reference(4, 1, &copy) == AZ_INVALID);
    assert(az_status_loaded_reference(0, 0, &copy) == AZ_INVALID);
    assert(az_status_loaded_reference(0, 1, NULL) == AZ_INVALID);
    assert(az_status_loaded_reference(0, 1, &copy) == AZ_BUSY);
    publish(AZ_STATUS_LOAD_TRACK, 1, AZ_PROV_FILE, 1, &ref, 0);
    assert(loaded && track.source_id == ref.source_id && track.track_id == ref.track_id);
    assert(az_status_loaded_reference(0, 1, &copy) == AZ_OK && !memcmp(&copy, &ref, sizeof ref));
    assert(az_status_loaded_reference(0, 2, &copy) == AZ_STALE);
    struct ds_field wrong = field(AZ_STATUS_LOAD_TRACK, 2, AZ_PROV_FILE, 2, &ref);
    assert(ds_publish(0, 1, 0, ds_now(), &wrong, 1, NULL, NULL, 0, 0) == AZ_INVALID);
    assert(track.source_id == ref.source_id); /* Wrong-scope input cannot replace the qualified track. */
    assert(ds_retire_file_binding(0, 1, &ref) == AZ_OK);
    assert(loaded && !track.source_id && az_status_loaded_reference(0, 1, &copy) == AZ_BUSY);
}
static void test_observer_interest(void) {
    az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 1, 30, AZ_STATUS_TOPIC(AZ_STATUS_LOAD)};
    az_deck_subscription subscription = 0;
    assert(ds_register(3, &watch, receive, NULL, &subscription) == AZ_OK);
    assert(ds_owner_subscriptions(3) == 1);
    assert(ds_drain(4) == 1 && callbacks == 1);
    assert(ds_owner_subscriptions(3) == 0);
    assert(ds_release(3, subscription) == AZ_STALE);
    for (unsigned d = 0; d < 4; ++d) {
        assert(native_interest[d] == AZ_STATUS_TOPIC(AZ_STATUS_LOAD));
        assert(store_interest[d] == AZ_STATUS_TOPIC(AZ_STATUS_LOAD));
    }
    assert(gui_interest & AZ_STATUS_TOPIC(AZ_STATUS_LOAD));
}
int main(void) {
    assert(sdk_status_analysis_bind(observe) == AZ_UNSUPPORTED);
    ds_enable();
    uint32_t state = AZ_STATUS_LOAD_LOADED;
    publish(AZ_STATUS_LOAD_STATE, 1, AZ_PROV_FILE, 1, &state, DS_BOUNDARY);
    assert(!notifications);
    assert(sdk_status_analysis_bind(observe) == AZ_OK && notifications == 1);
    assert(epoch == 1 && !loaded && !track.source_id); /* FILE state cannot assert native loaded. */
    publish(AZ_STATUS_LOAD_STATE, 1, AZ_PROV_NATIVE, 2, &state, DS_NATIVE_INPUT);
    assert(loaded && !track.source_id);
    test_loaded_reference();
    test_observer_interest();
    state = AZ_STATUS_LOAD_LOADING;
    publish(AZ_STATUS_LOAD_STATE, 2, AZ_PROV_NATIVE, 3, &state, DS_BOUNDARY | DS_NATIVE_INPUT);
    assert(epoch == 2 && !loaded && !track.source_id && bindings >= 2);
    unsigned before = notifications;
    assert(ds_publish(0, 1, DS_NATIVE_INPUT, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_STALE);
    assert(notifications == before && ds_drain(4) == 0 && callbacks == 1);
    puts("PASS independently linked status observer: native load, qualified identity, ordered cuts, unlocked callbacks and persistent core interest");
    return 0;
}
