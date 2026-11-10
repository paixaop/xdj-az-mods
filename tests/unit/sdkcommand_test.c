/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef SCMD_ENGINE_HEADER
#define SCMD_ENGINE_HEADER "sdk_command_engine.h"
#endif
#include SCMD_ENGINE_HEADER

#if defined(SCMD_TEST_HEAP)
int main(void) {
    volatile unsigned offset = 8;
    volatile char *bytes = malloc(4);
    assert(bytes);
    bytes[offset] = 1;
    free((void *) bytes);
    return 0;
}
#elif defined(SCMD_TEST_SIGNED)
int main(void) {
    volatile int maximum = INT_MAX;
    volatile int overflow = maximum + 1;
    (void) overflow;
    return 0;
}
#elif defined(SCMD_TEST_RACE)
static atomic_int plant_start;
static volatile int plant_count;
static void *planted_race(void *unused) {
    (void) unused;
    while (!atomic_load_explicit(&plant_start, memory_order_acquire)) sched_yield();
    for (int i = 0; i < 100000; i++) plant_count++;
    return NULL;
}
int main(void) {
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, planted_race, NULL));
    atomic_store_explicit(&plant_start, 1, memory_order_release);
    for (unsigned i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    return 0;
}
#else

enum fake_mode {
    APPLY,
    PENDING,
    FAIL_DISPATCH,
    BAD_COMPLETION,
    INLINE_COMPLETION,
    BLOCK_BEGIN,
    BLOCK_DISPATCH
};
struct fixture {
    struct scmd_engine engine;
    struct scmd_adapter adapter;
    struct scmd_target target[SCMD_DECKS];
    atomic_uint_fast64_t now;
    atomic_int argument_error, entered, resume, completed;
    pthread_t owner;
    int begin_error, mode, zero_clock_in_begin;
    uint64_t advance_in_begin, token[SCMD_DECKS];
    unsigned begins, ends, dispatched, applied;
    uint32_t order[128];
};
#define FIXTURE_INIT {.engine = SCMD_ENGINE_INIT}
static uint64_t fake_clock(void *context) { return atomic_load_explicit(&((struct fixture *) context)->now, memory_order_relaxed); }
static int fake_arguments(void *context, const az_command_request *r, const struct scmd_target *target) {
    (void) target;
    struct fixture *f = context;
    int error = atomic_load_explicit(&f->argument_error, memory_order_relaxed);
    if (error) return error;
    if (r->operation == AZ_CMD_LOOP_IN) {
        /* This synthetic adapter models atomic setters on a 1 kHz source. */
        int begin = compare_timestamps(r->position_s, 0., 1, 1000, 1);
        int end = compare_timestamps(r->position_s, 100., 1, 1000, 1);
        if (begin == AZ_TIME_INVALID || end == AZ_TIME_INVALID || begin == AZ_TIME_BEFORE || end == AZ_TIME_AFTER)
            return AZ_INVALID;
    }
    if (r->operation == AZ_CMD_LOOP_LENGTH && !(r->beats > 0)) return AZ_INVALID;
    return AZ_OK;
}
static int fake_begin(void *context, const az_command_request *r, struct scmd_target *out) {
    struct fixture *f = context;
    f->begins++;
    if (!pthread_equal(pthread_self(), f->owner)) return AZ_INVALID;
    if (f->begin_error) return f->begin_error;
    if (f->mode == BLOCK_BEGIN) {
        atomic_store_explicit(&f->entered, 1, memory_order_release);
        while (!atomic_load_explicit(&f->resume, memory_order_acquire)) sched_yield();
    }
    if (f->advance_in_begin) atomic_fetch_add_explicit(&f->now, f->advance_in_begin, memory_order_relaxed);
    if (f->zero_clock_in_begin) atomic_store_explicit(&f->now, 0, memory_order_relaxed);
    *out = f->target[r->deck];
    return AZ_OK;
}
static struct scmd_completion applied(struct fixture *f, unsigned deck) {
    f->applied++;
    return (struct scmd_completion) {AZ_COMMAND_APPLIED, AZ_OK, f->target[deck].load_generation, ++f->target[deck].revision, 1};
}
static int fake_dispatch(void *context, const az_command_request *r, uint64_t token, struct scmd_completion *out) {
    struct fixture *f = context;
    assert(pthread_equal(pthread_self(), f->owner));
    if (f->dispatched < sizeof f->order / sizeof *f->order) f->order[f->dispatched] = r->entry_id;
    f->dispatched++;
    f->token[r->deck] = token;
    if (f->mode == PENDING) return AZ_PENDING;
    if (f->mode == FAIL_DISPATCH) return AZ_FAILED;
    if (f->mode == BAD_COMPLETION) {
        out->outcome = AZ_COMMAND_ACCEPTED;
        return AZ_OK;
    }
    if (f->mode == BLOCK_DISPATCH) {
        atomic_store_explicit(&f->entered, 1, memory_order_release);
        while (!atomic_load_explicit(&f->resume, memory_order_acquire)) sched_yield();
    }
    *out = applied(f, r->deck);
    if (f->mode == INLINE_COMPLETION) {
        assert(scmd_complete(&f->engine, token, out) == AZ_OK);
        assert(scmd_release(&f->engine, 0, r->request_id) == AZ_BUSY);
        assert(scmd_step(&f->engine, r->deck, &f->adapter) == AZ_BUSY);
        return AZ_PENDING;
    }
    return AZ_OK;
}
static void fake_end(void *context, uint32_t deck) {
    (void) deck;
    ((struct fixture *) context)->ends++;
}
static void initialize(struct fixture *f) {
    atomic_init(&f->now, 100);
    atomic_init(&f->argument_error, 0);
    atomic_init(&f->entered, 0);
    atomic_init(&f->resume, 0);
    atomic_init(&f->completed, 0);
    f->engine.clock = fake_clock;
    f->engine.clock_context = f;
    f->adapter = (struct scmd_adapter) {f, fake_arguments, fake_begin, fake_dispatch, fake_end};
    f->owner = pthread_self();
    for (unsigned i = 0; i < SCMD_DECKS; i++)
        f->target[i] = (struct scmd_target) {1, 10, SCMD_ALL_COMMANDS, UINT64_C(1) << AZ_CMD_LOOP_IN,
                                             UINT64_C(1) << AZ_CMD_SAVED_SAVE, SCMD_TARGET_LOAD | SCMD_TARGET_REVISION, 1};
}
static az_command_request request(uint64_t id, unsigned deck) {
    return (az_command_request) {.size = sizeof(az_command_request), .version = AZ_DATA_VERSION, .operation = AZ_CMD_LOOP_EXIT, .deck = deck, .request_id = id, .expected_load_generation = 1, .deadline_ns = 1000};
}
static az_command_result result(void) { return (az_command_result) {.size = sizeof(az_command_result)}; }
static void submit(struct fixture *f, int owner, az_command_request r) {
    az_command_result out = result();
    enum scmd_detail detail;
    assert(scmd_submit(&f->engine, owner, &r, &f->target[r.deck], &f->adapter, &out, &detail) == AZ_OK);
    assert(out.outcome == AZ_COMMAND_ACCEPTED && out.reason == AZ_PENDING && out.request_id == r.request_id);
    int order = compare_timestamps(out.observed_ns, fake_clock(f), 1000000000, 1000000000, 1);
    assert(!out.revision && order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL && detail == SCMD_DETAIL_NONE);
}
static az_command_result poll(struct fixture *f, int owner, uint64_t id) {
    az_command_result out = result();
    assert(scmd_poll(&f->engine, owner, id, &out) == AZ_OK);
    return out;
}
static void drain(struct fixture *f) {
    atomic_store(&f->now, 10000);
    assert(scmd_expire(&f->engine) == AZ_OK);
    /* A test fake has no independent native worker: this explicitly settles every
     * retained token before destroying the real engine. */
    struct scmd_completion settled = {AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0, 0, 1};
    for (unsigned i = 0; i < SCMD_SLOTS; i++) {
        struct scmd_record *r = &f->engine.records[i];
        if (r->phase == SCMD_FREE) continue;
        if (r->awaiting) assert(scmd_complete(&f->engine, r->token, &settled) == AZ_OK);
        assert(scmd_release(&f->engine, r->owner, r->request.request_id) == AZ_OK);
    }
    for (unsigned d = 0; d < SCMD_DECKS; d++) assert(!f->engine.active[d]);
    assert(scmd_destroy(&f->engine) == AZ_OK);
}
static void contracts(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    az_command_request r = request(1, 0);
    az_command_result out = result();
    uint32_t *tiny = malloc(sizeof *tiny);
    assert(tiny);
    *tiny = sizeof *tiny;
    assert(scmd_submit(&f.engine, 0, (const void *) tiny, &f.target[0], &f.adapter, &out, NULL) == AZ_INVALID);
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, (void *) tiny, NULL) == AZ_INVALID);
    assert(scmd_poll(&f.engine, 0, 1, (void *) tiny) == AZ_INVALID);
    assert(scmd_cancel(&f.engine, 0, 1, (void *) tiny) == AZ_INVALID);
    assert(scmd_event(&f.engine, 0, 0, (void *) tiny) == AZ_INVALID && *tiny == sizeof *tiny);
    free(tiny);
    for (unsigned field = 0; field < 17; field++) {
        az_command_request bad = r;
        switch (field) {
        case 0: bad.size--; break;
        case 1: bad.version++; break;
        case 2: bad.operation = 0; break;
        case 3: bad.operation = AZ_CMD_CUE_LOOP_SET + 1; break;
        case 4: bad.deck = SCMD_DECKS; break;
        case 5: bad.request_id = 0; break;
        case 6: bad.expected_load_generation = 0; break;
        case 7: bad.deadline_ns = 0; break;
        case 8: bad.quantization = AZ_QUANTIZE_ON + 1; break;
        case 9: bad.flags = 4; break;
        case 10: bad.reserved = 1; break;
        case 11: bad.position_s = NAN; break;
        case 12: bad.end_s = INFINITY; break;
        case 13: bad.beats = -INFINITY; break;
        case 14: memset(bad.comment, 'x', sizeof bad.comment); break;
        case 15: bad.classification = AZ_CUE_MEMORY + 1; break;
        case 16: bad.rgb = UINT32_C(0xff000000); break;
        }
        memset(&out, 0xa5, sizeof out);
        out.size = sizeof out;
        az_command_result before = out;
        assert(scmd_submit(&f.engine, 0, &bad, &f.target[0], &f.adapter, &out, NULL) == AZ_INVALID);
        assert(!memcmp(&before, &out, sizeof out));
    }
    assert(scmd_submit(&f.engine, -1, &r, &f.target[0], &f.adapter, &out, NULL) == AZ_INVALID);
    assert(scmd_submit(&f.engine, SCMD_OWNERS, &r, &f.target[0], &f.adapter, &out, NULL) == AZ_INVALID);
    struct {
        az_command_result value;
        uint64_t guard[2];
    } large = {.value = {.size = sizeof large}, .guard = {UINT64_MAX, UINT64_MAX}};
    r.size += 32;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &large.value, NULL) == AZ_OK);
    assert(large.value.size == sizeof large && large.guard[0] == UINT64_MAX && large.guard[1] == UINT64_MAX);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK);
    assert(scmd_poll(&f.engine, 0, 1, &large.value) == AZ_OK && large.value.outcome == AZ_COMMAND_APPLIED);
    assert(large.value.size == sizeof large && large.guard[0] == UINT64_MAX && large.guard[1] == UINT64_MAX);
    assert(scmd_destroy(&f.engine) == AZ_BUSY);
    drain(&f);
}
static void acceptance(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    az_command_request r = request(1, 0);
    az_command_result out = result();
    enum scmd_detail detail;
    f.target[0].commands = 0;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_UNSUPPORTED);
    assert(out.outcome == AZ_COMMAND_REJECTED && out.request_id == 1 && detail == SCMD_DETAIL_CAPABILITY && !f.dispatched);
    f.target[0].commands = SCMD_ALL_COMMANDS;
    f.target[0].loaded = 0;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_STALE && detail == SCMD_DETAIL_LOAD);
    f.target[0].loaded = 1;
    r.operation = AZ_CMD_LOOP_IN;
    r.expected_revision = 0;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_INVALID && detail == SCMD_DETAIL_REVISION);
    r.expected_revision = 9;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_STALE && detail == SCMD_DETAIL_REVISION);
    r.expected_revision = 10;
    r.position_s = -1;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_INVALID && detail == SCMD_DETAIL_ARGUMENTS);
    r.operation = AZ_CMD_SAVED_SAVE;
    r.expected_revision = 0;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_INVALID && detail == SCMD_DETAIL_PERSISTENCE);
    r.flags = AZ_COMMAND_PERSIST;
    submit(&f, 0, r); /* Refused requests did not consume the ID. */
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK && f.dispatched == 1 && f.applied == 1);
    out = poll(&f, 0, 1);
    assert(out.outcome == AZ_COMMAND_APPLIED && out.reason == AZ_OK && out.revision == 11);
    assert(f.begins == 1 && f.ends == 1);
    drain(&f);
}
/* A native-unsupported operation must never dispatch, regardless of logical deck,
 * requested quantization or persistence. The synthetic applied route separately
 * checks that the shared policy can carry every operation without conflating IDs. */
static void operation_deck_matrix(void) {
    for (unsigned deck = 0; deck < SCMD_DECKS; ++deck)
        for (unsigned op = AZ_CMD_LOOP_IN; op <= AZ_CMD_CUE_COLOR; ++op) {
            struct fixture f = FIXTURE_INIT;
            initialize(&f);
            f.target[deck].commands = 0;
            for (unsigned quantize = AZ_QUANTIZE_NATIVE; quantize <= AZ_QUANTIZE_ON; ++quantize)
                for (unsigned persist = 0; persist < 2; ++persist) {
                    az_command_request r = request(1, deck);
                    r.operation = op;
                    r.quantization = quantize;
                    r.flags = persist ? AZ_COMMAND_PERSIST : 0;
                    az_command_result out = result();
                    enum scmd_detail detail;
                    assert(scmd_submit(&f.engine, 0, &r, &f.target[deck], &f.adapter, &out, &detail) == AZ_UNSUPPORTED);
                    assert(detail == SCMD_DETAIL_CAPABILITY && out.outcome == AZ_COMMAND_REJECTED);
                    assert(out.request_id == 1 && !out.revision && !f.dispatched && !f.begins && !f.ends);
                    assert(!f.engine.highest_id[0]);
                }
            f.target[deck].commands = SCMD_ALL_COMMANDS;
            az_command_request r = request(1, deck);
            r.operation = op;
            r.position_s = 1;
            r.end_s = 5;
            r.beats = 4;
            r.entry_id = 1;
            r.flags = AZ_COMMAND_PERSIST;
            r.expected_revision = f.target[deck].revision;
            submit(&f, 0, r);
            assert(scmd_step(&f.engine, deck, &f.adapter) == AZ_OK);
            az_command_result out = poll(&f, 0, 1);
            assert(out.outcome == AZ_COMMAND_APPLIED && out.reason == AZ_OK && out.revision == 11);
            assert(f.begins == 1 && f.dispatched == 1 && f.ends == 1 && f.order[0] == 1);
            for (unsigned other = 0; other < SCMD_DECKS; ++other)
                if (other != deck) assert(f.target[other].revision == 10 && !f.engine.active[other]);
            drain(&f);
        }
}
static void deduplication(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    az_command_request r = request(2, 0);
    strcpy(r.comment, "retry");
    submit(&f, 0, r);
    az_command_result out = result();
    enum scmd_detail detail;
    for (unsigned changed = 0; changed < 16; changed++) {
        az_command_request conflict = r;
        switch (changed) {
        case 0: conflict.operation = AZ_CMD_LOOP_ENTER; break;
        case 1: conflict.deck = 1; break;
        case 2: conflict.expected_load_generation++; break;
        case 3: conflict.expected_revision = 1; break;
        case 4: conflict.deadline_ns++; break;
        case 5: conflict.quantization = AZ_QUANTIZE_ON; break;
        case 6: conflict.flags = AZ_COMMAND_PERSIST; break;
        case 7: conflict.classification = AZ_CUE_MEMORY; break;
        case 8: conflict.entry_id = 1; break;
        case 9: conflict.position_s = 1; break;
        case 10: conflict.end_s = 1; break;
        case 11: conflict.beats = 1; break;
        case 12: conflict.rgb = 0x123456; break;
        case 13: strcpy(conflict.comment, "changed"); break;
        case 14: conflict.position_s = 1e-12; break;
        case 15: conflict.end_s = 1e-12; break;
        }
        assert(scmd_submit(&f.engine, 0, &conflict, &f.target[0], &f.adapter, &out, &detail) == AZ_INVALID && detail == SCMD_DETAIL_ID_CONFLICT);
    }
    r.position_s = r.end_s = -0.0;
    r.size += 16;
    r.comment[20] = 'x';
    assert(scmd_submit(&f.engine, 0, &r, NULL, NULL, &out, NULL) == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED);
    az_command_request other = request(2, 1);
    submit(&f, 1, other); /* Same ID in another owner is a separate request. */
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK && scmd_step(&f.engine, 1, &f.adapter) == AZ_OK);
    assert(f.dispatched == 2);
    az_command_result terminal = poll(&f, 0, 2);
    assert(scmd_submit(&f.engine, 0, &r, NULL, NULL, &out, NULL) == AZ_OK && !memcmp(&terminal, &out, sizeof out));
    assert(scmd_release(&f.engine, 0, 2) == AZ_OK);
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_STALE && detail == SCMD_DETAIL_ID_ORDER);
    az_command_request older = request(1, 0);
    assert(scmd_submit(&f.engine, 0, &older, &f.target[0], &f.adapter, &out, &detail) == AZ_STALE && detail == SCMD_DETAIL_ID_ORDER);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_EMPTY && f.dispatched == 2);
    drain(&f);
}
static void revalidation(void) {
    for (unsigned change = 0; change < 5; change++) {
        struct fixture f = FIXTURE_INIT;
        initialize(&f);
        az_command_request r = request(1, 0);
        r.expected_revision = 10;
        submit(&f, 0, r);
        if (change == 0) f.target[0].load_generation++;
        if (change == 1) f.target[0].revision++;
        if (change == 2) f.target[0].loaded = 0;
        if (change == 3) f.target[0].commands = 0;
        if (change == 4) atomic_store(&f.argument_error, AZ_INVALID);
        assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK && !f.dispatched && f.ends == 1);
        az_command_result out = poll(&f, 0, 1);
        assert(out.outcome == AZ_COMMAND_REJECTED);
        assert(out.reason == (change == 3 ? AZ_UNSUPPORTED : change == 4 ? AZ_INVALID
                                                                         : AZ_STALE));
        drain(&f);
    }
}
static void deadlines(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    az_command_request r = request(1, 0);
    az_command_result out = result();
    enum scmd_detail detail;
    r.deadline_ns = 100;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_STALE && detail == SCMD_DETAIL_DEADLINE);
    r.deadline_ns = 120;
    submit(&f, 0, r);
    f.advance_in_begin = 20;
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK && !f.dispatched && f.ends == 1);
    out = poll(&f, 0, 1);
    assert(out.outcome == AZ_COMMAND_CANCELLED && out.reason == AZ_STALE);
    assert(scmd_release(&f.engine, 0, 1) == AZ_OK);
    r = request(2, 0);
    r.deadline_ns = 140;
    submit(&f, 0, r);
    atomic_store(&f.now, 140);
    assert(scmd_expire(&f.engine) == AZ_OK && !f.dispatched);
    out = poll(&f, 0, 2);
    assert(out.outcome == AZ_COMMAND_CANCELLED && out.reason == AZ_STALE);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_EMPTY);
    drain(&f);
}
static void ordering_and_uncertainty(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    az_command_request first = request(1, 0), second = request(2, 0), another = request(1, 1);
    first.entry_id = 11;
    second.entry_id = 22;
    another.entry_id = 33;
    first.deadline_ns = 150;
    submit(&f, 0, first);
    submit(&f, 0, second);
    submit(&f, 1, another);
    f.mode = PENDING;
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_PENDING && f.dispatched == 1);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_BUSY && f.dispatched == 1);
    assert(scmd_step(&f.engine, 1, &f.adapter) == AZ_PENDING && f.dispatched == 2);
    atomic_store(&f.now, 150);
    assert(scmd_expire(&f.engine) == AZ_OK);
    az_command_result uncertain = poll(&f, 0, 1);
    assert(uncertain.outcome == AZ_COMMAND_UNCERTAIN && uncertain.reason == AZ_STALE);
    assert(scmd_release(&f.engine, 0, 1) == AZ_BUSY);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_BUSY); /* Deadline did not drain native work. */
    struct scmd_completion done = applied(&f, 0);
    assert(scmd_complete(&f.engine, f.token[0], &done) == AZ_OK);
    az_command_result after = poll(&f, 0, 1);
    assert(!memcmp(&uncertain, &after, sizeof after)); /* Late proof does not rewrite a terminal result. */
    assert(scmd_release(&f.engine, 0, 1) == AZ_OK);
    f.mode = APPLY;
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK);
    assert(f.order[0] == 11 && f.order[1] == 33 && f.order[2] == 22);
    assert(scmd_complete(&f.engine, f.token[0], &done) == AZ_STALE);
    drain(&f);
}
static void completion_contracts(void) {
    for (unsigned kind = 0; kind < 6; kind++) {
        struct fixture f = FIXTURE_INIT;
        initialize(&f);
        submit(&f, 0, request(1, 0));
        f.mode = kind == 0 ? FAIL_DISPATCH : kind == 1 ? BAD_COMPLETION
                                                       : PENDING;
        assert(scmd_step(&f.engine, 0, &f.adapter) == (kind < 2 ? AZ_OK : AZ_PENDING));
        if (kind >= 2) {
            struct scmd_completion done;
            if (kind == 2) done = (struct scmd_completion) {AZ_COMMAND_APPLIED, AZ_OK, 2, 11, 1};
            else if (kind == 3) done = (struct scmd_completion) {AZ_COMMAND_REJECTED, AZ_UNSUPPORTED, 0, 0, 1};
            else if (kind == 4) done = (struct scmd_completion) {AZ_COMMAND_CANCELLED, AZ_CANCELLED, 0, 0, 1};
            else done = (struct scmd_completion) {AZ_COMMAND_APPLIED, AZ_OK, 1, 9, 1};
            assert(scmd_complete(&f.engine, f.token[0], &done) == (kind == 2 || kind == 5 ? AZ_STALE : AZ_OK));
        }
        az_command_result out = poll(&f, 0, 1);
        assert(out.outcome == (kind == 3 ? AZ_COMMAND_REJECTED : kind == 4 ? AZ_COMMAND_CANCELLED
                                                                           : AZ_COMMAND_UNCERTAIN));
        assert(out.reason == (kind < 2 ? AZ_FAILED : kind == 3 ? AZ_UNSUPPORTED
                                                 : kind == 4   ? AZ_CANCELLED
                                                               : AZ_STALE));
        assert(!out.revision);
        drain(&f);
    }
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    submit(&f, 0, request(1, 0));
    struct scmd_change queued = {.size = sizeof queued};
    assert(scmd_event(&f.engine, 0, 0, &queued) == AZ_OK);
    struct scmd_completion premature = {AZ_COMMAND_APPLIED, AZ_OK, 1, 11, 1};
    assert(scmd_complete(&f.engine, queued.token, &premature) == AZ_STALE);
    assert(poll(&f, 0, 1).outcome == AZ_COMMAND_ACCEPTED && !f.dispatched);
    f.mode = INLINE_COMPLETION;
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK && f.dispatched == 1 && f.ends == 1);
    assert(poll(&f, 0, 1).outcome == AZ_COMMAND_APPLIED);
    drain(&f);
}
static void malformed_completions(void) {
    for (unsigned bad = 0; bad < 6; bad++) {
        struct fixture f = FIXTURE_INIT;
        initialize(&f);
        submit(&f, 0, request(1, 0));
        f.mode = PENDING;
        assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_PENDING);
        struct scmd_completion completion = {AZ_COMMAND_APPLIED, AZ_OK, 1, 11, 1};
        if (bad == 0) completion.revision = 0;
        if (bad == 1) completion.load_generation = 0;
        if (bad == 2) completion.settled = 0;
        if (bad == 3) completion.reason = AZ_STALE;
        if (bad == 4) completion.outcome = AZ_COMMAND_ACCEPTED;
        if (bad == 5) completion.settled = 2;
        assert(scmd_complete(&f.engine, f.token[0], &completion) == AZ_INVALID);
        az_command_result uncertain = poll(&f, 0, 1);
        assert(uncertain.outcome == AZ_COMMAND_UNCERTAIN && uncertain.reason == AZ_FAILED && !uncertain.revision);
        assert(scmd_release(&f.engine, 0, 1) == AZ_BUSY);
        assert(scmd_complete(&f.engine, f.token[0], &completion) == AZ_INVALID);
        completion = (struct scmd_completion) {AZ_COMMAND_UNCERTAIN, AZ_FAILED, 0, 0, 0};
        assert(scmd_complete(&f.engine, f.token[0], &completion) == AZ_PENDING);
        assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_BUSY);
        assert(scmd_complete(&f.engine, UINT64_MAX, &completion) == AZ_STALE);
        completion.settled = 1;
        assert(scmd_complete(&f.engine, f.token[0], &completion) == AZ_OK);
        az_command_result after = poll(&f, 0, 1);
        assert(!memcmp(&uncertain, &after, sizeof after));
        assert(scmd_release(&f.engine, 0, 1) == AZ_OK);
        drain(&f);
    }
}
static void clock_and_adapter_failures(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    az_command_request r = request(1, 0);
    az_command_result out = result();
    enum scmd_detail detail;
    assert(scmd_monotonic(NULL));
    atomic_store(&f.now, 0);
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_FAILED && detail == SCMD_DETAIL_CLOCK);
    atomic_store(&f.now, 100);
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], NULL, &out, &detail) == AZ_UNSUPPORTED && detail == SCMD_DETAIL_BACKEND);
    atomic_store(&f.argument_error, AZ_PENDING);
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_FAILED && detail == SCMD_DETAIL_ARGUMENTS);
    atomic_store(&f.argument_error, 0);
    submit(&f, 0, r);
    atomic_store(&f.now, 0);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_FAILED && !f.begins && !f.dispatched);
    assert(scmd_cancel(&f.engine, 0, 1, &out) == AZ_FAILED);
    assert(scmd_expire(&f.engine) == AZ_FAILED && poll(&f, 0, 1).outcome == AZ_COMMAND_ACCEPTED);
    atomic_store(&f.now, 100);
    f.zero_clock_in_begin = 1;
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK && f.ends == 1 && !f.dispatched);
    out = poll(&f, 0, 1);
    assert(out.outcome == AZ_COMMAND_REJECTED && out.reason == AZ_FAILED && !out.observed_ns);
    drain(&f);
    struct fixture error = FIXTURE_INIT;
    initialize(&error);
    submit(&error, 0, request(1, 0));
    error.begin_error = -1000;
    assert(scmd_step(&error.engine, 0, &error.adapter) == AZ_OK && !error.dispatched && !error.ends);
    out = poll(&error, 0, 1);
    assert(out.outcome == AZ_COMMAND_REJECTED && out.reason == AZ_FAILED);
    drain(&error);
}
static void *cancel_during_callback(void *context) {
    struct fixture *f = context;
    while (!atomic_load_explicit(&f->entered, memory_order_acquire)) sched_yield();
    az_command_result out = result();
    int rc;
    do { rc = scmd_cancel(&f->engine, 0, 1, &out); } while (rc == AZ_BUSY);
    if (f->mode == BLOCK_BEGIN) {
        assert(rc == AZ_OK && out.outcome == AZ_COMMAND_CANCELLED);
        assert(scmd_release(&f->engine, 0, 1) == AZ_BUSY);
    } else assert(rc == AZ_PENDING && out.outcome == AZ_COMMAND_ACCEPTED);
    atomic_store_explicit(&f->resume, 1, memory_order_release);
    return NULL;
}
static void cancellation_boundaries(void) {
    for (unsigned dispatched = 0; dispatched < 2; dispatched++) {
        struct fixture f = FIXTURE_INIT;
        initialize(&f);
        submit(&f, 0, request(1, 0));
        f.mode = dispatched ? BLOCK_DISPATCH : BLOCK_BEGIN;
        pthread_t canceller;
        assert(!pthread_create(&canceller, NULL, cancel_during_callback, &f));
        assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK);
        assert(!pthread_join(canceller, NULL));
        assert(f.dispatched == dispatched && f.applied == dispatched && f.ends == 1);
        az_command_result out = poll(&f, 0, 1);
        assert(out.outcome == (dispatched ? AZ_COMMAND_APPLIED : AZ_COMMAND_CANCELLED));
        az_command_result cancel = result();
        assert(scmd_cancel(&f.engine, 0, 1, &cancel) == (dispatched ? AZ_STALE : AZ_OK));
        assert(!memcmp(&out, &cancel, sizeof out));
        drain(&f);
    }
}
static void *wrong_thread(void *context) {
    struct fixture *f = context;
    assert(scmd_step(&f->engine, 0, &f->adapter) == AZ_INVALID);
    return NULL;
}
static void retry_and_busy(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    submit(&f, 0, request(1, 0));
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, wrong_thread, &f) && !pthread_join(thread, NULL));
    assert(!f.dispatched && !f.ends && poll(&f, 0, 1).outcome == AZ_COMMAND_ACCEPTED);
    for (unsigned i = 0; i < 2; i++) {
        f.begin_error = i ? AZ_PENDING : AZ_BUSY;
        assert(scmd_step(&f.engine, 0, &f.adapter) == f.begin_error && !f.dispatched);
    }
    f.begin_error = 0;
    az_command_request r = request(2, 0);
    az_command_result out = result();
    struct scmd_change event = {.size = sizeof event};
    assert(!pthread_mutex_lock(&f.engine.mu));
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, NULL) == AZ_BUSY);
    assert(scmd_poll(&f.engine, 0, 1, &out) == AZ_BUSY);
    assert(scmd_cancel(&f.engine, 0, 1, &out) == AZ_BUSY);
    assert(scmd_release(&f.engine, 0, 1) == AZ_BUSY);
    assert(scmd_complete(&f.engine, 1, &(struct scmd_completion) {0}) == AZ_BUSY);
    assert(scmd_expire(&f.engine) == AZ_BUSY);
    assert(scmd_event(&f.engine, 0, 0, &event) == AZ_BUSY);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_BUSY);
    assert(!pthread_mutex_unlock(&f.engine.mu));
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK && f.dispatched == 1 && f.ends == 1);
    drain(&f);
}
static void quotas_and_exhaustion(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    for (unsigned i = 1; i <= SCMD_PER_OWNER; i++) submit(&f, 0, request(i, 0));
    az_command_request r = request(SCMD_PER_OWNER + 1, 0);
    az_command_result out = result();
    enum scmd_detail detail;
    assert(scmd_submit(&f.engine, 0, &r, &f.target[0], &f.adapter, &out, &detail) == AZ_LIMIT && detail == SCMD_DETAIL_QUOTA);
    assert(scmd_cancel(&f.engine, 0, 1, &out) == AZ_OK && scmd_release(&f.engine, 0, 1) == AZ_OK);
    submit(&f, 0, r);
    for (unsigned owner = 1; owner < SCMD_SLOTS / SCMD_PER_OWNER; owner++)
        for (unsigned id = 1; id <= SCMD_PER_OWNER; id++) submit(&f, (int) owner, request(id, owner % SCMD_DECKS));
    r = request(1, 0);
    assert(scmd_submit(&f.engine, SCMD_SLOTS / SCMD_PER_OWNER, &r, &f.target[0], &f.adapter, &out, NULL) == AZ_LIMIT);
    drain(&f);
    struct fixture token = FIXTURE_INIT;
    initialize(&token);
    token.engine.next_token = UINT64_MAX;
    assert(scmd_submit(&token.engine, 0, &r, &token.target[0], &token.adapter, &out, NULL) == AZ_LIMIT);
    token.engine.next_token = 0;
    token.engine.sequence = UINT64_MAX - SCMD_SLOTS;
    assert(scmd_submit(&token.engine, 0, &r, &token.target[0], &token.adapter, &out, NULL) == AZ_LIMIT);
    assert(scmd_destroy(&token.engine) == AZ_OK);
}
static void events(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    submit(&f, 0, request(1, 0));
    submit(&f, 1, request(1, 1));
    struct {
        struct scmd_change value;
        uint64_t guard;
    } event = {.value = {.size = sizeof event}, .guard = UINT64_MAX};
    assert(scmd_event(&f.engine, 0, 0, &event.value) == AZ_OK);
    assert(event.value.result.outcome == AZ_COMMAND_ACCEPTED && event.value.sequence == 1 && !event.value.settled);
    assert(!memcmp(&event.value.result, &(az_command_result) {sizeof(az_command_result), AZ_DATA_VERSION, AZ_COMMAND_ACCEPTED, AZ_PENDING, 1, 0, 100}, sizeof(az_command_result)));
    uint64_t token = event.value.token;
    assert(scmd_event(&f.engine, 0, 1, &event.value) == AZ_EMPTY);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK);
    assert(scmd_event(&f.engine, 0, 1, &event.value) == AZ_OK);
    assert(event.value.token == token && event.value.result.outcome == AZ_COMMAND_APPLIED && event.value.settled && event.value.result.revision == 11);
    az_command_result state = poll(&f, 0, 1);
    assert(!memcmp(&state, &event.value.result, sizeof state));
    assert(event.value.size == sizeof event && event.guard == UINT64_MAX);
    assert(scmd_release(&f.engine, 0, 1) == AZ_OK);
    for (unsigned id = 2; id <= SCMD_EVENTS + 2; id++) {
        az_command_request r = request(id, 0);
        submit(&f, 0, r);
        assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK && scmd_release(&f.engine, 0, id) == AZ_OK);
    }
    assert(scmd_event(&f.engine, 0, 0, &event.value) == AZ_OK && event.value.missed);
    assert(scmd_event(&f.engine, 2, 0, &event.value) == AZ_OK && event.value.missed && !event.value.token && event.value.sequence == f.engine.sequence);
    struct scmd_change before = event.value;
    assert(scmd_event(&f.engine, 2, event.value.sequence, &event.value) == AZ_EMPTY);
    assert(scmd_event(&f.engine, 2, f.engine.sequence + 1, &event.value) == AZ_INVALID);
    assert(scmd_event(&f.engine, 2, UINT64_MAX, &event.value) == AZ_INVALID && !memcmp(&before, &event.value, sizeof before));
    drain(&f);
}
struct producer_context {
    struct fixture *fixture;
    int owner;
    struct scmd_target cached;
};
/* Pending work is polled by later frames in clients. Tight test-only polling
 * can starve the owner thread under full-system emulation; keep all 400 commands
 * and the same watchdog while allowing the scheduler to run the owner. */
static void producer_wait(void) {
    const struct timespec delay = {0, 1000000};
    assert(nanosleep(&delay, NULL) == 0);
}
static void *producer(void *context) {
    struct producer_context *p = context;
    for (uint64_t id = 1; id <= 100; id++) {
        az_command_request r = request(id, (unsigned) p->owner);
        az_command_result out = result();
        int rc;
        do {
            rc = scmd_submit(&p->fixture->engine, p->owner, &r, &p->cached, &p->fixture->adapter, &out, NULL);
            if (rc == AZ_BUSY) producer_wait();
        } while (rc == AZ_BUSY);
        assert(rc == AZ_OK);
        do {
            rc = scmd_poll(&p->fixture->engine, p->owner, id, &out);
            if (rc == AZ_BUSY || (rc == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED)) producer_wait();
        } while (rc == AZ_BUSY || (rc == AZ_OK && out.outcome == AZ_COMMAND_ACCEPTED));
        assert(rc == AZ_OK && out.outcome == AZ_COMMAND_APPLIED && out.revision);
        out.outcome = AZ_COMMAND_CANCELLED;
        do {
            rc = scmd_poll(&p->fixture->engine, p->owner, id, &out);
            if (rc == AZ_BUSY) producer_wait();
        } while (rc == AZ_BUSY);
        assert(rc == AZ_OK && out.outcome == AZ_COMMAND_APPLIED);
        do {
            rc = scmd_release(&p->fixture->engine, p->owner, id);
            if (rc == AZ_BUSY) producer_wait();
        } while (rc == AZ_BUSY);
        assert(rc == AZ_OK);
    }
    atomic_fetch_add_explicit(&p->fixture->completed, 1, memory_order_release);
    return NULL;
}
static void concurrent_callers(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    pthread_t threads[SCMD_DECKS];
    struct producer_context context[SCMD_DECKS];
    for (unsigned i = 0; i < SCMD_DECKS; i++) {
        context[i] = (struct producer_context) {&f, (int) i, f.target[i]};
        assert(!pthread_create(&threads[i], NULL, producer, &context[i]));
    }
    while (atomic_load_explicit(&f.completed, memory_order_acquire) < SCMD_DECKS) {
        for (unsigned deck = 0; deck < SCMD_DECKS; deck++) {
            int rc = scmd_step(&f.engine, deck, &f.adapter);
            assert(rc == AZ_OK || rc == AZ_BUSY || rc == AZ_EMPTY);
        }
        sched_yield();
    }
    for (unsigned i = 0; i < SCMD_DECKS; i++) assert(!pthread_join(threads[i], NULL));
    assert(f.dispatched == 400 && f.applied == 400 && f.begins == f.ends);
    drain(&f);
}
static void async_callback(const az_command_completion *completion, void *user) {
    unsigned *calls = user;
    assert(completion->size >= sizeof *completion);
    assert(completion->version == AZ_DATA_VERSION);
    assert(completion->result.request_id == 1);
    (*calls)++;
}
static void pending_delivery_identity(void) {
    struct fixture f = FIXTURE_INIT;
    initialize(&f);
    az_command_request r = request(1, 0);
    az_command_result out = result();
    unsigned calls = 0;
    enum scmd_detail detail;
    assert(scmd_submit_async(&f.engine, 0, &r, async_callback, &calls, &f.target[0], &f.adapter, &out, &detail, 0) == AZ_OK);
    assert(scmd_step(&f.engine, 0, &f.adapter) == AZ_OK);
    struct scmd_notification notification;
    assert(scmd_next_notification(&f.engine, &notification) == AZ_OK);
    assert(notification.kind == AZ_COMMAND_COMPLETION_TERMINAL);
    assert(scmd_acknowledge(&f.engine, 0, 1) == AZ_OK);
    /* Acknowledgement from the terminal callback's interval is accepted, but
     * the primary record cannot move until that callback returns. */
    assert(scmd_find_token(&f.engine, notification.token) != NULL);
    scmd_notification_done(&f.engine, &notification);
    assert(scmd_find_token(&f.engine, notification.token) == NULL);
    assert(scmd_find_pending_token(&f.engine, notification.token) != NULL);
    out = result();
    assert(scmd_submit_async(&f.engine, 0, &r, async_callback, &calls, &f.target[0], &f.adapter, &out, &detail, 0) == AZ_OK);
    assert(out.outcome == AZ_COMMAND_APPLIED && out.request_id == 1);
    assert(scmd_acknowledge(&f.engine, 0, 1) == AZ_OK);
    assert(scmd_next_notification(&f.engine, &notification) == AZ_OK);
    assert(notification.kind == AZ_COMMAND_COMPLETION_RELEASED && notification.completion.settled == 1);
    scmd_notification_done(&f.engine, &notification);
    assert(calls == 0);
    assert(scmd_acknowledge(&f.engine, 0, 1) == AZ_STALE);
    assert(scmd_destroy(&f.engine) == AZ_OK);
}
int main(void) {
    assert(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
    operation_deck_matrix();
    alarm(45);
#define RUN(test)                          \
    do {                                   \
        test();                            \
        puts("PASS sdk commands: " #test); \
    } while (0)
    RUN(contracts);
    RUN(acceptance);
    RUN(deduplication);
    RUN(revalidation);
    RUN(deadlines);
    RUN(ordering_and_uncertainty);
    RUN(completion_contracts);
    RUN(malformed_completions);
    RUN(clock_and_adapter_failures);
    RUN(cancellation_boundaries);
    RUN(retry_and_busy);
    RUN(quotas_and_exhaustion);
    RUN(events);
    RUN(concurrent_callers);
    RUN(pending_delivery_identity);
#undef RUN
    alarm(0);
    puts("sdk command engine: 14 suites passed; no native firmware was called");
    return 0;
}
#endif
