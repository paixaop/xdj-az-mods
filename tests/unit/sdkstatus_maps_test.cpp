/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/azstem/sdk_status_maps.cpp"
#include <cassert>
#include <cstdio>
#include <vector>
namespace {
struct Ref {
    uintptr_t object;
    unsigned count;
};
std::vector<Ref> refs;
std::vector<azstatus_native_receipt> observed;
azn_pack_observer observer;
uintptr_t playing;
uint64_t epoch = 7;
unsigned gaps;
void (*during_current)();
uintptr_t replacement_holder, replacement_content;
template <size_t N>
struct alignas(16) Buf {
    unsigned char bytes[N] = {};
    uintptr_t addr() { return reinterpret_cast<uintptr_t>(bytes); }
};
template <class T>
void put(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void *>(a), &v, sizeof v); }
void drain(const azn_maps_api *api) {
    for (auto &r : observed) api->release(&r.map);
    observed.clear();
}
} // namespace
extern "C" uint64_t azn_owned_pin(uintptr_t p) {
    if (!p) return 0;
    refs.push_back({p, 1});
    return refs.size();
}
extern "C" int azn_pin_hold(uint64_t t, uintptr_t *out) {
    if (!t || t > refs.size() || !refs[t - 1].count) return 0;
    refs[t - 1].count++;
    *out = refs[t - 1].object;
    return 1;
}
extern "C" void azn_pin_drop(uint64_t t) {
    if (t) {
        assert(t <= refs.size() && refs[t - 1].count);
        refs[t - 1].count--;
    }
}
extern "C" int azn_pack_acquire(unsigned d, azn_pack_view *v) {
    if (d || !playing) return 0;
    *v = {azn_owned_pin(playing), 5, 13, epoch, playing};
    return 1;
}
extern "C" int azn_pack_current(unsigned d, const azn_pack_view *v) {
    if (during_current) {
        auto interleave = during_current;
        during_current = nullptr;
        interleave();
    }
    return !d && v->load == epoch && v->object == playing;
}
extern "C" int azn_pack_retains(uintptr_t pack) { return pack && playing == pack; }
extern "C" int azn_pack_observe(azn_pack_observer o) {
    observer = o;
    return 1;
}
extern "C" uint64_t azn_interest(unsigned) { return AZ_STATUS_ALL_V1; }
extern "C" void azn_map_gap(unsigned) { ++gaps; }
extern "C" int azn_offer(azstatus_native_receipt *r) {
    observed.push_back(*r);
    return 1;
}
extern "C" uint64_t azn_map_begin_mask(unsigned mask) { return mask; }
extern "C" void azn_map_end_mask(uint64_t ticket) { assert(ticket <= 1); }
extern "C" int azn_offer_map_cut(azstatus_native_receipt *r) { return azn_offer(r); }
int main(int argc, char **argv) {
    assert(argc == 2 || (argc == 3 && std::strcmp(argv[2], "browse-retain") == 0));
    const int build = argv[1][0] - '0';
    assert(azn_maps_configure(build));
    const azn_maps_api *api = azn_maps_service();
    assert(api && observer);
    Buf<0x150> pack;
    Buf<0x80> gh, ph, gc, pc, wrong;
    const Profile &pr = profiles[build - 1];
    put(pack.addr(), pr.pack);
    put(gh.addr(), pr.grid_holder);
    put(ph.addr(), pr.phrase_holder);
    put(gc.addr(), pr.grid_content);
    put(pc.addr(), pr.phrase_content);
    put(pack.addr() + 0xd8, gh.addr());
    put(pack.addr() + 0x120, ph.addr());
    put(gh.addr() + 0x28, gc.addr());
    put(gh.addr() + 0x30, int64_t(-441));
    put(gh.addr() + 0x40, uint32_t(44100));
    put(ph.addr() + 0x28, pc.addr());
    struct Cell {
        int64_t sample;
        double bpm;
    } beats[] = {{441, 122}, {22491, 122}, {44541, 122}, {66591, 122}};
    uint32_t sections[][5] = {{1, 1, 2, 0, 0}, {2, 2, 4, 0, 0}};
    int32_t downbeats[] = {0, 3};
    put(gc.addr() + 0x38, reinterpret_cast<uintptr_t>(downbeats));
    put(gc.addr() + 0x40, int32_t(2));
    put(gc.addr() + 0x28, reinterpret_cast<uintptr_t>(beats));
    put(gc.addr() + 0x30, int32_t(4));
    put(pc.addr() + 0x40, reinterpret_cast<uintptr_t>(sections));
    put(pc.addr() + 0x48, int32_t(2));
    put(pc.addr() + 0x3c, uint32_t(1));
    auto prove = [&](uintptr_t p) { return api->prove_pair(138, p, gh.addr(), gc.addr(), ph.addr(), pc.addr()); };
    assert(!prove(wrong.addr()));
    assert(prove(pack.addr()) && observed.empty()); // authenticated GUI pair before deck completion
    playing = pack.addr();
    observer(0, 5, 13, epoch);
    assert(observed.size() == 1 && observed[0].load_generation == 7);
    auto snap = observed[0].map;
    if (argc == 3) {
        // Browse clears the only GUI owner while the track stays loaded.
        api->retire_pair(138);
        assert(observed.size() == 1 && azn_maps_current(0, epoch, &snap));
        // An actual holder edit is still observed while Browse owns the screen.
        map_writer_original[1] = reinterpret_cast<void *>(+[](uintptr_t dst, int64_t offset) -> uintptr_t {
            put(dst + 8, offset);
            return 1;
        });
        assert(grid_retime(gh.addr() + 0x28, -440) == 1 && observed.size() == 2);
        assert(observed.back().map.grid.revision == snap.grid.revision + 1);
        assert(!azn_maps_current(0, epoch, &snap) && azn_maps_current(0, epoch, &observed.back().map));
        // A real withdrawal terminates the lease even without a GUI callback.
        playing = 0;
        observer(0, 5, 14, 0);
        assert(observed.size() == 2 && !azn_maps_current(0, epoch, &observed.back().map));
        drain(api);
        for (const auto &ref : refs) assert(!ref.count);
        assert(!gaps);
        puts("PASS Browse detach retains the loaded map through real edits and releases it at load retirement");
        return 0;
    }
    az_beat b[4];
    az_phrase row[2];
    uint32_t n;
    assert(api->read(&snap.grid, AZNM_GRID, nullptr, 0, &n) == AZ_OK && n == 4);
    assert(api->read(&snap.grid, AZNM_GRID, b, 3, &n) == AZ_LIMIT);
    assert(api->read(&snap.grid, AZNM_GRID, b, 4, &n) == AZ_OK && n == 4 &&
           compare_timestamps(b[0].time_s, 0, 1, snap.grid.sample_rate, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(b[3].time_s, 1.5, 1, snap.grid.sample_rate, 1) == AZ_TIME_EQUAL && b[1].bpm == 122);
    int64_t ticks[4];
    uint32_t clock = 0;
    assert(api->read_samples(&snap.grid, ticks, 3, &n, &clock) == AZ_LIMIT && !clock);
    assert(api->read_samples(&snap.grid, ticks, 4, &n, &clock) == AZ_OK && n == 4 && clock == 44100);
    assert(compare_timestamps(ticks[0], 0, clock, clock, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(ticks[3], 66150, clock, clock, 1) == AZ_TIME_EQUAL);
    auto negative_origin = snap.grid;
    --negative_origin.offset_samples;
    assert(api->read_samples(&negative_origin, ticks, 4, &n, &clock) == AZ_OK &&
           committed_grid_order(ticks[0], -1, clock) == AZ_TIME_EQUAL);
    assert(b[0].beat_in_bar == 1 && b[1].beat_in_bar == 2 && b[2].beat_in_bar == 3 && b[3].beat_in_bar == 1);
    assert(api->read(&snap.phrases, AZNM_PHRASES, row, 2, &n) == AZ_OK && n == 2 &&
           row[0].start_beat == 0 && row[0].end_beat == 1 && row[1].raw_kind == 2 && row[1].end_beat == 3);
    assert(prove(pack.addr()) && observed.size() == 1);
    assert(api->prove_pair(239, pack.addr(), gh.addr(), gc.addr(), ph.addr(), pc.addr()));
    assert(observed.size() == 1 && azn_maps_current(0, epoch, &snap));
    put(gh.addr() + 0x30, int64_t(0));
    assert(prove(pack.addr()) && observed.size() == 2);
    assert(observed[1].map.grid.provider == snap.grid.provider && observed[1].map.grid.revision == snap.grid.revision + 1);
    assert(observed[1].map.phrases.revision == snap.phrases.revision);
    assert(api->read(&snap.grid, AZNM_GRID, b, 4, &n) == AZ_OK &&
           compare_timestamps(b[0].time_s, 0, 1, snap.grid.sample_rate, 1) == AZ_TIME_EQUAL); // immutable callback cut
    map_writer_original[0] = reinterpret_cast<void *>(+[](uintptr_t dst, uintptr_t src) -> uintptr_t {
        put(dst, read_value<uintptr_t>(src));
        return 1;
    });
    map_writer_original[1] = reinterpret_cast<void *>(+[](uintptr_t dst, int64_t offset) -> uintptr_t {
        put(dst + 8, offset);
        return 1;
    });
    assert(azn_maps_current(0, epoch, &observed.back().map));
    Buf<0x80> new_grid = gc;
    uintptr_t typed = new_grid.addr();
    assert(grid_assign(gh.addr() + 0x28, reinterpret_cast<uintptr_t>(&typed)) == 1 && observed.size() == 3);
    typed = gc.addr();
    grid_assign(gh.addr() + 0x28, reinterpret_cast<uintptr_t>(&typed));
    grid_retime(gh.addr() + 0x28, -441);
    assert(observed.size() == 5);
    assert(observed[4].map.grid.revision == snap.grid.revision + 4 && observed[4].map.phrases.revision == snap.phrases.revision);
    assert(!azn_maps_current(0, epoch, &snap) && azn_maps_current(0, epoch, &observed.back().map));
    map_writer_original[3] = reinterpret_cast<void *>(+[](uintptr_t, unsigned) -> uintptr_t { return 37; });
    Buf<0x80> new_phrase = pc;
    typed = new_phrase.addr();
    phrase_argument = reinterpret_cast<uintptr_t>(&typed);
    put(ph.addr() + 0x28, typed);
    assert(!azn_maps_current(0, epoch, &observed.back().map));
    assert(phrase_broadcast(ph.addr(), 0) == 37 && observed.size() == 6);
    typed = pc.addr();
    put(ph.addr() + 0x28, typed);
    phrase_broadcast(ph.addr(), 0);
    phrase_argument = 0;
    assert(observed.size() == 7 && observed.back().map.phrases.revision == snap.phrases.revision + 2);
    api->retire_pair(777);
    assert(observed.size() == 7);
    auto before_last_owner = observed.back().map;
    api->retire_pair(138);
    assert(observed.size() == 7 && azn_maps_current(0, epoch, &observed.back().map));
    api->retire_pair(239);
    assert(observed.size() == 7 && azn_maps_current(0, epoch, &before_last_owner));
    drain(api);
    assert(!gaps);
    assert(prove(pack.addr()) && observed.empty());
    api->seed(0);
    assert(observed.size() == 1);
    auto before_reader = observed.back().map;
    replacement_holder = gh.addr();
    replacement_content = new_grid.addr();
    during_current = +[] {
        assert(grid_assign(replacement_holder + 0x28, reinterpret_cast<uintptr_t>(&replacement_content)) == 1);
    };
    /* A genuine native update crosses a validation reader's final pack check.
     * The reader must reject the old edition without making the writer lose
     * its captured update merely because validation was in progress. */
    assert(!azn_maps_current(0, epoch, &before_reader));
    assert(!during_current && !gaps && observed.size() == 2);
    assert(observed.back().map.grid.provider == before_reader.grid.provider &&
           observed.back().map.grid.revision == before_reader.grid.revision + 1);
    assert(azn_maps_current(0, epoch, &observed.back().map));
    auto before_retire = observed.back().map;
    during_current = +[] {
        retire_pair(138);
        playing = 0;
        observer(0, 5, 14, 0);
    };
    assert(!azn_maps_current(0, epoch, &before_retire) && !during_current && !gaps);
    assert(observed.size() == 2);
    assert(api->read(&before_reader.grid, AZNM_GRID, b, 4, &n) == AZ_OK && n == 4);
    /* Reusing exactly the same native addresses after retirement cannot make
     * either old stamp valid, even with unchanged row bytes. */
    assert(api->prove_pair(138, pack.addr(), gh.addr(), new_grid.addr(), ph.addr(), pc.addr()));
    playing = pack.addr();
    ++epoch;
    observer(0, 5, 14, epoch);
    assert(observed.size() == 3 && observed.back().load_generation == epoch);
    assert(!azn_maps_current(0, epoch, &before_reader) && !azn_maps_current(0, epoch, &before_retire));
    assert(azn_maps_current(0, epoch, &observed.back().map));
    playing = 0;
    api->retire_pair(138);
    drain(api);
    for (const auto &r : refs) assert(!r.count);
    puts("PASS typed map owners, load binding, clock/offset, immutable sections, revisions, reader/writer collision, retirement and address reuse");
}
