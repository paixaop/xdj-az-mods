/* SPDX-License-Identifier: GPL-3.0-only */
/* Host fixture only: no EP147, native task allocation, queue, or facade calls. */
#include "aznativecommands.h"
#include "sdk_command_engine.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include "native_abi_fixture.h"
#include "aztimestamp.h"
#define CHECK(x)                                                                         \
    do {                                                                                 \
        if (!(x)) {                                                                      \
            fprintf(stderr, "FAIL nativecommands: %s:%d: %s\n", __FILE__, __LINE__, #x); \
            abort();                                                                     \
        }                                                                                \
    } while (0)
#define UNTIL(condition)                                                                                                                                                                                 \
    do {                                                                                                                                                                                                 \
        struct timespec deadline, now;                                                                                                                                                                   \
        CHECK(!clock_gettime(CLOCK_MONOTONIC, &deadline));                                                                                                                                               \
        deadline.tv_sec += 10;                                                                                                                                                                           \
        while (!(condition)) {                                                                                                                                                                           \
            CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));                                                                                                                                                \
            int order = compare_timestamps((uint64_t) now.tv_sec * UINT64_C(1000000000) + now.tv_nsec, (uint64_t) deadline.tv_sec * UINT64_C(1000000000) + deadline.tv_nsec, 1000000000, 1000000000, 1); \
            CHECK(order != AZ_TIME_INVALID && order == AZ_TIME_BEFORE);                                                                                                                                  \
            sched_yield();                                                                                                                                                                               \
        }                                                                                                                                                                                                \
    } while (0)
static uint64_t clock_now(void *context) { return *((uint64_t *) (context)); }
static az_command_request request(unsigned operation) {
    az_command_request r = {0};
    r.size = sizeof r;
    r.version = AZ_DATA_VERSION;
    r.operation = operation;
    r.request_id = 1;
    r.expected_load_generation = 42;
    r.expected_revision = 7;
    r.deadline_ns = 10000;
    return r;
}
static struct scmd_target target(uint64_t commands) {
    struct scmd_target t = {0};
    t.load_generation = 42;
    t.revision = 7;
    t.commands = commands;
    t.revision_commands = commands;
    t.valid = SCMD_TARGET_LOAD | SCMD_TARGET_REVISION;
    t.loaded = 1;
    return t;
}
static void unavailable() {
    CHECK(azsdk_native_commands_capabilities() == 0);
    azsdk_native_commands_adapter(NULL);
    struct scmd_adapter a = {0};
    azsdk_native_commands_adapter(&a);
    CHECK(a.context == NULL && a.arguments && a.begin && a.dispatch && a.end);
    struct scmd_engine engine = {0};
    CHECK(pthread_mutex_init(&engine.mu, NULL) == 0);
    uint64_t now = 100;
    engine.clock = clock_now;
    engine.clock_context = &now;
    for (unsigned op = AZ_CMD_LOOP_IN; op <= AZ_CMD_CUE_COLOR; ++op) {
        for (unsigned persist = 0; persist < 2; ++persist) {
            unsigned flags = persist ? AZ_COMMAND_PERSIST : 0;
            __auto_type r = request(op);
            r.flags = flags;
            __auto_type t = target(SCMD_ALL_COMMANDS);
            __auto_type before = t;
            struct scmd_completion completion;
            memset(&completion, 0xa5, sizeof completion);
            __auto_type completion_before = completion;
            CHECK(a.arguments(a.context, &r, &t) == AZ_UNSUPPORTED);
            CHECK(a.begin(a.context, &r, &t) == AZ_UNSUPPORTED);
            CHECK(memcmp(&t, &before, sizeof t) == 0);
            CHECK(a.dispatch(a.context, &r, 1, &completion) == AZ_UNSUPPORTED);
            CHECK(memcmp(&completion, &completion_before, sizeof completion) == 0);
            a.end(a.context, r.deck);
            az_command_result result = {0};
            result.size = sizeof result;
            enum scmd_detail detail = SCMD_DETAIL_NONE;
            CHECK(scmd_submit(&engine, 0, &r, &t, &a, &result, &detail) == AZ_UNSUPPORTED);
            CHECK(result.outcome == AZ_COMMAND_REJECTED && result.reason == AZ_UNSUPPORTED);
            CHECK(engine.next_token == 0 && engine.sequence == 0);
            for (unsigned d = 0; d < SCMD_DECKS; ++d) CHECK(engine.active[d] == 0);
        }
    }
    CHECK(a.arguments(NULL, NULL, NULL) == AZ_UNSUPPORTED);
    CHECK(a.begin(NULL, NULL, NULL) == AZ_UNSUPPORTED);
    CHECK(a.dispatch(NULL, NULL, 0, NULL) == AZ_UNSUPPORTED);
    CHECK(pthread_mutex_destroy(&engine.mu) == 0);
    CHECK(azsdk_native_commands_capabilities() == 0);
}

typedef struct Owner {
    pthread_t thread;
    bool any_thread;
    _Atomic(unsigned) checks, touches;
    _Atomic(bool) alive;
    int check_mode;
    struct az_owner_gate *gate;
    uint64_t epoch;
    struct scmd_target view;
} Owner;
static Owner owner_init(bool any) {
    return (Owner) {.thread = pthread_self(), .any_thread = any, .alive = true, .view = target(UINT64_C(1) << AZ_CMD_LOOP_EXIT)};
}
static Owner *owner_new(bool any) {
    Owner *o = malloc(sizeof *o);
    CHECK(o);
    *o = owner_init(any);
    return o;
}
static bool owner_check(void *opaque) {
    Owner *o = opaque;
    CHECK(atomic_load_explicit(&o->alive, memory_order_acquire));
    atomic_fetch_add_explicit(&o->checks, 1, memory_order_relaxed);
    if (o->check_mode == 1) fixture_raise();
    if (o->check_mode == 2) CHECK(az_owner_retire(o->gate, o->epoch) == AZ_BUSY);
    return o->any_thread || pthread_equal(o->thread, pthread_self());
}
static void owner_touch(Owner *o) {
    CHECK(atomic_load_explicit(&o->alive, memory_order_acquire));
    atomic_fetch_add_explicit(&o->touches, 1, memory_order_relaxed);
}
static void *foreign_lease(void *opaque) {
    struct az_owner_lease *lease = opaque;
    CHECK(!az_owner_lease_owner(lease) && !az_owner_lease_generation(lease));
    CHECK(az_owner_lease_end(lease) == AZ_INVALID);
    return NULL;
}
static void *foreign_gate(void *opaque) {
    AZ_OWNER_LEASE(wrong);
    CHECK(az_owner_enter(opaque, &wrong) == AZ_INVALID);
    CHECK(!az_owner_lease_owner(&wrong) && !az_owner_lease_generation(&wrong));
    return NULL;
}
struct WaitContext {
    struct az_owner_gate *gate;
    Owner *owner;
    uint64_t epoch;
    _Atomic(bool) entered, touch, touched, release, retired;
};
static void *held_reader(void *opaque) {
    struct WaitContext *w = opaque;
    AZ_OWNER_LEASE(lease);
    CHECK(az_owner_enter(w->gate, &lease) == AZ_OK);
    CHECK(az_owner_lease_owner(&lease) == w->owner);
    atomic_store_explicit(&w->entered, true, memory_order_release);
    UNTIL(atomic_load_explicit(&w->touch, memory_order_acquire));
    owner_touch(az_owner_lease_owner(&lease));
    atomic_store_explicit(&w->touched, true, memory_order_release);
    UNTIL(atomic_load_explicit(&w->release, memory_order_acquire));
    CHECK(az_owner_lease_end(&lease) == AZ_OK);
    return NULL;
}
static void *held_retirer(void *opaque) {
    struct WaitContext *w = opaque;
    CHECK(az_owner_retire(w->gate, w->epoch) == AZ_OK);
    CHECK(atomic_load_explicit(&w->release, memory_order_acquire));
    atomic_store_explicit(&w->owner->alive, false, memory_order_release);
    free(w->owner);
    atomic_store_explicit(&w->retired, true, memory_order_release);
    return NULL;
}
struct ReaderContext {
    struct az_owner_gate *gate;
    _Atomic(bool) stop;
};
static void *repeated_reader(void *opaque) {
    struct ReaderContext *r = opaque;
    while (!atomic_load_explicit(&r->stop, memory_order_acquire)) {
        AZ_OWNER_LEASE(lease);
        int status = az_owner_enter(r->gate, &lease);
        if (status == AZ_OK) owner_touch(az_owner_lease_owner(&lease));
        else CHECK(status == AZ_BUSY || status == AZ_UNSUPPORTED || status == AZ_STALE);
    }
    return NULL;
}
struct EngineContext {
    struct scmd_engine *engine;
    struct scmd_adapter *adapter;
};
static void *wrong_engine(void *opaque) {
    struct EngineContext *c = opaque;
    CHECK(scmd_step(c->engine, 0, c->adapter) == AZ_INVALID);
    return NULL;
}
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
static void *record_identity(void *opaque) {
    uint64_t *identity = opaque;
    *identity = az_owner_test_thread_token();
    CHECK(*identity && *identity == az_owner_test_thread_token());
    return NULL;
}
static void *last_identity(void *opaque) {
    struct WaitContext *w = opaque;
    AZ_OWNER_LEASE(lease);
    CHECK(az_owner_enter(w->gate, &lease) == AZ_OK && az_owner_lease_owner(&lease) == w->owner);
    CHECK(az_owner_test_thread_token() == UINT64_MAX);
    return NULL;
}
static void *no_identity(void *opaque) {
    AZ_OWNER_LEASE(lease);
    CHECK(az_owner_enter(opaque, &lease) == AZ_BUSY && !az_owner_lease_owner(&lease));
    CHECK(!az_owner_test_thread_token());
    return NULL;
}
#endif
static void lifecycle() {
    AZ_OWNER_GATE(gate);
    Owner one = owner_init(false), two = owner_init(false);
    AZ_OWNER_LEASE(lease);
    AZ_OWNER_LEASE(second);
    uint64_t epoch = UINT64_C(0xa5a5a5a5);
    CHECK(az_owner_generation(&(gate)) == 0);
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_UNSUPPORTED);
    CHECK(az_owner_lease_owner(&(lease)) == NULL && az_owner_lease_generation(&lease) == 0 && az_owner_lease_end(&(lease)) == AZ_INVALID);
    CHECK(az_owner_publish(&(gate), NULL, owner_check, &epoch) == AZ_INVALID && epoch == UINT64_C(0xa5a5a5a5));
    CHECK(az_owner_publish(&(gate), &one, NULL, &epoch) == AZ_INVALID);
    CHECK(az_owner_publish(&(gate), &one, owner_check, NULL) == AZ_INVALID);
    CHECK(az_owner_retire(&(gate), 0) == AZ_INVALID);
    CHECK(az_owner_publish(&(gate), &one, owner_check, &epoch) == AZ_OK && epoch != 0);
    CHECK(az_owner_generation(&(gate)) == epoch);
    uint64_t unchanged = 123;
    CHECK(az_owner_publish(&(gate), &two, owner_check, &unchanged) == AZ_BUSY && unchanged == 123);
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_OK && az_owner_lease_owner(&(lease)) == &one && az_owner_lease_generation(&lease) == epoch);
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_BUSY && az_owner_lease_owner(&(lease)) == &one);
    CHECK(az_owner_enter(&(gate), &(second)) == AZ_BUSY && az_owner_lease_owner(&(second)) == NULL);
    pthread_t foreign;
    CHECK(!pthread_create(&foreign, NULL, foreign_lease, &lease));
    CHECK(!pthread_join(foreign, NULL));
    CHECK(az_owner_lease_owner(&(lease)) == &one);
    CHECK(az_owner_retire(&(gate), epoch + 1) == AZ_STALE);
    CHECK(az_owner_lease_end(&(lease)) == AZ_OK);
    CHECK(az_owner_lease_owner(&(lease)) == NULL && az_owner_lease_generation(&lease) == 0 && az_owner_lease_end(&(lease)) == AZ_INVALID);
    pthread_t wrong_owner;
    CHECK(!pthread_create(&wrong_owner, NULL, foreign_gate, &gate));
    CHECK(!pthread_join(wrong_owner, NULL));
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_OK); /* failed owner check released admission */
    CHECK(az_owner_retire(&(gate), epoch) == AZ_BUSY); /* never wait on our own lease */
    CHECK(az_owner_enter(&(gate), &(second)) == AZ_BUSY);
    CHECK(az_owner_lease_end(&(lease)) == AZ_OK);
    CHECK(az_owner_enter(&(gate), &(second)) == AZ_UNSUPPORTED); /* reentrant close stays closed */
    CHECK(az_owner_retire(&(gate), epoch) == AZ_OK && az_owner_generation(&(gate)) == 0);
    CHECK(az_owner_retire(&(gate), epoch) == AZ_STALE);
    uint64_t replacement = 0;
    CHECK(az_owner_publish(&(gate), &two, owner_check, &replacement) == AZ_OK && replacement > epoch);
    CHECK(az_owner_retire(&(gate), epoch) == AZ_STALE);
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_OK && az_owner_lease_owner(&(lease)) == &two && az_owner_lease_generation(&lease) == replacement);
    CHECK(az_owner_lease_end(&(lease)) == AZ_OK && az_owner_retire(&(gate), replacement) == AZ_OK);
    CHECK(azsdk_native_commands_capabilities() == 0); /* a synthetic gate certifies nothing */
}
static void enter_raising_owner(void *opaque) {
    AZ_OWNER_LEASE(lease);
    (void) az_owner_enter(opaque, &lease);
}
static void predicate_failures() {
    AZ_OWNER_GATE(gate);
    Owner owner = owner_init(false);
    owner.gate = &gate;
    uint64_t epoch = 0;
    CHECK(az_owner_publish(&(gate), &owner, owner_check, &epoch) == AZ_OK);
    owner.epoch = epoch;
    AZ_OWNER_LEASE(lease);
    owner.check_mode = 1;
    CHECK(!fixture_native_try(enter_raising_owner, &gate) && az_owner_lease_owner(&lease) == NULL);
    owner.check_mode = 0;
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_OK); /* exception also releases admission */
    CHECK(az_owner_lease_end(&(lease)) == AZ_OK);
    owner.check_mode = 2;
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_STALE && az_owner_lease_owner(&(lease)) == NULL);
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_UNSUPPORTED);
    CHECK(az_owner_retire(&(gate), epoch) == AZ_OK);
}
static void destructor_release() {
    AZ_OWNER_GATE(gate);
    Owner owner = owner_init(false);
    uint64_t epoch = 0;
    CHECK(az_owner_publish(&(gate), &owner, owner_check, &epoch) == AZ_OK);
    {
        AZ_OWNER_LEASE(lease);
        CHECK(az_owner_enter(&(gate), &(lease)) == AZ_OK);
    }
    CHECK(az_owner_retire(&(gate), epoch) == AZ_OK);
    /* A quiescent gate also retires on its own destruction. */
    AZ_OWNER_GATE(other);
    CHECK(az_owner_publish(&(other), &owner, owner_check, &epoch) == AZ_OK);
}
static void retirement_waits() {
    AZ_OWNER_GATE(gate);
    Owner *owner = owner_new(true);
    uint64_t epoch = 0;
    CHECK(az_owner_publish(&(gate), owner, owner_check, &epoch) == AZ_OK);
    struct WaitContext wait = {.gate = &gate, .owner = owner, .epoch = epoch};
    pthread_t reader, retirer;
    CHECK(!pthread_create(&reader, NULL, held_reader, &wait));
    UNTIL(atomic_load_explicit(&wait.entered, memory_order_acquire));
    CHECK(!pthread_create(&retirer, NULL, held_retirer, &wait));
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    UNTIL(atomic_load_explicit(&(wait.retired), memory_order_acquire) || (__atomic_load_n(&gate.state, __ATOMIC_ACQUIRE) & 4));
    CHECK((__atomic_load_n(&gate.state, __ATOMIC_ACQUIRE) & 4));
#endif
    CHECK(!atomic_load_explicit(&(wait.retired), memory_order_acquire));
    AZ_OWNER_LEASE(cannot_enter);
    CHECK(az_owner_enter(&(gate), &(cannot_enter)) == AZ_BUSY);
    CHECK(!az_owner_lease_owner(&(cannot_enter)));
    atomic_store_explicit(&(wait.touch), true, memory_order_release);
    UNTIL(atomic_load_explicit(&(wait.touched), memory_order_acquire));
    CHECK(!atomic_load_explicit(&(wait.retired), memory_order_acquire));
    atomic_store_explicit(&(wait.release), true, memory_order_release);
    CHECK(!pthread_join(reader, NULL));
    CHECK(!pthread_join(retirer, NULL));
    CHECK(atomic_load_explicit(&(wait.retired), memory_order_acquire) && az_owner_generation(&(gate)) == 0);
    CHECK(az_owner_enter(&(gate), &(cannot_enter)) == AZ_UNSUPPORTED);
}
static void repeated_teardown() {
    AZ_OWNER_GATE(gate);
    struct ReaderContext reading = {.gate = &gate};
    pthread_t readers[4];
    for (unsigned n = 0; n < 4; ++n) CHECK(!pthread_create(&readers[n], NULL, repeated_reader, &reading));
    uint64_t previous = 0;
    for (unsigned n = 0; n < 200; ++n) {
        Owner *owner = owner_new(true);
        uint64_t epoch = 0;
        CHECK(az_owner_publish(&(gate), owner, owner_check, &epoch) == AZ_OK && epoch > previous);
        UNTIL(atomic_load_explicit(&(owner->touches), memory_order_acquire) != 0);
        CHECK(az_owner_retire(&(gate), epoch) == AZ_OK);
        atomic_store_explicit(&(owner->alive), false, memory_order_release);
        free(owner);
        CHECK(az_owner_retire(&(gate), epoch) == AZ_STALE);
        previous = epoch;
    }
    atomic_store_explicit(&(reading.stop), true, memory_order_release);
    for (unsigned n = 0; n < 4; ++n) CHECK(!pthread_join(readers[n], NULL));
}

/* This adapter is entirely synthetic. It exercises pairing with the portable
 * policy; it cannot enable the default native adapter or certify EP147. */
typedef struct FixtureAdapter {
    struct az_owner_gate gate;
    struct az_owner_lease lease;
    unsigned begins, dispatches, ends;
    az_command_request captured;
} FixtureAdapter;
static void fixture_adapter_drop(FixtureAdapter *f) {
    az_owner_lease_drop(&f->lease);
    az_owner_gate_drop(&f->gate);
}
static int fixture_arguments(void *unused_0 __attribute__((unused)), const az_command_request *r, const struct scmd_target *unused_2 __attribute__((unused))) {
    return r->operation == AZ_CMD_LOOP_EXIT && r->quantization == AZ_QUANTIZE_NATIVE && !r->flags ? AZ_OK : AZ_UNSUPPORTED;
}
static int fixture_begin(void *context, const az_command_request *unused_1 __attribute__((unused)), struct scmd_target *out) {
    __auto_type self = &(*((FixtureAdapter *) (context)));
    int rc = az_owner_enter(&(self->gate), &(self->lease));
    if (rc != AZ_OK) return rc;
    ++self->begins;
    *out = ((Owner *) (az_owner_lease_owner(&(self->lease))))->view;
    return AZ_OK;
}
static int fixture_dispatch(void *context, const az_command_request *r, uint64_t token, struct scmd_completion *out) {
    __auto_type self = &(*((FixtureAdapter *) (context)));
    __auto_type owner = ((Owner *) (az_owner_lease_owner(&(self->lease))));
    CHECK(owner && token && owner->view.load_generation == r->expected_load_generation);
    owner_touch(owner);
    ++self->dispatches;
    self->captured = *r;
    *out = (struct scmd_completion) {AZ_COMMAND_APPLIED, AZ_OK, owner->view.load_generation, ++owner->view.revision, 1};
    return AZ_OK;
}
static void fixture_end(void *context, uint32_t deck) {
    __auto_type self = &(*((FixtureAdapter *) (context)));
    CHECK(deck == 0 && az_owner_lease_owner(&(self->lease)));
    ++self->ends;
    CHECK(az_owner_lease_end(&self->lease) == AZ_OK);
}
static struct scmd_adapter fixture_adapter(FixtureAdapter *f) {
    return (struct scmd_adapter) {f, fixture_arguments, fixture_begin, fixture_dispatch, fixture_end};
}
static void engine_pairing() {
    FixtureAdapter fixture __attribute__((cleanup(fixture_adapter_drop))) = {.gate = AZ_OWNER_GATE_INIT};
    Owner owner = owner_init(false);
    uint64_t epoch = 0;
    CHECK(az_owner_publish(&(fixture.gate), &owner, owner_check, &epoch) == AZ_OK);
    __auto_type adapter = fixture_adapter(&fixture);
    struct scmd_engine engine = {0};
    CHECK(pthread_mutex_init(&engine.mu, NULL) == 0);
    uint64_t now = 100;
    engine.clock = clock_now;
    engine.clock_context = &now;
    __auto_type r = request(AZ_CMD_LOOP_EXIT);
    __auto_type before = owner.view;
    az_command_result result = {0};
    result.size = sizeof result;
    CHECK(scmd_submit(&engine, 0, &r, &before, &adapter, &result, NULL) == AZ_OK);
    struct EngineContext context = {&engine, &adapter};
    pthread_t wrong;
    CHECK(!pthread_create(&wrong, NULL, wrong_engine, &context));
    CHECK(!pthread_join(wrong, NULL));
    CHECK(fixture.begins == 0 && fixture.dispatches == 0 && fixture.ends == 0);
    CHECK(scmd_poll(&engine, 0, 1, &result) == AZ_OK && result.outcome == AZ_COMMAND_ACCEPTED);
    CHECK(scmd_step(&engine, 0, &adapter) == AZ_OK);
    CHECK(fixture.begins == 1 && fixture.dispatches == 1 && fixture.ends == 1);
    CHECK(scmd_poll(&engine, 0, 1, &result) == AZ_OK && result.outcome == AZ_COMMAND_APPLIED && result.revision == 8);
    CHECK(scmd_request_equal(&fixture.captured, &r));
    r.request_id = 2;
    r.expected_revision = 8;
    before = owner.view;
    CHECK(scmd_submit(&engine, 0, &r, &before, &adapter, &result, NULL) == AZ_OK);
    ++owner.view.load_generation; /* replacement between acceptance and owner-thread admission */
    CHECK(scmd_step(&engine, 0, &adapter) == AZ_OK);
    CHECK(scmd_poll(&engine, 0, 2, &result) == AZ_OK && result.outcome == AZ_COMMAND_REJECTED && result.reason == AZ_STALE);
    CHECK(fixture.begins == 2 && fixture.dispatches == 1 && fixture.ends == 2);
    r.request_id = 3;
    r.expected_load_generation = owner.view.load_generation;
    before = owner.view;
    CHECK(scmd_submit(&engine, 0, &r, &before, &adapter, &result, NULL) == AZ_OK);
    CHECK(az_owner_retire(&(fixture.gate), epoch) == AZ_OK);
    CHECK(scmd_step(&engine, 0, &adapter) == AZ_OK);
    CHECK(scmd_poll(&engine, 0, 3, &result) == AZ_OK && result.outcome == AZ_COMMAND_REJECTED && result.reason == AZ_UNSUPPORTED);
    CHECK(fixture.begins == 2 && fixture.dispatches == 1 && fixture.ends == 2);
    CHECK(pthread_mutex_destroy(&engine.mu) == 0);
    CHECK(azsdk_native_commands_capabilities() == 0);
}
static void exhaustion() {
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    AZ_OWNER_GATE(gate);
    Owner owner = owner_init(false);
    gate.last_generation = UINT64_MAX - 1;
    uint64_t epoch = 0;
    CHECK(az_owner_publish(&(gate), &owner, owner_check, &epoch) == AZ_OK && epoch == UINT64_MAX);
    CHECK(az_owner_retire(&(gate), epoch) == AZ_OK);
    epoch = 123;
    CHECK(az_owner_publish(&(gate), &owner, owner_check, &epoch) == AZ_LIMIT && epoch == 123);
    CHECK(az_owner_generation(&(gate)) == 0);
    AZ_OWNER_LEASE(lease);
    CHECK(az_owner_enter(&(gate), &(lease)) == AZ_UNSUPPORTED);
#endif
}
static void thread_identity() {
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    uint64_t identities[128];
    for (unsigned n = 0; n < 128; ++n) {
        pthread_t later;
        CHECK(!pthread_create(&later, NULL, record_identity, &identities[n]));
        CHECK(!pthread_join(later, NULL));
        for (unsigned i = 0; i < n; ++i) CHECK(identities[i] != identities[n]);
    }
#endif
}
static void thread_exhaustion() {
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    AZ_OWNER_GATE(gate);
    Owner owner = owner_init(true);
    uint64_t epoch = 0;
    CHECK(az_owner_publish(&(gate), &owner, owner_check, &epoch) == AZ_OK);
    az_owner_test_last_thread_token();
    struct WaitContext context = {.gate = &gate, .owner = &owner};
    pthread_t last, exhausted;
    CHECK(!pthread_create(&last, NULL, last_identity, &context));
    CHECK(!pthread_join(last, NULL));
    CHECK(!pthread_create(&exhausted, NULL, no_identity, &gate));
    CHECK(!pthread_join(exhausted, NULL));
    CHECK(atomic_load_explicit(&(owner.checks), memory_order_relaxed) == 1);
    CHECK(az_owner_retire(&(gate), epoch) == AZ_OK);
#endif
}
int main() {
    unavailable();
    lifecycle();
    predicate_failures();
    destructor_release();
    retirement_waits();
    repeated_teardown();
    engine_pairing();
    exhaustion();
    thread_identity();
    thread_exhaustion();
    puts("PASS: native command boundary, admission, retirement and policy pairing (synthetic)");
}
