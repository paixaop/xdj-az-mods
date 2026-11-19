/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic GNU ELF/weak ownership model in C; no device is executed. */
#include "azliveobserver.h"
#include "aztimestamp.h"
#include "live_native_fixture.h"
#include <pthread.h>
#ifdef AZ_STATUS_NATIVE_PRODUCER
#include "azstatus_native.h"
/* The producer boundary consumes copied receipts; ownership and scalar reads
 * below still run through the actual GNU listener and production observer. */
static _Atomic unsigned native_play_receipts, native_retirements;
void azn_emit(struct azstatus_native_receipt *receipt) {
    CHECK(receipt->kind == AZN_PLAY && receipt->deck < 4 && receipt->unit);
    atomic_fetch_add(&native_play_receipts, 1);
}
void azn_raw_state(unsigned deck, uint64_t generation, int retired) {
    CHECK(deck < 4 && generation && (retired == 0 || retired == 1));
    if (retired) atomic_fetch_add(&native_retirements, 1);
}
static void native_sync_seed(void) {
    for (int build = 1; build <= 2; ++build) {
        Fixture f __attribute__((cleanup(fixture_finish)));
        fixture_init(&f);
        azsdk_live_configure(1, build);
        azsdk_live_status_start(build, 1);
        uintptr_t unit = (uintptr_t) &f.unit;
        int raw = -7;
        CHECK(!azn_sync_seed(4, unit, &raw));
        CHECK(!azn_sync_seed(0, 0, &raw));
        CHECK(!azn_sync_seed(0, unit, NULL));
        CHECK(!azn_sync_seed(0, unit, &raw) && raw == -7);
        CHECK(azsdk_live_register(0, &f.unit));
        CHECK(!azn_sync_seed(0, unit, &raw) && raw == -7);
        emit_fixture_scalar(&f, 3, 2, 0);
        CHECK(azn_sync_seed(0, unit, &raw) && raw == 2);
        CHECK(!azn_sync_seed(0, unit + 8, &raw) && raw == 2);
        emit_fixture_scalar(&f, 3, 1, 0);
        CHECK(!azn_sync_seed(0, unit, &raw) && raw == 2);
        emit_fixture_scalar(&f, 3, 0, 0);
        CHECK(azn_sync_seed(0, unit, &raw) && raw == 0);
        unsigned before = atomic_load(&native_retirements);
        azsdk_live_retire(0);
        CHECK(atomic_load(&native_retirements) == before + 1);
        CHECK(!azn_sync_seed(0, unit, &raw) && raw == 0);
        CHECK(azsdk_live_register(0, &f.unit));
        CHECK(!azn_sync_seed(0, unit, &raw) && raw == 0);
        emit_fixture_scalar(&f, 3, 2, 0);
        CHECK(azn_sync_seed(0, unit, &raw) && raw == 2);
        azsdk_live_retire(0);
    }
    CHECK(atomic_load(&native_play_receipts) >= 4);
}
#endif
static void unchanged(int d) {
    struct azsdk_live_sample out, before;
    memset(&out, 0xa5, sizeof out);
    before = out;
    CHECK(!azsdk_live_read(d, &out));
    CHECK(!memcmp(&out, &before, sizeof out));
}
static void sync_unchanged(int d) {
    struct azsdk_sync_sample out, before;
    memset(&out, 0xa5, sizeof out);
    before = out;
    CHECK(!azsdk_sync_read(d, &out));
    CHECK(!memcmp(&out, &before, sizeof out));
}
static struct azsdk_sync_sample sync_read(int d) {
    struct Guard {
        uint64_t pre;
        struct azsdk_sync_sample value;
        uint64_t post;
    } out = {0};
    out.pre = UINT64_C(0x123456789abcdef0);
    out.post = UINT64_C(0x0fedcba987654321);
    CHECK(azsdk_sync_read(d, &out.value));
    CHECK(out.pre == UINT64_C(0x123456789abcdef0) && out.post == UINT64_C(0x0fedcba987654321));
    CHECK(out.value.unit_generation && out.value.revision && out.value.observed_ns && out.value.valid <= 1);
    CHECK(!out.value.valid || out.value.raw_mode == 0 || out.value.raw_mode == 2);
    return out.value;
}
static struct azsdk_live_sample read(int d) {
    struct Guard {
        uint64_t pre;
        struct azsdk_live_sample value;
        uint64_t post;
    } out = {0};
    out.pre = UINT64_C(0x123456789abcdef0);
    out.post = UINT64_C(0x0fedcba987654321);
    CHECK(azsdk_live_read(d, &out.value));
    CHECK(out.pre == UINT64_C(0x123456789abcdef0) && out.post == UINT64_C(0x0fedcba987654321));
    CHECK(out.value.unit_generation && out.value.observed_fields && !out.value.reserved);
    CHECK(!(out.value.valid_fields & ~out.value.observed_fields));
    for (unsigned s = 0; s < 3; ++s) {
        bool observed = out.value.observed_fields & (1u << s);
        CHECK((out.value.revisions[s] != 0) == observed);
        CHECK((out.value.observed_ns[s] != 0) == observed);
    }
    return out.value;
}
static void validation() {
    Fixture f __attribute__((cleanup(fixture_finish)));
    fixture_init(&f);
    unchanged(-1);
    unchanged(4);
    unchanged(0);
    CHECK(!azsdk_live_read(0, NULL));
    sync_unchanged(-1);
    sync_unchanged(4);
    sync_unchanged(0);
    CHECK(!azsdk_sync_read(0, NULL));
    CHECK(!azsdk_live_register(-1, &f.unit) && !azsdk_live_register(4, &f.unit));
    {
        const int values[] = {-1, 0, 3};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            int build = values[index];
            azsdk_live_configure(1, build);
            CHECK(!azsdk_live_capabilities() && !azsdk_live_register(0, &f.unit));
            sync_unchanged(0);
        }
    }
    azsdk_live_configure(0, 2);
    CHECK(!azsdk_live_register(0, &f.unit));
    azsdk_live_configure(1, 2);
    CHECK(!azsdk_live_register(0, NULL));
    Unit empty = unit_create(NULL);
    CHECK(!azsdk_live_register(0, &empty));
    struct {
        const void *const *vtable;
    } wrong = {wrong_table.slots};
    Unit wrong_publisher = unit_create((struct Publisher *) &wrong);
    CHECK(!azsdk_live_register(0, &wrong_publisher));
    f.publisher.server = ((struct Server *) (&wrong));
    CHECK(!azsdk_live_register(0, &f.unit));
    f.publisher.server = NULL;
    CHECK(!azsdk_live_register(0, &f.unit));
    f.publisher.server = &f.server;
    /* The copied table is in this ELF, but one purported method is ordinary RW data. */
    _Alignas(uintptr_t) static uintptr_t bad_table[28];
    __auto_type original = vtable(&f.server);
    memcpy(bad_table, ((const void *) (original - 16)), sizeof bad_table);
    bad_table[2 + 19] = ((uintptr_t) (bad_table));
    uintptr_t forged = ((uintptr_t) (bad_table + 2));
    ((uintptr_t *) (&f.server))[0] = forged;
    CHECK(!azsdk_live_register(0, &f.unit));
    ((uintptr_t *) (&f.server))[0] = original;
    CHECK(azsdk_live_register(0, &f.unit));
    CHECK(azsdk_live_capabilities() == 1 && !azsdk_live_register(0, &f.unit));
    for (unsigned s = 0; s < 4; ++s) CHECK(f.server.count[s] == 1);
    unchanged(0); // Registration does not manufacture an initial receipt.
    Lease p __attribute__((cleanup(fixture_drop))) = get_lease(&f, 0);
    Play n = {{1, 1, 1}};
    dispatch(p, &n, 0, false);
    unchanged(0);
    dispatch(p, &n, 1, true);
    unchanged(0);
    azsdk_live_retire(-1);
    azsdk_live_retire(4);
    CHECK(azsdk_live_capabilities() == 1);
    azsdk_live_retire(0);
    CHECK(!azsdk_live_capabilities());
    dispatch(p, &n, 0, true);
    unchanged(0);
    fixture_drop(&p);
    for (unsigned s = 0; s < 4; ++s) CHECK(fixture_expired(f.server.listeners[s][0]));
}
static void components_and_lifetime() {
    Fixture f __attribute__((cleanup(fixture_finish)));
    fixture_init(&f);
    azsdk_live_configure(1, 1);
    CHECK(azsdk_live_register(0, &f.unit));
    Lease p __attribute__((cleanup(fixture_drop))) = get_lease(&f, 0);
    Lease q __attribute__((cleanup(fixture_drop))) = get_lease(&f, 1);
    Lease k __attribute__((cleanup(fixture_drop))) = get_lease(&f, 2);
    emit_play(p, 0, 0, 0, 0);
    __auto_type first = read(0);
    CHECK(first.observed_fields == AZSDK_LIVE_PLAY && first.valid_fields == AZSDK_LIVE_PLAY);
    CHECK(!first.mode_is_2 && !first.play_pause && !first.play_aux && first.quantize == -1 && first.key_lock == -1);
    for (unsigned a = 0; a < 2; ++a)
        for (unsigned b = 0; b < 2; ++b)
            for (unsigned c = 0; c < 2; ++c) {
                emit_play(p, a, b, c, 0);
                __auto_type out = read(0);
                CHECK(out.mode_is_2 == ((int) (a)) && out.play_pause == ((int) (b)) && out.play_aux == ((int) (c)));
            }
    __auto_type before = read(0);
    emit_scalar(q, 0, 0);
    __auto_type quantize = read(0);
    CHECK(quantize.observed_fields == 3 && quantize.valid_fields == 3 && quantize.quantize == 0);
    CHECK(quantize.revisions[0] == before.revisions[0] && compare_timestamps(quantize.observed_ns[0], before.observed_ns[0], 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    emit_scalar(k, 1, 0);
    __auto_type key = read(0);
    CHECK(key.observed_fields == 7 && key.valid_fields == 7 && key.key_lock == 1);
    CHECK(key.revisions[0] == before.revisions[0] && key.revisions[1] == quantize.revisions[1]);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned a = i == 0 ? 2 : 1, b = i == 1 ? 255 : 1, c = i == 2 ? 2 : 1;
        __auto_type old = read(0);
        emit_play(p, a, b, c, 0);
        __auto_type out = read(0);
        CHECK(out.observed_fields == 7 && out.valid_fields == 6);
        CHECK(out.mode_is_2 == -1 && out.play_pause == -1 && out.play_aux == -1);
        CHECK(out.quantize == 0 && out.key_lock == 1 && out.revisions[0] > old.revisions[0]);
        CHECK(out.observed_ns[0] >= old.observed_ns[0] && out.revisions[1] == old.revisions[1]);
    }
    {
        const uint32_t values[] = {2u, 256u, UINT32_MAX};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            uint32_t invalid = values[index];
            emit_scalar(q, invalid, 0);
            emit_scalar(k, invalid, 0);
            __auto_type out = read(0);
            CHECK(out.observed_fields == 7 && !out.valid_fields && out.quantize == -1 && out.key_lock == -1);
            emit_scalar(q, 0, 0);
            emit_scalar(k, 1, 0);
        }
    }
    emit_play(p, 1, 0, 1, 0);
    emit_scalar(q, 1, 0);
    emit_scalar(k, 0, 0);
    __auto_type old = read(0);
    CHECK(old.valid_fields == 7 && old.quantize == 1 && !old.key_lock);
    azsdk_live_retire(0);
    emit_play(p, 1, 1, 1, 0);
    emit_scalar(q, 1, 0);
    emit_scalar(k, 1, 0);
    unchanged(0);
    Fixture next __attribute__((cleanup(fixture_finish)));
    fixture_init(&next);
    CHECK(azsdk_live_register(0, &next.unit));
    unchanged(0);
    emit_play(p, 1, 1, 1, 0);
    emit_scalar(q, 1, 0);
    emit_scalar(k, 1, 0);
    unchanged(0);
    emit_fixture_scalar(&next, 1, 0, 0);
    __auto_type current = read(0);
    CHECK(current.unit_generation != old.unit_generation && current.observed_fields == 2 && current.quantize == 0);
    Lease next_play __attribute__((cleanup(fixture_drop))) = get_lease(&next, 0);
    Lease held __attribute__((cleanup(fixture_drop))) = get_lease(&next, 1);
    Lease next_key __attribute__((cleanup(fixture_drop))) = get_lease(&next, 2);
    emit_play(next_play, 1, 0, 0, 0);
    emit_scalar(next_key, 0, 0);
    __auto_type coherent = read(0);
    emit_play(p, 1, 1, 1, 0);
    emit_scalar(q, 1, 0);
    emit_scalar(k, 1, 0);
    __auto_type preserved = read(0);
    CHECK(preserved.valid_fields == coherent.valid_fields && preserved.unit_generation == coherent.unit_generation);
    for (unsigned s = 0; s < 3; ++s) CHECK(preserved.revisions[s] == coherent.revisions[s]);
    azsdk_live_configure(1, 2);
    emit_scalar(held, 1, 0);
    unchanged(0);
}
static void failure_rollback() {
    for (int stream = 0; stream < 5; ++stream)
        for (int after = 0; after < 2; ++after) {
            Fixture f __attribute__((cleanup(fixture_finish)));
            fixture_init(&f);
            azsdk_live_configure(1, 2);
            f.server.fail_stream = stream;
            f.server.fail_after = after;
            CHECK(fixture_registration_unwind(azsdk_live_register, 0, &f.unit));
            CHECK(!azsdk_live_capabilities());
            unchanged(0);
            sync_unchanged(0);
            for (unsigned s = 0; s < 5; ++s)
                for (unsigned i = 0; i < f.server.count[s]; ++i) CHECK(fixture_expired(f.server.listeners[s][i]));
            f.server.fail_stream = -1;
            CHECK(azsdk_live_register(0, &f.unit));
            emit_fixture_scalar(&f, 1, 1, 0);
            CHECK(read(0).quantize == 1);
            emit_fixture_scalar(&f, 3, 2, 0);
            CHECK(sync_read(0).valid && sync_read(0).raw_mode == 2);
            azsdk_live_retire(0);
        }
    for (int allocation = 0; allocation < 5; ++allocation) {
        Fixture f __attribute__((cleanup(fixture_finish)));
        fixture_init(&f);
        azsdk_live_configure(1, 2);
        atomic_store_explicit(&(allocation_failure), allocation, memory_order_seq_cst);
        CHECK(!azsdk_live_register(0, &f.unit));
        atomic_store_explicit(&(allocation_failure), -1, memory_order_seq_cst);
        CHECK(!azsdk_live_capabilities());
        unchanged(0);
        sync_unchanged(0);
        for (unsigned s = 0; s < 5; ++s) CHECK(!f.server.count[s]);
        CHECK(azsdk_live_register(0, &f.unit));
        azsdk_live_retire(0);
    }
}
static void every_deck() {
    Fixture f[4] __attribute__((cleanup(fixtures_drop))) = {0};
    for (unsigned i = 0; i < 4; ++i) fixture_init(&f[i]);
    azsdk_live_configure(1, 2);
    for (unsigned d = 0; d < 4; ++d) CHECK(azsdk_live_register(d, &f[d].unit));
    CHECK(azsdk_live_capabilities() == 15);
    for (unsigned d = 0; d < 4; ++d) {
        for (unsigned wrong = 0; wrong < 4; ++wrong)
            if (wrong != d) {
                emit_fixture_play(&f[d], 0, 1, 1, 1, wrong);
                emit_fixture_scalar(&f[d], 1, 1, wrong);
                emit_fixture_scalar(&f[d], 2, 1, wrong);
            }
        unchanged(d);
        emit_fixture_play(&f[d], 0, d & 1, (d >> 1) & 1, 1, d);
        emit_fixture_scalar(&f[d], 1, d & 1, d);
        emit_fixture_scalar(&f[d], 2, (d >> 1) & 1, d);
        __auto_type out = read(d);
        CHECK(out.valid_fields == 7 && out.mode_is_2 == ((int) (d & 1)) && out.play_pause == ((int) ((d >> 1) & 1)));
        CHECK(out.quantize == ((int) (d & 1)) && out.key_lock == ((int) ((d >> 1) & 1)));
    }
    azsdk_live_configure(0, 2);
    for (unsigned d = 0; d < 4; ++d) unchanged(d);
}
static void sync_components_and_lifetime() {
    {
        const int values[] = {1, 2};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            int build = values[index];
            Fixture f[4] __attribute__((cleanup(fixtures_drop))) = {0};
            for (unsigned i = 0; i < 4; ++i) fixture_init(&f[i]);
            Lease held[4] __attribute__((cleanup(leases_drop))) = {0};
            uint64_t old_generation[4] = {0};
            azsdk_live_configure(1, build);
            for (unsigned d = 0; d < 4; ++d) {
                CHECK(azsdk_live_register(d, &f[d].unit));
                held[d] = get_lease(&f[d], 3);
                sync_unchanged(d);
                unchanged(d);
                uint32_t on = 2;
                dispatch(held[d], &on, d, false);
                sync_unchanged(d); // Slot 2 lacks routing; no initial replay is invented.
                for (unsigned wrong = 0; wrong < 4; ++wrong)
                    if (wrong != d) emit_scalar(held[d], 2, wrong);
                sync_unchanged(d);
                {
                    const uint32_t values[] = {0u, 2u, 1u, 3u, 256u, UINT32_MAX, UINT32_C(0x80000000), 0u};
                    for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
                        uint32_t mode = values[index];
                        emit_scalar(held[d], mode, d);
                        __auto_type receipt = sync_read(d);
                        int32_t expected;
                        memcpy(&expected, &mode, sizeof expected);
                        CHECK(receipt.raw_mode == expected);
                        CHECK(receipt.valid == (mode == 0 || mode == 2));
                        old_generation[d] = receipt.unit_generation;
                        unchanged(d); // Fourth stream cannot overwrite legacy arrays or fields.
                    }
                }
                emit_fixture_play(&f[d], 0, 1, 0, 1, d);
                emit_fixture_scalar(&f[d], 1, 0, d);
                emit_fixture_scalar(&f[d], 2, 1, d);
                __auto_type legacy = read(d);
                __auto_type before = sync_read(d);
                emit_scalar(held[d], 2, d);
                __auto_type after = sync_read(d);
                CHECK(after.revision > before.revision && after.observed_ns >= before.observed_ns);
                CHECK(after.unit_generation == legacy.unit_generation);
                __auto_type same_legacy = read(d);
                CHECK(!memcmp(&legacy, &same_legacy, sizeof legacy));
                emit_fixture_play(&f[d], 0, 0, 1, 0, d);
                emit_fixture_scalar(&f[d], 1, 1, d);
                emit_fixture_scalar(&f[d], 2, 0, d);
                __auto_type same_sync = sync_read(d);
                CHECK(!memcmp(&after, &same_sync, sizeof after));
                emit_scalar(held[d], 1, d);
                __auto_type invalid = sync_read(d);
                CHECK(!invalid.valid && invalid.raw_mode == 1 && invalid.revision > after.revision);
                CHECK(invalid.observed_ns >= after.observed_ns); // Invalid receipt clears prior valid state.
                azsdk_live_retire(d);
                emit_scalar(held[d], 2, d);
                sync_unchanged(d);
                unchanged(d);
            }
            CHECK(!azsdk_live_capabilities());
            Fixture replacement[4] __attribute__((cleanup(fixtures_drop))) = {0};
            for (unsigned i = 0; i < 4; ++i) fixture_init(&replacement[i]);
            for (unsigned d = 0; d < 4; ++d) {
                CHECK(azsdk_live_register(d, &replacement[d].unit));
                emit_scalar(held[d], 2, d); // Strong leases from retired units cannot republish.
                sync_unchanged(d);
                emit_fixture_scalar(&replacement[d], 3, 0, d);
                __auto_type current = sync_read(d);
                CHECK(current.valid && current.raw_mode == 0 && current.unit_generation != old_generation[d]);
            }
            Lease replacement_held[4] __attribute__((cleanup(leases_drop))) = {0};
            for (unsigned d = 0; d < 4; ++d) replacement_held[d] = get_lease(&replacement[d], 3);
            azsdk_live_configure(0, build);
            for (unsigned d = 0; d < 4; ++d) {
                emit_scalar(replacement_held[d], 2, d);
                sync_unchanged(d);
            }
        }
    }
}
struct worker_context {
    Lease p, q, k, sync;
    _Atomic(bool) stop;
};
static void *writer_thread(void *opaque) {
    struct worker_context *context = opaque;
    uint32_t zero = 0, one = 1;
    Play a = {{0, 0, 0}}, b = {{1, 1, 1}};
    while (!atomic_load(&context->stop)) {
        dispatch(context->p, &a, 0, true);
        dispatch(context->p, &b, 0, true);
        dispatch(context->q, &zero, 0, true);
        dispatch(context->q, &one, 0, true);
        dispatch(context->k, &one, 0, true);
        dispatch(context->k, &zero, 0, true);
        uint32_t sync_on = 2;
        dispatch(context->sync, &sync_on, 0, true);
        dispatch(context->sync, &one, 0, true);
        dispatch(context->sync, &zero, 0, true);
    }

    return NULL;
}
static void *other_thread(void *opaque) {
    struct worker_context *context = opaque;
    Play n = {{1, 1, 1}};
    while (!atomic_load(&context->stop)) dispatch(context->p, &n, 0, true);

    return NULL;
}
static void concurrent() {
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        Fixture f __attribute__((cleanup(fixture_finish)));
        fixture_init(&f);
        azsdk_live_configure(1, 2);
        CHECK(azsdk_live_register(0, &f.unit));
        Lease p __attribute__((cleanup(fixture_drop))) = get_lease(&f, 0);
        Lease q __attribute__((cleanup(fixture_drop))) = get_lease(&f, 1);
        Lease k __attribute__((cleanup(fixture_drop))) = get_lease(&f, 2);
        emit_play(p, 0, 0, 0, 0);
        __auto_type generation = read(0).unit_generation;
        Lease sync __attribute__((cleanup(fixture_drop))) = get_lease(&f, 3);
        emit_scalar(sync, 0, 0);
        struct worker_context context = {.p = p, .q = q, .k = k, .sync = sync};
        pthread_t writer, other;
        CHECK(!pthread_create(&writer, NULL, writer_thread, &context));
        CHECK(!pthread_create(&other, NULL, other_thread, &context));
        for (unsigned n = 0; n < 4000; ++n) {
            struct azsdk_live_sample out;
            if (azsdk_live_read(0, &out)) {
                CHECK(out.unit_generation == generation);
                if (out.valid_fields & 1) CHECK(out.mode_is_2 == out.play_pause && out.play_pause == out.play_aux);
                CHECK(!(out.valid_fields & ~7u));
            }
            struct azsdk_sync_sample sync_out;
            if (azsdk_sync_read(0, &sync_out)) {
                CHECK(sync_out.unit_generation == generation && sync_out.revision && sync_out.observed_ns);
                CHECK(sync_out.valid == (sync_out.raw_mode == 0 || sync_out.raw_mode == 2));
            }
        }
        if (cycle & 1) azsdk_live_retire(0);
        else azsdk_live_configure(1, 1);
        for (unsigned n = 0; n < 100; ++n) {
            unchanged(0);
            sync_unchanged(0);
        }
        atomic_store(&context.stop, true);
        CHECK(!pthread_join(writer, NULL));
        CHECK(!pthread_join(other, NULL));
        CHECK(!azsdk_live_capabilities());
    }
}
static Fixture *early_fixture;
/* A native ELF constructor can register before this translation unit's ordinary startup
 * initialization. Constant initialization must preserve that publication and ownership. */
__attribute__((constructor(101))) static void early_configuration() {
    early_fixture = calloc(1, sizeof(Fixture));
    fixture_init(early_fixture);
    azsdk_live_configure(1, 2);
    CHECK(azsdk_live_register(2, &early_fixture->unit));
    CHECK(early_fixture->server.count[3] == 1);
    CHECK(early_fixture->server.count[4] == 1);
    emit_fixture_scalar(early_fixture, 1, 1, 2);
    emit_fixture_scalar(early_fixture, 3, 2, 2);
    emit_fixture_scalar(early_fixture, 4, 1, 2);
}
static void tempo_lock_registration() {
    for (int build = 1; build <= 2; ++build) {
        Fixture units[4] __attribute__((cleanup(fixtures_drop))) = {0};
        for (unsigned i = 0; i < 4; ++i) fixture_init(&units[i]);
        azsdk_live_configure(1, build);
        for (int deck = 0; deck < 4; ++deck) {
            CHECK(azsdk_live_register(deck, &units[deck].unit));
            CHECK(units[deck].server.count[4] == 1);
            CHECK(!fixture_expired(units[deck].server.listeners[4][0]));
            struct azsdk_tempo_lock_sample sample, before;
            memset(&sample, 0xa5, sizeof sample);
            before = sample;
            CHECK(!azsdk_tempo_lock_read(deck, &sample) && !memcmp(&sample, &before, sizeof sample));
            Lease held __attribute__((cleanup(fixture_drop))) = get_lease(&units[deck], 4);
            emit_scalar(held, 1, (deck + 1) % 4);
            CHECK(!azsdk_tempo_lock_read(deck, &sample));
            uint64_t revision = 0;
            {
                const uint32_t values[] = {0u, 1u, 2u, 0x100u, 0xffffffffu, 0u};
                for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
                    uint32_t value = values[index];
                    emit_scalar(held, value, deck);
                    CHECK(azsdk_tempo_lock_read(deck, &sample));
                    CHECK(sample.valid == (value <= 1) && sample.revision > revision && sample.observed_ns);
                    CHECK(((uint32_t) (sample.raw_locked)) == value);
                    revision = sample.revision;
                }
            }
            before = sample;
            emit_fixture_scalar(&units[deck], 1, 1, deck);
            CHECK(azsdk_tempo_lock_read(deck, &sample) && !memcmp(&sample, &before, sizeof sample));
            azsdk_live_retire(deck);
            emit_scalar(held, 1, deck);
            CHECK(!azsdk_tempo_lock_read(deck, &sample));
        }
        for (int deck = 0; deck < 4; ++deck) azsdk_live_retire(deck);
    }
}
int main(int argc, char **argv) {
    _Static_assert(sizeof(struct azsdk_live_sample) == 88, "legacy private sample ABI unchanged");
    _Static_assert(sizeof(struct azsdk_sync_sample) == 32, "separate copied Sync sample ABI");
    allocator_contract();
    CHECK(early_fixture && azsdk_live_capabilities() == 4 && read(2).quantize == 1);
    CHECK(sync_read(2).valid && sync_read(2).raw_mode == 2);
    struct azsdk_tempo_lock_sample early_tempo;
    CHECK(azsdk_tempo_lock_read(2, &early_tempo) && early_tempo.valid && early_tempo.raw_locked == 1);
    for (unsigned s = 0; s < 5; ++s) CHECK(!fixture_expired(early_fixture->server.listeners[s][0]));
    azsdk_live_retire(2);
    for (unsigned s = 0; s < 5; ++s) CHECK(fixture_expired(early_fixture->server.listeners[s][0]));
    fixture_finish(early_fixture);
    free(early_fixture);
    tempo_lock_registration();
    if (argc == 2 && !strcmp(argv[1], "--sync-payload-short")) {
        Fixture short_payload __attribute__((cleanup(fixture_finish)));
        fixture_init(&short_payload);
        azsdk_live_configure(1, 2);
        CHECK(azsdk_live_register(0, &short_payload.unit));
        Play *bytes = malloc(sizeof *bytes);
        CHECK(bytes);
        *bytes = (Play) {{2, 0, 0}};
        dispatch_fixture(&short_payload, 3, bytes, 0, true);
        fputs("FAIL live: three-byte Sync payload lacked ASan diagnosis\n", stderr);
        return 1;
    }
    CHECK(argc == 1);
    validation();
    components_and_lifetime();
    failure_rollback();
    every_deck();
    sync_components_and_lifetime();
    concurrent();
#ifdef AZ_STATUS_NATIVE_PRODUCER
    native_sync_seed();
#endif
    azsdk_live_configure(0, 2);
    puts("PASS live: GNU observer ABI, bounded payloads, per-stream validity, lifecycle, rollback, concurrency");
}
