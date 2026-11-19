/* SPDX-License-Identifier: GPL-3.0-only */
/* Real installers, synthetic firmware text, and retained patch acknowledgments. */
#define main native_status_behavior_main
#include "sdkstatus_native_test.c"
#undef main
#include <sys/mman.h>

typedef struct {
    uintptr_t address;
    unsigned char signature[16];
    size_t size;
    void *hook;
    unsigned accepted;
} InstallSite;
static InstallSite install_sites[64];
static unsigned install_count, install_attempts, install_new;
static int failed_site = -1;
static uintptr_t image_base;
static void add_install_site(uintptr_t offset, const void *signature, size_t size) {
    assert(install_count < 64 && size <= 16 && offset + size < 0x3000000);
    InstallSite *site = &install_sites[install_count++];
    site->address = image_base + offset;
    site->size = size;
    memcpy(site->signature, signature, size);
    memcpy((void *) site->address, signature, size);
}
static int installer_patch(uintptr_t address, void *hook, void **original, const char *name) {
    assert(hook && original && name && *name);
    for (unsigned i = 0; i < install_count; ++i) {
        InstallSite *site = &install_sites[i];
        if (site->address != address) continue;
        ++install_attempts;
        if (!site->accepted) assert(!memcmp((const void *) address, site->signature, site->size));
        if ((int) i == failed_site) return 0;
        if (site->accepted) assert(site->hook == hook);
        else {
            site->hook = hook;
            site->accepted = 1;
            ++install_new;
            /* A real hook replaces the entry instruction. Retried installers
             * must recognize retained hooks before checking stock bytes. */
            const uint32_t branch = 0x14000000;
            memcpy((void *) address, &branch, sizeof branch);
        }
        *original = (void *) address;
        return 1;
    }
    assert(!"unknown firmware patch site");
    return 0;
}
static void *installer_entry(uintptr_t address, void *hook, void **original) {
    return installer_patch(address, hook, original, "native-entry") ? *original : NULL;
}
static unsigned original_mask(void *const *entries, unsigned count) {
    unsigned mask = 0;
    for (unsigned i = 0; i < count; ++i)
        if (entries[i]) mask |= 1u << i;
    return mask;
}
static void seed_settings(int build) {
    const uintptr_t reloop_sites[][4] = {{0x13e4f88, 0x13e5ea0, 0x13e6258, 0x13e6330}, {0x14a03c8, 0x14a12e0, 0x14a1698, 0x14a1770}};
    const uint32_t reloop_first[][2] = {{0x39402022, 0x34001ca2}, {0xb9408001, 0x34000a21}, {0xa9ba7bfd, 0x910003fd}, {0x52800061, 0xb9008001}};
    for (unsigned i = 0; i < 4; ++i) add_install_site(reloop_sites[build - 1][i], reloop_first[i], 8);
    const RateProfile *rate = &rate_profiles[build - 1];
    for (unsigned i = 0; i < 4; ++i) add_install_site(rate->sites[i], &rate->first[i], 4);
    const uint32_t generator[][4] = {{0x6d408002, 0xd2e7d002, 0x9e670043, 0xd10043ff}, {0x6d408402, 0xd2e7d002, 0x9e670044, 0x7ee1d440}};
    add_install_site(build == 1 ? 0xdb21e0 : 0xe3ca30, generator[build - 1], 16);
    const uint32_t transport_first[] = {0xa9be7bfd, 0xa9b57bfd, 0x12001c21, 0xa9be7bfd};
    for (unsigned i = 0; i < 4; ++i) add_install_site(transport_profiles[build - 1].writers[i], &transport_first[i], 4);
    const uint32_t setting_first[][2] = {{0x39004001, 0xd65f03c0}, {0x12001c42, 0x72001c21}, {0xa9be7bfd, 0x910003fd}};
    for (unsigned i = 0; i < 3; ++i) add_install_site(setting_profiles[build - 1].writer[i], setting_first[i], 8);
    const uint32_t key_first[][4] = {{0xa9b67bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4}, {0xa9b67bfd, 0x910003fd, 0xa90153f3, 0xa9025bf5}};
    add_install_site(build == 1 ? 0x141bb08 : 0x14d6ef8, key_first[build - 1], 16);
    const uint32_t slip_first[] = {0x3941a002, 0x3941a403, 0x6b02007f, 0x54000080};
    add_install_site(build == 1 ? 0x146a6b0 : 0x1525d38, slip_first, 16);
    const uintptr_t slider_sites[][6] = {{0x13def18, 0x13dfd28, 0x13e0238, 0x13e0290, 0x13e02c8, 0x13e08a0}, {0x1499f50, 0x149ad60, 0x149b2b0, 0x149b308, 0x149b340, 0x149c3e0}};
    const uint32_t slider_first[][2] = {{0xf9404401, 0x39418821}, {0xf940e002, 0x7901881f}, {0xa9be7bfd, 0x910003fd}, {0xa9be7bfd, 0x910003fd}, {0xa9bc7bfd, 0xd0ffffa1}, {0xa9be7bfd, 0x72001c3f}};
    for (unsigned i = 0; i < 6; ++i) add_install_site(slider_sites[build - 1][i], slider_first[i], 8);
    const SyncProfile *sync = &sync_profiles[build - 1];
    const uintptr_t sync_sites[] = {sync->period, sync->election, sync->beat};
    const uint32_t sync_first[][2] = {{0xa9be7bfd, 0x910003fd}, {0xa9bd7bfd, 0x72001c5f}, {0xb940bc02, build == 1 ? 0x34000922u : 0x34000902u}};
    for (unsigned i = 0; i < 3; ++i) add_install_site(sync_sites[i], sync_first[i], 8);
}
static void assert_settings_admission(bool complete) {
    assert(atomic_load(&reloop_installed) == original_mask(reloop_original, 4));
    assert(atomic_load(&rate_hooks) == original_mask(rate_original, 4));
    assert(atomic_load(&rate_generator_hooked) == (rate_generator_original != NULL));
    assert(atomic_load(&transport_installed) == original_mask(transport_original, 4));
    unsigned settings = original_mask(setting_original, 3);
    assert(atomic_load(&key_hooked) == (key_original != NULL));
    assert(atomic_load(&slip_installed) == (slip_original != NULL));
    assert(slider_lock_hooks == original_mask(slider_lock_original, 6));
    assert(sync_hooks == original_mask(sync_original, 3));
    assert(atomic_load(&sync_installed) == (sync_hooks == 7 && (settings & 4)));
    if (slider_lock_hooks == 63 && (settings & 4)) settings |= 8;
    assert(atomic_load(&setting_installed) == settings);
    unsigned caps = azstem_status_service_v1()->capabilities(0);
    assert(!!(caps & AZN_CAP_RATE) == (atomic_load(&rate_hooks) == 15 && atomic_load(&rate_generator_hooked)));
    assert(!!(caps & AZN_CAP_TRANSPORT_STATE) == (atomic_load(&transport_installed) == 15));
    assert(!azstem_status_service_v1()->capabilities(1));
    if (complete) {
        assert(settings == 15 && reloop_installed == 15 && rate_hooks == 15 && transport_installed == 15);
        assert(key_hooked && slip_installed && sync_installed && rate_generator_hooked);
    }
}
static void seed_terminal(int build) {
    const uintptr_t sites[][5] = {{0x14b0060, 0xd8d338, 0x13f8448, 0x13578a0, 0x14a3ea4}, {0x156b538, 0xe18148, 0x14b37a0, 0x1412c08, 0x155f590}};
    const uint32_t entries[][4] = {{0xa9b37bfd, 0x910003fd, 0xa90153f3, 0xaa0103f3}, {0xa9b87bfd, 0x910003fd, 0xa90153f3, 0xf9400413}, {0xa9bc7bfd, 0x910003fd, 0xa90153f3, 0x9100e014}};
    for (unsigned i = 0; i < 3; ++i) add_install_site(sites[build - 1][i], entries[i], 16);
    uintptr_t clamp = image_base + sites[build - 1][4];
    put(clamp - 4, UINT32_C(0xd63f0040));
    put(clamp, UINT32_C(0xf9400e81));
    put(clamp + 16, UINT32_C(0x9a80d033));
    put(clamp + 44, UINT32_C(0xf9000e93));
    const uintptr_t duration_sites[][4] = {{0x778660, 0x773fc0, 0x770eb8, 0x7668a8}, {0x7b9f60, 0x7b58c0, 0x7b27b8, 0x7a8128}};
    const uint32_t duration_first[][4] = {{0xa9b97bfd, 0x910003fd, 0xa90153f3, 0xaa0003f4}, {0xa9b47bfd, 0x910003fd, 0xa90363f7, 0xaa0003f7}, {0xa9be7bfd, 0x910003fd, 0xa90153f3, 0xaa0103f3}};
    for (unsigned i = 0; i < 3; ++i) add_install_site(duration_sites[build - 1][i], duration_first[i], 16);
    const uint32_t destroy[][4] = {{0xa9be7bfd, 0xf000f481, 0x913a4021, 0x910003fd}, {0xa9be7bfd, 0xf0010c41, 0x9113a021, 0x910003fd}};
    add_install_site(duration_sites[build - 1][3], destroy[build - 1], 16);
}
static void assert_terminal_admission(bool complete) {
    assert(atomic_load(&end_enabled) == (original_mask(end_original, 3) == 7));
    assert(atomic_load(&duration_enabled) == (original_mask(duration_original, 4) == 15));
    if (complete) assert(end_enabled && duration_enabled && end_length_return && end_clamp_return && duration_adapter_table && duration_buffer_table && duration_track_table);
}
static void seed_motion(int build) {
    for (unsigned i = 0; i < 7; ++i) add_install_site(azn_entries[build - 1][i].address, azn_entries[build - 1][i].prologue, 16);
    add_install_site(azn_hot_edit_entries[build - 1].address, azn_hot_edit_entries[build - 1].prologue, 16);
    const uint32_t limit_first[] = {0xf9418001, 0xb9400020, 0xb9402021, 0x9b207c20};
    add_install_site(build == 1 ? 0x766220 : 0x7a7aa0, limit_first, 16);
    const uint32_t continuous[][4] = {{0xa9b37bfd, 0xb000a302, 0x91240042, 0x910003fd}, {0xa9b37bfd, 0xd000b782, 0x9116c042, 0x910003fd}};
    add_install_site(build == 1 ? 0x1358ea8 : 0x1414218, continuous[build - 1], 16);
    state().profile.core_jump = exit_profiles[build - 1].core_jump;
    add_install_site(state().profile.core_jump.address, state().profile.core_jump.prologue, 16);
    state().profile.core_jump.address += image_base;
    const NoQuaProfile *n = &noqua_profiles[build - 1];
    add_install_site(n->entry, n->first, 16);
    const uint32_t grid_first[] = {0xa9a97bfd, 0x910003fd, 0xa90573fb, 0x2a0103fb};
    add_install_site(n->grid_entry, grid_first, 16);
}
static void assert_motion_admission(int admitted, bool complete) {
    assert(!!(admitted & AZEXIT_PASSIVE_STATUS) == (original_mask(status_original, 7) == 127));
    assert(atomic_load(&hot_metadata) == (hot_edit_original != NULL));
    assert(state().motion_hooked == (status_limit_original && status_continuous_original && state().core_jump_original));
    if (complete) assert((admitted & AZEXIT_PASSIVE_STATUS) && state().motion_hooked && hot_metadata && noqua_original && beat_grid_original);
}
static int perform_install(const char *kind, int build) {
    if (!strcmp(kind, "settings")) return azn_install_settings(build, installer_patch);
    if (!strcmp(kind, "terminal")) return azn_install_terminal(build, installer_patch);
    return install_exit_status(&state(), AZEXIT_PASSIVE_STATUS, build, installer_entry);
}
static void assert_install_admission(const char *kind, int result, bool complete) {
    if (!strcmp(kind, "settings")) assert_settings_admission(complete);
    else if (!strcmp(kind, "terminal")) assert_terminal_admission(complete);
    else assert_motion_admission(result, complete);
}
static uintptr_t callback_buffer, callback_reader;
static unsigned callback_mode, callback_calls, consumer_calls;
static int64_t callback_length = 480000;
static int64_t fixture_end_length(uintptr_t reader, uintptr_t id) {
    assert(reader == callback_reader && id == atomic_load(&state().decks[0].core) + 0x18);
    return callback_length;
}
static uintptr_t fixture_snapshot(uintptr_t self, uintptr_t payload) {
    ++callback_calls;
    if (!end_read) return self + 1;
    EndRead *frame = end_read;
    assert(!az_owner_held(&state().decks[0].gate));
    assert(compare_timestamps(end_reader_length(callback_reader, frame->core + 0x18), callback_length, frame->rate, frame->rate, 1) == AZ_TIME_EQUAL);
    end_record_length(frame, end_length_return, callback_reader, frame->core + 0x18, callback_length);
    (void) duration_get_length(callback_buffer, frame->core + 0x18);
    memcpy((void *) (payload + 0x28), (const void *) (frame->core + 0x18), 12);
    put(payload + 0x38, (uint64_t) callback_length);
    put(payload + 0x14, (uint8_t) 1);
    put(payload + 0x15, (uint8_t) 1);
    if (callback_mode == 1) put(payload + 0x30, matched_id + 1);
    if (callback_mode == 2) atomic_fetch_add(&duration_mutation, 1);
    if (callback_mode == 3) put(frame->core + 0x68, (uint32_t) 44100);
    return self + 1;
}
static uintptr_t fixture_range_consumer(uintptr_t self, uintptr_t frames, uintptr_t position, unsigned flag) {
    assert(self == 17 && frames == 23 && position == atomic_load(&state().decks[0].core) + 0x10 && flag == 5);
    ++consumer_calls;
    return 47;
}
static void assert_source_sample(uint64_t value, uint64_t expected) {
    assert(compare_timestamps(value, expected, 48000, 48000, 1) == AZ_TIME_EQUAL);
}
static void test_terminal_callbacks(void) {
    Buffer_300 core = {0};
    Buffer_200 unit = {0};
    Buffer_50 basis = {0};
    Buffer_20 adapter = {0}, tables[2] = {0};
    Buffer_160 buffer = {0};
    Buffer_d8 row = {0};
    _Alignas(16) unsigned char server[0x2000] = {0}, payload[0x1000] = {0};
    uintptr_t c = (uintptr_t) core.bytes, u = (uintptr_t) unit.bytes, b = (uintptr_t) basis.bytes;
    callback_buffer = (uintptr_t) buffer.bytes;
    callback_reader = (uintptr_t) adapter.bytes;
    duration_adapter_table = (uintptr_t) tables[0].bytes;
    duration_buffer_table = (uintptr_t) tables[1].bytes;
    put(c + 0x20, matched_id);
    put(c + 0x68, (uint32_t) 48000);
    put(c + 0x28, UINT64_C(480000));
    put(c + 0xa8, callback_reader);
    put(u + 0x180, b);
    put(b + 0x30, c);
    put(b + 0x28, callback_reader);
    put(callback_reader, duration_adapter_table);
    put(callback_reader + 8, callback_buffer);
    put(callback_buffer, duration_buffer_table);
    put(callback_buffer + 0x140, (uintptr_t) row.bytes);
    put((uintptr_t) row.bytes + 0x30, INT64_C(480000));
    put((uintptr_t) server + 0x1f38, c);
    Slot *slot = &state().decks[0];
    slot->unit = u;
    atomic_store(&slot->core, c);
    uint64_t generation = 0;
    assert(az_owner_publish(&slot->gate, slot, admitted, &generation) == AZ_OK);
    const struct azstatus_native_v1 *api = azstem_status_service_v1();
    assert(api->wake_fd() >= 0);
    api->interest(0, end_routes | duration_routes);
    end_original[0] = (void *) fixture_snapshot;
    end_original[1] = (void *) fixture_end_length;
    end_original[2] = (void *) fixture_range_consumer;
    duration_original[2] = (void *) native_callback_22;
    end_length_return = 0x1234;
    assert(end_snapshot((uintptr_t) server, (uintptr_t) payload) == (uintptr_t) server + 1 && callback_calls == 1);
    atomic_store(&end_enabled, true);
    atomic_store(&duration_enabled, true);
    Motion *m = &slot->motion;
    m->known = true;
    m->load = fixture_load.load_generation;
    m->rate = 48000;
    m->previous = 480000;
    m->generation = 1;
    m->order = 1;
    memcpy(m->identity, (const void *) (c + 0x18), 12);
    assert(end_snapshot((uintptr_t) server, (uintptr_t) payload) == (uintptr_t) server + 1);
    assert(!end_read && !duration_read && !az_owner_held(&slot->gate));
    unsigned duration_receipts = 0, end_receipts = 0;
    azstatus_native_receipt receipt = {.size = sizeof receipt, .version = 1};
    while (api->take(0, &receipt)) {
        if (receipt.kind == AZN_DURATION) {
            assert(receipt.valid && point_at(receipt.points[0], 480000, 1));
            ++duration_receipts;
        } else if (receipt.kind == AZN_END) {
            assert(receipt.valid && receipt.a == 1 && (receipt.motion_flags & AZN_TERMINAL_CONFIRMED));
            ++end_receipts;
        } else assert(!"unexpected terminal snapshot receipt");
    }
    assert(duration_receipts == 1 && end_receipts == 1);
    assert((api->capabilities(0) & (AZN_CAP_END | AZN_CAP_DURATION)) == (AZN_CAP_END | AZN_CAP_DURATION));
    uint64_t certificate = atomic_load(&duration_state[0].value.serial);
    for (callback_mode = 1; callback_mode <= 3; ++callback_mode) {
        assert(end_snapshot((uintptr_t) server, (uintptr_t) payload) == (uintptr_t) server + 1);
        assert(atomic_load(&duration_state[0].value.serial) == certificate);
        assert(!api->take(0, &receipt));
        assert(!end_read && !duration_read && !az_owner_held(&slot->gate));
        put(c + 0x68, (uint32_t) 48000);
    }
    callback_mode = 0;
    callback_length = 0;
    assert(end_snapshot((uintptr_t) server, (uintptr_t) payload) == (uintptr_t) server + 1);
    assert(end_state[0].ended == -1);
    assert_source_sample(end_state[0].length, 0);
    assert(api->take(0, &receipt) && receipt.kind == AZN_GAP && receipt.lost_topics == end_routes);
    assert(!api->take(0, &receipt));
    callback_length = 480000;
    assert(end_snapshot((uintptr_t) server, (uintptr_t) payload) == (uintptr_t) server + 1);
    unsigned recovered_end = 0;
    while (api->take(0, &receipt)) {
        assert(receipt.valid);
        if (receipt.kind == AZN_END) {
            assert(receipt.a == 1);
            ++recovered_end;
        } else assert(receipt.kind == AZN_DURATION && point_at(receipt.points[0], 480000, 1));
    }
    assert(recovered_end == 1);
    EndRead frame = {.d = 0, .core = c, .load = fixture_load, .rate = 48000};
    end_record_length(NULL, end_length_return, callback_reader, c + 0x18, 480000);
    end_record_length(&frame, end_length_return + 1, callback_reader, c + 0x18, 480000);
    end_record_length(&frame, end_length_return, callback_reader, c + 0x18, INT64_MAX);
    end_record_length(&frame, end_length_return, callback_reader, c + 0x18, 0);
    end_record_length(&frame, end_length_return, callback_reader + 8, c + 0x18, 480000);
    end_record_length(&frame, end_length_return, callback_reader, c + 0x19, 480000);
    frame.rate = 0;
    end_record_length(&frame, end_length_return, callback_reader, c + 0x18, 480000);
    frame.rate = 48000;
    ++matched_id;
    end_record_length(&frame, end_length_return, callback_reader, c + 0x18, 480000);
    --matched_id;
    assert_source_sample(frame.length, 0);
    end_record_length(&frame, end_length_return, callback_reader, c + 0x18, 480000);
    assert_source_sample(frame.length, 480000);
    processing = NULL;
    end_clamp_length(callback_reader, c + 0x18, 480000);
    assert_source_sample(end_clamp.before, 0);
    processing = slot;
    atomic_store(&end_state[0].relevant, 1);
    put(c + 0x28, UINT64_C(480001));
    end_clamp_length(callback_reader, c + 0x18, INT64_MAX);
    end_clamp_length(callback_reader + 8, c + 0x18, 480000);
    end_clamp_length(callback_reader, c + 0x19, 480000);
    end_clamp_length(callback_reader, c + 0x18, -1);
    assert_source_sample(end_clamp.before, 0);
    assert(!end_clamp_times(UINT64_MAX, 480000, 48000));
    assert(!end_clamp_times(480000, 480000, 48000));
    assert(!end_clamp_times(480001, 480000, 0));
    end_clamp_length(callback_reader, c + 0x18, 480000);
    assert_source_sample(end_clamp.before, 480001);
    EndState *e = &end_state[0];
    e->pending_order = m->order;
    e->length = 480000;
    e->length_rate = 48000;
    m->previous = 480001;
    put(c + 0x28, UINT64_C(480000));
    assert(end_range_consumer(17, 23, c + 0x10, 5) == 47 && consumer_calls == 1);
    assert_source_sample(m->previous, 480000);
    assert_source_sample(end_clamp.before, 0);
    assert(end_range_consumer(17, 23, c + 0x10, 5) == 47 && consumer_calls == 2);
    processing = NULL;
    assert(az_owner_retire(&slot->gate, generation) == AZ_OK);
    assert(az_owner_generation(&slot->gate) == 0);
    puts("PASS terminal callbacks: owner release, source/rate fences, duration and END receipts, exact clamp forwarding");
}

static unsigned edge_command_calls, edge_process_calls;
static bool edge_command(void *self) {
    assert(self);
    ++edge_command_calls;
    return true;
}
static void edge_process(void *self, const void *frames) {
    assert(self && frames);
    ++edge_process_calls;
}
static void test_exit_native_edges(void) {
    State *s = &state();
    Slot *slot = &s->decks[2];
    Buffer_200 unit = {0};
    Buffer_50 basis = {0};
    Buffer_300 core = {0}, facade = {0};
    Buffer_500 context = {0};
    Buffer_40 controller = {0}, frames = {0}, command = {0};
    uintptr_t callback = 91, reference = 123;
    slot->unit = (uintptr_t) unit.bytes;
    slot->facade = (uintptr_t) facade.bytes;
    put(slot->unit + 0x180, (uintptr_t) basis.bytes);
    put((uintptr_t) basis.bytes + 0x38, (uintptr_t) controller.bytes);
    assert(!jump_status_context(controller.bytes, &reference, &callback) && callback == 91);
    processing = slot;
    assert(!jump_status_context(controller.bytes, &reference, &callback));
    s->motion_actions = true;
    assert(jump_status_context(controller.bytes, &reference, &callback) && callback == reference);
    assert(jump_status_context(controller.bytes, NULL, &callback));
    assert(!jump_status_context(frames.bytes, &reference, &callback));
    processing = NULL;
    Node node = {0};
    atomic_store(&node.token, 1);
    atomic_store(&node.generation, 7);
    atomic_store(&node.borrowed, 1);
    atomic_store(&node.type, AZEXIT_QUANTIZE_PENDING);
    atomic_store(&node.executed, 1);
    atomic_store(&node.required, 2);
    causal = (Scope) {.slot = slot, .token = 2, .generation = 7};
    reuse_captured_node(&node, true);
    assert(node.token == 2 && node.executed == 0 && node.required == 0 && !slot->untracked);
    atomic_store(&node.executed, 1);
    reuse_captured_node(&node, true);
    assert(node.executed == 1);
    causal.token = 3;
    causal.generation = 8;
    reuse_captured_node(&node, true);
    assert(node.token == 2 && !slot->untracked);
    reuse_captured_node(&node, false);
    assert(slot->untracked && node.token == 2);
    causal = (Scope) {0};
    slot->untracked = false;
    LoopPoints wanted = {.in_valid = true, .out_valid = true, .rate = 48000, .source = 1, .in = 48000, .out = 96000, .identity = {1}};
    assert(hot_install_compatible((LoopPoints) {0}, wanted));
    assert(hot_install_compatible(wanted, wanted));
    LoopPoints before = wanted;
    before.rate = 44100;
    assert(!hot_install_compatible(before, wanted));
    before = wanted;
    ++before.in;
    assert(!hot_install_compatible(before, wanted));
    before = wanted;
    ++before.out;
    assert(!hot_install_compatible(before, wanted));
    before = wanted;
    before.identity[0] = 2;
    assert(!hot_install_compatible(before, wanted));
    before = wanted;
    before.out_valid = false;
    assert(!hot_install_compatible(before, wanted));
    wanted.out_valid = false;
    assert(hot_install_compatible(before, wanted));
    put(slot->facade + 0x68, (uintptr_t) context.bytes);
    uintptr_t working = (uintptr_t) context.bytes + 0x160;
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t position = working + (i ? 0x88 : 0x30);
        put(position, (uint8_t) 1);
        put(position + 4, (uint8_t) 1);
        put(position + 6, (uint8_t) 1);
        put(position + 8, (uint32_t) 3);
        put(position + 0x10, i ? UINT64_C(96000) : UINT64_C(48000));
        put(position + 0x20, UINT64_C(0x10000));
        put(position + 0x30, (uint32_t) 48000);
    }
    slot->retained_before = points(working);
    slot->operation = AZ_CMD_MEMORY_RECALL;
    assert(recall_loop_postcondition(slot));
    put(working + 0x98, UINT64_C(96001));
    assert(!recall_loop_postcondition(slot));
    put(working + 0x98, UINT64_C(96000));
    put(working + 0x50, UINT64_C(0x10008));
    assert(!recall_loop_postcondition(slot));
    put(working + 0x50, UINT64_C(0x10000));
    put(working + 0x30, (uint8_t) 0);
    assert(!recall_loop_postcondition(slot));
    slot->retained_before.out_valid = false;
    assert(recall_loop_postcondition(slot));
    const struct azstatus_native_v1 *api = azstem_status_service_v1();
    assert(api->wake_fd() >= 0);
    announce_owner_capabilities(s, 2);
    assert(!api->capabilities(2));
    atomic_store(&s->enabled, AZEXIT_PASSIVE_STATUS);
    atomic_store(&rate_hooks, 15);
    atomic_store(&rate_generator_hooked, true);
    atomic_store(&transport_installed, 15);
    s->motion_hooked = true;
    announce_owner_capabilities(s, 2);
    unsigned expected = AZN_CAP_LOOP | AZN_CAP_LOOP_MODE | AZN_CAP_RELOOP | AZN_CAP_RATE | AZN_CAP_TRANSPORT_STATE | AZN_CAP_MOTION;
    assert(api->capabilities(2) == expected);
    uintptr_t command_table[3] = {0, 0, (uintptr_t) edge_command}, jump_table[3] = {0, 0, (uintptr_t) edge_command};
    s->profile.commands[0].table = (uintptr_t) command_table;
    s->profile.commands[0].execute = (uintptr_t) edge_command;
    s->profile.jump_table = (uintptr_t) jump_table;
    s->profile.jump_request = (uintptr_t) edge_command;
    assert(configure_passive_exit(s, AZEXIT_PASSIVE_STATUS) && s->motion_actions);
    assert(command_table[2] == (uintptr_t) command_hook && jump_table[2] == (uintptr_t) jump_hook);
    put((uintptr_t) command.bytes, (uintptr_t) command_table);
    processing = slot;
    assert(command_hook(command.bytes) && edge_command_calls == 1);
    processing = NULL;
    atomic_store(&slot->core, (uintptr_t) core.bytes);
    uint64_t generation;
    assert(az_owner_publish(&slot->gate, slot, admitted, &generation) == AZ_OK);
    s->original = (void *) edge_process;
    process_hook(core.bytes, frames.bytes);
    assert(edge_process_calls == 1 && !processing && !az_owner_held(&slot->gate));
    assert(az_owner_retire(&slot->gate, generation) == AZ_OK);
    process_hook(core.bytes, frames.bytes);
    assert(edge_process_calls == 1);
    process_hook(frames.bytes, frames.bytes);
    assert(edge_process_calls == 2);
#ifdef __linux__
    struct dl_phdr_info info = {.dlpi_name = "dependency.so", .dlpi_addr = 77};
    assert(image(&info, 0, s) == 0);
    info.dlpi_name = "";
    assert(image(&info, 0, s) == 1 && s->bias == 77);
#endif
    puts("PASS exit native edges: passive owner hooks, exact retained loops, borrowed generations and main-image selection");
}

static void test_map_transaction_clock(void) {
    const struct azstatus_native_v1 *api = azstem_status_service_v1();
    assert(api->wake_fd() >= 0);
    api->interest(0, AZ_STATUS_ALL_V1);
    Motion *m = &state().decks[0].motion;
    m->load = fixture_load.load_generation;
    m->known = true;
    m->generation = 1;
    m->order = 1;
    m->rate = 48000;
    m->previous = 24000;
    m->window_samples = 64;
    m->window_rate = 48000;
    azstatus_native_receipt cut = {.kind = AZN_MAPS, .load_generation = fixture_load.load_generation};
    cut.map.owner = 11;
    cut.map.grid.provider = 12;
    cut.map.phrases.provider = 13;
    cut.map.grid.content = 14;
    cut.map.phrases.content = 15;
    cut.map.grid.revision = cut.map.phrases.revision = 1;
    cut.map.changed = AZNM_PAIR;
    assert(!offer_map_cut(NULL));
    azstatus_native_receipt invalid = cut;
    invalid.deck = 4;
    assert(!offer_map_cut(&invalid));
    invalid = cut;
    invalid.load_generation = 0;
    assert(!offer_map_cut(&invalid));
    assert(offer_map_cut(&cut));
    azstatus_native_receipt receipt = take(api);
    assert(receipt.kind == AZN_MAPS && !receipt.lost_topics && receipt.motion_order == 2);
    uint64_t serial = atomic_load(&map_clocks[0].serial);
    assert(offer_map_cut(&cut));
    receipt = take(api);
    assert(!receipt.lost_topics && atomic_load(&map_clocks[0].serial) == serial && m->order == 2);
    uint64_t bracket = begin_map_mask(1);
    assert(bracket && exact_map_bracket(0) && !exact_map_bracket(1));
    ++cut.map.grid.revision;
    assert(offer_map_cut(&cut));
    receipt = take(api);
    assert(!receipt.lost_topics && receipt.motion_order == 3 && receipt.observed_window_samples == 64);
    assert(!exact_map_bracket(0)); /* the preceding committed cut consumed the bracket's order */
    end_map_mask(bracket);
    uint64_t one = begin_map_mask(1), two = begin_map_mask(1);
    assert(one && two && !exact_map_bracket(0));
    ++cut.map.grid.revision;
    assert(offer_map_cut(&cut));
    receipt = take(api);
    assert(receipt.lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION) && receipt.motion_order == 4);
    end_map_mask(two);
    end_map_mask(one);
    assert(!atomic_load(&map_mutations[0].active));
    cut.lost_topics = 0;
    ++cut.map.grid.revision;
    atomic_store(&map_clocks[0].serial, UINT64_MAX);
    assert(!offer_map_cut(&cut));
    receipt = take(api);
    uint64_t routes = AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) |
                      AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
    assert((receipt.lost_topics & routes) == routes); /* consumer drains the scoped gap before any new edition */
    atomic_store(&map_clocks[0].serial, serial + 6);
    bracket = begin_map_mask(1);
    assert(bracket && offer_map_cut(&cut));
    receipt = take(api);
    assert(receipt.kind == AZN_MAPS && !receipt.lost_topics && receipt.motion_order == 5);
    end_map_mask(bracket);
    puts("PASS map clock: matching editions, exact mutation ordering, overlapping brackets and rejected serial recovery");
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "map-clock")) {
        test_map_transaction_clock();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "edges")) {
        test_exit_native_edges();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "callbacks")) {
        test_terminal_callbacks();
        return 0;
    }
    assert(argc == 5);
    int build = atoi(argv[2]), failure = atoi(argv[3]), corrupt = atoi(argv[4]);
    assert(build == 1 || build == 2);
    void *memory = mmap(NULL, 0x3000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    image_base = (uintptr_t) memory;
    State *s = &state();
    s->bias = image_base;
    s->build = build;
    if (!strcmp(argv[1], "settings")) seed_settings(build);
    else if (!strcmp(argv[1], "terminal")) seed_terminal(build);
    else seed_motion(build);
    assert(failure >= -1 && failure < (int) install_count);
    assert(!azn_install_settings(build, installer_patch));
    assert(!azn_install_terminal(build, installer_patch));
    atomic_store(&s->enabled, AZEXIT_PASSIVE_STATUS);
    assert(!azn_install_settings(0, installer_patch) && !azn_install_settings(3, installer_patch));
    assert(!azn_install_settings(3 - build, installer_patch) && !azn_install_settings(build, NULL));
    assert(!azn_install_terminal(0, installer_patch) && !azn_install_terminal(3, installer_patch));
    assert(!azn_install_terminal(3 - build, installer_patch) && !azn_install_terminal(build, NULL));
    assert(!install_attempts);
    atomic_store(&s->decks[0].core, image_base);
    if (failure >= 0 && corrupt) *(unsigned char *) install_sites[failure].address ^= 1;
    else failed_site = failure;
    int result = perform_install(argv[1], build);
    assert_install_admission(argv[1], result, failure < 0);
    if (failure >= 0) {
        assert(!install_sites[failure].accepted);
        memcpy((void *) install_sites[failure].address, install_sites[failure].signature, install_sites[failure].size);
    }
    failed_site = -1;
    result = perform_install(argv[1], build);
    assert_install_admission(argv[1], result, true);
    assert(install_new == install_count);
    unsigned completed = install_new;
    result = perform_install(argv[1], build);
    assert_install_admission(argv[1], result, true);
    assert(install_new == completed);
    assert(!munmap(memory, 0x3000000));
    return 0;
}
