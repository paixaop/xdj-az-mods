/* SPDX-License-Identifier: GPL-3.0-only */
namespace {
unsigned analysis_made, analysis_freed, analysis_notified;
std::vector<uintptr_t> analysis_allocated;
void (*during_make)();
void test_be16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
void test_be32(uint8_t *p, uint32_t n) { p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n; }
unsigned char test_grant(uintptr_t p) {
    uint32_t count = read_value<uint32_t>(p, 0xc);
    assert(count);
    put(p + 0xc, count + 1);
    return 1;
}
unsigned char test_release(uintptr_t p) {
    uint32_t count = read_value<uint32_t>(p, 0xc);
    assert(count);
    put(p + 0xc, count - 1);
    return count == 1;
}
void test_trash(uintptr_t, uintptr_t p) {
    assert(!read_value<uint32_t>(p, 0xc));
    for (auto &allocated : analysis_allocated) if (allocated == p) {
        allocated = 0;
        free(reinterpret_cast<void *>(read_value<uintptr_t>(p, 0x40)));
        free(reinterpret_cast<void *>(p));
        ++analysis_freed;
        return;
    }
}
AnalysisPhrase test_make(const AnalysisAtomRef *ref) {
    const AnalysisAtom &atom = *ref->value;
    assert(!ref->control && atom.header == 32 && atom.prefix == 18 && atom.stride == 24 &&
           atom.rows.size == atom.count * 24 && atom.raw.size == 32 + atom.rows.size);
    assert(!std::memcmp(reinterpret_cast<void *>(atom.raw.data), "PSSI", 4));
    uintptr_t p = reinterpret_cast<uintptr_t>(calloc(1, 0x50));
    auto *rows = static_cast<uint32_t *>(calloc(atom.count, 20));
    assert(p && rows);
    put(p, profile->phrase_content);
    put(p + 0xc, uint32_t(1));
    assert(atom.mood >= 1 && atom.mood <= 3);
    put(p + 0x3c, uint32_t(atom.mood - 1));
    put(p + 0x40, reinterpret_cast<uintptr_t>(rows));
    put(p + 0x48, atom.count);
    for (unsigned i = 0; i < atom.count; i++) {
        const auto *raw = reinterpret_cast<const uint8_t *>(atom.rows.data);
        rows[5 * i] = analysis_u16(raw + i * 24 + 4);
        rows[5 * i + 1] = analysis_u16(raw + i * 24 + 2);
        rows[5 * i + 2] = i + 1 < atom.count ? analysis_u16(raw + (i + 1) * 24 + 2) : atom.end;
    }
    analysis_allocated.push_back(p);
    ++analysis_made;
    if (during_make) { auto callback = during_make; during_make = nullptr; callback(); }
    AnalysisPhrase result;
    result.content = p;
    return result;
}
int test_analysis(const azn_maps_api *api) {
    assert(profile == &profiles[1]);
    aznm_packet initial {};
    assert(api->capture(0, epoch, &initial) == AZ_UNSUPPORTED);
    analysis_ops.make = test_make;
    analysis_ops.memory = +[](AnalysisMemory *memory, const void *data, size_t size) {
        memory->data = reinterpret_cast<uintptr_t>(malloc(size)); memory->size = size;
        assert(memory->data); std::memcpy(reinterpret_cast<void *>(memory->data), data, size);
    };
    analysis_ops.memory_drop = +[](AnalysisMemory *memory) { free(reinterpret_cast<void *>(memory->data)); };
    analysis_ops.grant = test_grant;
    analysis_ops.release = test_release;
    analysis_ops.trash = test_trash;
    analysis_ready.store(true);
    map_writer_installed = 15;
    map_writer_original[1] = reinterpret_cast<void *>(+[](uintptr_t dst, int64_t offset) -> uintptr_t {
        put(dst + 8, offset); return 1;
    });
    map_writer_original[3] = reinterpret_cast<void *>(+[](uintptr_t, uintptr_t) -> uintptr_t {
        ++analysis_notified; return 0;
    });
    analysis_refs = true;
    Buf<0x150> pack;
    Buf<0x80> gh, ph, gc, pc;
    for (uintptr_t p : {pack.addr(), gh.addr(), ph.addr(), gc.addr(), pc.addr()}) put(p + 0xc, uint32_t(1));
    put(pack.addr(), profile->pack); put(gh.addr(), profile->grid_holder); put(ph.addr(), profile->phrase_holder);
    put(gc.addr(), profile->grid_content); put(pc.addr(), profile->phrase_content);
    put(pack.addr() + 0xd8, gh.addr()); put(pack.addr() + 0x120, ph.addr());
    put(gh.addr() + 0x28, gc.addr()); put(gh.addr() + 0x30, int64_t(0)); put(gh.addr() + 0x40, uint32_t(48000));
    put(ph.addr() + 0x28, pc.addr());
    struct Cell { int64_t sample; double bpm; } cells[] = {{0, 120}, {24000, 120}, {48000, 120}, {72000, 120}};
    put(gc.addr() + 0x28, reinterpret_cast<uintptr_t>(cells)); put(gc.addr() + 0x30, int32_t(4));
    playing = pack.addr();
    assert(api->prove_pair(138, pack.addr(), gh.addr(), gc.addr(), ph.addr(), pc.addr()));
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    assert(api->current(0, epoch, &initial) == AZ_OK && api->current(0, epoch + 1, &initial) == AZ_STALE);
    assert(api->capture(4, epoch, &initial) == AZ_INVALID);
    replacement_holder = gh.addr();
    during_current = +[] {
        during_current = +[] { grid_retime(replacement_holder + 0x28, 1); };
    };
    aznm_packet raced {};
    assert(api->capture(0, epoch, &raced) == AZ_STALE && !raced.grid.content && !gaps);
    api->release(&initial);
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    uint8_t pssi[80] = {};
    std::memcpy(pssi, "PSSI", 4); test_be32(pssi + 4, 32); test_be32(pssi + 8, sizeof pssi);
    test_be16(pssi + 14, 24); test_be16(pssi + 16, 2); test_be16(pssi + 18, 1); test_be16(pssi + 26, 4);
    test_be16(pssi + 34, 1); test_be16(pssi + 36, 1); test_be16(pssi + 58, 2); test_be16(pssi + 60, 2);
    test_be16(pssi + 26, 5);
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_INVALID && !analysis_made);
    test_be16(pssi + 26, 4); test_be16(pssi + 58, 1);
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_INVALID && !analysis_made);
    test_be16(pssi + 58, 2);
    grid_retime(gh.addr() + 0x28, 2);
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_STALE && !analysis_made);
    api->release(&initial);
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    during_make = +[] { ++epoch; };
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_STALE);
    assert(analysis_made == 1 && analysis_freed == 1 && !analysis_notified && read_value<uintptr_t>(ph.addr(), 0x28) == pc.addr());
    api->release(&initial);
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    replacement_holder = gh.addr();
    during_make = +[] { grid_retime(replacement_holder + 0x28, 3); };
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_STALE);
    assert(analysis_made == 2 && analysis_freed == 2 && !analysis_notified);
    api->release(&initial);
    assert(api->capture(0, epoch, &initial) == AZ_OK);
    shared_second = true;
    size_t before = observed.size();
    assert(api->publish_phrases(0, epoch, &initial, pssi, sizeof pssi) == AZ_OK);
    assert(analysis_made == 3 && analysis_freed == 2 && analysis_notified == 1 && observed.size() == before + 2);
    assert(observed[before].deck == 0 && observed[before + 1].deck == 1);
    for (size_t i = before; i < observed.size(); i++) {
        assert(observed[i].map.grid.provider == initial.grid.provider && observed[i].map.grid.revision == initial.grid.revision);
        assert(observed[i].map.phrases.provider == initial.phrases.provider && observed[i].map.phrases.revision == initial.phrases.revision + 1);
    }
    uint32_t count;
    assert(api->read(&initial.phrases, AZNM_PHRASES, nullptr, 0, &count) == AZ_OK && !count);
    assert(api->current(0, epoch, &initial) == AZ_STALE);
    aznm_packet refreshed {};
    assert(api->capture(1, epoch + 1, &refreshed) == AZ_OK);
    az_phrase rows[2];
    assert(api->read(&refreshed.phrases, AZNM_PHRASES, rows, 2, &count) == AZ_OK && count == 2 &&
           rows[0].start_beat == 0 && rows[0].end_beat == 1 && rows[1].end_beat == 3);
    assert(rows[0].raw_mood == 1 && rows[1].raw_mood == 1);
    assert(api->publish_phrases(1, epoch + 1, &refreshed, pssi, sizeof pssi) == AZ_NOT_FOUND && analysis_made == 3);
    uintptr_t installed = read_value<uintptr_t>(ph.addr(), 0x28);
    playing = 0;
    api->retire_pair(138);
    drain(api);
    api->release(&initial); api->release(&refreshed);
    analysis_drop(installed); /* native holder destruction */
    for (const auto &ref : refs) assert(!ref.count);
    assert(analysis_made == analysis_freed && !gaps);
    for (uintptr_t allocated : analysis_allocated) assert(!allocated);
    puts("PASS loaded phrase publication, exact grid/reload fences, shared-pack notifications, immutable snapshots and counted release");
    return 0;
}
} // namespace
