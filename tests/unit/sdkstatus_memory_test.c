/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/azstem/sdk_status_memory.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <pthread.h>
#include <sched.h>


typedef struct Object {
    _Alignas(8) unsigned char bytes[0x188];
} Object;
#define put(address, value)                                       \
    do {                                                          \
        __auto_type put_value = (value);                          \
        memcpy((void *) (address), &put_value, sizeof put_value); \
    } while (0)
Object pack = {0}, foreign_pack = {0}, holder = {0}, foreign_holder = {0}, alpha = {0}, beta = {0}, rows_a = {0}, rows_b = {0};
uint64_t fence = 7, load = 12, unit = 3;
unsigned refs = 0, releases = 0, offer_count = 0, lost = 0, caps = 0;
_Atomic(unsigned) wake_count = 0;
struct azstatus_native_receipt receipts[32];
_Atomic(bool) stall = false, entered = false, resume = false;
bool nested = false;
unsigned input_begins = 0, input_opens = 0, input_mask = 0, input_cancels = 0;
int input_holds = 0;
FILE *input_file = NULL;
bool input_cache_fresh = true;
uintptr_t input_cache_object = 0, input_cache_control = 0;
uintptr_t common_handle = 0, common_path = 0;
uintptr_t common_other_handle = 0, common_other_path = 0;
unsigned common_begins = 0, common_seals = 0, common_joins = 0, common_mask = 1;
unsigned common_reopen_mode = 0, common_open_calls = 0;
int common_holds = 0;
uint64_t common_begin(const char *path, size_t bytes, const uint16_t *audio, size_t units) {
    assert(path && bytes == strlen(path) && !audio && !units);
    ++common_begins;
    common_open_calls = 0;
    return common_begins;
}
unsigned common_opened(uint64_t read, int descriptor) {
    assert(read == common_begins && input_file && descriptor == fileno(input_file));
    ++common_open_calls;
    if (common_open_calls == 2 && common_reopen_mode == 3) return 0;
    if (common_open_calls == 2 && common_reopen_mode == 6) return common_mask == 1 ? 2 : 1;
    return common_mask;
}
uint64_t common_seal(uint64_t read, int success) {
    assert(read == common_begins);
    ++common_seals;
    if (!success) return 0;
    ++common_holds;
    return 2000 + read;
}
void common_hold(const uint64_t *tokens, size_t count, int acquire) {
    for (size_t i = 0; i < count; ++i)
        if (tokens[i]) common_holds += acquire ? 1 : -1;
    assert(common_holds >= 0);
}
uint64_t common_join(const uint64_t *tokens) {
    assert(tokens && (tokens[0] || tokens[1]));
    ++common_joins;
    return 9000;
}
const struct azn_file_input_api *common_api() {
    static const struct azn_file_input_api api = {sizeof api, 1, common_begin, common_opened, common_seal, common_hold, common_join};
    return &api;
}
int input_fresh_original(uintptr_t unused_0 __attribute__((unused)), uintptr_t output, uintptr_t unused_2 __attribute__((unused))) {
    if (common_handle && file_fresh_input) {
        int error = 0;
        unsigned flags = common_reopen_mode == 7 ? 99 : 1;
        assert(file_input_open(common_handle, common_path, flags, &error) == (flags == 99 ? 7u : 0u));
        flags = common_reopen_mode == 4 ? 99 : 1;
        assert(file_input_open(common_reopen_mode == 1 ? common_other_handle : common_handle,
                               common_reopen_mode == 2 ? common_other_path : common_path,
                               flags, &error) == (flags == 99 ? 7u : 0u));
        if (common_reopen_mode == 5) assert(file_input_open(common_handle, common_path, 1, &error) == 0);
    }
    put(output, input_cache_object);
    put(output + 8, input_cache_control);
    return 0;
}
int input_cache_original(uintptr_t owner, uintptr_t output, uintptr_t extension) {
    if (input_cache_fresh) return file_fresh_load(owner, output, extension);
    return input_fresh_original(owner, output, extension);
}
uint64_t input_begin(const char *unused_0 __attribute__((unused)), size_t unused_1 __attribute__((unused)), const uint16_t *unused_2 __attribute__((unused)), size_t unused_3 __attribute__((unused)), const uint16_t *unused_4 __attribute__((unused)), size_t unused_5 __attribute__((unused))) {
    ++input_begins;
    input_mask = 0;
    return input_begins;
}
unsigned input_opened(uint64_t read, int descriptor) {
    assert(read == input_begins && input_file && descriptor == fileno(input_file));
    const unsigned bit = input_opens == 1 ? 1 : 2;
    input_mask |= bit;
    return bit;
}
void input_complete(uint64_t read, const struct azn_file_cue_row *unused_1 __attribute__((unused)), size_t count, uint64_t *tokens) {
    assert(read == input_begins);
    if (!count) ++input_cancels;
    if (input_mask == 3)
        for (size_t i = 0; i < count; ++i) tokens[i] = 111 + i;
}
void input_hold(const uint64_t *tokens, size_t count, int acquire) {
    for (size_t i = 0; i < count; ++i)
        if (tokens[i]) input_holds += acquire ? 1 : -1;
}
unsigned input_original(uintptr_t self, uintptr_t unused_1 __attribute__((unused)), unsigned flags, int *unused_3 __attribute__((unused))) {
    assert((input_begins && file_reading && file_reading->began) || (file_fresh_input && file_fresh_input->read));
    if (flags == 99) return 7;
    FILE *next = tmpfile();
    assert(next);
    if (input_file) fclose(input_file);
    input_file = next;
    ++input_opens;
    put(self + 0x28, ((uintptr_t) (next)));
    return 0;
}
void replace_holder(void *unused_0 __attribute__((unused)), const uintptr_t *where, const uintptr_t *h) { put(*where + 0xe8, *h); }
void broadcast(void *h, void *unused_1 __attribute__((unused))) {
    if (nested) {
        nested = false;
        uintptr_t a = ((uintptr_t) alpha.bytes);
        set_content(h, &a, NULL);
    }
}
bool replace_content(void *h, const uintptr_t *c, void *listener) {
    __auto_type field = ((uintptr_t *) (((uintptr_t) (h)) + 0x28));
    uintptr_t old = __atomic_exchange_n(field, *c, __ATOMIC_ACQ_REL);
    if (old == *c) return false;
    if (atomic_exchange_explicit(&(stall), false, memory_order_seq_cst)) {
        entered = true;
        while (!atomic_load_explicit(&(resume), memory_order_seq_cst)) sched_yield();
    }
    on_broadcast(h, listener);
    return true;
}
uintptr_t construct_holder(void *manager, const void *unused_1 __attribute__((unused)), const uintptr_t *c) {
    put(((uintptr_t) (manager)) + 0x28, *c);
    return 1;
}
uintptr_t *inline_cache_body(uintptr_t *result, const unsigned char *track __attribute__((unused)), const uintptr_t *c, void *holder) {
    replace_content(holder, c, NULL);
    *result = (uintptr_t) holder;
    return result;
}
extern void inline_cache(void);
static uintptr_t cache_update(const unsigned char *track, const uintptr_t *input, void *repository) {
    uintptr_t result = 0;
    azn_invoke_cache_update((void *) azn_cache_update_entry, &result, track, input, repository);
    return result;
}
static void *competing_commit(void *opaque) {
    set_content(foreign_holder.bytes, opaque, NULL);
    return NULL;
}
void setup_rows(Object *content, Object *rows, const char *comment, uint32_t rgb, uint64_t in, bool loop) {
    __auto_type r = ((uintptr_t) rows->bytes);
    __auto_type c = ((uintptr_t) content->bytes);
    put(c, profile.content);
    put(c + 0x28, r);
    put(c + 0x30, r + 0x80);
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    put(r + 4, (uint32_t) (48000));
    put(r + 8, in * 1000000 / 48000);
    put(r + 16, in);
    put(r + 24, loop ? in * 1000000 / 48000 + 1000000 : absent);
    put(r + 32, loop ? in + 48000 : (uint64_t) (0));
    put(r + 0x28, (uint8_t) (loop));
    put(r + 0x70, ((uintptr_t) (comment)));
    put(r + 0x78, rgb);
    put(r + 0x7c, (uint8_t) (3));
}
void expect(unsigned i, const char *comment, bool loop) {
    assert(i < offer_count);
    __auto_type r = &(receipts[i]);
    assert(r->kind == AZN_MEMORY && r->valid == AZ_AVAIL_READY && r->memory_pin && r->load_generation == load);
    struct azn_memory_rows rows = {0};
    rows.size = sizeof rows;
    rows.version = 1;
    assert(read_rows(r->memory_pin, &rows) && rows.availability == AZ_AVAIL_READY && rows.count == 1);
    __auto_type cue = &(rows.rows[0]);
    assert(cue->entry_id == 1 && cue->classification == AZ_CUE_MEMORY && cue->type == (loop ? AZ_CUE_LOOP : AZ_CUE_POINT));
    assert(!strcmp(cue->comment, comment) && cue->saved == -1 && cue->modified == -1 && cue->auto_active == loop && cue->color_id == 3);
    assert(rows.clock_version == 2 && rows.points[0][0].state == AZN_COORD && rows.points[0][0].rate == 48000);
    HOLD(content, r->memory_pin);
    __auto_type source = azcue_read_timestamps(((const void *) (object_word(pin_for(content.token)->object + 0x28))));
    const int stored = compare_timestamps(rows.stored_ms[0][0], source.in_us / 1000, 1000, 1000, 1);
    assert(stored != AZ_TIME_INVALID && stored == AZ_TIME_EQUAL);
    const int start = compare_timestamps(rows.points[0][0].samples, (long double) cue->start_s * 48000, 48000, 48000, 1);
    assert(start != AZ_TIME_INVALID && start == AZ_TIME_EQUAL);
    if (loop) {
        const int end = compare_timestamps(rows.points[0][1].samples, (long double) cue->end_s * 48000, 48000, 48000, 1);
        assert(rows.points[0][1].state == AZN_COORD && end != AZ_TIME_INVALID && end == AZ_TIME_EQUAL);
    } else assert(rows.points[0][1].state == AZN_ABSENT);
    /* The appended clock tail must not overwrite an older private caller. */
    rows.size = offsetof(struct azn_memory_rows, stored_ms);
    assert(read_rows(r->memory_pin, &rows) && rows.clock_version == 1);
    rows.size = offsetof(struct azn_memory_rows, clock_version);
    rows.clock_version = 0xfeed;
    assert(read_rows(r->memory_pin, &rows) && rows.clock_version == 0xfeed);
}

int azn_ref_pin(uintptr_t unused_0 __attribute__((unused))) {
    __atomic_fetch_add(&refs, 1, __ATOMIC_RELAXED);
    return 1;
}
void azn_ref_release(uintptr_t unused_0 __attribute__((unused))) { ++releases; }
void azn_notify() { wake_count++; }
uint64_t azn_interest(unsigned unused_0 __attribute__((unused))) { return AZ_STATUS_TOPIC(AZ_STATUS_CUES) | AZ_STATUS_TOPIC(AZ_STATUS_LOOP); }
uint64_t azn_lifecycle_fence(unsigned d) { return d == 0 ? fence : 0; }
int azsdk_load_read(int d, struct azsdk_load_sample *out) {
    if (d != 0) return 0;
    out->invalidation_epoch = fence;
    out->unit_generation = unit;
    out->load_generation = load;
    return 1;
}
void azn_capability(unsigned d, unsigned flag, int ready) {
    if (!d && ready) caps |= flag;
}
int azn_offer(struct azstatus_native_receipt *r) {
    // Overlap fixture ensures the first worker is parked until the other has returned.
    assert(offer_count < 32);
    receipts[offer_count++] = *r;
    return 1;
}
void azn_emit(struct azstatus_native_receipt *r) {
    assert(r->kind == AZN_MEMORY && r->lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    ++lost;
}
#if defined(__aarch64__)
#include "sdkstatus_file_maps_test.inc"
#endif
int main(int argc, char **argv) {
    assert(argc == 2);
    profile = mem_profiles[atoi(argv[1]) - 1];
    originals[0] = ((void *) (replace_holder));
    originals[1] = ((void *) (replace_content));
    originals[2] = ((void *) (broadcast));
    originals[3] = ((void *) (construct_holder));
    originals[4] = ((void *) (inline_cache));
    unsigned char track[12] = {1, 2, 3};
    uintptr_t p = ((uintptr_t) pack.bytes), h = ((uintptr_t) holder.bytes), f = ((uintptr_t) foreign_holder.bytes), a = ((uintptr_t) alpha.bytes), b = ((uintptr_t) beta.bytes), none = 0;
    put(p, profile.pack);
    put(p + 0x30, (uint32_t) (19));
    put(p + 0x78, (uint8_t) (1));
    memcpy(pack.bytes + 0x38, track, 12);
    memcpy(pack.bytes + 0x80, track, 12);
    put(h, profile.holder);
    put(f, profile.holder);
    setup_rows(&(alpha), &(rows_a), "first", 0x23ab67, 0, false);
    setup_rows(&(beta), &(rows_b), "second", 0xab1045, 48000, true);
    enabled = true;
    pack_profile_ready = true;
    assert(create_holder(holder.bytes, track, &a) == 1 && refs == 0); // old consumer never acquires a ref
    azn_memory_service()->connect();
    assert(caps & AZN_CAP_MEMORY);
    assert(cache_update(track, &a, holder.bytes) == h);
    set_holder(NULL, &p, &h);
    azn_memory_bind(0, unit, fence, load, p, track);
    expect(0, "first", false);
    struct azn_pack_view held = {0};
    assert(azn_pack_acquire(0, &held) && held.object == p && held.load == load);
    uint64_t provider = receipts[0].attempt;
    assert(provider == AZSP_MEMORY + 1 && provider != AZSP_MASTER);
    nested = true;
    cache_update(track, &b, holder.bytes);
    assert(offer_count == 3);
    expect(1, "second", true);
    expect(2, "first", false);
    assert(receipts[1].attempt == provider && receipts[1].memory_serial < receipts[2].memory_serial);
    assert(!set_content(holder.bytes, &a, NULL) && offer_count == 3);
    set_content(holder.bytes, &none, NULL);
    assert(receipts[3].valid == AZ_AVAIL_EMPTY);
    seed_active(0);
    assert(receipts[4].valid == AZ_AVAIL_EMPTY);
    create_holder(foreign_holder.bytes, track, &a);
    set_holder(NULL, &p, &f);
    expect(5, "first", false);
    assert(receipts[5].attempt != provider);
    set_content(holder.bytes, &b, NULL);
    assert(offer_count == 6);
    stall = true;
    pthread_t one;
    assert(!pthread_create(&one, NULL, competing_commit, &b));
    while (!atomic_load_explicit(&(entered), memory_order_seq_cst)) sched_yield();
    set_content(foreign_holder.bytes, &a, NULL);
    resume = true;
    assert(!pthread_join(one, NULL));
    assert(lost && offer_count == 7);
    expect(6, "first", false);
    // An old completion may not remove a current incarnation, even with equal pack/track.
    ++fence;
    ++load;
    azn_memory_bind(0, unit, fence, load, p, track);
    assert(offer_count == 8 && receipts[7].attempt != receipts[5].attempt);
    assert(!azn_pack_current(0, &held));
    azn_pin_drop(held.token);
    azn_memory_retire(0, fence);
    azn_memory_bind(0, unit, fence - 1, load - 1, p, track);
    seed_active(0);
    expect(8, "first", false);
    ++fence;
    azn_memory_retire(0, fence);
    unsigned char wrong_track[12] = {2};
    azn_memory_bind(0, unit, fence, load, p, wrong_track); // wrong track, no adoption
    seed_active(0);
    assert(offer_count == 9);
    ++load;
    azn_memory_bind(0, unit, fence, load, p, track);
    assert(set_content(foreign_holder.bytes, &b, NULL));
    recall_profile = &recall_profiles[atoi(argv[1]) - 1];
    RecallCandidates selected = {0};
    selected.deck[0] = selected_edition(0, p, b, 1);
    assert(selected.deck[0].entry == 1 && selected.deck[0].provider == receipts[offer_count - 1].attempt);
    assert(!selected_edition(0, p, a, 1).entry && !selected_edition(0, p, b, 2).entry);
    Object task = {0}, facade = {0}, context = {0}, retained = {0};
    put(((uintptr_t) facade.bytes), recall_profile->facade_table);
    put(((uintptr_t) facade.bytes) + 0x60, ((uintptr_t) context.bytes));
    put(((uintptr_t) context.bytes) + 0x18, ((uintptr_t) retained.bytes));
    put(((uintptr_t) task.bytes) + 0x10, (uint64_t) (333));
    RecallFrame frame = {&selected, ((uintptr_t) facade.bytes), 0, 0, ((uintptr_t) task.bytes) + 0x28, 0, 0};
    seal_job(((uintptr_t) task.bytes), &(frame));
    RecallCandidates delivered = {0};
    assert(!consume_job(((uintptr_t) task.bytes), 332, ((uintptr_t) facade.bytes), &delivered));
    assert(consume_job(((uintptr_t) task.bytes), 333, ((uintptr_t) facade.bytes), &delivered) &&
           delivered.deck[0].serial == selected.deck[0].serial && !consume_job(((uintptr_t) task.bytes), 333, ((uintptr_t) facade.bytes), &delivered));
    POINTER_SCOPE(frame_scope, recall_frame, &frame);
    put(((uintptr_t) retained.bytes) + 8, (uint32_t) (11));
    put(((uintptr_t) context.bytes) + 0x168, (uint32_t) (21));
    assert(!azn_memory_copied(0, fence, load, ((uintptr_t) context.bytes), ((uintptr_t) retained.bytes), frame.owned, 10, NULL).state);
    assert(!azn_memory_copied(0, fence, load, ((uintptr_t) context.bytes), ((uintptr_t) context.bytes) + 0x160, ((uintptr_t) retained.bytes), 20, NULL).state);
    put(((uintptr_t) retained.bytes) + 8, (uint32_t) (12));
    // A physical atom tail belongs only to its nominal atom and declared extent.
    file_profile = &file_profiles[atoi(argv[1]) - 1];
    file_bias = 0;
    Object atoms[2] = {0}, controls[2] = {0}, merged = {0}, vector = {0}, block = {0}, blob = {0};
    uintptr_t list[1] = {((uintptr_t) merged.bytes)};
    for (unsigned i = 0; i < 2; ++i) {
        __auto_type atom = ((uintptr_t) atoms[i].bytes);
        put(atom, file_profile->atoms[i]);
        put(atom + 0x18, (uint32_t) (16));
        put(atom + 0x1c, (uint32_t) (20));
        put(atom + 0x28, (uint64_t) (20));
        put(atom + 0x30, ((uintptr_t) blob.bytes));
        put(atom + 0x38, (uint8_t) (1));
        put(((uintptr_t) merged.bytes) + (i ? 0x40 : 0x30), atom);
        put(((uintptr_t) controls[i].bytes) + 8, (int32_t) (1));
        put(((uintptr_t) controls[i].bytes) + 12, (int32_t) (1));
        put(((uintptr_t) merged.bytes) + (i ? 0x48 : 0x38), ((uintptr_t) controls[i].bytes));
    }
    put(((uintptr_t) merged.bytes) + 0x10, (uint64_t) (16));
    put(((uintptr_t) vector.bytes), ((uintptr_t) (list)));
    put(((uintptr_t) vector.bytes) + 8, ((uintptr_t) (list + 1)));
    FileRead rd __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
    assert(merged_rows(((uintptr_t) vector.bytes), &(rd)) && rd.count == 1 && rd.atoms[0].pcp2.total == 20);
    char expected[65];
    azsha256_hex(blob.bytes, 6, expected);
    assert(!strcmp(expected, rd.atoms[0].pcp2.digest));
    // Only the actual fresh fopen wrapper begins an edition. Both borrowed
    // descriptors must be authenticated before the paired row can acquire it.
    const struct azn_file_cue_callbacks callbacks = {
        sizeof callbacks, 2, input_begin, input_complete, input_hold, input_opened, NULL};
    atomic_store_explicit(&(file_callbacks), &callbacks, memory_order_seq_cst);
    Object accessor = {0}, storage = {0}, identity = {0}, handle = {0};
    const char root[] = "/media/fixture";
    const uint16_t name[] = {'a', 0};
    put(((uintptr_t) accessor.bytes) + 0x18, ((uintptr_t) storage.bytes));
    put(((uintptr_t) storage.bytes) + 8, ((uintptr_t) (root)));
    put(((uintptr_t) storage.bytes) + 16, (uint64_t) (sizeof root - 1));
    put(((uintptr_t) identity.bytes), ((uintptr_t) (name)));
    put(((uintptr_t) identity.bytes) + 8, (uint64_t) (1));
    put(((uintptr_t) identity.bytes) + 32, ((uintptr_t) (name)));
    put(((uintptr_t) identity.bytes) + 40, (uint64_t) (1));
    put(((uintptr_t) handle.bytes), file_profile->input_table);
    rd.callbacks = &callbacks;
    rd.accessor = ((uintptr_t) accessor.bytes);
    rd.identity = ((uintptr_t) identity.bytes);
    file_original[10] = ((void *) (input_original));
    {
        POINTER_SCOPE(reading, file_reading, &rd);
        int error = 0;
        assert(file_input_open(((uintptr_t) handle.bytes), 0, 1, &error) == 0);
        uint64_t partial[AZN_FILE_CUE_MAX] = {0};
        input_complete(rd.token, rd.atoms, rd.count, partial);
        assert(!partial[0]);
        assert(file_input_open(((uintptr_t) handle.bytes), 0, 1, &error) == 0);
    }
    assert(input_begins == 1 && input_opens == 2 && input_mask == 3);
    uint64_t origin_tokens[AZN_FILE_CUE_MAX] = {0};
    input_complete(rd.token, rd.atoms, rd.count, origin_tokens);
    rd.token = 0;
    remember_origins(&(rd), origin_tokens);
    assert(input_holds == 1 && SCALAR(int32_t, ((uintptr_t) controls[0].bytes) + 8) == 3);
    {
        FileRead cached __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
        assert(merged_rows(((uintptr_t) vector.bytes), &(cached)) && cached.reused[0] == 111);
        assert(!cached.token && !cached.began && input_begins == 1);
    }
    blob.bytes[0] ^= 1;
    {
        FileRead changed __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
        assert(merged_rows(((uintptr_t) vector.bytes), &(changed)) && !changed.reused[0]);
    }
    blob.bytes[0] ^= 1;
    {
        FileRead failed __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
        failed.callbacks = &callbacks;
        failed.accessor = ((uintptr_t) accessor.bytes);
        failed.identity = ((uintptr_t) identity.bytes);
        POINTER_SCOPE(reading, file_reading, &failed);
        int error = 0;
        assert(file_input_open(((uintptr_t) handle.bytes), 0, 99, &error) == 7);
        assert(!input_mask);
    }
    assert(input_cancels == 1 && input_begins == 2);
    fclose(input_file);
    input_file = NULL;
    // Every cache participant must be from the same genuine fresh-load
    // operation, or the exact FileData owner already captured by this getter.
    Object cache_data[2] = {0}, cache_control[2] = {0}, cache_result = {0};
    for (unsigned control_index = 0; control_index < sizeof(cache_control) / sizeof(cache_control)[0]; ++control_index) {
        Object *control = &(cache_control[control_index]);
        put(((uintptr_t) control->bytes) + 8, (int32_t) (1));
        put(((uintptr_t) control->bytes) + 12, (int32_t) (1));
    }
    file_original[11] = ((void *) (input_cache_original));
    file_original[12] = ((void *) (input_fresh_original));
    input_cache_object = ((uintptr_t) cache_data[0].bytes);
    input_cache_control = ((uintptr_t) cache_control[0].bytes);
    {
        FileRead mixed __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
        POINTER_SCOPE(reading, file_reading, &mixed);
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
        assert(mixed.tag_seen && !mixed.cached && mixed.fresh_count == 1);
        input_cache_fresh = false;
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
        assert(!mixed.cached && mixed.fresh_count == 1);
        input_cache_object = ((uintptr_t) cache_data[1].bytes);
        input_cache_control = ((uintptr_t) cache_control[1].bytes);
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
        assert(mixed.cached);
    }
    assert(SCALAR(int32_t, ((uintptr_t) cache_control[0].bytes) + 8) == 1 &&
           SCALAR(int32_t, ((uintptr_t) cache_control[1].bytes) + 8) == 1);
    // A GRID/PSSI getter can create the same FileData later used by CUES.
    // Its physical inception is independent of a cue request or active read.
    const struct azn_file_cue_callbacks common_callbacks = {
        sizeof common_callbacks, 2, input_begin, input_complete, input_hold, input_opened, common_api};
    atomic_store_explicit(&(file_callbacks), &common_callbacks, memory_order_seq_cst);
    Object physical_path = {0};
    const char analysis_path[] = "/media/fixture/PIONEER/USBANLZ/P01/ANLZ0000.DAT";
    put(((uintptr_t) physical_path.bytes), file_profile->path_table);
    put(((uintptr_t) physical_path.bytes) + 0x10, ((uintptr_t) (analysis_path)));
    put(((uintptr_t) physical_path.bytes) + 0x18, (uint64_t) (sizeof analysis_path - 1));
    common_handle = ((uintptr_t) handle.bytes);
    common_path = ((uintptr_t) physical_path.bytes);
    input_cache_object = ((uintptr_t) cache_data[0].bytes);
    input_cache_control = ((uintptr_t) cache_control[0].bytes);
    input_cache_fresh = true;
    assert(!file_reading && file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
    assert(common_begins == 1 && common_seals == 1 && common_holds == 1);
    assert(SCALAR(int32_t, ((uintptr_t) cache_control[0].bytes) + 8) == 1 &&
           SCALAR(int32_t, ((uintptr_t) cache_control[0].bytes) + 12) == 2);
    input_cache_fresh = false;
    {
        FileRead cached __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
        POINTER_SCOPE(reading, file_reading, &cached);
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
        assert(cached.inputs.seen && !cached.inputs.bad && cached.inputs.tokens[0] == 2001);
        assert(file_inputs_join(&cached.inputs) == 9000 && common_joins == 1 && common_begins == 1);
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0 && common_holds == 2);
        input_cache_object = ((uintptr_t) cache_data[1].bytes);
        input_cache_control = ((uintptr_t) cache_control[1].bytes);
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
        assert(cached.inputs.bad && !file_inputs_join(&cached.inputs) && common_begins == 1);
    }
    assert(common_holds == 1);
    common_mask = 2;
    input_cache_fresh = true;
    assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 203) == 0);
    assert(common_begins == 2 && common_seals == 2 && common_holds == 2);
    input_cache_fresh = false;
    {
        FileRead paired __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
        POINTER_SCOPE(reading, file_reading, &paired);
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 203) == 0);
        input_cache_object = ((uintptr_t) cache_data[0].bytes);
        input_cache_control = ((uintptr_t) cache_control[0].bytes);
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
        assert(paired.inputs.tokens[0] == 2001 && paired.inputs.tokens[1] == 2002 && file_inputs_join(&paired.inputs) == 9000);
    }
    // A reused payload address with a different live control is not the old
    // FileData incarnation, even when its current bytes could be identical.
    input_cache_control = ((uintptr_t) cache_control[1].bytes);
    {
        FileRead reused_address __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
        POINTER_SCOPE(reading, file_reading, &reused_address);
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
        assert(reused_address.inputs.bad && !file_inputs_join(&reused_address.inputs));
    }
    for (unsigned origin_index = 0; origin_index < sizeof(file_input_origins) / sizeof(file_input_origins)[0]; ++origin_index) {
        FileInputOrigin *origin = &(file_input_origins[origin_index]);
        file_input_release(origin);
    }
    assert(!common_holds && SCALAR(int32_t, ((uintptr_t) cache_control[0].bytes) + 8) == 1 &&
           SCALAR(int32_t, ((uintptr_t) cache_control[0].bytes) + 12) == 1 && SCALAR(int32_t, ((uintptr_t) cache_control[1].bytes) + 12) == 1);
    /* A stock repeated open is one physical input, but every successful
     * descriptor must prove that same inception. Ambiguous retries cannot
     * manufacture an origin for a subsequently cached FileData. */
    Object second_handle = {0}, second_path = {0};
    const char other_path[] = "/media/fixture/PIONEER/USBANLZ/P02/ANLZ0000.DAT";
    put(((uintptr_t) second_handle.bytes), file_profile->input_table);
    put(((uintptr_t) second_path.bytes), file_profile->path_table);
    put(((uintptr_t) second_path.bytes) + 0x10, ((uintptr_t) (other_path)));
    put(((uintptr_t) second_path.bytes) + 0x18, (uint64_t) (sizeof other_path - 1));
    common_other_handle = ((uintptr_t) second_handle.bytes);
    common_other_path = ((uintptr_t) second_path.bytes);
    input_cache_object = ((uintptr_t) cache_data[0].bytes);
    input_cache_control = ((uintptr_t) cache_control[0].bytes);
    input_cache_fresh = true;
    common_mask = 1;
    for (common_reopen_mode = 1; common_reopen_mode <= 7; ++common_reopen_mode) {
        unsigned begins = common_begins, seals = common_seals, mask = 0;
        assert(file_cache_get(101, ((uintptr_t) cache_result.bytes), 202) == 0);
        assert(common_begins == begins + 1 && common_seals == seals + 1 && !common_holds);
        assert(!file_input_find(common_api(), input_cache_object, input_cache_control, &(mask)) && !mask);
    }
    common_reopen_mode = 0;
    fclose(input_file);
    input_file = NULL;
    common_handle = common_path = 0;
    atomic_store_explicit(&(file_callbacks), &callbacks, memory_order_seq_cst);
#if defined(__aarch64__)
    map_adapter_cases(atoi(argv[1]), &common_callbacks);
#endif
    put(((uintptr_t) atoms[1].bytes), file_profile->atoms[0]);
    FileRead foreign __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
    assert(!merged_rows(((uintptr_t) vector.bytes), &(foreign)));
    put(((uintptr_t) block.bytes), ((uintptr_t) blob.bytes));
    put(((uintptr_t) block.bytes) + 8, (uint64_t) (16));
    put(((uintptr_t) blob.bytes), (uint32_t) (16));
    FileServe wire = {0};
    assert(wire_rows(&(rd), ((uintptr_t) block.bytes), &(wire)) && wire.rows[0].bytes == 12);
    put(((uintptr_t) blob.bytes), (uint32_t) (12));
    assert(!wire_rows(&(rd), ((uintptr_t) block.bytes), &(wire)));
    // Preserve a witnessed insertion through sorting, without accepting an
    // identical row from another immutable object or a duplicated native key.
    FileImport imported __attribute__((cleanup(file_import_drop))) = {0};
    imported.count = imported.decoded = imported.bank[0].added = 1;
    imported.bank[0].content = b;
    imported.bank[0].pin = azn_owned_pin(b);
    imported.bank[0].row[0].key = file_key(((uintptr_t) rows_b.bytes));
    imported.bank[0].row[0].token = 111;
    finish_import(&(imported));
    assert(imported_memory_row(b, 1) == 111 && !imported_memory_row(a, 1));
    uint64_t file_id = 0;
    __auto_type link = azn_memory_copied(0, fence, load, ((uintptr_t) context.bytes), ((uintptr_t) context.bytes) + 0x160, ((uintptr_t) retained.bytes), 20, &file_id);
    assert(file_id == 111);
    assert(link.state == AZN_MEMORY_LINK_READY && link.provider == selected.deck[0].provider &&
           link.stamp == selected.deck[0].serial && link.slot == 1);
    assert(set_content(foreign_holder.bytes, &a, NULL));
    assert(!azn_memory_copied(0, fence, load, ((uintptr_t) context.bytes), ((uintptr_t) context.bytes) + 0x160, ((uintptr_t) retained.bytes), 20, NULL).state);
    azn_memory_retire(0, ++fence);
    for (unsigned r_index = 0; r_index < sizeof(receipts) / sizeof(receipts)[0]; ++r_index) {
        struct azstatus_native_receipt *r = &(receipts[r_index]);
        if (r->memory_pin) drop(r->memory_pin);
    }
    for (unsigned i = 0; i < cache_count; i++) {
        for (unsigned n = 0; n < 2; ++n) {
            _Atomic(uint64_t) *cache = n ? cached_holder : cached_content;
            __auto_type id = atomic_exchange_explicit(&(cache[i]), 0, memory_order_seq_cst);
            if (id) drop(id);
        }
    }
    for (unsigned origin_index = 0; origin_index < sizeof(imported_files) / sizeof(imported_files)[0]; ++origin_index) {
        FileContent *origin = &(imported_files[origin_index]);
        if (origin->pin) {
            hold_file_content(origin, 0);
            azn_pin_drop(origin->pin);
        }
        (*origin) = (FileContent) {0};
    }
    for (unsigned origin_index = 0; origin_index < sizeof(file_origins) / sizeof(file_origins)[0]; ++origin_index) {
        FileOrigins *origin = &(file_origins[origin_index]);
        release_origins(origin);
        (*origin) = (FileOrigins) {0};
    }
    for (unsigned i = 0; i < AZN_FILE_CUE_MAX; ++i)
        for (unsigned j = 0; j < 2; ++j) file_atom_release(&(rd.owners[i][j]));
    assert(!input_holds && SCALAR(int32_t, ((uintptr_t) controls[0].bytes) + 8) == 1 &&
           SCALAR(int32_t, ((uintptr_t) controls[1].bytes) + 8) == 1);
    atomic_store_explicit(&(file_callbacks), NULL, memory_order_seq_cst);
    while (atomic_load_explicit(&(reap_needed), memory_order_seq_cst)) azn_memory_maintenance();
    assert(refs > 0 && refs == releases && atomic_load_explicit(&(wake_count), memory_order_seq_cst));
    puts("PASS memory: matched cached pack, immutable UTF-8 rows A/B/A, empty, holder change, competing commits, stale load, balanced refs");
    puts("PASS FILE input: fresh native open/fd, partial input refusal, original cached-atom custody, changed bytes and balanced owners");
    puts("PASS FILE common input: repeated-open physical proof, ambiguous retries rejected, cached DAT/EXT custody, address reuse and weak-only retention");
    return 0;
}
