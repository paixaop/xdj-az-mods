/* SPDX-License-Identifier: GPL-3.0-only */
#define main memory_behavior_main
#include "sdkstatus_memory_test.c"
#undef main
#include "native_abi_fixture.h"
#include <sys/mman.h>
extern void fixture_recall_accept(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, void *);
extern void fixture_recall_accept_return(void), fixture_memory_call_return(void);
extern void fixture_recall_ctor0_return(void), fixture_recall_ctor1_return(void), fixture_recall_ctor2_return(void);
extern void fixture_memory_call(uintptr_t, unsigned, uintptr_t, void *);
extern uintptr_t fixture_recall_ctor0(uintptr_t, uintptr_t, void *);
extern uintptr_t fixture_recall_ctor1(uintptr_t, uintptr_t, void *);
extern uintptr_t fixture_recall_ctor2(uintptr_t, uintptr_t, void *);
static uintptr_t fixture_image_bias;
int __wrap_dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *opaque) {
    struct dl_phdr_info info = {.dlpi_addr = fixture_image_bias, .dlpi_name = ""};
    return callback(&info, sizeof info, opaque);
}
static Object task_object, facade_object, property_object, first_object, owned_object;
static RecallProfile fixture_recall_profile;
static bool expect_selected, expect_executed;
static unsigned accept_calls, memory_calls, constructor_calls, execute_calls, destroy_calls, throw_stage;
static uintptr_t fixture_pack, fixture_content;
static unsigned fixture_index;
static void original_accept(uintptr_t manager, uintptr_t track, uintptr_t p, uintptr_t name, uintptr_t row, uintptr_t option) {
    assert(manager == 11 && track == 12 && p == fixture_pack && name == 13 && option == 17 && object_word(row) == 0xabc);
    ++accept_calls;
    assert(!!selection == expect_selected);
    if (selection) assert(selection->deck[0].entry == 1 && selection->deck[0].load == load);
    if (throw_stage == 1) fixture_raise();
    fixture_memory_call((uintptr_t) facade_object.bytes, 0, (uintptr_t) property_object.bytes, (void *) memory_call);
}
static uintptr_t original_ctor(uintptr_t dst, uintptr_t src) {
    ++constructor_calls;
    if (throw_stage == 3) fixture_raise();
    memcpy((void *) dst, (const void *) src, 0x80);
    return dst + 7;
}
static void original_memory_call(uintptr_t facade, unsigned kind, uintptr_t property) {
    assert(facade == (uintptr_t) facade_object.bytes && kind == 0 && property == (uintptr_t) property_object.bytes);
    ++memory_calls;
    assert(!!recall_frame == expect_selected);
    if (throw_stage == 2) fixture_raise();
    uintptr_t first = (uintptr_t) first_object.bytes, owned = (uintptr_t) owned_object.bytes, task = (uintptr_t) task_object.bytes;
    assert(fixture_recall_ctor0(first, property, (void *) recall_ctor) == first + 7);
    assert(fixture_recall_ctor1(owned, first, (void *) recall_ctor) == owned + 7);
    assert(fixture_recall_ctor2(task + 0x28, owned, (void *) recall_ctor) == task + 0x2f);
}
static void original_execute(uintptr_t task) {
    assert(task == (uintptr_t) task_object.bytes);
    ++execute_calls;
    assert(!!recall_frame == expect_executed);
    if (recall_frame) {
        assert(recall_frame->owned == task + 0x28 && recall_frame->candidates->deck[0].entry == 1);
        assert(recall_frame->candidates->deck[0].provider == receipts[offer_count - 1].attempt);
    }
    if (throw_stage == 4) fixture_raise();
}
static uintptr_t original_destroy(uintptr_t task) {
    assert(task == (uintptr_t) task_object.bytes);
    ++destroy_calls;
    return task + 3;
}
static void call_accept(void *unused) {
    (void) unused;
    fixture_recall_accept(11, 12, fixture_pack, 13, 17, fixture_index, fixture_content, (void *) azn_recall_accept_entry);
}
static void call_execute(void *unused) {
    (void) unused;
    recall_execute((uintptr_t) task_object.bytes);
}
static void setup_recall(int build) {
    profile = mem_profiles[build - 1];
    originals[0] = (void *) replace_holder;
    originals[1] = (void *) replace_content;
    originals[2] = (void *) broadcast;
    originals[3] = (void *) construct_holder;
    originals[4] = (void *) inline_cache;
    const unsigned char track[12] = {1, 2, 3};
    fixture_pack = (uintptr_t) pack.bytes;
    fixture_content = (uintptr_t) alpha.bytes;
    uintptr_t h = (uintptr_t) holder.bytes;
    put(fixture_pack, profile.pack);
    put(fixture_pack + 0x30, (uint32_t) 19);
    put(fixture_pack + 0x78, (uint8_t) 1);
    memcpy(pack.bytes + 0x38, track, 12);
    memcpy(pack.bytes + 0x80, track, 12);
    put(h, profile.holder);
    setup_rows(&alpha, &rows_a, "recalled loop", 0x23ab67, 48000, true);
    enabled = pack_profile_ready = true;
    azn_memory_service()->connect();
    assert(create_holder(holder.bytes, track, &fixture_content) == 1);
    assert(cache_update(track, &fixture_content, holder.bytes) == h);
    set_holder(NULL, &fixture_pack, &h);
    azn_memory_bind(0, unit, fence, load, fixture_pack, track);
    assert(offer_count == 1 && selected_edition(0, fixture_pack, fixture_content, 1).entry == 1);
    fixture_recall_profile = recall_profiles[build - 1];
    recall_profile = &fixture_recall_profile;
    recall_bias = 0;
    fixture_recall_profile.dispatcher = (uintptr_t) fixture_memory_call_return;
    fixture_recall_profile.construct[0] = (uintptr_t) fixture_recall_ctor0_return;
    fixture_recall_profile.construct[1] = (uintptr_t) fixture_recall_ctor1_return;
    fixture_recall_profile.construct[2] = (uintptr_t) fixture_recall_ctor2_return;
    put((uintptr_t) facade_object.bytes, fixture_recall_profile.facade_table);
    put((uintptr_t) task_object.bytes, fixture_recall_profile.task_table);
    put((uintptr_t) task_object.bytes + 0x10, UINT64_C(333));
    put((uintptr_t) task_object.bytes + 0x18, (uintptr_t) facade_object.bytes);
    recall_original[0] = (void *) original_accept;
    recall_original[1] = (void *) original_memory_call;
    recall_original[2] = (void *) original_ctor;
    recall_original[3] = (void *) original_execute;
    recall_original[4] = recall_original[5] = (void *) original_destroy;
    recall_ready = true;
}
static void test_recall_flow(int build) {
    setup_recall(build);
    for (unsigned caller = 0; caller < 4; ++caller) {
        for (unsigned i = 0; i < 4; ++i) fixture_recall_profile.calls[i] = i == caller ? (uintptr_t) fixture_recall_accept_return : 0;
        expect_selected = expect_executed = true;
        call_accept(NULL);
        assert(!selection && !recall_frame);
        call_execute(NULL);
        assert(!selection && !recall_frame);
        expect_executed = false;
        call_execute(NULL);
    }
    fixture_index = AZ_NATIVE_CUE_LIMIT;
    expect_selected = false;
    call_accept(NULL);
    fixture_index = 1;
    call_accept(NULL);
    fixture_index = 0;
    recall_ready = false;
    call_accept(NULL);
    recall_ready = true;
    uintptr_t content = fixture_content;
    fixture_content = (uintptr_t) beta.bytes;
    call_accept(NULL);
    fixture_content = content;
    expect_selected = true;
    for (throw_stage = 1; throw_stage <= 3; ++throw_stage) {
        assert(!fixture_native_try(call_accept, NULL));
        assert(!selection && !recall_frame);
    }
    throw_stage = 0;
    call_accept(NULL);
    put((uintptr_t) task_object.bytes + 0x10, UINT64_C(334));
    expect_executed = false;
    call_execute(NULL);
    put((uintptr_t) task_object.bytes + 0x10, UINT64_C(333));
    expect_executed = true;
    throw_stage = 4;
    assert(!fixture_native_try(call_execute, NULL));
    assert(!selection && !recall_frame);
    throw_stage = 0;
    expect_executed = false;
    call_execute(NULL);
    for (unsigned destructor = 4; destructor < 6; ++destructor) {
        call_accept(NULL);
        uintptr_t task = (uintptr_t) task_object.bytes;
        assert((destructor == 4 ? recall_destroy_4(task) : recall_destroy_5(task)) == task + 3);
        call_execute(NULL);
    }
    assert(accept_calls && memory_calls && constructor_calls && execute_calls && destroy_calls == 2 && fixture_exception_deletions == 4);
    azn_memory_retire(0, ++fence);
    for (unsigned i = 0; i < offer_count; ++i)
        if (receipts[i].memory_pin) drop(receipts[i].memory_pin);
    for (unsigned i = 0; i < cache_count; ++i) {
        uint64_t id = atomic_exchange(&cached_holder[i], 0);
        if (id) drop(id);
        id = atomic_exchange(&cached_content[i], 0);
        if (id) drop(id);
    }
    while (atomic_load(&reap_needed)) azn_memory_maintenance();
    assert(refs == releases);
    puts("PASS memory recall entry: caller stack/index ABI, copied task chain, execution/destruction fences and native unwind");
}
static unsigned install_calls, accepted_hooks;
static int fail_hook;
static bool accepted[11];
static Entry fixture_entries[11];
static void *memory_install_patch(uintptr_t address, void *hook, void **original) {
    assert(hook && original);
    for (unsigned i = 0; i < 11; ++i) {
        if (address != fixture_image_bias + fixture_entries[i].address) continue;
        ++install_calls;
        assert(!accepted[i]);
        assert(!memcmp((const void *) address, fixture_entries[i].prologue, 16));
        if ((int) i == fail_hook) return NULL;
        accepted[i] = true;
        ++accepted_hooks;
        const uint32_t branch = 0x14000000;
        memcpy((void *) address, &branch, sizeof branch);
        *original = (void *) address;
        return *original;
    }
    assert(!"unknown memory patch");
    return NULL;
}
static void test_recall_install(int build, int failure, bool corrupt) {
    void *image = mmap(NULL, 0x3000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    fixture_image_bias = (uintptr_t) image;
    for (unsigned i = 0; i < 11; ++i) {
        fixture_entries[i] = i < 5 ? mem_profiles[build - 1].entry[i] : recall_profiles[build - 1].entries[i - 5];
        memcpy((void *) (fixture_image_bias + fixture_entries[i].address), fixture_entries[i].prologue, 16);
    }
    azn_memory_service()->connect();
    fail_hook = corrupt ? -1 : failure;
    if (corrupt) *(unsigned char *) (fixture_image_bias + fixture_entries[failure].address) ^= 1;
    int first = azn_memory_configure(build, memory_install_patch);
    assert(first == (failure >= 5));
    assert(atomic_load(&enabled) == (failure >= 5));
    assert(!recall_ready && !(caps & AZN_CAP_MEMORY_LINK));
    assert(!!(caps & AZN_CAP_MEMORY) == (failure >= 5));
    assert(!accepted[failure]);
    unsigned calls = install_calls;
    assert(!azn_memory_configure(0, memory_install_patch) && !azn_memory_configure(3, memory_install_patch));
    assert(!azn_memory_configure(build, NULL));
    if (installed_memory_build) {
        assert(!azn_memory_configure(3 - build, memory_install_patch));
        ++fixture_image_bias;
        assert(!azn_memory_configure(build, memory_install_patch));
        --fixture_image_bias;
    }
    assert(calls == install_calls);
    fail_hook = -1;
    memcpy((void *) (fixture_image_bias + fixture_entries[failure].address), fixture_entries[failure].prologue, 16);
    assert(azn_memory_configure(build, memory_install_patch) == (failure < 5));
    assert(enabled && recall_ready && accepted_hooks == 11 && (caps & AZN_CAP_MEMORY_LINK));
    calls = install_calls;
    assert(!azn_memory_configure(build, memory_install_patch));
    assert(calls == install_calls);
    assert(!munmap(image, 0x3000000));
}
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3 || argc == 4);
    if (argc == 3) {
        assert(!strcmp(argv[2], "existing"));
        return memory_behavior_main(2, argv);
    }
    int build = atoi(argv[1]);
    assert(build == 1 || build == 2);
    if (argc == 2) test_recall_flow(build);
    else test_recall_install(build, atoi(argv[2]), atoi(argv[3]) != 0);
    return 0;
}
