/* SPDX-License-Identifier: GPL-3.0-only */
#define main memory_baseline_main
#include "sdkstatus_memory_test.c"
#undef main

static unsigned observed_packs, retired_packs, broadcasts, writes;
static uintptr_t expected_pack;
static void pack_observed(unsigned deck, uint64_t u, uint64_t e, uint64_t l) {
    assert(deck == 0 && u == unit && e == fence);
    if (!l) {
        assert(!azn_pack_retains(expected_pack));
        ++retired_packs;
    } else {
        assert(l == load && azn_pack_retains(expected_pack));
        ++observed_packs;
    }
}
static void other_observer(unsigned d, uint64_t u, uint64_t e, uint64_t l) {
    (void) d;
    (void) u;
    (void) e;
    (void) l;
    assert(!"second observer must not be installed");
}
static void count_broadcast(void *self, void *listener) {
    assert(self == holder.bytes && listener == &broadcasts);
    ++broadcasts;
}
static bool silent_content(void *self, const uintptr_t *input, void *listener) {
    assert(self == holder.bytes && listener == &writes);
    ++writes;
    if (!input) return false;
    put((uintptr_t) self + 0x28, *input);
    return true;
}
static bool changed_during_broadcast(void *self, const uintptr_t *input, void *listener) {
    assert(input && listener == &broadcasts);
    put((uintptr_t) self + 0x28, (uintptr_t) alpha.bytes);
    on_broadcast(self, listener);
    return true;
}
uintptr_t *silent_cache_body(uintptr_t *result, const unsigned char *track, const uintptr_t *content, void *owner) {
    assert(track && content && owner == holder.bytes);
    put((uintptr_t) owner + 0x28, *content);
    *result = (uintptr_t) owner;
    return result;
}
extern void silent_cache(void);
static unsigned row_calls;
static int row_holds;
static void row_hold(const uint64_t *tokens, size_t count, int acquire) {
    assert(count == 1 && tokens[0] == 712);
    ++row_calls;
    row_holds += acquire ? 1 : -1;
    assert(row_holds >= 0);
}
static void test_file_row_custody(void) {
    struct azn_file_cue_callbacks callbacks = {.size = sizeof callbacks, .version = 1, .hold = row_hold};
    atomic_store(&file_callbacks, NULL);
    azn_file_row_hold(712, 1);
    atomic_store(&file_callbacks, &callbacks);
    azn_file_row_hold(0, 1);
    callbacks.size = offsetof(struct azn_file_cue_callbacks, hold);
    azn_file_row_hold(712, 1);
    callbacks.size = sizeof callbacks;
    callbacks.hold = NULL;
    azn_file_row_hold(712, 1);
    assert(!row_calls);
    callbacks.hold = row_hold;
    azn_file_row_hold(712, 1);
    assert(row_holds == 1);
    azn_file_row_hold(712, 0);
    assert(row_calls == 2 && !row_holds);

    Object first = {0}, first_control = {0}, second = {0}, second_control = {0};
    put((uintptr_t) first_control.bytes + 8, (int32_t) 1);
    /* A row with one live native atom and one expired atom cannot enroll an
     * origin. Acquiring the first atom must unwind before returning. */
    FileRead read = {.count = 1, .callbacks = &callbacks};
    read.owners[0][0] = (FileAtomOwner) {(uintptr_t) first.bytes, (uintptr_t) first_control.bytes};
    read.owners[0][1] = (FileAtomOwner) {(uintptr_t) second.bytes, (uintptr_t) second_control.bytes};
    uint64_t token = 712;
    unsigned next = file_origin_next;
    remember_origins(&read, &token);
    assert(file_origin_next == next && row_calls == 2 && !row_holds);
    assert(SCALAR(int32_t, (uintptr_t) first_control.bytes + 8) == 1);
    atomic_store(&file_callbacks, NULL);
}
static void release_memory(void) {
    azn_memory_retire(0, ++fence);
    for (unsigned i = 0; i < offer_count; ++i)
        if (receipts[i].memory_pin) drop(receipts[i].memory_pin);
    for (unsigned i = 0; i < cache_count; ++i) {
        drop(atomic_exchange(&cached_holder[i], 0));
        drop(atomic_exchange(&cached_content[i], 0));
    }
    while (atomic_load(&reap_needed)) azn_memory_maintenance();
    assert(refs == releases);
}
static void test_pack_and_writers(void) {
    const unsigned char track[12] = {1, 2, 3};
    uintptr_t p = (uintptr_t) pack.bytes, h = (uintptr_t) holder.bytes;
    uintptr_t a = (uintptr_t) alpha.bytes, b = (uintptr_t) beta.bytes;
    expected_pack = p;
    put(p, profile.pack);
    put(p + 0x30, (uint32_t) 19);
    put(p + 0x78, (uint8_t) 1);
    memcpy(pack.bytes + 0x38, track, 12);
    memcpy(pack.bytes + 0x80, track, 12);
    put(h, profile.holder);
    setup_rows(&alpha, &rows_a, "first", 0x23ab67, 0, false);
    setup_rows(&beta, &rows_b, "second", 0xab1045, 48000, true);
    originals[0] = (void *) replace_holder;
    originals[1] = (void *) replace_content;
    originals[2] = (void *) broadcast;
    originals[3] = (void *) construct_holder;
    originals[4] = (void *) inline_cache;
    enabled = pack_profile_ready = true;
    azn_memory_service()->connect();
    assert(create_holder(holder.bytes, track, &a) == 1);
    assert(cache_update(track, &a, holder.bytes) == h);
    set_holder(NULL, &p, &h);
    assert(!azn_pack_retains(p) && !azn_pack_retains(0));
    azn_memory_bind(0, unit, fence, load, p, track);
    expect(0, "first", false);
    assert(azn_pack_retains(p) && !azn_pack_retains((uintptr_t) foreign_pack.bytes));
    assert(!azn_pack_observe(NULL));
    assert(azn_pack_observe(pack_observed) && observed_packs == 1);
    assert(azn_pack_observe(pack_observed) && observed_packs == 1);
    assert(!azn_pack_observe(other_observer) && observed_packs == 1);
    struct azn_pack_view held = {0};
    assert(azn_pack_acquire(0, &held));
    uintptr_t object = 0;
    assert(!azn_pin_hold(held.token, NULL) && !azn_pin_hold(0, &object));
    assert(azn_pin_hold(held.token, &object) && object == p);
    azn_pin_drop(held.token);
    azn_pin_drop(held.token);
    ++load;
    assert(azn_pack_retains(p)); /* retained lifetime survives stale load identity */
    --load;

    originals[1] = (void *) silent_content;
    unsigned before = lost;
    assert(!set_content(holder.bytes, NULL, &writes) && writes == 1 && lost == before);
    assert(set_content(holder.bytes, &b, &writes) && writes == 2 && lost == before + 1);
    assert(offer_count == 1 && writer_top == NULL); /* unobserved commit cuts, never invents a snapshot */
    connected = false;
    assert(set_content(holder.bytes, &a, &writes) && writes == 3 && lost == before + 1);
    connected = true;

    originals[1] = (void *) changed_during_broadcast;
    originals[2] = (void *) count_broadcast;
    assert(set_content(holder.bytes, &b, &broadcasts) && broadcasts == 1 && lost == before + 2);
    assert(offer_count == 1 && writer_top == NULL);
    originals[1] = (void *) replace_content;
    Writing *replacement = &holder_writers[cache_slot(p)];
    atomic_store(&replacement->count, 1);
    uint64_t interference = atomic_load(&replacement->interference);
    assert(set_content(holder.bytes, &b, &broadcasts) && broadcasts == 2 && lost == before + 3);
    assert(atomic_load(&replacement->interference) == interference + 1);
    atomic_store(&replacement->count, 0);
    assert(!writer_top && offer_count == 1);
    on_broadcast(holder.bytes, &broadcasts);
    assert(broadcasts == 3 && offer_count == 1); /* unsolicited broadcast has no owned writer */
    originals[4] = (void *) silent_cache;
    unsigned offers = offer_count;
    assert(cache_update(track, &a, holder.bytes) == h);
    assert(offer_count == offers + 1);
    expect(offers, "first", false);
    assert(!writer_top && receipts[offers].memory_serial == receipts[0].memory_serial);
    /* A later observed content commit has advanced the edition. The cache
     * fallback still supplies pinned data but explicitly cuts continuity. */
    assert(set_content(holder.bytes, &b, &broadcasts));
    expect(offers + 1, "second", true);
    assert(receipts[offers + 1].memory_serial > receipts[offers].memory_serial);
    assert(cache_update(track, &a, holder.bytes) == h);
    assert(offer_count == offers + 3);
    expect(offers + 2, "first", false);
    assert(receipts[offers + 2].lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    release_memory();
    assert(!azn_pack_retains(p) && retired_packs == 1);
}
static void test_bounded_seed_cache(void) {
    Object objects[cache_count + 1] = {0};
    _Atomic(uint64_t) local[cache_count] = {0};
    for (unsigned i = 0; i <= cache_count; ++i) {
        put((uintptr_t) objects[i].bytes, profile.content);
        uint64_t token = remember(local, (uintptr_t) objects[i].bytes, profile.content);
        assert(token && pin_for(token)->object == (uintptr_t) objects[i].bytes);
        uint64_t same = remember(local, (uintptr_t) objects[i].bytes, profile.content);
        assert(same == token);
        drop(same);
        drop(token);
    }
    unsigned occupied = 0;
    for (unsigned i = 0; i < cache_count; ++i)
        if (atomic_load(&local[i])) ++occupied;
    assert(occupied == cache_count);
    assert(!remember(local, (uintptr_t) objects[0].bytes, profile.holder));
    for (unsigned i = 0; i < cache_count; ++i) drop(atomic_exchange(&local[i], 0));
    while (atomic_load(&reap_needed)) azn_memory_maintenance();
    assert(refs == releases);
    uint64_t tokens[pool_count];
    for (unsigned i = 0; i < pool_count; ++i) {
        tokens[i] = make_pin((uintptr_t) objects[0].bytes, 0, 0, 0);
        assert(tokens[i]);
    }
    assert(!remember(local, (uintptr_t) objects[0].bytes, profile.content));
    for (unsigned i = 0; i < pool_count; ++i) drop(tokens[i]);
    while (atomic_load(&reap_needed)) azn_memory_maintenance();
    assert(refs == releases);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    profile = mem_profiles[atoi(argv[1]) - 1];
    test_pack_and_writers();
    test_bounded_seed_cache();
    test_file_row_custody();
    puts("PASS memory ownership: observer admission, stale pack custody, missing/mismatched broadcasts, replacement overlap, bounded cache and pool exhaustion");
    return 0;
}
