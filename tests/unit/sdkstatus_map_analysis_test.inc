/* SPDX-License-Identifier: GPL-3.0-only */

#include "native_abi_fixture.h"
static int memory_failure = -1;
static unsigned memory_calls, memory_live;
static bool make_failure;
unsigned analysis_made, analysis_freed, analysis_notified;
static uintptr_t analysis_allocated[64];
static unsigned analysis_count;
void (*during_make)();
void test_be16(uint8_t *p, unsigned n) {
    p[0] = n >> 8;
    p[1] = n;
}
void test_be32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
unsigned char test_grant(uintptr_t p) {
    uint32_t count = read_u32(p, 0xc);
    assert(count);
    put(p + 0xc, count + 1);
    return 1;
}
unsigned char test_release(uintptr_t p) {
    uint32_t count = read_u32(p, 0xc);
    assert(count);
    put(p + 0xc, count - 1);
    return count == 1;
}
void test_trash(uintptr_t unused __attribute__((unused)), uintptr_t p) {
    assert(!read_u32(p, 0xc));
    for (unsigned i = 0; i < analysis_count; ++i)
        if (analysis_allocated[i] == p) {
            analysis_allocated[i] = 0;
            free(((void *) (read_ptr(p, 0x40))));
            free(((void *) (p)));
            ++analysis_freed;
            return;
        }
}
void test_make_body(uintptr_t *result, const struct AnalysisAtomRef *ref) {
    const struct AnalysisAtom *atom = &(*ref->value);
    assert(!ref->control && atom->header == 32 && atom->prefix == 18 && atom->stride == 24 &&
           atom->rows.size == atom->count * 24 && atom->raw.size == 32 + atom->rows.size);
    assert(!memcmp(((void *) (atom->raw.data)), "PSSI", 4));
    if (make_failure) fixture_raise();
    uintptr_t p = ((uintptr_t) (calloc(1, 0x50)));
    __auto_type rows = ((uint32_t *) (calloc(atom->count, 20)));
    assert(p && rows);
    put(p, profile->phrase_content);
    put(p + 0xc, (uint32_t) (1));
    assert(atom->mood >= 1 && atom->mood <= 3);
    put(p + 0x3c, (uint32_t) (atom->mood - 1));
    put(p + 0x40, ((uintptr_t) (rows)));
    put(p + 0x48, atom->count);
    for (unsigned i = 0; i < atom->count; i++) {
        __auto_type raw = ((const uint8_t *) (atom->rows.data));
        rows[5 * i] = analysis_u16(raw + i * 24 + 4);
        rows[5 * i + 1] = analysis_u16(raw + i * 24 + 2);
        rows[5 * i + 2] = i + 1 < atom->count ? analysis_u16(raw + (i + 1) * 24 + 2) : atom->end;
    }
    assert(analysis_count < sizeof analysis_allocated / sizeof *analysis_allocated);
    analysis_allocated[analysis_count++] = p;
    ++analysis_made;
    if (during_make) {
        __auto_type callback = during_make;
        during_make = NULL;
        callback();
    }
    *result = p;
}
extern void test_make(void);
static void test_memory(struct AnalysisMemory *memory, const void *data, size_t size) {
    if ((int) memory_calls++ == memory_failure) fixture_raise();
    memory->data = (uintptr_t) malloc(size);
    ++memory_live;
    memory->size = size;
    assert(memory->data);
    memcpy((void *) memory->data, data, size);
}
static void test_memory_drop(struct AnalysisMemory *memory) {
    assert(memory_live);
    --memory_live;
    free((void *) memory->data);
}
static uintptr_t test_notify(uintptr_t holder __attribute__((unused)), uintptr_t reason __attribute__((unused))) {
    ++analysis_notified;
    return 0;
}
static void current_retime(void) { grid_retime(replacement_holder + 0x28, 1); }
static void current_arm_retime(void) { during_current = current_retime; }
static void make_reload(void) { ++epoch; }
static void make_retime(void) { grid_retime(replacement_holder + 0x28, 3); }
int test_analysis(const struct azn_maps_api *api) {
    assert(profile == &profiles[1]);
    struct aznm_packet initial = {0};
    assert(api->capture(0, epoch, &initial) == AZ_UNSUPPORTED);
    analysis_ops.make = (void *) test_make;
    analysis_ops.memory = test_memory;
    analysis_ops.memory_drop = test_memory_drop;
    analysis_ops.grant = test_grant;
    analysis_ops.release = test_release;
    analysis_ops.trash = test_trash;
    atomic_store_explicit(&(analysis_ready), true, memory_order_seq_cst);
    map_writer_installed = 15;
    map_writer_original[1] = (void *) test_retime;
    map_writer_original[3] = (void *) test_notify;
    analysis_refs = true;
    Buf150 pack = {0};
    Buf80 gh = {0}, ph = {0}, gc = {0}, pc = {0};
    const uintptr_t objects[] = {((uintptr_t) pack.bytes), ((uintptr_t) gh.bytes), ((uintptr_t) ph.bytes), ((uintptr_t) gc.bytes), ((uintptr_t) pc.bytes)};
    for (unsigned i = 0; i < sizeof objects / sizeof *objects; ++i) put(objects[i] + 0xc, (uint32_t) (1));
    put(((uintptr_t) pack.bytes), profile->pack);
    put(((uintptr_t) gh.bytes), profile->grid_holder);
    put(((uintptr_t) ph.bytes), profile->phrase_holder);
    put(((uintptr_t) gc.bytes), profile->grid_content);
    put(((uintptr_t) pc.bytes), profile->phrase_content);
    put(((uintptr_t) pack.bytes) + 0xd8, ((uintptr_t) gh.bytes));
    put(((uintptr_t) pack.bytes) + 0x120, ((uintptr_t) ph.bytes));
    put(((uintptr_t) gh.bytes) + 0x28, ((uintptr_t) gc.bytes));
    put(((uintptr_t) gh.bytes) + 0x30, (int64_t) (0));
    put(((uintptr_t) gh.bytes) + 0x40, (uint32_t) (48000));
    put(((uintptr_t) ph.bytes) + 0x28, ((uintptr_t) pc.bytes));
    struct Cell {
        int64_t sample;
        double bpm;
    } cells[] = {{0, 120}, {24000, 120}, {48000, 120}, {72000, 120}};
    put(((uintptr_t) gc.bytes) + 0x28, ((uintptr_t) (cells)));
    put(((uintptr_t) gc.bytes) + 0x30, (int32_t) (4));
    playing = ((uintptr_t) pack.bytes);
    assert(api->prove_pair(138, ((uintptr_t) pack.bytes), ((uintptr_t) gh.bytes), ((uintptr_t) gc.bytes), ((uintptr_t) ph.bytes), ((uintptr_t) pc.bytes)));
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    assert(api->current(0, epoch, &initial) == AZ_OK && api->current(0, epoch + 1, &initial) == AZ_STALE);
    assert(api->capture(4, epoch, &initial) == AZ_INVALID);
    replacement_holder = ((uintptr_t) gh.bytes);
    during_current = current_arm_retime;
    struct aznm_packet raced = {0};
    assert(api->capture(0, epoch, &raced) == AZ_STALE && !raced.grid.content && !gaps);
    api->release(&initial);
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    uint8_t pssi[80] = {0};
    memcpy(pssi, "PSSI", 4);
    test_be32(pssi + 4, 32);
    test_be32(pssi + 8, sizeof pssi);
    test_be16(pssi + 14, 24);
    test_be16(pssi + 16, 2);
    test_be16(pssi + 18, 1);
    test_be16(pssi + 26, 4);
    test_be16(pssi + 34, 1);
    test_be16(pssi + 36, 1);
    test_be16(pssi + 58, 2);
    test_be16(pssi + 60, 2);
    test_be16(pssi + 26, 5);
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_INVALID && !analysis_made);
    test_be16(pssi + 26, 4);
    test_be16(pssi + 58, 1);
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_INVALID && !analysis_made);
    test_be16(pssi + 58, 2);
    grid_retime(((uintptr_t) gh.bytes) + 0x28, 2);
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_STALE && !analysis_made);
    api->release(&initial);
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    /* Construction errors release both native input buffers and preserve the
     * current immutable pair. Exercise the real unwind path from C. */
    for (int stage = 0; stage < 3; ++stage) {
        memory_failure = stage;
        memory_calls = 0;
        make_failure = stage == 2;
        assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_FAILED);
        assert(!memory_live && !analysis_made && !analysis_notified);
        assert(fixture_exception_deletions == (unsigned) stage + 1);
        assert(api->current(0, epoch, &initial) == AZ_OK);
    }
    memory_failure = -1;
    make_failure = false;
    during_make = make_reload;
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_STALE);
    assert(analysis_made == 1 && analysis_freed == 1 && !analysis_notified && read_ptr(((uintptr_t) ph.bytes), 0x28) == ((uintptr_t) pc.bytes));
    api->release(&initial);
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    replacement_holder = ((uintptr_t) gh.bytes);
    during_make = make_retime;
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_STALE);
    assert(analysis_made == 2 && analysis_freed == 2 && !analysis_notified);
    api->release(&initial);
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    shared_second = true;
    size_t before = observed_count;
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_OK);
    assert(analysis_made == 3 && analysis_freed == 2 && analysis_notified == 1 && observed_count == before + 2);
    assert(observed[before].deck == 0 && observed[before + 1].deck == 1);
    for (size_t i = before; i < observed_count; i++) {
        assert(observed[i].map.grid.provider == initial.grid.provider && observed[i].map.grid.revision == initial.grid.revision);
        assert(observed[i].map.phrases.provider == initial.phrases.provider && observed[i].map.phrases.revision == initial.phrases.revision + 1);
    }
    uint32_t count;
    assert(api->read(&initial.phrases, AZNM_PHRASES, NULL, 0, &count) == AZ_OK && !count);
    assert(api->current(0, epoch, &initial) == AZ_STALE);
    struct aznm_packet refreshed = {0};
    assert(api->capture(1, epoch + 1, &refreshed) == AZ_OK);
    struct az_phrase rows[2];
    assert(api->read(&refreshed.phrases, AZNM_PHRASES, rows, 2, &count) == AZ_OK && count == 2 &&
           rows[0].start_beat == 0 && rows[0].end_beat == 1 && rows[1].end_beat == 3);
    assert(rows[0].raw_mood == 1 && rows[1].raw_mood == 1);
    assert(api->publish_phrases(1, epoch + 1, &refreshed, pssi, sizeof pssi) == AZ_NOT_FOUND && analysis_made == 3);
    uintptr_t installed = read_ptr(((uintptr_t) ph.bytes), 0x28);
    playing = 0;
    api->retire_pair(138);
    drain(api);
    api->release(&initial);
    api->release(&refreshed);
    analysis_drop(installed); /* native holder destruction */
    for (size_t i = 0; i < refs_count; ++i) assert(!refs[i].count);
    assert(analysis_made == analysis_freed && !gaps);
    for (unsigned i = 0; i < analysis_count; ++i) assert(!analysis_allocated[i]);
    puts("PASS loaded phrase publication, exact grid/reload fences, shared-pack notifications, immutable snapshots and counted release");
    return 0;
}
