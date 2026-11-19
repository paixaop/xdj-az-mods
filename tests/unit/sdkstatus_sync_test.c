/* SPDX-License-Identifier: GPL-3.0-only */
#define main native_status_fixture_main
#include "sdkstatus_native_test.c"
#undef main

static unsigned sync_original_calls;
static bool sync_nested;
static _Atomic(bool) sync_entered, sync_release;
static void *loop_test_owner;
static unsigned loop_test_calls;
static void loop_test_clear(void *self) {
    assert(self == loop_test_owner);
    ++loop_test_calls;
}
static void loop_test_four(void *self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    assert(a == 11 && b == 22 && c == 33 && d == 44);
    loop_test_clear(self);
}
static void loop_test_five(void *self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    assert(e == 55);
    loop_test_four(self, a, b, c, d);
}
static void loop_test_six(void *self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {
    assert(f == 66);
    loop_test_five(self, a, b, c, d, e);
}
static void loop_test_one(void *self, uintptr_t a) {
    assert(a == 77);
    loop_test_clear(self);
}
static void rate_test_start(void *self, unsigned value) {
    assert(self == loop_test_owner && value == 7);
    assert(atomic_load(&state().decks[0].motion.active_writers) == 1);
}
static void rate_test_engine(void *self, double value, unsigned mode) {
    assert(self == loop_test_owner && value == 1.25 && mode == 8);
    assert(!atomic_load(&state().decks[0].motion.active_writers));
}
static void test_unbound_writers(const struct azstatus_native_v1 *api) {
    Buffer_300 owner = {0};
    loop_test_owner = owner.bytes;
    assert(!processing);
    status_original[1] = (void *) loop_test_clear;
    status_original[2] = (void *) loop_test_four;
    status_original[3] = (void *) loop_test_five;
    status_original[4] = (void *) loop_test_six;
    status_original[6] = (void *) loop_test_one;
    status_writer_1(owner.bytes);
    status_writer_2(owner.bytes, 11, 22, 33, 44);
    status_writer_3(owner.bytes, 11, 22, 33, 44, 55);
    status_writer_4(owner.bytes, 11, 22, 33, 44, 55, 66);
    status_writer_6(owner.bytes, 77);
    assert(loop_test_calls == 5);
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    assert(!api->take(0, &receipt));
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    atomic_store(&state().decks[0].core, (uintptr_t) owner.bytes);
    atomic_store(&rate_state[0].bound_engine, (uintptr_t) owner.bytes);
    rate_original[0] = (void *) rate_test_start;
    rate_original[2] = (void *) rate_test_engine;
    for (unsigned engine = 0; engine < 2; ++engine) {
        uint64_t fault = atomic_load(&rate_state[0].fault);
        if (engine) rate_writer_2(owner.bytes, 1.25, 8);
        else rate_writer_0(owner.bytes, 7);
        assert(atomic_load(&rate_state[0].fault) == fault + 1);
        receipt = take(api);
        assert(receipt.kind == AZN_GAP && receipt.b == AZN_RATE && receipt.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT));
    }
    assert(!atomic_load(&state().decks[0].motion.active_writers));
    atomic_store(&state().decks[0].core, 0);
    atomic_store(&rate_state[0].bound_engine, 0);
    rate_writer_2(owner.bytes, 1.25, 8);
    assert(!api->take(0, &receipt));
    const unsigned kinds[] = {AZN_QUANTIZE, AZN_KEY_LOCK, AZN_SYNC, AZN_SLIDER_LOCK};
    for (unsigned i = 0; i < 4; ++i) {
        azn_setting_gap(0, kinds[i]);
        receipt = take(api);
        assert(receipt.kind == AZN_GAP && receipt.b == (int) kinds[i] && receipt.lost_topics);
    }
    azn_setting_gap(4, AZN_SYNC);
    azn_setting_gap(0, AZN_LOOP);
    assert(!api->take(0, &receipt));
    puts("PASS unbound writers: all loop argument registers preserved, core/engine rate loss fences, setting-specific recovery lanes");
}
static void sync_test_period(void *object) {
    ++sync_original_calls;
    uintptr_t parent = (uintptr_t) object;
    put(parent + 0xbc, (uint32_t) 1);
    put(word(parent + 0x1d0) + 0x48, (uint32_t) 1);
    if (sync_nested) {
        sync_nested = false;
        sync_writer_1(object, 0, 2);
        assert(atomic_load(&sync_roles[0].writers) == 1);
    }
}
static void sync_test_election(void *object, unsigned role, unsigned member) {
    ++sync_original_calls;
    put((uintptr_t) object + 0xbc, role);
    put((uintptr_t) object + 0xc0, member);
}
static void sync_test_beat(void *object, const void *target) {
    ++sync_original_calls;
    memcpy((void *) ((uintptr_t) object + 0x120), target, sizeof(double));
}
static void sync_test_receipt(const struct azstatus_native_v1 *api, uint64_t owner, int follow, int locked) {
    struct azstatus_native_receipt receipt = take(api);
    assert(receipt.kind == AZN_SYNC_ROLE && receipt.unit == owner && receipt.valid == 1);
    assert(receipt.a == follow && receipt.b == locked);
    assert(!api->take(0, &receipt));
}
static void *sync_test_competing(void *parent) {
    SyncRoleWriter writer;
    sync_role_writer_init(&writer, (uintptr_t) parent);
    assert(writer.deck == 0);
    atomic_store(&sync_entered, true);
    while (!atomic_load(&sync_release)) sched_yield();
    sync_role_writer_drop(&writer);
    return NULL;
}
static void sync_test_gap_receipt(const struct azstatus_native_v1 *api, uintptr_t parent, uint64_t owner) {
    struct azstatus_native_receipt receipt = take(api);
    assert(receipt.kind == AZN_GAP && receipt.lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_SYNC));
    assert(!api->take(0, &receipt));
    /* The lane refuses publication until its loss cut has been consumed.
     * Its new seed must cause a retry even when the native values are equal. */
    sync_capture(0, parent, owner);
    sync_test_receipt(api, owner, 0, 0);
}
static void test_history_guards(const struct azstatus_native_v1 *api) {
    struct ReloopState loop = {0};
    loop.raw[0] = 2;
    loop.raw[1] = 3;
    uint64_t locks[2] = {2, 3};
    assert(!reloop_unwitnessed_lock(&loop, locks, -1, 0));
    locks[0] = 4;
    assert(!reloop_unwitnessed_lock(&loop, locks, 0, 2));
    assert(reloop_unwitnessed_lock(&loop, locks, 0, 1));
    assert(reloop_unwitnessed_lock(&loop, locks, 1, 2));
    locks[0] = 2;
    locks[1] = 4;
    assert(!reloop_unwitnessed_lock(&loop, locks, 1, 3));
    assert(reloop_unwitnessed_lock(&loop, locks, -1, 3));
    const struct TransportProfile *profile = &transport_profiles[state().build - 1];
    assert(transport_write_cause(0, state().bias + profile->normal_play) == TransportPlay);
    assert(transport_write_cause(0, state().bias + profile->audition) == TransportAudition);
    assert(transport_write_cause(0, 0) == TransportOtherPlay);
    assert(transport_write_cause(1, 0) == TransportPause);
    assert(transport_write_cause(2, 0) == TransportCue && transport_write_cause(3, 0) == TransportCue);
    api->interest(1, AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    HotState hot = {.ready = true, .provider = 7, .serial = 4, .loss_cut = 10};
    struct HotCapture capture = {.cut = 10};
    assert(status_hot_advance(1, &hot, &capture) && hot.serial == 4);
    capture.semantic_changes = 1;
    assert(status_hot_advance(1, &hot, &capture) && hot.serial == 5);
    capture.semantic_changes = 0;
    capture.replaced = true;
    assert(status_hot_advance(1, &hot, &capture) && hot.serial == 6 && hot.provider != 7);
    hot.serial = UINT64_MAX;
    capture.replaced = false;
    capture.gap = true;
    assert(!status_hot_advance(1, &hot, &capture) && !hot.ready && hot.serial == UINT64_MAX);
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    assert(api->take(1, &receipt) && receipt.kind == AZN_GAP && receipt.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    hot.ready = true;
    atomic_store(&hot_providers, AZSP_SERIAL_MAX);
    capture.replaced = true;
    assert(!status_hot_advance(1, &hot, &capture) && !hot.ready && !hot.provider);
    assert(api->take(1, &receipt) && receipt.kind == AZN_GAP && !(api->capabilities(1) & AZN_CAP_HOT));
    assert(!api->take(1, &receipt));
    puts("PASS history guards: witnessed lock changes, exact transport callers and nonwrapping cue revisions/providers");
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int build = atoi(argv[1]);
    assert(build == 1 || build == 2);
    State *s = &state();
    s->build = build;
    s->bias = 0;
    const SyncProfile *p = &sync_profiles[build - 1];
    Buffer_210 parent = {0};
    Buffer_c0 slave = {0};
    Buffer_20 master = {0}, server = {0};
    Buffer_60 job = {0};
    uintptr_t address = (uintptr_t) parent.bytes;
    put(address, setting_profiles[build - 1].sync_function);
    put(address + 0xb0, (uintptr_t) server.bytes);
    put((uintptr_t) server.bytes, p->server);
    put(address + 0xb8, (uint8_t) 1);
    put(address + 0xbc, (uint32_t) 1);
    put(address + 0xc0, (uint32_t) 2);
    put(address + 0x1c8, (uintptr_t) master.bytes);
    put((uintptr_t) master.bytes, p->master);
    put((uintptr_t) master.bytes + 8, address);
    put(address + 0x1d0, (uintptr_t) slave.bytes);
    put(address + 0x1c0, (uintptr_t) slave.bytes);
    put((uintptr_t) slave.bytes, p->slave);
    put((uintptr_t) slave.bytes + 8, address);
    put((uintptr_t) slave.bytes + 0xb0, (uintptr_t) job.bytes);
    put((uintptr_t) job.bytes, p->job);
    put((uintptr_t) job.bytes + 0x30, (uintptr_t) slave.bytes);
    put(address + 0x118, (uint8_t) 1);
    put(address + 0x119, (uint8_t) 1);
    put(address + 0x120, (double) 126.5);
    put(address + 0x158, (uint32_t) 2);
    put((uintptr_t) slave.bytes + 0x10, (uint32_t) 1);
    put((uintptr_t) slave.bytes + 0x48, (uint32_t) 2);
    Setting *setting = &s->decks[0].settings[2];
    atomic_store(&setting->address, address);
    atomic_store(&setting->provider, 41);
    atomic_store(&sync_installed, true);
    sync_original[0] = (void *) sync_test_period;
    sync_original[1] = (void *) sync_test_election;
    sync_original[2] = (void *) sync_test_beat;
    const struct azstatus_native_v1 *api = azstem_status_service_v1();
    assert(api->wake_fd() >= 0);
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_SYNC));
    sync_capture(0, address, 41);
    sync_test_receipt(api, 41, 1, 1);
    sync_capture(0, address, 41);
    struct azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    assert(!api->take(0, &receipt));
    sync_writer_1(parent.bytes, 0, 2);
    sync_test_receipt(api, 41, 0, 0);
    sync_writer_0(parent.bytes);
    sync_test_receipt(api, 41, 1, 0);
    double empty_target = 0;
    sync_writer_2(parent.bytes, &empty_target);
    sync_test_receipt(api, 41, 0, 0);
    put(address + 0x120, (double) 126.5);
    sync_nested = true;
    sync_writer_0(parent.bytes);
    assert(!sync_nested && !sync_writer_frame && !atomic_load(&sync_roles[0].writers));
    assert(!api->take(0, &receipt)); /* nested election finishes at the same value */
    assert(sync_original_calls == 5);

    /* A new subscriber's seed receives the same native values again. */
    api->interest(0, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO));
    sync_capture(0, address, 41);
    sync_test_receipt(api, 41, 0, 0);
    atomic_flag_test_and_set(&sync_roles[0].publish);
    sync_capture(0, address, 41);
    atomic_flag_clear(&sync_roles[0].publish);
    sync_capture(0, address, 41);
    sync_test_gap_receipt(api, address, 41);

    pthread_t competing;
    assert(!pthread_create(&competing, NULL, sync_test_competing, parent.bytes));
    while (!atomic_load(&sync_entered)) sched_yield();
    SyncRoleWriter writer;
    sync_role_writer_init(&writer, address);
    assert(writer.deck == 0 && atomic_load(&sync_roles[0].writers) == 2);
    sync_role_writer_drop(&writer);
    assert(atomic_load(&sync_roles[0].writers) == 1);
    atomic_store(&sync_release, true);
    assert(!pthread_join(competing, NULL));
    sync_test_gap_receipt(api, address, 41);
    assert(!sync_writer_frame && !atomic_load(&sync_roles[0].writers));

    /* An outstanding callback cannot publish under a replacement owner. */
    sync_role_writer_init(&writer, address);
    atomic_store(&setting->provider, 42);
    sync_role_writer_drop(&writer);
    assert(!api->take(0, &receipt));
    sync_capture(0, address, 42);
    sync_test_receipt(api, 42, 0, 0);
    atomic_store(&setting->address, 0);
    sync_capture(0, address, 42);
    assert(!api->take(0, &receipt));
    sync_role_writer_init(&writer, address);
    assert(writer.deck == 4);
    sync_role_writer_drop(&writer);
    atomic_store(&setting->address, address);
    put(address, (uintptr_t) 0);
    sync_capture(0, address, 42);
    assert(!api->take(0, &receipt));
    test_unbound_writers(api);
    test_history_guards(api);
    puts("PASS sync role: original writers, nested dispatch, reseed/dedup, contention gaps, concurrent writers, replacement owner and invalid native type");
    return 0;
}
