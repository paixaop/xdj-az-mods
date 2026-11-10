/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/azstem/sdk_status_memory.cpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <thread>

namespace {
struct Object {
    alignas(8) unsigned char bytes[0x188] {};
    uintptr_t address() { return reinterpret_cast<uintptr_t>(bytes); }
};
template <class T>
void put(uintptr_t address, T value) { std::memcpy(reinterpret_cast<void *>(address), &value, sizeof value); }
Object pack, foreign_pack, holder, foreign_holder, alpha, beta, rows_a, rows_b;
uint64_t fence = 7, load = 12, unit = 3;
unsigned refs = 0, releases = 0, offer_count = 0, lost = 0, caps = 0;
std::atomic<unsigned> wake_count {0};
azstatus_native_receipt receipts[32];
std::atomic<bool> stall {false}, entered {false}, resume {false};
bool nested = false;
unsigned input_begins = 0, input_opens = 0, input_mask = 0, input_cancels = 0;
int input_holds = 0;
FILE *input_file = nullptr;
bool input_cache_fresh = true;
uintptr_t input_cache_object = 0, input_cache_control = 0;
uintptr_t common_handle = 0, common_path = 0;
uintptr_t common_other_handle = 0, common_other_path = 0;
unsigned common_begins = 0, common_seals = 0, common_joins = 0, common_mask = 1;
unsigned common_reopen_mode = 0, common_open_calls = 0;
int common_holds = 0;
uint64_t common_begin(const char *path, size_t bytes, const uint16_t *audio, size_t units) {
    assert(path && bytes == std::strlen(path) && !audio && !units);
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
const azn_file_input_api *common_api() {
    static const azn_file_input_api api {sizeof api, 1, common_begin, common_opened, common_seal, common_hold, common_join};
    return &api;
}
int input_fresh_original(uintptr_t, uintptr_t output, uintptr_t) {
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
uint64_t input_begin(const char *, size_t, const uint16_t *, size_t, const uint16_t *, size_t) {
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
void input_complete(uint64_t read, const azn_file_cue_row *, size_t count, uint64_t *tokens) {
    assert(read == input_begins);
    if (!count) ++input_cancels;
    if (input_mask == 3)
        for (size_t i = 0; i < count; ++i) tokens[i] = 111 + i;
}
void input_hold(const uint64_t *tokens, size_t count, int acquire) {
    for (size_t i = 0; i < count; ++i)
        if (tokens[i]) input_holds += acquire ? 1 : -1;
}
unsigned input_original(uintptr_t self, uintptr_t, unsigned flags, int *) {
    assert((input_begins && file_reading && file_reading->began) || (file_fresh_input && file_fresh_input->read));
    if (flags == 99) return 7;
    FILE *next = tmpfile();
    assert(next);
    if (input_file) fclose(input_file);
    input_file = next;
    ++input_opens;
    put(self + 0x28, reinterpret_cast<uintptr_t>(next));
    return 0;
}
void replace_holder(void *, const uintptr_t *where, const uintptr_t *h) { put(*where + 0xe8, *h); }
void broadcast(void *h, void *) {
    if (nested) {
        nested = false;
        uintptr_t a = alpha.address();
        set_content(h, &a, nullptr);
    }
}
bool replace_content(void *h, const uintptr_t *c, void *listener) {
    auto field = reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(h) + 0x28);
    uintptr_t old = __atomic_exchange_n(field, *c, __ATOMIC_ACQ_REL);
    if (old == *c) return false;
    if (stall.exchange(false)) {
        entered = true;
        while (!resume.load()) std::this_thread::yield();
    }
    on_broadcast(h, listener);
    return true;
}
uintptr_t construct_holder(void *manager, const void *, const uintptr_t *c) {
    put(reinterpret_cast<uintptr_t>(manager) + 0x28, *c);
    return 1;
}
HolderResult inline_cache(const unsigned char *, const uintptr_t *c, void *holder) {
    replace_content(holder, c, nullptr);
    return HolderResult {reinterpret_cast<uintptr_t>(holder)};
}
void setup_rows(Object &content, Object &rows, const char *comment, uint32_t rgb, uint64_t in, bool loop) {
    auto r = rows.address(), c = content.address();
    put(c, profile.content);
    put(c + 0x28, r);
    put(c + 0x30, r + 0x80);
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    put(r + 4, uint32_t(48000));
    put(r + 8, in * 1000000 / 48000);
    put(r + 16, in);
    put(r + 24, loop ? in * 1000000 / 48000 + 1000000 : absent);
    put(r + 32, loop ? in + 48000 : uint64_t(0));
    put(r + 0x28, uint8_t(loop));
    put(r + 0x70, reinterpret_cast<uintptr_t>(comment));
    put(r + 0x78, rgb);
    put(r + 0x7c, uint8_t(3));
}
void expect(unsigned i, const char *comment, bool loop) {
    assert(i < offer_count);
    auto &r = receipts[i];
    assert(r.kind == AZN_MEMORY && r.valid == AZ_AVAIL_READY && r.memory_pin && r.load_generation == load);
    azn_memory_rows rows {};
    rows.size = sizeof rows;
    rows.version = 1;
    assert(read_rows(r.memory_pin, &rows) && rows.availability == AZ_AVAIL_READY && rows.count == 1);
    auto &cue = rows.rows[0];
    assert(cue.entry_id == 1 && cue.classification == AZ_CUE_MEMORY && cue.type == (loop ? AZ_CUE_LOOP : AZ_CUE_POINT));
    assert(!strcmp(cue.comment, comment) && cue.saved == -1 && cue.modified == -1 && cue.auto_active == loop && cue.color_id == 3);
    assert(rows.clock_version == 2 && rows.points[0][0].state == AZN_COORD && rows.points[0][0].rate == 48000);
    Hold content(r.memory_pin);
    const auto source = azcue_read_timestamps(reinterpret_cast<const void *>(object_word(content.get()->object + 0x28)));
    const int stored = compare_timestamps(rows.stored_ms[0][0], source.in_us / 1000, 1000, 1000, 1);
    assert(stored != AZ_TIME_INVALID && stored == AZ_TIME_EQUAL);
    const int start = compare_timestamps(rows.points[0][0].samples, (long double) cue.start_s * 48000, 48000, 48000, 1);
    assert(start != AZ_TIME_INVALID && start == AZ_TIME_EQUAL);
    if (loop) {
        const int end = compare_timestamps(rows.points[0][1].samples, (long double) cue.end_s * 48000, 48000, 48000, 1);
        assert(rows.points[0][1].state == AZN_COORD && end != AZ_TIME_INVALID && end == AZ_TIME_EQUAL);
    } else assert(rows.points[0][1].state == AZN_ABSENT);
    /* The appended clock tail must not overwrite an older private caller. */
    rows.size = offsetof(azn_memory_rows, stored_ms);
    assert(read_rows(r.memory_pin, &rows) && rows.clock_version == 1);
    rows.size = offsetof(azn_memory_rows, clock_version);
    rows.clock_version = 0xfeed;
    assert(read_rows(r.memory_pin, &rows) && rows.clock_version == 0xfeed);
}
} // namespace
extern "C" int azn_ref_pin(uintptr_t) {
    __atomic_fetch_add(&refs, 1, __ATOMIC_RELAXED);
    return 1;
}
extern "C" void azn_ref_release(uintptr_t) { ++releases; }
extern "C" void azn_notify() { wake_count++; }
extern "C" uint64_t azn_interest(unsigned) { return AZ_STATUS_TOPIC(AZ_STATUS_CUES) | AZ_STATUS_TOPIC(AZ_STATUS_LOOP); }
extern "C" uint64_t azn_lifecycle_fence(unsigned d) { return d == 0 ? fence : 0; }
extern "C" int azsdk_load_read(int d, azsdk_load_sample *out) noexcept {
    if (d != 0) return 0;
    out->invalidation_epoch = fence;
    out->unit_generation = unit;
    out->load_generation = load;
    return 1;
}
extern "C" void azn_capability(unsigned d, unsigned flag, int ready) {
    if (!d && ready) caps |= flag;
}
extern "C" int azn_offer(azstatus_native_receipt *r) {
    // Overlap fixture ensures the first worker is parked until the other has returned.
    assert(offer_count < 32);
    receipts[offer_count++] = *r;
    return 1;
}
extern "C" void azn_emit(azstatus_native_receipt *r) {
    assert(r->kind == AZN_MEMORY && r->lost_topics == AZ_STATUS_TOPIC(AZ_STATUS_CUES));
    ++lost;
}
#if defined(__aarch64__)
#include "sdkstatus_file_maps_test.inc"
#endif
int main(int argc, char **argv) {
    assert(argc == 2);
    profile = mem_profiles[std::atoi(argv[1]) - 1];
    originals[0] = reinterpret_cast<void *>(replace_holder);
    originals[1] = reinterpret_cast<void *>(replace_content);
    originals[2] = reinterpret_cast<void *>(broadcast);
    originals[3] = reinterpret_cast<void *>(construct_holder);
    originals[4] = reinterpret_cast<void *>(inline_cache);
    unsigned char track[12] = {1, 2, 3};
    uintptr_t p = pack.address(), h = holder.address(), f = foreign_holder.address(), a = alpha.address(), b = beta.address(), none = 0;
    put(p, profile.pack);
    put(p + 0x30, uint32_t(19));
    put(p + 0x78, uint8_t(1));
    memcpy(pack.bytes + 0x38, track, 12);
    memcpy(pack.bytes + 0x80, track, 12);
    put(h, profile.holder);
    put(f, profile.holder);
    setup_rows(alpha, rows_a, "first", 0x23ab67, 0, false);
    setup_rows(beta, rows_b, "second", 0xab1045, 48000, true);
    enabled = true;
    pack_profile_ready = true;
    assert(create_holder(holder.bytes, track, &a) == 1 && refs == 0); // old consumer never acquires a ref
    azn_memory_service()->connect();
    assert(caps & AZN_CAP_MEMORY);
    assert(cache_update(track, &a, holder.bytes).object == h);
    set_holder(nullptr, &p, &h);
    azn_memory_bind(0, unit, fence, load, p, track);
    expect(0, "first", false);
    azn_pack_view held {};
    assert(azn_pack_acquire(0, &held) && held.object == p && held.load == load);
    uint64_t provider = receipts[0].attempt;
    assert(provider == AZSP_MEMORY + 1 && provider != AZSP_MASTER);
    nested = true;
    cache_update(track, &b, holder.bytes);
    assert(offer_count == 3);
    expect(1, "second", true);
    expect(2, "first", false);
    assert(receipts[1].attempt == provider && receipts[1].memory_serial < receipts[2].memory_serial);
    assert(!set_content(holder.bytes, &a, nullptr) && offer_count == 3);
    set_content(holder.bytes, &none, nullptr);
    assert(receipts[3].valid == AZ_AVAIL_EMPTY);
    seed_active(0);
    assert(receipts[4].valid == AZ_AVAIL_EMPTY);
    create_holder(foreign_holder.bytes, track, &a);
    set_holder(nullptr, &p, &f);
    expect(5, "first", false);
    assert(receipts[5].attempt != provider);
    set_content(holder.bytes, &b, nullptr);
    assert(offer_count == 6);
    stall = true;
    std::thread one([&] { set_content(foreign_holder.bytes, &b, nullptr); });
    while (!entered.load()) std::this_thread::yield();
    set_content(foreign_holder.bytes, &a, nullptr);
    resume = true;
    one.join();
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
    assert(set_content(foreign_holder.bytes, &b, nullptr));
    recall_profile = &recall_profiles[std::atoi(argv[1]) - 1];
    RecallCandidates selected {};
    selected.deck[0] = selected_edition(0, p, b, 1);
    assert(selected.deck[0].entry == 1 && selected.deck[0].provider == receipts[offer_count - 1].attempt);
    assert(!selected_edition(0, p, a, 1).entry && !selected_edition(0, p, b, 2).entry);
    Object task, facade, context, retained;
    put(facade.address(), recall_profile->facade_table);
    put(facade.address() + 0x60, context.address());
    put(context.address() + 0x18, retained.address());
    put(task.address() + 0x10, uint64_t(333));
    RecallFrame frame {&selected, facade.address(), 0, 0, task.address() + 0x28, 0, 0};
    seal_job(task.address(), frame);
    RecallCandidates delivered {};
    assert(!consume_job(task.address(), 332, facade.address(), &delivered));
    assert(consume_job(task.address(), 333, facade.address(), &delivered) &&
           delivered.deck[0].serial == selected.deck[0].serial && !consume_job(task.address(), 333, facade.address(), &delivered));
    RecallScope<RecallFrame *> frame_scope(recall_frame, &frame);
    put(retained.address() + 8, uint32_t(11));
    put(context.address() + 0x168, uint32_t(21));
    assert(!azn_memory_copied(0, fence, load, context.address(), retained.address(), frame.owned, 10, nullptr).state);
    assert(!azn_memory_copied(0, fence, load, context.address(), context.address() + 0x160, retained.address(), 20, nullptr).state);
    put(retained.address() + 8, uint32_t(12));
    // A physical atom tail belongs only to its nominal atom and declared extent.
    file_profile = &file_profiles[std::atoi(argv[1]) - 1];
    file_bias = 0;
    Object atoms[2], controls[2], merged, vector, block, blob;
    uintptr_t list[1] = {merged.address()};
    for (unsigned i = 0; i < 2; ++i) {
        auto atom = atoms[i].address();
        put(atom, file_profile->atoms[i]);
        put(atom + 0x18, uint32_t(16));
        put(atom + 0x1c, uint32_t(20));
        put(atom + 0x28, uint64_t(20));
        put(atom + 0x30, blob.address());
        put(atom + 0x38, uint8_t(1));
        put(merged.address() + (i ? 0x40 : 0x30), atom);
        put(controls[i].address() + 8, int32_t(1));
        put(controls[i].address() + 12, int32_t(1));
        put(merged.address() + (i ? 0x48 : 0x38), controls[i].address());
    }
    put(merged.address() + 0x10, uint64_t(16));
    put(vector.address(), reinterpret_cast<uintptr_t>(list));
    put(vector.address() + 8, reinterpret_cast<uintptr_t>(list + 1));
    FileRead rd {};
    assert(merged_rows(vector.address(), rd) && rd.count == 1 && rd.atoms[0].pcp2.total == 20);
    char expected[65];
    azsha256_hex(blob.bytes, 6, expected);
    assert(!std::strcmp(expected, rd.atoms[0].pcp2.digest));
    // Only the actual fresh fopen wrapper begins an edition. Both borrowed
    // descriptors must be authenticated before the paired row can acquire it.
    const azn_file_cue_callbacks callbacks = {
        sizeof callbacks, 2, input_begin, input_complete, input_hold, input_opened, nullptr};
    file_callbacks.store(&callbacks);
    Object accessor, storage, identity, handle;
    const char root[] = "/media/fixture";
    const uint16_t name[] = {'a', 0};
    put(accessor.address() + 0x18, storage.address());
    put(storage.address() + 8, reinterpret_cast<uintptr_t>(root));
    put(storage.address() + 16, uint64_t(sizeof root - 1));
    put(identity.address(), reinterpret_cast<uintptr_t>(name));
    put(identity.address() + 8, uint64_t(1));
    put(identity.address() + 32, reinterpret_cast<uintptr_t>(name));
    put(identity.address() + 40, uint64_t(1));
    put(handle.address(), file_profile->input_table);
    rd.callbacks = &callbacks;
    rd.accessor = accessor.address();
    rd.identity = identity.address();
    file_original[10] = reinterpret_cast<void *>(input_original);
    {
        RecallScope<FileRead *> reading(file_reading, &rd);
        int error = 0;
        assert(file_input_open(handle.address(), 0, 1, &error) == 0);
        uint64_t partial[AZN_FILE_CUE_MAX] {};
        input_complete(rd.token, rd.atoms, rd.count, partial);
        assert(!partial[0]);
        assert(file_input_open(handle.address(), 0, 1, &error) == 0);
    }
    assert(input_begins == 1 && input_opens == 2 && input_mask == 3);
    uint64_t origin_tokens[AZN_FILE_CUE_MAX] {};
    input_complete(rd.token, rd.atoms, rd.count, origin_tokens);
    rd.token = 0;
    remember_origins(rd, origin_tokens);
    assert(input_holds == 1 && scalar<int32_t>(controls[0].address() + 8) == 3);
    {
        FileRead cached {};
        assert(merged_rows(vector.address(), cached) && cached.reused[0] == 111);
        assert(!cached.token && !cached.began && input_begins == 1);
    }
    blob.bytes[0] ^= 1;
    {
        FileRead changed {};
        assert(merged_rows(vector.address(), changed) && !changed.reused[0]);
    }
    blob.bytes[0] ^= 1;
    {
        FileRead failed {};
        failed.callbacks = &callbacks;
        failed.accessor = accessor.address();
        failed.identity = identity.address();
        RecallScope<FileRead *> reading(file_reading, &failed);
        int error = 0;
        assert(file_input_open(handle.address(), 0, 99, &error) == 7);
        assert(!input_mask);
    }
    assert(input_cancels == 1 && input_begins == 2);
    fclose(input_file);
    input_file = nullptr;
    // Every cache participant must be from the same genuine fresh-load
    // operation, or the exact FileData owner already captured by this getter.
    Object cache_data[2], cache_control[2], cache_result;
    for (auto &control : cache_control) {
        put(control.address() + 8, int32_t(1));
        put(control.address() + 12, int32_t(1));
    }
    file_original[11] = reinterpret_cast<void *>(input_cache_original);
    file_original[12] = reinterpret_cast<void *>(input_fresh_original);
    input_cache_object = cache_data[0].address();
    input_cache_control = cache_control[0].address();
    {
        FileRead mixed {};
        RecallScope<FileRead *> reading(file_reading, &mixed);
        assert(file_cache_get(101, cache_result.address(), 202) == 0);
        assert(mixed.tag_seen && !mixed.cached && mixed.fresh_count == 1);
        input_cache_fresh = false;
        assert(file_cache_get(101, cache_result.address(), 202) == 0);
        assert(!mixed.cached && mixed.fresh_count == 1);
        input_cache_object = cache_data[1].address();
        input_cache_control = cache_control[1].address();
        assert(file_cache_get(101, cache_result.address(), 202) == 0);
        assert(mixed.cached);
    }
    assert(scalar<int32_t>(cache_control[0].address() + 8) == 1 &&
           scalar<int32_t>(cache_control[1].address() + 8) == 1);
    // A GRID/PSSI getter can create the same FileData later used by CUES.
    // Its physical inception is independent of a cue request or active read.
    const azn_file_cue_callbacks common_callbacks = {
        sizeof common_callbacks, 2, input_begin, input_complete, input_hold, input_opened, common_api};
    file_callbacks.store(&common_callbacks);
    Object physical_path;
    const char analysis_path[] = "/media/fixture/PIONEER/USBANLZ/P01/ANLZ0000.DAT";
    put(physical_path.address(), file_profile->path_table);
    put(physical_path.address() + 0x10, reinterpret_cast<uintptr_t>(analysis_path));
    put(physical_path.address() + 0x18, uint64_t(sizeof analysis_path - 1));
    common_handle = handle.address();
    common_path = physical_path.address();
    input_cache_object = cache_data[0].address();
    input_cache_control = cache_control[0].address();
    input_cache_fresh = true;
    assert(!file_reading && file_cache_get(101, cache_result.address(), 202) == 0);
    assert(common_begins == 1 && common_seals == 1 && common_holds == 1);
    assert(scalar<int32_t>(cache_control[0].address() + 8) == 1 &&
           scalar<int32_t>(cache_control[0].address() + 12) == 2);
    input_cache_fresh = false;
    {
        FileRead cached {};
        RecallScope<FileRead *> reading(file_reading, &cached);
        assert(file_cache_get(101, cache_result.address(), 202) == 0);
        assert(cached.inputs.seen && !cached.inputs.bad && cached.inputs.tokens[0] == 2001);
        assert(cached.inputs.join() == 9000 && common_joins == 1 && common_begins == 1);
        assert(file_cache_get(101, cache_result.address(), 202) == 0 && common_holds == 2);
        input_cache_object = cache_data[1].address();
        input_cache_control = cache_control[1].address();
        assert(file_cache_get(101, cache_result.address(), 202) == 0);
        assert(cached.inputs.bad && !cached.inputs.join() && common_begins == 1);
    }
    assert(common_holds == 1);
    common_mask = 2;
    input_cache_fresh = true;
    assert(file_cache_get(101, cache_result.address(), 203) == 0);
    assert(common_begins == 2 && common_seals == 2 && common_holds == 2);
    input_cache_fresh = false;
    {
        FileRead paired {};
        RecallScope<FileRead *> reading(file_reading, &paired);
        assert(file_cache_get(101, cache_result.address(), 203) == 0);
        input_cache_object = cache_data[0].address();
        input_cache_control = cache_control[0].address();
        assert(file_cache_get(101, cache_result.address(), 202) == 0);
        assert(paired.inputs.tokens[0] == 2001 && paired.inputs.tokens[1] == 2002 && paired.inputs.join() == 9000);
    }
    // A reused payload address with a different live control is not the old
    // FileData incarnation, even when its current bytes could be identical.
    input_cache_control = cache_control[1].address();
    {
        FileRead reused_address {};
        RecallScope<FileRead *> reading(file_reading, &reused_address);
        assert(file_cache_get(101, cache_result.address(), 202) == 0);
        assert(reused_address.inputs.bad && !reused_address.inputs.join());
    }
    for (auto &origin : file_input_origins) file_input_release(origin);
    assert(!common_holds && scalar<int32_t>(cache_control[0].address() + 8) == 1 &&
           scalar<int32_t>(cache_control[0].address() + 12) == 1 && scalar<int32_t>(cache_control[1].address() + 12) == 1);
    /* A stock repeated open is one physical input, but every successful
     * descriptor must prove that same inception. Ambiguous retries cannot
     * manufacture an origin for a subsequently cached FileData. */
    Object second_handle, second_path;
    const char other_path[] = "/media/fixture/PIONEER/USBANLZ/P02/ANLZ0000.DAT";
    put(second_handle.address(), file_profile->input_table);
    put(second_path.address(), file_profile->path_table);
    put(second_path.address() + 0x10, reinterpret_cast<uintptr_t>(other_path));
    put(second_path.address() + 0x18, uint64_t(sizeof other_path - 1));
    common_other_handle = second_handle.address();
    common_other_path = second_path.address();
    input_cache_object = cache_data[0].address();
    input_cache_control = cache_control[0].address();
    input_cache_fresh = true;
    common_mask = 1;
    for (common_reopen_mode = 1; common_reopen_mode <= 7; ++common_reopen_mode) {
        unsigned begins = common_begins, seals = common_seals, mask = 0;
        assert(file_cache_get(101, cache_result.address(), 202) == 0);
        assert(common_begins == begins + 1 && common_seals == seals + 1 && !common_holds);
        assert(!file_input_find(common_api(), input_cache_object, input_cache_control, mask) && !mask);
    }
    common_reopen_mode = 0;
    fclose(input_file);
    input_file = nullptr;
    common_handle = common_path = 0;
    file_callbacks.store(&callbacks);
#if defined(__aarch64__)
    map_adapter_cases(std::atoi(argv[1]), &common_callbacks);
#endif
    put(atoms[1].address(), file_profile->atoms[0]);
    FileRead foreign {};
    assert(!merged_rows(vector.address(), foreign));
    put(block.address(), blob.address());
    put(block.address() + 8, uint64_t(16));
    put(blob.address(), uint32_t(16));
    FileServe wire {};
    assert(wire_rows(rd, block.address(), wire) && wire.rows[0].bytes == 12);
    put(blob.address(), uint32_t(12));
    assert(!wire_rows(rd, block.address(), wire));
    // Preserve a witnessed insertion through sorting, without accepting an
    // identical row from another immutable object or a duplicated native key.
    FileImport imported {};
    imported.count = imported.decoded = imported.bank[0].added = 1;
    imported.bank[0].content = b;
    imported.bank[0].pin = azn_owned_pin(b);
    imported.bank[0].row[0].key = file_key(rows_b.address());
    imported.bank[0].row[0].token = 111;
    finish_import(imported);
    assert(imported_memory_row(b, 1) == 111 && !imported_memory_row(a, 1));
    uint64_t file_id = 0;
    auto link = azn_memory_copied(0, fence, load, context.address(), context.address() + 0x160, retained.address(), 20, &file_id);
    assert(file_id == 111);
    assert(link.state == AZN_MEMORY_LINK_READY && link.provider == selected.deck[0].provider &&
           link.stamp == selected.deck[0].serial && link.slot == 1);
    assert(set_content(foreign_holder.bytes, &a, nullptr));
    assert(!azn_memory_copied(0, fence, load, context.address(), context.address() + 0x160, retained.address(), 20, nullptr).state);
    azn_memory_retire(0, ++fence);
    for (auto &r : receipts)
        if (r.memory_pin) drop(r.memory_pin);
    for (unsigned i = 0; i < cache_count; i++) {
        for (auto cache : {cached_content, cached_holder}) {
            auto id = cache[i].exchange(0);
            if (id) drop(id);
        }
    }
    for (auto &origin : imported_files) {
        if (origin.pin) {
            hold_file_content(origin, 0);
            azn_pin_drop(origin.pin);
        }
        origin = FileContent {};
    }
    for (auto &origin : file_origins) {
        release_origins(origin);
        origin = {};
    }
    for (auto &pair : rd.owners)
        for (auto &owner : pair) file_atom_release(owner);
    assert(!input_holds && scalar<int32_t>(controls[0].address() + 8) == 1 &&
           scalar<int32_t>(controls[1].address() + 8) == 1);
    file_callbacks.store(nullptr);
    while (reap_needed.load()) azn_memory_maintenance();
    assert(refs > 0 && refs == releases && wake_count.load());
    puts("PASS memory: matched cached pack, immutable UTF-8 rows A/B/A, empty, holder change, competing commits, stale load, balanced refs");
    puts("PASS FILE input: fresh native open/fd, partial input refusal, original cached-atom custody, changed bytes and balanced owners");
    puts("PASS FILE common input: repeated-open physical proof, ambiguous retries rejected, cached DAT/EXT custody, address reuse and weak-only retention");
}
