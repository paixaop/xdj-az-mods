/* SPDX-License-Identifier: GPL-3.0-only */
/* Explicit native virtual dispatch and layout fixtures in C; never loads EP147. */
#include "load_native_fixture.h"
static unsigned patch_calls, decline_at, partial_at, mismatch_at;
static void *mismatch_original;
static void other_load(void *self, const void *track, const void *option, const void *where, const void *speed, void *listener) {
    ((void (*)(void *, const void *, const void *, const void *, const void *, void *))(mismatch_original))(self, track, option, where, speed, listener);
}
static void other_unload(void *self, const void *option, void *listener) {
    ((void (*)(void *, const void *, void *))(mismatch_original))(self, option, listener);
}
static void other_complete(void *self, const void *id, uint32_t result, const void *where) {
    ((void (*)(void *, const void *, uint32_t, const void *))(mismatch_original))(self, id, result, where);
}
static int installer(uintptr_t slot, void *hook, void **original) {
    CHECK(original && __atomic_load_n(original, __ATOMIC_ACQUIRE));
    void *expected = __atomic_load_n(original, __ATOMIC_ACQUIRE);
    CHECK(__atomic_load_n(((void **) (slot)), __ATOMIC_ACQUIRE) == expected);
    ++patch_calls;
    if (decline_at == patch_calls) return 0;
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0 && !(slot & 7u));
    uintptr_t page = slot & ~(uintptr_t) (page_size - 1);
    CHECK(mprotect(((void *) (page)), ((size_t) (page_size)), PROT_READ | PROT_WRITE) == 0);
    if (mismatch_at == patch_calls) {
        mismatch_original = expected;
        void *other = mismatch_at == 1 ? ((void *) (other_load)) : mismatch_at == 4 ? ((void *) (other_complete))
                                                                                    : ((void *) (other_unload));
        __atomic_store_n(((void **) (slot)), other, __ATOMIC_RELEASE);
        CHECK(__atomic_load_n(((void **) (slot)), __ATOMIC_ACQUIRE) != expected);
        return 0; /* parent's guarded installer declines a competing slot change */
    }
    /* Fixture owns this process. Leave only its vtable data page writable. */
    __atomic_store_n(((void **) (slot)), hook, __ATOMIC_RELEASE);
    return partial_at == patch_calls ? 0 : 1;
}
static void unavailable(int deck) {
    struct azsdk_load_sample value, before;
    memset(&value, 0xa5, sizeof value);
    before = value;
    CHECK(!azsdk_load_read(deck, &value));
    CHECK(memcmp(&value, &before, sizeof value) == 0);
}
static struct azsdk_load_sample sample(int deck) {
    struct azsdk_load_sample value;
    CHECK(azsdk_load_read(deck, &value));
    CHECK(value.unit_generation && value.load_generation && value.invalidation_epoch && value.observed_ns && !value.reserved);
    return value;
}
static uintptr_t word(uintptr_t p) {
    uintptr_t value;
    memcpy(&value, ((void *) (p)), sizeof value);
    return value;
}
static void native_profile(int build) {
    Fixture f;
    fixture_init(&f);
    __auto_type primary = word(((uintptr_t) (&f.facade)));
    __auto_type secondary = word(((uintptr_t) (unload_interface(&f.facade))));
    __auto_type completed = word(((uintptr_t) (listener_interface(&f.controller))));
    azsdk_load_test_profile(build, ((void *) (word(primary + 16))), ((void *) (word(primary + 24))),
                            ((void *) (word(secondary + 16))), ((void *) (word(completed + 16))));
}
typedef struct {
    const void *const *vtable;
} ShortType;
static const char short_name[] = "N";
static const uintptr_t fake_type[] = {0, ((uintptr_t) (short_name))};
static uintptr_t fake_table[] = {0, ((uintptr_t) (fake_type)), 0, 0};
static void nominal_guards() {
    ShortType short_object = {short_table.slots};
    CHECK(!azsdk_load_register(0, &short_object)); /* no read at object+0x190 */
    uintptr_t fake_object = ((uintptr_t) (fake_table + 2));
    CHECK(!azsdk_load_register(0, &fake_object)); /* name's first NUL is inside ASan's redzone boundary */
    CHECK(!azsdk_load_register(-1, &short_object));
    CHECK(!azsdk_load_register(4, &short_object));
    CHECK(!azsdk_load_register(0, NULL));
}
static _Atomic(bool) publish_entered = false, publish_release = false;
static void pause_publish(int deck) {
    CHECK(deck == 0);
    atomic_store_explicit(&(publish_entered), true, memory_order_release);
    until(&publish_release);
}
struct fixture_work {
    Fixture *fixture;
    unsigned id;
    bool start;
};
static void *fixture_worker(void *opaque) {
    struct fixture_work *work = opaque;
    if (work->start) start(work->fixture, work->id, NULL);
    else finish(work->fixture, work->id, 0);
    return NULL;
}
struct reader_work {
    struct azsdk_load_sample baseline;
    _Atomic(bool) done;
};
static void *reader_worker(void *opaque) {
    struct reader_work *context = opaque;
    while (!atomic_load_explicit(&context->done, memory_order_acquire)) {
        struct azsdk_load_sample s;
        if (azsdk_load_read(0, &s)) {
            CHECK(s.unit_generation == context->baseline.unit_generation && s.native_request_id >= 100);
            CHECK(s.load_generation - context->baseline.load_generation == s.native_request_id - 100);
            CHECK(s.invalidation_epoch - context->baseline.invalidation_epoch == s.native_request_id - 100);
        }
    }
    return NULL;
}
struct configure_work {
    int on, build;
};
static void *configure_worker(void *opaque) {
    struct configure_work *work = opaque;
    azsdk_load_configure(work->on, work->build, installer);
    return NULL;
}
static void epoch_race(Fixture *f, unsigned id, bool retirement) {
    start(f, id, NULL);
    atomic_store_explicit(&(publish_entered), false, memory_order_release);
    atomic_store_explicit(&(publish_release), false, memory_order_release);
    azsdk_load_test_before_publish(pause_publish);
    struct fixture_work work = {f, id, false};
    pthread_t old;
    CHECK(!pthread_create(&old, NULL, fixture_worker, &work));
    until(&publish_entered);
    if (retirement) azsdk_load_retire(0);
    else eject(f, id + 1, false);
    unavailable(0);
    atomic_store_explicit(&(publish_release), true, memory_order_release);
    CHECK(!pthread_join(old, NULL));
    azsdk_load_test_before_publish(NULL);
    unavailable(0);
}
static void coherent_reads(Fixture *f) {
    start(f, 100, NULL);
    finish(f, 100, 0);
    __auto_type baseline = sample(0);
    struct reader_work work = {.baseline = baseline};
    pthread_t readers[3];
    for (unsigned i = 0; i < 3; ++i) CHECK(!pthread_create(&readers[i], NULL, reader_worker, &work));
    for (unsigned id = 101; id <= 1100; ++id) {
        start(f, id, NULL);
        finish(f, id, 0);
    }
    atomic_store_explicit(&work.done, true, memory_order_release);
    for (unsigned i = 0; i < 3; ++i) CHECK(!pthread_join(readers[i], NULL));
    CHECK(sample(0).native_request_id == 1100);
}
static void happy(int build) {
    azsdk_load_configure(1, build, installer);
    nominal_guards();
    Fixture f;
    fixture_init(&f);
    CHECK(azsdk_load_register(0, &f.unit));
    CHECK(!azsdk_load_register(0, &f.unit));
    CHECK(!azsdk_load_register(1, &f.unit));
    unavailable(0);
    atomic_store_explicit(&(f.facade.automatic), 1, memory_order_release);
    atomic_store_explicit(&(f.facade.expect_inline_sample), 1, memory_order_release);
    start(&f, 0, NULL); /* native ID0 is a valid first value, and receipt precedes load return */
    __auto_type first = sample(0);
    CHECK(first.native_request_id == 0 && f.facade.loads == 1 && f.controller.callbacks == 1);
    atomic_store_explicit(&(f.facade.automatic), 0, memory_order_release);
    atomic_store_explicit(&(f.facade.expect_inline_sample), 0, memory_order_release);
    start(&f, 1, NULL);
    unavailable(0);
    start(&f, 2, NULL);
    unavailable(0);
    finish(&f, 1, 0);
    unavailable(0); /* late A cannot complete B */
    finish(&f, 2, 0);
    __auto_type second = sample(0);
    CHECK(second.native_request_id == 2 && second.load_generation > first.load_generation);
    finish(&f, 2, 0);
    CHECK(sample(0).load_generation == second.load_generation); /* duplicate */
    finish(&f, 1, 0);
    CHECK(sample(0).load_generation == second.load_generation); /* old receipt after B */
    start(&f, 3, NULL);
    finish(&f, 3, 1);
    unavailable(0);
    start(&f, 4, NULL);
    finish(&f, 4, 4);
    unavailable(0);
    start(&f, 5, NULL);
    finish(&f, 5, 0);
    __auto_type third = sample(0);
    CHECK(third.load_generation > second.load_generation); /* same opaque track arguments throughout */
    start(&f, 5, NULL);
    finish(&f, 5, 0);
    unavailable(0); /* reused */
    start(&f, 4, NULL);
    finish(&f, 4, 0);
    unavailable(0); /* backward */
    start(&f, 6, NULL);
    finish(&f, 6, 0);
    sample(0);
    eject(&f, 7, false);
    unavailable(0); /* secondary interface clone */
    start(&f, 8, NULL);
    finish(&f, 8, 0);
    sample(0);
    eject(&f, 9, true);
    unavailable(0); /* primary complete-object implementation */
    void *wrong_interface = stock_interface(&f.controller);
    start(&f, 10, wrong_interface);
    finish(&f, 10, 0);
    unavailable(0);
    LoadController unrelated;
    controller_init(&unrelated);
    unrelated.where_expected = &f.where;
    start(&f, 11, NULL);
    emit(&unrelated, 11, 0, &f.where);
    unavailable(0);
    finish(&f, 11, 0);
    sample(0);
    azsdk_load_test_hold(0, 1);
    start(&f, 12, NULL); /* invalidates and records ID despite lost capture */
    unavailable(0);
    azsdk_load_test_hold(0, 0);
    finish(&f, 12, 0);
    unavailable(0);
    start(&f, 12, NULL);
    finish(&f, 12, 0);
    unavailable(0); /* contended IDs cannot later be reused */
    start(&f, 13, NULL);
    azsdk_load_test_hold(0, 1);
    finish(&f, 13, 0); /* a lost current completion invalidates */
    azsdk_load_test_hold(0, 0);
    finish(&f, 13, 0);
    unavailable(0);
    epoch_race(&f, 14, false);
    epoch_race(&f, 16, true);
    CHECK(azsdk_load_register(0, &f.unit)); /* same addresses, different registration */
    finish(&f, 16, 0);
    unavailable(0);
    start(&f, 16, NULL);
    finish(&f, 16, 0);
    unavailable(0); /* history survives retirement */
    start(&f, 17, NULL);
    finish(&f, 17, 0);
    __auto_type replacement = sample(0);
    CHECK(replacement.unit_generation > first.unit_generation && replacement.load_generation > third.load_generation);
    azsdk_load_configure(0, build, installer);
    unavailable(0);
    unsigned forwarded = atomic_load_explicit(&(f.facade.loads), memory_order_seq_cst);
    start(&f, 18, NULL);
    finish(&f, 18, 0);
    CHECK(f.facade.loads == forwarded + 1);
    unavailable(0); /* disabled hooks forward */
    azsdk_load_configure(1, build, installer);
    CHECK(azsdk_load_register(0, &f.unit));
    start(&f, 17, NULL);
    finish(&f, 17, 0);
    unavailable(0); /* configuration never resets history */
    start(&f, 19, NULL);
    finish(&f, 19, 0);
    sample(0);
    coherent_reads(&f);
    Fixture other;
    fixture_init(&other);
    CHECK(azsdk_load_register(1, &other.unit));
    start(&other, 2, NULL);
    finish(&other, 2, 0);
    CHECK(sample(1).native_request_id == 2);
    start(&f, UINT32_MAX, NULL);
    finish(&f, UINT32_MAX, 0);
    CHECK(sample(0).native_request_id == UINT32_MAX);
    start(&f, 0, NULL);
    finish(&f, 0, 0);
    unavailable(0); /* wrapped */
    azsdk_load_retire(0);
    CHECK(azsdk_load_register(0, &f.unit));
    start(&f, 1, NULL);
    finish(&f, 1, 0);
    unavailable(0); /* wrap quarantine survives retirement */
    azsdk_load_test_exhaust(1, 1);
    start(&other, 3, NULL);
    finish(&other, 3, 0);
    unavailable(1);
    azsdk_load_retire(1);
    CHECK(!azsdk_load_register(1, &other.unit));
    Fixture sequence_exhausted;
    fixture_init(&sequence_exhausted);
    CHECK(azsdk_load_register(3, &sequence_exhausted.unit));
    azsdk_load_test_exhaust(3, 3);
    start(&sequence_exhausted, 0, NULL);
    finish(&sequence_exhausted, 0, 0);
    unavailable(3);
    azsdk_load_retire(3);
    Fixture exhausted;
    fixture_init(&exhausted);
    CHECK(azsdk_load_register(2, &exhausted.unit));
    azsdk_load_test_exhaust(2, 2);
    start(&exhausted, 1, NULL);
    finish(&exhausted, 1, 0);
    unavailable(2);
    azsdk_load_retire(0);
    azsdk_load_retire(2);
    azsdk_load_test_exhaust(0, 4);
    CHECK(!azsdk_load_register(0, &f.unit));
    azsdk_load_configure(1, 99, installer);
    CHECK(!azsdk_load_register(0, &f.unit));
    unavailable(0);
    unavailable(-1);
    unavailable(4);
    CHECK(!azsdk_load_read(0, NULL));
}
static void declined(int build, const char *mode) {
    unsigned number = ((unsigned) (mode[strlen(mode) - 1] - '0'));
    if (strncmp(mode, "decline", 7) == 0) decline_at = number;
    else if (strncmp(mode, "partial", 7) == 0) partial_at = number;
    else mismatch_at = number;
    azsdk_load_configure(1, build, installer);
    Fixture f;
    fixture_init(&f);
    int registered = azsdk_load_register(0, &f.unit);
    CHECK(registered == (number == 4));
    start(&f, 1, NULL);
    finish(&f, 1, 0);
    CHECK(f.facade.loads == 1 && f.controller.callbacks == 1);
    unavailable(0);
    eject(&f, 2, false);
    eject(&f, 3, true);
    CHECK(f.facade.unloads == 2);
    unavailable(0);
    if (partial_at) {
        azsdk_load_retire(0);
        if (number < 4) CHECK(!azsdk_load_register(0, &f.unit));
        else CHECK(azsdk_load_register(0, &f.unit));
        start(&f, 4, NULL);
        finish(&f, 4, 0);
        unavailable(0); /* unacknowledged partial remains forward-only */
    }
}
static void shared(int build, bool concurrent) {
    azsdk_load_configure(1, build, installer);
    Fixture first, second;
    fixture_init(&first);
    fixture_init(&second);
    first.controller.where_expected = NULL;
    CHECK(azsdk_load_register(0, &first.unit) && azsdk_load_register(1, &second.unit));
    atomic_store_explicit(&(second.facade.automatic), 1, memory_order_release);
    if (concurrent) {
        atomic_store_explicit(&(publish_entered), false, memory_order_release);
        atomic_store_explicit(&(publish_release), false, memory_order_release);
        azsdk_load_test_before_association(pause_publish);
        struct fixture_work work = {&first, 1, true};
        pthread_t active;
        CHECK(!pthread_create(&active, NULL, fixture_worker, &work));
        until(&publish_entered);
        start(&second, 1, listener_interface(&first.controller));
        atomic_store_explicit(&(publish_release), true, memory_order_release);
        CHECK(!pthread_join(active, NULL));
        azsdk_load_test_before_association(NULL);
    } else {
        start(&first, 1, NULL);
        start(&second, 1, listener_interface(&first.controller)); /* before-return callback has same ID */
    }
    unavailable(0);
    unavailable(1);
    finish(&first, 1, 0);
    finish(&first, 2, 0);
    unavailable(0);
    start(&second, 2, listener_interface(&first.controller));
    finish(&first, 2, 0);
    unavailable(0);
    unavailable(1);
    CHECK(first.facade.loads == 1 && second.facade.loads == 2);
    azsdk_load_retire(0);
    azsdk_load_retire(1);
    CHECK(!azsdk_load_register(0, &first.unit) && !azsdk_load_register(1, &second.unit));
}
typedef LoadRequestID OtherRequest;
static void unknown_attempt(int build, bool is_unload) {
    azsdk_load_configure(1, build, installer);
    Fixture f;
    fixture_init(&f);
    CHECK(azsdk_load_register(0, &f.unit));
    start(&f, 1, NULL);
    finish(&f, 1, 0);
    sample(0);
    OtherRequest unknown = {other_table.slots, 2};
    __auto_type option = &(*((const TrackLoadOption *) (&unknown)));
    if (is_unload) unload(&f.facade, (*option), &f.speed);
    else load(&f.facade, (*option), &f.track, &f.where, &f.speed, listener_interface(&f.controller));
    unavailable(0);
    start(&f, 2, NULL);
    finish(&f, 2, 0);
    unavailable(0);
    azsdk_load_retire(0);
    CHECK(!azsdk_load_register(0, &f.unit));
}
static _Atomic(bool) enabling = false, disabling = false, enable_release = false;
static void configuration_pause(int stage, int on) {
    if (stage == 1 && on) {
        atomic_store_explicit(&(enabling), true, memory_order_release);
        until(&enable_release);
    }
    if (stage == 0 && !on) atomic_store_explicit(&(disabling), true, memory_order_release);
}
static void configure_race(int build) {
    azsdk_load_configure(1, build, installer);
    Fixture f;
    fixture_init(&f);
    azsdk_load_test_configure_hook(configuration_pause);
    struct configure_work enable_work = {1, build};
    pthread_t enable;
    CHECK(!pthread_create(&enable, NULL, configure_worker, &enable_work));
    until(&enabling);
    struct configure_work disable_work = {0, build};
    pthread_t disable;
    CHECK(!pthread_create(&disable, NULL, configure_worker, &disable_work));
    until(&disabling);
    atomic_store_explicit(&(enable_release), true, memory_order_release);
    CHECK(!pthread_join(enable, NULL));
    CHECK(!pthread_join(disable, NULL));
    azsdk_load_test_configure_hook(NULL);
    CHECK(!azsdk_load_register(0, &f.unit));
    unavailable(0);
}
static void wrong_function(int build, unsigned which) {
    azsdk_load_configure(1, build, installer);
    Fixture f;
    fixture_init(&f);
    uintptr_t slot = 0;
    if (which <= 2) slot = word(((uintptr_t) (&f.facade))) + (which == 1 ? 16 : 24);
    else if (which == 3) slot = word(((uintptr_t) (unload_interface(&f.facade)))) + 16;
    else slot = word(((uintptr_t) (listener_interface(&f.controller)))) + 16;
    __auto_type expected = ((void *) (word(slot)));
    long page_size = sysconf(_SC_PAGESIZE);
    CHECK(mprotect(((void *) (slot & ~(uintptr_t) (page_size - 1))), page_size, PROT_READ | PROT_WRITE) == 0);
    __atomic_store_n(((void **) (slot)), ((void *) (other_load)), __ATOMIC_RELEASE);
    int registered = azsdk_load_register(0, &f.unit);
    CHECK(registered == (which == 4));
    if (which == 4) {
        start(&f, 1, NULL);
        CHECK(patch_calls == 3);
    }
    __atomic_store_n(((void **) (slot)), expected, __ATOMIC_RELEASE);
    if (which == 4) {
        finish(&f, 1, 0);
        unavailable(0);
    }
}
static void capacity(int build) {
    azsdk_load_configure(1, build, installer);
    Fixture f;
    fixture_init(&f);
    LoadController listeners[17];
    for (unsigned i = 0; i < 17; ++i) controller_init(&listeners[i]);
    CHECK(azsdk_load_register(0, &f.unit));
    for (unsigned n = 0; n < 17; ++n) {
        listeners[n].where_expected = &f.where;
        start(&f, n, listener_interface(&listeners[n]));
        emit(&listeners[n], n, 0, &f.where);
        if (n < 16) CHECK(sample(0).native_request_id == n);
        else unavailable(0);
    }
    azsdk_load_retire(0);
    CHECK(!azsdk_load_register(0, &f.unit));
}
#ifdef AZSDK_LOAD_PUBLIC_TEST
void sdk_test_public_load(int deck, const struct azsdk_load_sample *expected);
void sdk_test_public_load_optout(int deck);
static void public_load(int build) {
    azsdk_load_configure(1, build, installer);
    Fixture f[4];
    for (unsigned i = 0; i < 4; ++i) fixture_init(&f[i]);
    struct azsdk_load_sample current[4];
    for (int d = 0; d < 4; ++d) {
        CHECK(azsdk_load_register(d, &f[d].unit));
        sdk_test_public_load(d, NULL);
        start(&f[d], 1, NULL);
        sdk_test_public_load(d, NULL);
        finish(&f[d], 1, 0);
        current[d] = sample(d);
        sdk_test_public_load(d, &current[d]);
    }
    for (int d = 0; d < 4; ++d) {
        __auto_type previous = current[d];
        start(&f[d], 2, NULL); /* Same native track argument, new request. */
        sdk_test_public_load(d, NULL);
        finish(&f[d], 1, 0); /* Old completion cannot commit the reload. */
        sdk_test_public_load(d, NULL);
        for (int other = 0; other < 4; ++other)
            if (other != d) sdk_test_public_load(other, &current[other]);
        finish(&f[d], 2, 0);
        current[d] = sample(d);
        CHECK(current[d].load_generation > previous.load_generation &&
              current[d].unit_generation == previous.unit_generation &&
              current[d].invalidation_epoch > previous.invalidation_epoch);
        sdk_test_public_load(d, &current[d]);
        sdk_test_public_load_optout(d);
    }
    for (int d = 0; d < 4; ++d) {
        start(&f[d], 3, NULL);
        finish(&f[d], 3, 1);
        sdk_test_public_load(d, NULL);
        start(&f[d], 4, NULL);
        finish(&f[d], 4, 0);
        current[d] = sample(d);
        sdk_test_public_load(d, &current[d]);
        eject(&f[d], 5, false);
        sdk_test_public_load(d, NULL);
        finish(&f[d], 4, 0);
        sdk_test_public_load(d, NULL);
        azsdk_load_retire(d);
        sdk_test_public_load(d, NULL);
        CHECK(azsdk_load_register(d, &f[d].unit));
        start(&f[d], 6, NULL);
        finish(&f[d], 6, 0);
        __auto_type replacement = sample(d);
        CHECK(replacement.unit_generation > current[d].unit_generation &&
              replacement.load_generation > current[d].load_generation);
        sdk_test_public_load(d, &replacement);
        azsdk_load_retire(d);
        sdk_test_public_load(d, NULL);
    }
}
#endif
int main(int argc, char **argv) {
    CHECK(argc == 3);
    int build = atoi(argv[1]);
    CHECK(build == 1 || build == 2);
    native_profile(build);
#ifdef AZSDK_LOAD_PUBLIC_TEST
    if (strcmp(argv[2], "public") == 0) public_load(build);
    else
#endif
        if (strcmp(argv[2], "happy") == 0)
        happy(build);
    else if (strcmp(argv[2], "shared") == 0) shared(build, false);
    else if (strcmp(argv[2], "shared-concurrent") == 0) shared(build, true);
    else if (strcmp(argv[2], "unknown") == 0) unknown_attempt(build, false);
    else if (strcmp(argv[2], "unknown-unload") == 0) unknown_attempt(build, true);
    else if (strcmp(argv[2], "configure") == 0) configure_race(build);
    else if (strcmp(argv[2], "capacity") == 0) capacity(build);
    else if (strncmp(argv[2], "wrong", 5) == 0) wrong_function(build, (unsigned) (argv[2][5] - '0'));
    else declined(build, argv[2]);
    printf("PASS load: build%d %s (synthetic native virtual dispatch)\n", build, argv[2]);
    return 0;
}
