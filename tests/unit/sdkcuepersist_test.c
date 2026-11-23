/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azcuepersist.h"
#include "../../src/common/aznative_calls.h"
#include "../../src/azmod/azmod-mod.h"
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include "native_abi_fixture.h"
static bool confirm_raises;



typedef struct {
    _Alignas(8) unsigned char bytes[0x200];
} Bytes;
Bytes maintainer = {0}, queue = {0}, property = {0}, handler = {0}, receiver = {0}, payload = {0}, foreign_receiver = {0};
void (*request_entry)(void *, const void *);
bool (*handler_entry)(void *);
void (*reply_entry)(void *, const void *, const void *, const void *, const void *, const int *);
void (*registered_entry)(void *);
uintptr_t handler_table[4] = {0}, receiver_table[3] = {0};
unsigned producer_calls, request_calls, handler_calls, task_calls, ready_calls, confirm_calls, allocations, destructions;
bool identity_valid = true, effect_confirmed = true, invalidate_in_producer = false, publication_checks = false;
char observed_comment[128];
uint32_t observed_color;
unsigned char observed_auto;
void setword(void *base, unsigned offset, uintptr_t word) { memcpy(((unsigned char *) (base)) + offset, &word, sizeof word); }
uintptr_t getword(const void *base, unsigned offset) {
    uintptr_t word;
    memcpy(&word, ((unsigned char *) (base)) + offset, sizeof word);
    return word;
}
Bytes source_root = {0}, memory_set = {0}, hot_set = {0};
typedef int (*Helper)(void *, const void *, const void *, const void *, const void *, unsigned);
Helper memory_entry, hot_entry;
unsigned helper_calls = 0, publications = 0, ref_copies = 0, ref_releases = 0;
unsigned char **owned_contents;
size_t owned_count, owned_capacity;
Bytes helper_request = {0}, helper_cue = {0};
bool helper_pending = false, helper_cue_owned = false;
unsigned current_operation = 0;
int64_t locator_shift_us = 0;
void copy_row(void *out, const void *in) {
    memcpy(out, in, 0x80);
    const char *text = ((const char *) (getword(((void *) (in)), 0x70)));
    setword(out, 0x70, ((uintptr_t) (strdup(text ? text : ""))));
}
void destroy_row(void *row) {
    free(((void *) (getword(row, 0x70))));
    memset(row, 0, 0x80);
}
void copy_ref(void *out, const void *in) {
    memcpy(out, in, 8);
    if (getword(out, 0)) ++ref_copies;
}
void release_ref(void *p) {
    if (getword(p, 0)) ++ref_releases;
    setword(p, 0, 0);
}
void vector_append(void *p, const void *row) {
    uintptr_t begin = getword(p, 0), end = getword(p, 8);
    size_t length = end - begin;
    void *next = realloc(((void *) (begin)), length + 0x80);
    assert(next);
    setword(p, 0, ((uintptr_t) (next)));
    setword(p, 8, ((uintptr_t) (next)) + length + 0x80);
    setword(p, 16, ((uintptr_t) (next)) + length + 0x80);
    copy_row(((char *) (next)) + length, row);
}
void vector_destroy(void *p) {
    uintptr_t begin = getword(p, 0), end = getword(p, 8);
    for (uintptr_t at = begin; at != end; at += 0x80) free(((void *) (getword(((void *) (at)), 0x70))));
    free(((void *) (begin)));
    memset(p, 0, 24);
}
void from_vector(void *out, const void *v) {
    ++ref_copies;
    unsigned char *content = calloc(1, 0x50);
    assert(content);
    uintptr_t vector[3] = {0};
    uintptr_t begin = getword(((void *) (v)), 0), end = getword(((void *) (v)), 8);
    for (uintptr_t at = begin; at != end; at += 0x80) vector_append(vector, ((void *) (at)));
    memcpy(content + 0x28, vector, 24);
    if (owned_count == owned_capacity) {
        size_t capacity = owned_capacity ? owned_capacity * 2 : 16;
        void *next = realloc(owned_contents, capacity * sizeof *owned_contents);
        assert(next);
        owned_contents = next;
        owned_capacity = capacity;
    }
    owned_contents[owned_count++] = content;
    setword(out, 0, ((uintptr_t) (content)));
}
bool content_publish(void *set, const void *ref, void *listener) {
    assert(!listener);
    ++publications;
    ++ref_copies;
    ++ref_releases;
    setword(set, 0x28, getword(((void *) (ref)), 0));
    return true;
}
void reply(int result);
bool receipt_in_helper = false, hot_fresh = false, invalidate_reply = false, empty_reply_roots = false, rebuild_reply = false;
int expected_publication = 1;
static bool create_native_cue;
static int native_create(const void *cue, const void *set_ref) {
    void *set = (void *) getword(set_ref, 0);
    uintptr_t content = getword(set, 0x28), vector[3] = {0}, built = 0;
    uintptr_t begin = getword((void *) content, 0x28), end = getword((void *) content, 0x30);
    for (uintptr_t row = begin; row != end; row += 0x80) vector_append(vector, (void *) row);
    vector_append(vector, cue);
    from_vector(&built, vector);
    vector_destroy(vector);
    assert(content_publish(set, &built, NULL));
    release_ref(&built);
    return 0;
}
int native_memory(void *unused_0, const void *unused_1, const void *unused_2, const void *unused_3, const void *unused_4, unsigned unused_5) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    (void) unused_3;
    (void) unused_4;
    (void) unused_5;
    ++helper_calls;
    if (create_native_cue) return native_create(unused_3, unused_4);
    if (receipt_in_helper) {
        unsigned before = ready_calls;
        reply(0);
        registered_entry(payload.bytes);
        assert(ready_calls == before);
    }
    return 1;
}
int native_hot(void *unused_0, const void *unused_1, const void *unused_2, const void *unused_3, const void *unused_4, unsigned unused_5) {
    (void) unused_0;
    (void) unused_1;
    (void) unused_2;
    (void) unused_3;
    (void) unused_4;
    (void) unused_5;
    ++helper_calls;
    if (create_native_cue) return native_create(unused_3, unused_4);
    return hot_fresh ? 0 : 1;
}
static void mix(uint64_t *hash, const void *p, size_t n) {
    __auto_type b = ((const unsigned char *) (p));
    for (size_t i = 0; i < n; ++i) (*hash) = ((*hash) ^ b[i]) * 1099511628211ULL;
}
uint64_t full_hash(const struct azcuepersist_witness *w) {
    uint64_t hash = 1469598103934665603ULL;

    mix(&(hash), w->source_id, 12);
    uintptr_t mb = w->memory_content ? getword(((void *) (w->memory_content)), 0x28) : 0, me = w->memory_content ? getword(((void *) (w->memory_content)), 0x30) : 0;
    uintptr_t hb = w->hot_content ? getword(((void *) (w->hot_content)), 0x28) : 0, he = w->hot_content ? getword(((void *) (w->hot_content)), 0x30) : 0;
    uint64_t count = (me - mb) / 0x80;
    mix(&(hash), &count, 8);
    mix(&(hash), ((void *) (mb)), me - mb);
    mix(&(hash), ((void *) (hb)), he - hb);
    return hash;
}
struct azcuepersist_witness witness() {
    struct azcuepersist_witness w = {0};
    w.source_info = ((uintptr_t) (source_root.bytes));
    w.memory_set = ((uintptr_t) (memory_set.bytes));
    w.hot_set = ((uintptr_t) (hot_set.bytes));
    w.memory_content = getword(memory_set.bytes, 0x28);
    w.hot_content = getword(hot_set.bytes, 0x28);
    w.revision = 1;
    for (unsigned i = 0; i < 12; ++i) w.source_id[i] = i + 1;
    w.fingerprint = full_hash(&(w));
    return w;
}
int submit(const struct azexit_cue_request *r, void *m, void *n, unsigned kind, const void *p, uint64_t cookie) {
    __auto_type w = witness();
    current_operation = r->operation;
    uintptr_t content = kind ? w.hot_content : w.memory_content;
    Bytes locator = {0};
    copy_row(locator.bytes, ((void *) (getword(((void *) (content)), 0x28) + (kind ? kind - 1 : 0) * 0x80)));
    setword(locator.bytes, 8, getword(locator.bytes, 8) + locator_shift_us);
    w.selected_cue = ((uintptr_t) (locator.bytes));
    int result = azsdk_cue_persist_submit(r, m, n, kind, p, cookie, &w);
    destroy_row(locator.bytes);
    return result;
}
struct HelperJob {
    unsigned kind;
    uintptr_t *set;
    int *result;
};
static void *helper_worker(void *opaque) {
    struct HelperJob *j = opaque;
    *j->result = (j->kind ? hot_entry : memory_entry)(NULL, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, j->set, 1);
    return NULL;
}
void run_helper(int expected_result) {
    if (!helper_pending) return;
    helper_pending = false;
    unsigned kind = handler.bytes[0x38];
    uintptr_t set = ((uintptr_t) (kind ? hot_set.bytes : memory_set.bytes));
    int result = -1;
    struct HelperJob job = {kind, &set, &result};
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, helper_worker, &job));
    assert(!pthread_join(worker, NULL));
    assert(result == expected_result);
}
void init_collections() {
    for (unsigned hot = 0; hot < 2; ++hot) {
        uintptr_t v[3] = {0};
        for (unsigned i = 0; i < (hot ? 8u : 10u); ++i) {
            Bytes r = (__typeof__(r)) {0};
            r.bytes[0] = hot ? i + 1 : 0;
            uint32_t rate = 44100;
            memcpy(r.bytes + 4, &rate, 4);
            r.bytes[0x7c] = 3;
            setword(r.bytes, 8, 1000 + i);
            setword(r.bytes, 16, 100 + i);
            setword(r.bytes, 24, 2000 + i);
            setword(r.bytes, 32, 200 + i);
            setword(r.bytes, 0x70, ((uintptr_t) ("preserved")));
            vector_append(v, r.bytes);
        }
        uintptr_t content = 0;
        from_vector(&content, v);
        vector_destroy(v);
        setword(hot ? hot_set.bytes : memory_set.bytes, 0x28, content);
    }
}
uint64_t deadline() {
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t) (t.tv_sec) * 1000000000 + t.tv_nsec + 10000000000;
}
typedef struct {
    void *data;
} NativeString;
extern struct native_result_storage allocate(size_t size);
void allocate_body(void *out, size_t size) {
    ++allocations;
    void *data = malloc(size);
    assert(data);
    memcpy(out, &data, sizeof data);
}
void assign(struct az_native_string *dest, const struct az_native_string *source) {
    void *pointer;
    memcpy(&pointer, source, sizeof pointer);
    free(((void *) (getword(dest, 0))));
    char *copy = strdup(((const char *) (pointer)));
    assert(copy);
    setword(dest, 0, ((uintptr_t) (copy)));
}
void destroy(struct az_native_string *string) {
    ++destructions;
    free(((void *) (getword(string, 0))));
}
void color(void *dest, unsigned r, unsigned g, unsigned b) {
    uint32_t rgb = 0xff000000u | (r << 16) | (g << 8) | b;
    memcpy(dest, &rgb, 4);
}
bool native_handler(void *self) {
    assert(self == handler.bytes);
    ++handler_calls;
    snprintf(observed_comment, sizeof observed_comment, "%s", ((const char *) (((void *) (getword(self, 0xa8))))));
    memcpy(&observed_color, ((unsigned char *) (self)) + 0xb0, 4);
    observed_auto = ((unsigned char *) (self))[0x60];
    return true; /* admission is deliberately not completion */
}
void native_request(void *owner, const void *record) {
    assert(owner == maintainer.bytes);
    ++request_calls;
    memset(handler.bytes, 0, sizeof handler.bytes);
    memset(receiver.bytes, 0, sizeof receiver.bytes);
    setword(handler.bytes, 0, ((uintptr_t) (handler_table)));
    setword(receiver.bytes, 0, ((uintptr_t) (receiver_table)));
    setword(receiver.bytes, 0x10, ((uintptr_t) (owner)));
    uint32_t tag = 44;
    memcpy(handler.bytes + 8, &tag, 4);
    memcpy(receiver.bytes + 8, &tag, 4);
    for (unsigned i = 0; i < 16; ++i) handler.bytes[0x10 + i] = ((unsigned char) (i + 1));
    setword(handler.bytes, 0xc0, ((uintptr_t) (receiver.bytes)));
    setword(handler.bytes, 0xa8, ((uintptr_t) (strdup("preserved"))));
    setword(owner, 0x160, ((uintptr_t) (handler.bytes)));
    unsigned kind = ((const unsigned char *) (record))[4];
    uintptr_t c = getword(kind ? hot_set.bytes : memory_set.bytes, 0x28), row = getword(((void *) (c)), 0x28) + (kind ? kind - 1 : 0) * 0x80;
    free(((void *) (getword(handler.bytes, 0xa8))));
    copy_row(handler.bytes + 0x38, ((void *) (row)));
    uint32_t serial = 1234;
    memcpy(handler.bytes + 0x30, &serial, 4);
    memcpy(helper_request.bytes + 8, &serial, 4);
    if (current_operation == AZ_CMD_SAVED_AUTO || current_operation == AZ_CMD_CUE_COMMENT || current_operation == AZ_CMD_CUE_COLOR || current_operation == AZ_CMD_HOT_SET) {
        /* The firmware's Property conversion loses unrequested stored fields. */
        destroy_row(handler.bytes + 0x38);
        copy_row(handler.bytes + 0x38, ((void *) (row)));
        free(((void *) (getword(handler.bytes, 0xa8))));
        setword(handler.bytes, 0xa8, ((uintptr_t) (strdup(""))));
        uint32_t zero = 0;
        if (current_operation != AZ_CMD_HOT_SET) memcpy(handler.bytes + 0x3c, &zero, 4);
        memcpy(handler.bytes + 0xb0, &zero, 4);
        handler.bytes[0x60] = 0;
        handler.bytes[0xb4] = 0;
    }
    if (current_operation == AZ_CMD_CUE_POINT_SET || current_operation == AZ_CMD_CUE_LOOP_SET) {
        // Both stock serializers drop Property labels before repository work.
        free(((void *) (getword(handler.bytes, 0xa8))));
        setword(handler.bytes, 0xa8, ((uintptr_t) (strdup(""))));
    }
    assert(handler_entry(handler.bytes));
    if (helper_cue_owned) destroy_row(helper_cue.bytes);
    copy_row(helper_cue.bytes, handler.bytes + 0x38);
    helper_cue_owned = true;
    helper_pending = current_operation == AZ_CMD_CUE_COMMENT || current_operation == AZ_CMD_CUE_COLOR || current_operation == AZ_CMD_SAVED_AUTO || current_operation == AZ_CMD_HOT_SET;
}
void produce(void *owner, unsigned kind, const void *prop, bool save) {
    ++producer_calls;
    _Alignas(8) unsigned char record[0x108] = {0};
    record[0] = save;
    memcpy(record + 4, &kind, 4);
    memcpy(record + 8, prop, 0xf8);
    if (invalidate_in_producer) identity_valid = false;
    request_entry(owner, record);
}
void producer(void *owner, unsigned kind, const void *prop) { produce(owner, kind, prop, true); }
void erase(void *owner, unsigned kind, const void *prop) { produce(owner, kind, prop, false); }
void native_reply(void *self, const void *unused_1, const void *track, const void *unused_3, const void *unused_4, const int *result) {
    (void) unused_1;
    (void) unused_3;
    (void) unused_4;
    memset(payload.bytes, 0, sizeof payload.bytes);
    setword(payload.bytes, 0, getword(self, 0x10));
    memcpy(payload.bytes + 8, result, 4);
    memcpy(payload.bytes + 12, ((unsigned char *) (self)) + 8, 4);
    memcpy(payload.bytes + 16, track, 16);
}
void native_registered(void *self) {
    assert(self == payload.bytes);
    ++task_calls;
    if (getword(self, 0) != ((uintptr_t) (maintainer.bytes))) return;
    if (getword(maintainer.bytes, 0x160)) {
        free(((void *) (getword(handler.bytes, 0xa8))));
        setword(handler.bytes, 0xa8, 0);
    }
    setword(maintainer.bytes, 0x158, 0);
    setword(maintainer.bytes, 0x160, 0);
}
void *install(uintptr_t address, void *hook, void **original) {
    *original = ((void *) (address));
    if (address == ((uintptr_t) (native_request))) request_entry = ((__typeof__(request_entry)) (hook));
    else if (address == ((uintptr_t) (native_handler))) handler_entry = ((__typeof__(handler_entry)) (hook));
    else if (address == ((uintptr_t) (native_reply))) reply_entry = ((__typeof__(reply_entry)) (hook));
    else if (address == ((uintptr_t) (native_registered))) {
        registered_entry = ((__typeof__(registered_entry)) (hook));
        unsigned before = task_calls;
        registered_entry(payload.bytes); /* callback can arrive before configure publishes enabled */
        assert(task_calls == before + 1 && !ready_calls && !azsdk_cue_persist_capabilities());
    } else if (address == ((uintptr_t) (native_memory))) memory_entry = ((Helper) (hook));
    else if (address == ((uintptr_t) (native_hot))) hot_entry = ((Helper) (hook));
    else assert(false);
    return hook;
}
int validate(const struct azexit_cue_request *unused_0, void *owner, uint64_t cookie, void *unused_3) {
    (void) unused_0;
    (void) unused_3;
    return identity_valid && owner == maintainer.bytes && cookie == 7;
}
int confirm(const struct azexit_cue_request *r, void *unused_1, uint64_t unused_2, uint64_t *revision, void *unused_4) {
    if (confirm_raises) fixture_raise();
    (void) unused_1;
    (void) unused_2;
    (void) unused_4;
    ++confirm_calls;
    if (publication_checks) {
        __auto_type fresh = witness();
        fresh.revision = 91;
        assert(azsdk_cue_persist_confirm_publication(r, &fresh) == expected_publication);
        __auto_type wrong = fresh;
        wrong.source_id[0] ^= 1;
        assert(!azsdk_cue_persist_confirm_publication(r, &wrong));
        struct azexit_cue_request wrong_request = *r;
        ++wrong_request.entry_id;
        assert(!azsdk_cue_persist_confirm_publication(&wrong_request, &fresh));
        /* A genuine native publication with identical logical rows is still
         * foreign: it is not the content retained before this request published. */
        for (unsigned hot = 0; hot < 2; ++hot) {
            void *root = hot ? ((void *) (hot_set.bytes)) : ((void *) (memory_set.bytes));
            uintptr_t original_content = getword(root, 0x28), vector[3];
            memcpy(vector, ((void *) (original_content + 0x28)), 24);
            uintptr_t foreign = 0;
            from_vector(&foreign, vector);
            content_publish(root, &foreign, NULL);
            release_ref(&foreign);
            __auto_type foreign_witness = witness();
            foreign_witness.revision = 92;
            assert(azsdk_cue_persist_confirm_publication(r, &foreign_witness) == 2);
            content_publish(root, &original_content, NULL);
        }
        fresh = witness();
        fresh.revision = 93;
        assert(azsdk_cue_persist_confirm_publication(r, &fresh) == expected_publication);
    }
    *revision = 91;
    return effect_confirmed;
}
void ready(unsigned deck, uint64_t token, void *unused_2) {
    (void) unused_2;
    assert(deck == 0 && token);
    ++ready_calls;
}
struct azexit_cue_request request(unsigned operation, uint64_t token) {
    struct azexit_cue_request value = {0};
    value.size = sizeof value;
    value.operation = operation;
    value.identity = (struct azexit_request) {token, 3, deadline(), 0, 0};
    value.entry_id = 1;
    value.expected_revision = 1;
    value.flags = AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD;
    return value;
}
void reply(int result) {
    unsigned char track[16];
    for (unsigned i = 0; i < 16; ++i) track[i] = ((unsigned char) (i + 1));
    uintptr_t memory_root = ((uintptr_t) (memory_set.bytes)), hot_root = ((uintptr_t) (hot_set.bytes));
    uintptr_t memory_content = getword(memory_set.bytes, 0x28), hot_content = getword(hot_set.bytes, 0x28);
    if (rebuild_reply) {
        for (unsigned hot = 0; hot < 2; ++hot) {
            void *root = hot ? ((void *) (hot_set.bytes)) : ((void *) (memory_set.bytes));
            uintptr_t old = getword(root, 0x28), vector[3], fresh = 0;
            memcpy(vector, ((void *) (old + 0x28)), 24);
            from_vector(&fresh, vector);
            content_publish(root, &fresh, NULL);
            release_ref(&fresh);
        }
    }
    if (invalidate_reply) {
        setword(memory_set.bytes, 0x28, 0);
        setword(hot_set.bytes, 0x28, 0);
    }
    if (empty_reply_roots) memory_root = hot_root = 0;
    reply_entry(receiver.bytes, helper_request.bytes, track, &memory_root, &hot_root, &result);
    if (invalidate_reply) {
        setword(memory_set.bytes, 0x28, memory_content);
        setword(hot_set.bytes, 0x28, hot_content);
    }
}
static void *response_worker(void *opaque) {
    reply(*(int *) opaque);
    return NULL;
}
static void *task_worker(void *opaque) {
    (void) opaque;
    registered_entry(payload.bytes);
    return NULL;
}
void finish(struct azexit_cue_request *r, unsigned outcome, int native_result) {
    bool had_helper = helper_pending;
    run_helper(0);
    if (had_helper) {
        unsigned kind = handler.bytes[0x38];
        uintptr_t content = getword(kind ? hot_set.bytes : memory_set.bytes, 0x28);
        uintptr_t begin = getword(((void *) (content)), 0x28), end = getword(((void *) (content)), 0x30);
        assert((end - begin) / 0x80 == (kind ? 8u : 10u));
        __auto_type row = ((unsigned char *) (begin + (kind ? kind - 1 : 0) * 0x80));
        uint32_t source_rate;
        memcpy(&source_rate, property.bytes + 0x60, 4);
        assert(azcue_timestamps_equal(azcue_read_timestamps(row), azcue_read_timestamps(handler.bytes + 0x38), source_rate));
        if (r->operation == AZ_CMD_CUE_COMMENT) assert(!strcmp(((char *) (getword(row, 0x70))), r->comment));
        if (r->operation == AZ_CMD_CUE_COLOR || r->operation == AZ_CMD_HOT_SET) {
            uint32_t rgb;
            memcpy(&rgb, row + 0x78, 4);
            assert(rgb == (0xff000000u | r->rgb));
        }
        if (r->operation == AZ_CMD_SAVED_AUTO) assert(row[0x28] == ((unsigned char) (r->position_s)));
        assert(!strcmp(((const char *) (getword(helper_cue.bytes, 0x70))), ((const char *) (getword(row, 0x70)))));
        assert(!memcmp(helper_cue.bytes, row, 8) && !memcmp(helper_cue.bytes + 0x28, row + 0x28, 8));
        assert(azcue_timestamps_equal(azcue_read_timestamps(helper_cue.bytes), azcue_read_timestamps(row), source_rate));
        assert(!memcmp(helper_cue.bytes + 0x78, row + 0x78, 5));
    }
    pthread_t response;
    assert(!pthread_create(&response, NULL, response_worker, &native_result));
    assert(!pthread_join(response, NULL));
    unsigned previous = ready_calls;
    assert(azsdk_cue_persist_poll(r, maintainer.bytes, ((struct azexit_result *) (property.bytes + 0x100))) == AZ_PENDING);
    pthread_t task;
    assert(!pthread_create(&task, NULL, task_worker, NULL));
    assert(!pthread_join(task, NULL));
    assert(ready_calls == previous + 1);
    struct azexit_result out = (__typeof__(out)) {0};
    assert(azsdk_cue_persist_poll(r, maintainer.bytes, &out) == AZ_OK && out.outcome == outcome);
    assert(out.token == r->identity.token && out.load_generation == 3);
    assert(out.revision == (outcome == AZ_COMMAND_APPLIED ? 91u : 0u));
}

static void poll_raising(void *request) {
    struct azexit_result result = {0};
    (void) azsdk_cue_persist_poll(request, maintainer.bytes, &result);
}
int main() {
    struct azcuepersist_profile profile = {0};
    profile.save = ((uintptr_t) (producer));
    profile.erase = ((uintptr_t) (erase));
    profile.request = ((uintptr_t) (native_request));
    profile.handler_request = ((uintptr_t) (native_handler));
    profile.reception_reply = ((uintptr_t) (native_reply));
    profile.registered_task = ((uintptr_t) (native_registered));
    profile.handler_table = ((uintptr_t) (handler_table));
    profile.reception_table = ((uintptr_t) (receiver_table));
    profile.string_allocate = ((uintptr_t) (allocate));
    profile.string_assign = ((uintptr_t) (assign));
    profile.string_destroy = ((uintptr_t) (destroy));
    profile.color_construct = ((uintptr_t) (color));
    profile.cue_copy = ((uintptr_t) (copy_row));
    profile.cue_destroy = ((uintptr_t) (destroy_row));
    profile.memory_helper = ((uintptr_t) (native_memory));
    profile.hot_helper = ((uintptr_t) (native_hot));
    profile.content_copy = ((uintptr_t) (copy_ref));
    profile.content_release = ((uintptr_t) (release_ref));
    profile.vector_append = ((uintptr_t) (vector_append));
    profile.vector_destroy = ((uintptr_t) (vector_destroy));
    profile.content_from_vector = ((uintptr_t) (from_vector));
    profile.hot_content_from_vector = ((uintptr_t) (from_vector));
    profile.content_publish = ((uintptr_t) (content_publish));
    profile.hot_content_publish = ((uintptr_t) (content_publish));
    init_collections();
    struct azcuepersist_callbacks callbacks = {validate, confirm, ready, NULL};
    assert(!azsdk_cue_persist_capabilities());
    assert(azsdk_cue_persist_configure(&profile, install, &callbacks));
    assert(!azsdk_cue_persist_configure(&profile, install, &callbacks));
    setword(maintainer.bytes, 0x150, ((uintptr_t) (queue.bytes)));
    for (unsigned i = 0; i < 12; ++i) property.bytes[0x30 + i] = ((unsigned char) (i + 7));
    setword(property.bytes, 0x40, 12345);
    uint64_t token = 1;
    {
        unsigned range_values_1[] = {44100u, 48000u, 96000u, 192000u};
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(range_values_1) / sizeof(range_values_1)[0]; ++range_index_1) {
            unsigned rate = range_values_1[range_index_1];
            {
                int range_values_2[] = {-1, 1};
                for (unsigned range_index_2 = 0; range_index_2 < sizeof(range_values_2) / sizeof(range_values_2)[0]; ++range_index_2) {
                    int direction = range_values_2[range_index_2];

                    memcpy(property.bytes + 0x60, &rate, 4);
                    __auto_type r = request(AZ_CMD_CUE_COMMENT, token++);
                    strcpy(r.comment, "within source sample window");
                    uint64_t inside = 1000000 / rate;
                    locator_shift_us = direction * (int64_t) (inside);
                    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
                    locator_shift_us = 0;
                    setword(helper_cue.bytes, 8, getword(helper_cue.bytes, 8) + direction * inside);
                    setword(helper_cue.bytes, 24, getword(helper_cue.bytes, 24) - direction * inside);
                    finish(&(r), AZ_COMMAND_APPLIED, 0);
                    r = request(AZ_CMD_CUE_COMMENT, token++);
                    strcpy(r.comment, "outside source sample window");
                    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
                    setword(helper_cue.bytes, 8, getword(helper_cue.bytes, 8) + direction * (inside + 1));
                    run_helper(1);
                    finish(&(r), AZ_COMMAND_UNCERTAIN, 0);
                }
            }
        }
    }
    unsigned property_rate = 44100;
    memcpy(property.bytes + 0x60, &property_rate, 4);
    {
        unsigned range_values_3[] = {AZ_CMD_HOT_SET, AZ_CMD_HOT_CLEAR, AZ_CMD_MEMORY_SET, AZ_CMD_MEMORY_DELETE,
                                     AZ_CMD_SAVED_SAVE, AZ_CMD_SAVED_DELETE, AZ_CMD_SAVED_AUTO, AZ_CMD_CUE_COMMENT, AZ_CMD_CUE_COLOR};
        for (unsigned range_index_3 = 0; range_index_3 < sizeof(range_values_3) / sizeof(range_values_3)[0]; ++range_index_3) {
            unsigned operation = range_values_3[range_index_3];

            assert(azsdk_cue_persist_capabilities() & (UINT64_C(1) << operation));
            __auto_type r = request(operation, token++);
            r.rgb = operation == AZ_CMD_CUE_COLOR ? 0xff7100 : 0x123456;
            if (operation == AZ_CMD_CUE_COMMENT) strcpy(r.comment, "caf\xc3\xa9 \xf0\x9f\x8e\xb5");
            if (operation == AZ_CMD_SAVED_AUTO) r.position_s = 1;
            unsigned kind = (operation == AZ_CMD_HOT_SET || operation == AZ_CMD_HOT_CLEAR) ? 1 : 0;
            unsigned before = producer_calls;
            assert(submit(&r, maintainer.bytes, maintainer.bytes, kind, property.bytes, 7) == AZ_PENDING);
            assert(producer_calls == before + 1);
            struct azexit_result out = {999, 999, 999, 999, 999};
            assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &out) == AZ_PENDING && out.token == 999);
            assert(submit(&r, maintainer.bytes, maintainer.bytes, kind, property.bytes, 7) == AZ_BUSY);
            if (operation == AZ_CMD_CUE_COMMENT) assert(!strcmp(observed_comment, r.comment) && allocations == destructions);
            if (operation == AZ_CMD_CUE_COLOR || operation == AZ_CMD_HOT_SET) assert(observed_color == (0xff000000u | r.rgb));
            if (operation == AZ_CMD_SAVED_AUTO) assert(observed_auto == 1);
        /* Unrelated native receipts with identical result/tag/track must not
         * complete this SDK request or consume its owned receipt. */
            memcpy(foreign_receiver.bytes, receiver.bytes, sizeof receiver.bytes);
            setword(foreign_receiver.bytes, 0x10, ((uintptr_t) (queue.bytes)));
            unsigned notifications = ready_calls;
            int result = 0;
            reply_entry(foreign_receiver.bytes, NULL, handler.bytes + 0x10, NULL, NULL, &result);
            registered_entry(payload.bytes);
            assert(ready_calls == notifications);
            finish(&(r), AZ_COMMAND_APPLIED, 0);
        }
    }
    {
        int range_values_4[] = {3, 5};
        for (unsigned range_index_4 = 0; range_index_4 < sizeof(range_values_4) / sizeof(range_values_4)[0]; ++range_index_4) {
            int result = range_values_4[range_index_4];

            __auto_type result_request = request(AZ_CMD_SAVED_SAVE, token++);
            assert(submit(&result_request, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
            finish(&(result_request), result == 5 ? AZ_COMMAND_APPLIED : AZ_COMMAND_UNCERTAIN, result);
        }
    }
    __auto_type r = request(AZ_CMD_CUE_COMMENT, token++);
    strcpy(r.comment, "\xc0\x80");
    unsigned before = producer_calls;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_INVALID && producer_calls == before);
    r = request(AZ_CMD_SAVED_SAVE, token++);
    setword(queue.bytes, 0x68, 8);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_BUSY && producer_calls == before);
    setword(queue.bytes, 0x68, 0);
    identity_valid = false;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_STALE && producer_calls == before);
    identity_valid = true;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    effect_confirmed = false;
    reply(0);
    registered_entry(payload.bytes);
    struct azexit_result delayed = {999, 999, 999, 999, 999};
    unsigned submissions = producer_calls, notifications = ready_calls;
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &delayed) == AZ_PENDING && delayed.token == 999);
    assert(producer_calls == submissions && ready_calls == notifications);
    confirm_raises = true;
    assert(!fixture_native_try(poll_raising, &r));
    assert(fixture_exception_deletions == 1);
    confirm_raises = false;
    effect_confirmed = true;
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &delayed) == AZ_OK && delayed.outcome == AZ_COMMAND_APPLIED);
    assert(producer_calls == submissions && delayed.revision == 91);

    r = request(AZ_CMD_SAVED_SAVE, token++);
    r.identity.deadline_ns = deadline() - UINT64_C(10000000000) + UINT64_C(50000000);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    reply(0);
    registered_entry(payload.bytes);
    effect_confirmed = false;
    submissions = producer_calls;
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &delayed) == AZ_PENDING);
    usleep(60000);
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &delayed) == AZ_OK && delayed.outcome == AZ_COMMAND_UNCERTAIN && !delayed.revision);
    assert(producer_calls == submissions);
    effect_confirmed = true;
    r = request(AZ_CMD_SAVED_SAVE, token++);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    azsdk_cue_persist_retire(maintainer.bytes);
    unsigned dispatches = handler_calls;
    assert(!handler_entry(handler.bytes) && handler_calls == dispatches);
    struct azexit_result abandoned = {0};
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_BUSY);
    reply(0);
    registered_entry(payload.bytes); /* retired slot drains without owner polling */
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &abandoned) == AZ_STALE);
    r = request(AZ_CMD_SAVED_SAVE, token++);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    finish(&(r), AZ_COMMAND_APPLIED, 0);
    r = request(AZ_CMD_CUE_COMMENT, token++);
    unsigned actual_requests = request_calls, actual_handlers = handler_calls;
    invalidate_in_producer = true;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_STALE);
    assert(request_calls == actual_requests && handler_calls == actual_handlers);
    invalidate_in_producer = false;
    identity_valid = true;
    r = request(AZ_CMD_SAVED_SAVE, token++);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    finish(&(r), AZ_COMMAND_APPLIED, 0);
    assert(request_calls + 1 == producer_calls && task_calls >= 11 && confirm_calls >= 10);
    assert(publications >= 4);
    {
        unsigned range_values_5[] = {AZ_CMD_CUE_COMMENT, AZ_CMD_CUE_COLOR};
        for (unsigned range_index_5 = 0; range_index_5 < sizeof(range_values_5) / sizeof(range_values_5)[0]; ++range_index_5) {
            unsigned operation = range_values_5[range_index_5];

            r = request(operation, token++);
            r.rgb = 0xaabbcc;
            strcpy(r.comment, "hot unicode \xe2\x9c\x93");
            assert(submit(&r, maintainer.bytes, maintainer.bytes, 1, property.bytes, 7) == AZ_PENDING);
            finish(&(r), AZ_COMMAND_APPLIED, 0);
        }
    }
    /* All collection updates execute later on a repository worker, with no
     * submitting TLS. Foreign request IDs and tracks forward unchanged. */
    r = request(AZ_CMD_CUE_COMMENT, token++);
    strcpy(r.comment, "async unicode \xe2\x9c\x93");
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    uintptr_t memory = ((uintptr_t) (memory_set.bytes));
    unsigned saved_publications = publications;
    uint32_t old_serial;
    memcpy(&old_serial, helper_request.bytes + 8, 4);
    uint32_t wrong_serial = old_serial + 1;
    memcpy(helper_request.bytes + 8, &wrong_serial, 4);
    assert(memory_entry(NULL, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &memory, 1) == 1 && publications == saved_publications);
    memcpy(helper_request.bytes + 8, &old_serial, 4);
    handler.bytes[0x10] ^= 1;
    assert(memory_entry(NULL, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &memory, 1) == 1 && publications == saved_publications);
    handler.bytes[0x10] ^= 1;
    finish(&(r), AZ_COMMAND_APPLIED, 0);
    for (unsigned failure = 0; failure < 9; ++failure) {
        r = request(AZ_CMD_CUE_COLOR, token++);
        r.rgb = 0x00c3ff;
        if (failure == 0) r.identity.deadline_ns = deadline() - 10000000000ULL + 50000000ULL;
        uintptr_t content = getword(memory_set.bytes, 0x28), begin = getword(((void *) (content)), 0x28);
        uint64_t saved_in = getword(((void *) (begin + 0x80)), 8), saved_out = getword(((void *) (begin + 0x80)), 16);
        uint64_t saved_out_usec = getword(((void *) (begin + 0x80)), 24), saved_out_sample = getword(((void *) (begin + 0x80)), 32);
        if (failure == 2) {
            setword(((void *) (begin + 0x80)), 8, getword(((void *) (begin)), 8));
            setword(((void *) (begin + 0x80)), 16, getword(((void *) (begin)), 16));
            setword(((void *) (begin + 0x80)), 24, getword(((void *) (begin)), 24));
            setword(((void *) (begin + 0x80)), 32, getword(((void *) (begin)), 32));
        }
        int submitted = submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7);
        if (failure == 2) {
            assert(submitted == AZ_STALE);
            setword(((void *) (begin + 0x80)), 8, saved_in);
            setword(((void *) (begin + 0x80)), 16, saved_out);
            setword(((void *) (begin + 0x80)), 24, saved_out_usec);
            setword(((void *) (begin + 0x80)), 32, saved_out_sample);
            continue;
        }
        assert(submitted == AZ_PENDING);
        saved_publications = publications;
        if (failure == 0) usleep(60000);
        if (failure == 1) {
            __auto_type c = ((unsigned char *) (getword(hot_set.bytes, 0x28)));
            __auto_type row = ((unsigned char *) (getword(c, 0x28)));
            row[0x28] ^= 1;
        }
        if (failure == 3) setword(helper_cue.bytes, 8, getword(helper_cue.bytes, 8) + 1000000 / property_rate + 1);
        if (failure >= 6) {
            unsigned offset = failure == 6 ? 16 : failure == 7 ? 24
                                                               : 32;
            setword(helper_cue.bytes, offset, getword(helper_cue.bytes, offset) + (offset == 24 ? 1000000 / property_rate + 1 : 1));
        }
        uintptr_t old_hot_content = getword(hot_set.bytes, 0x28);
        if (failure == 4) setword(hot_set.bytes, 0x28, content);
        if (failure == 5) identity_valid = false;
        helper_pending = false;
        assert(memory_entry(NULL, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &memory, 1) == 1 && publications == saved_publications);
        if (failure == 1) {
            __auto_type c = ((unsigned char *) (getword(hot_set.bytes, 0x28)));
            __auto_type row = ((unsigned char *) (getword(c, 0x28)));
            row[0x28] ^= 1;
        }
        if (failure == 4) setword(hot_set.bytes, 0x28, old_hot_content);
        identity_valid = true;
        if (failure == 2) {
            setword(((void *) (begin + 0x80)), 8, saved_in);
            setword(((void *) (begin + 0x80)), 16, saved_out);
            setword(((void *) (begin + 0x80)), 24, saved_out_usec);
            setword(((void *) (begin + 0x80)), 32, saved_out_sample);
        }
        finish(&(r), AZ_COMMAND_UNCERTAIN, 5);
    }

    /* Distinct OUT points remain distinct even with identical IN coordinates. */
    uintptr_t content = getword(memory_set.bytes, 0x28), begin = getword(((void *) (content)), 0x28);
    uint64_t saved_in = getword(((void *) (begin + 0x80)), 8), saved_sample = getword(((void *) (begin + 0x80)), 16);
    setword(((void *) (begin + 0x80)), 8, getword(((void *) (begin)), 8));
    setword(((void *) (begin + 0x80)), 16, getword(((void *) (begin)), 16));
    uint64_t other_out = getword(((void *) (begin + 0x80)), 24), other_out_sample = getword(((void *) (begin + 0x80)), 32);
    r = request(AZ_CMD_CUE_COMMENT, token++);
    strcpy(r.comment, "exact complete tuple");
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    finish(&(r), AZ_COMMAND_APPLIED, 0);
    content = getword(memory_set.bytes, 0x28);
    begin = getword(((void *) (content)), 0x28);
    assert(getword(((void *) (begin + 0x80)), 24) == other_out && getword(((void *) (begin + 0x80)), 32) == other_out_sample);
    setword(((void *) (begin + 0x80)), 8, saved_in);
    setword(((void *) (begin + 0x80)), 16, saved_sample);
    /* A receipt arriving while the helper owns the slot must wait until all
     * native clone refs are released, then issue exactly one continuation. */
    r = request(AZ_CMD_CUE_COMMENT, token++);
    strcpy(r.comment, "receipt while owned");
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    receipt_in_helper = true;
    unsigned previous_ready = ready_calls;
    run_helper(1);
    receipt_in_helper = false;
    assert(ready_calls == previous_ready + 1);
    struct azexit_result concurrent = {0};
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &concurrent) == AZ_OK && concurrent.outcome == AZ_COMMAND_UNCERTAIN);
    /* Native successful fresh Hot insertion is forwarded with no clone. */
    r = request(AZ_CMD_HOT_SET, token++);
    r.expected_revision = 0;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 1, property.bytes, 7) == AZ_PENDING);
    hot_fresh = true;
    helper_pending = false;
    saved_publications = publications;
    uintptr_t hot_root = ((uintptr_t) (hot_set.bytes));
    assert(hot_entry(NULL, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &hot_root, 1) == 0 && publications == saved_publications);
    hot_fresh = false;
    finish(&(r), AZ_COMMAND_APPLIED, 0);
    r = request(AZ_CMD_CUE_COMMENT, token++);
    __auto_type stale_witness = witness();
    stale_witness.fingerprint ^= 1;
    unsigned before_producer = producer_calls;
    assert(azsdk_cue_persist_submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7, &stale_witness) == AZ_STALE && producer_calls == before_producer);
    r = request(AZ_CMD_SAVED_AUTO, token++);
    r.position_s = 0;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    __auto_type before_publication = witness();
    assert(!azsdk_cue_persist_confirm_publication(&r, &before_publication));
    publication_checks = true;
    rebuild_reply = true;
    finish(&(r), AZ_COMMAND_APPLIED, 0);
    rebuild_reply = false;
    publication_checks = false;
    r = request(AZ_CMD_SAVED_AUTO, token++);
    r.position_s = 1;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    publication_checks = true;
    invalidate_reply = true;
    expected_publication = 2;
    finish(&(r), AZ_COMMAND_APPLIED, 0);
    publication_checks = false;
    invalidate_reply = false, empty_reply_roots = false;
    expected_publication = 1;
    r = request(AZ_CMD_SAVED_AUTO, token++);
    r.position_s = 0;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    publication_checks = true;
    empty_reply_roots = true;
    expected_publication = 2;
    finish(&(r), AZ_COMMAND_APPLIED, 0);
    publication_checks = false;
    empty_reply_roots = false;
    expected_publication = 1;
    r = request(AZ_CMD_CUE_COLOR, token++);
    r.rgb = 0x2ba6d9;
    unsigned rejected_before = producer_calls;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_UNSUPPORTED && producer_calls == rejected_before);
    {
        unsigned range_values_6[] = {0x000000u, 0xff00c6u, 0xff0000u, 0xff7100u, 0xffff00u, 0x00e300u, 0x00c3ffu, 0x0051ffu, 0x9c08ffu};
        for (unsigned range_index_6 = 0; range_index_6 < sizeof(range_values_6) / sizeof(range_values_6)[0]; ++range_index_6) {
            unsigned rgb = range_values_6[range_index_6];

            r = request(AZ_CMD_CUE_COLOR, token++);
            r.rgb = rgb;
            assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
            finish(&(r), AZ_COMMAND_APPLIED, 0);
        }
    }
    // Independently exercise the production repository guard. Even if an owner
    // snapshot were stale, creation cannot call the native write helper on an
    // occupied Hot slot or an already-full Memory collection.
    {
        unsigned range_values_7[] = {0u, 1u};
        for (unsigned range_index_7 = 0; range_index_7 < sizeof(range_values_7) / sizeof(range_values_7)[0]; ++range_index_7) {
            unsigned kind = range_values_7[range_index_7];

            r = request(AZ_CMD_CUE_POINT_SET, token++);
            r.entry_id = kind;
            r.classification = kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
            r.flags |= AZ_COMMAND_NATIVE_CUE_IDS;
            r.position_s = 4;
            strcpy(r.comment, "Create UTF8 \342\234\223");
            r.rgb = 0x00c3ff;
            assert(submit(&r, maintainer.bytes, maintainer.bytes, kind, property.bytes, 7) == AZ_PENDING);
            assert(!strcmp(observed_comment, r.comment) && observed_color == (0xff000000u | r.rgb) && allocations == destructions);
            helper_pending = true;
            unsigned before_calls = helper_calls;
            run_helper(1);
            assert(helper_calls == before_calls);
            finish(&(r), AZ_COMMAND_UNCERTAIN, 0);
        }
    }
    // Missing baseline roots may bind only a newly created EMPTY native set.
    // A raced nonempty repository set must refuse before its native helper.
    r = request(AZ_CMD_CUE_POINT_SET, token++);
    r.entry_id = 1;
    r.classification = AZ_CUE_HOT;
    r.flags |= AZ_COMMAND_NATIVE_CUE_IDS;
    r.position_s = 4;
    __auto_type absent = witness();
    absent.memory_set = absent.hot_set = absent.memory_content = absent.hot_content = 0;
    absent.fingerprint = full_hash(&(absent));
    current_operation = r.operation;
    assert(azsdk_cue_persist_submit(&r, maintainer.bytes, maintainer.bytes, 1, property.bytes, 7, &absent) == AZ_PENDING);
    helper_pending = true;
    unsigned before_calls = helper_calls;
    run_helper(1);
    assert(helper_calls == before_calls);
    finish(&(r), AZ_COMMAND_UNCERTAIN, 0);
    // The exact first native publication is admitted when its newly bound roots
    // are still empty; the helper's own receipt must retain that published content.
    r = request(AZ_CMD_CUE_POINT_SET, token++);
    r.entry_id = 1;
    r.classification = AZ_CUE_HOT;
    r.flags |= AZ_COMMAND_NATIVE_CUE_IDS;
    strcpy(r.comment, "first Hot cue");
    r.rgb = 0x123456;
    absent = witness();
    absent.memory_set = absent.hot_set = absent.memory_content = absent.hot_content = 0;
    absent.fingerprint = full_hash(&absent);
    current_operation = r.operation;
    assert(azsdk_cue_persist_submit(&r, maintainer.bytes, maintainer.bytes, 1, property.bytes, 7, &absent) == AZ_PENDING);
    for (unsigned hot = 0; hot < 2; ++hot) {
        uintptr_t empty_vector[3] = {0}, empty_content = 0;
        from_vector(&empty_content, empty_vector);
        content_publish(hot ? hot_set.bytes : memory_set.bytes, &empty_content, NULL);
        release_ref(&empty_content);
    }
    create_native_cue = true;
    helper_pending = true;
    before_calls = helper_calls;
    run_helper(0);
    create_native_cue = false;
    assert(helper_calls == before_calls + 1);
    uintptr_t first_content = getword(hot_set.bytes, 0x28);
    uintptr_t first_row = getword((void *) first_content, 0x28);
    assert(getword((void *) first_content, 0x30) - first_row == 0x80);
    assert(*(unsigned char *) first_row == 1);
    assert(!strcmp((char *) getword((void *) first_row, 0x70), r.comment));
    assert((uint32_t) getword((void *) first_row, 0x78) == (0xff000000u | r.rgb));
    publication_checks = true;
    finish(&r, AZ_COMMAND_APPLIED, 0);
    publication_checks = false;
    if (helper_cue_owned) destroy_row(helper_cue.bytes);
    for (size_t i = 0; i < owned_count; ++i) {
        unsigned char *content = owned_contents[i];
        uintptr_t v[3];
        memcpy(v, content + 0x28, 24);
        vector_destroy(v);
        free(content);
    }
    free(owned_contents);
    ref_releases += 2; /* final fixture publication roots */
    assert(ref_copies == ref_releases);
}

void azsdk_cue_commands_persist_retired(unsigned unused_0, uint64_t unused_1) {
    (void) unused_0;
    (void) unused_1;
}
