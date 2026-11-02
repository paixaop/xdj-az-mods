/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcuestore.h"
#include "../common/azcueprofile.h"
#include "../common/azcuepersist.h"
#include <cstring>
#include <limits>
#ifdef AZSDK_CUE_DIAGNOSTIC
#include <atomic>
#include <cstdio>
#include <time.h>
#endif
namespace azcuestore {
namespace {
/* Diagnostic builds only: at most one native snapshot diagnostic per second,
 * and at most 128 for the lifetime of the process. Never inspect request data. */
void diagnostic(const char *stage, uintptr_t position = 0, uintptr_t source = 0, uintptr_t pack = 0) noexcept {
#ifdef AZSDK_CUE_DIAGNOSTIC
    static std::atomic<uint64_t> last {0};
    static std::atomic<unsigned> emitted {0};
    timespec clock {};
    if (clock_gettime(CLOCK_MONOTONIC, &clock) || emitted.load(std::memory_order_relaxed) >= 128) return;
    const uint64_t now = uint64_t(clock.tv_sec) * 1000000000ULL + uint64_t(clock.tv_nsec);
    uint64_t previous = last.load(std::memory_order_relaxed);
    if (previous && compare_timestamps(now, previous, 1000000000, 1, 1) == AZ_TIME_EQUAL) return;
    if (!last.compare_exchange_strong(previous, now, std::memory_order_relaxed)) return;
    if (emitted.fetch_add(1, std::memory_order_relaxed) >= 128) return;
    char position_hex[33] = {}, pack_hex[33] = {};
    const char digits[] = "0123456789abcdef";
    if (position && pack && !std::strcmp(stage, "track16")) {
        const auto *a = reinterpret_cast<const unsigned char *>(position + 8);
        const auto *b = reinterpret_cast<const unsigned char *>(pack + 0x80);
        for (unsigned i = 0; i < 16; ++i) {
            position_hex[i * 2] = digits[a[i] >> 4]; position_hex[i * 2 + 1] = digits[a[i] & 15];
            pack_hex[i * 2] = digits[b[i] >> 4]; pack_hex[i * 2 + 1] = digits[b[i] & 15];
        }
    }
    std::fprintf(stderr, "SDK_CUE_STORE stage=%s native_position=%p native_source=%p native_pack=%p position16=%s pack16=%s\n",
                 stage, reinterpret_cast<void *>(position), reinterpret_cast<void *>(source), reinterpret_cast<void *>(pack), position_hex, pack_hex);
#else
    (void)stage; (void)position; (void)source; (void)pack;
#endif
}
template <class T>
T read(uintptr_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(a), sizeof v);
    return v;
}
uintptr_t word(uintptr_t a) { return read<uintptr_t>(a); }
template <class F>
F method(uintptr_t object, unsigned slot) { return reinterpret_cast<F>(word(word(object) + slot * sizeof(uintptr_t))); }
bool nonzero(const unsigned char *p, size_t n) {
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) v |= p[i];
    return v != 0;
}
uint64_t hash(uint64_t h, const void *p, size_t n) {
    const auto *b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}
bool complete(const Profile &p) { return p.copy_source && p.copy_content && p.release_ref && p.position && p.get_cue && p.destroy_cue && p.convert && p.destroy_property && p.where_from; }
struct CueVector {
    uintptr_t begin = 0;
    size_t count = 0;
};
bool cue_vector(uintptr_t content, size_t limit, CueVector &out) {
    out = {};
    if (!content) return true;
    const uintptr_t begin = word(content + 0x28), end = word(content + 0x30);
    if (end < begin || (end - begin) % 0x80 || (end - begin) / 0x80 > limit || (end != begin && !begin)) return false;
    out.begin = begin;
    out.count = (end - begin) / 0x80;
    return true;
}
bool same_variant(uintptr_t a, uintptr_t b) {
    const int32_t tag = read<int32_t>(a);
    if (tag != read<int32_t>(b) || read<uint32_t>(a + 0x18) != read<uint32_t>(b + 0x18)) return false;
    if (tag == 0) return true; /* Native copy does not read the inactive union. */
    /* Native stored-Cue serializer 1177a00 reads a single MPEG point (tag 2)
     * as two u32 values, or a single FLAC point (tag 4) as two u64 and one u32.
     * Ref/control addresses and FLAC tail padding are not values. Full seek
     * arrays (tags 1/3) and unproved lengths must fail closed. */
    if ((tag != 2 && tag != 4) || read<uint32_t>(a + 0x18) != 1) return false;
    const uintptr_t value_a = word(a + 8), value_b = word(b + 8);
    return value_a && value_b && !std::memcmp(reinterpret_cast<const void *>(value_a), reinterpret_cast<const void *>(value_b), tag == 2 ? 8 : 20);
}
enum class CueDelta { None, Auto, Comment, Color };
bool same_cue(uintptr_t a, uintptr_t b, CueDelta delta, bool desired, uint32_t source_rate,
              const char *comment = nullptr, uint32_t rgb = 0) {
    if (read<unsigned char>(a) != read<unsigned char>(b) || read<uint32_t>(a + 4) != read<uint32_t>(b + 4) ||
        (read<unsigned char>(a) != 0xff && !azcue_timestamps_equal(azcue_read_timestamps(reinterpret_cast<const void *>(a)), azcue_read_timestamps(reinterpret_cast<const void *>(b)), source_rate)) ||
        read<uint32_t>(a + 0x2a) != read<uint32_t>(b + 0x2a)) return false;
    if (delta == CueDelta::Auto ? read<unsigned char>(b + 0x28) != unsigned(desired)
                               : read<unsigned char>(a + 0x28) != read<unsigned char>(b + 0x28)) return false;
    const uint32_t old_color = read<uint32_t>(a + 0x78), new_color = read<uint32_t>(b + 0x78);
    const bool memory_color = delta == CueDelta::Color && read<unsigned char>(a) == 0;
    if (!memory_color && read<unsigned char>(a + 0x7c) != read<unsigned char>(b + 0x7c)) return false;
    if (delta == CueDelta::Color) {
        // A Memory RGB edit also updates the native palette index. The native
        // opaque color constructor uses 0xff; uncolored Memory black can be 0.
        if (new_color != (0xff000000u | rgb) && !(memory_color && !rgb && !new_color)) return false;
        if (memory_color) {
            const uint32_t palette[] = {0, 0xff00c6, 0xff0000, 0xff7100, 0xffff00, 0x00e300, 0x00c3ff, 0x0051ff, 0x9c08ff};
            unsigned index = 0;
            while (index < sizeof palette / sizeof *palette && palette[index] != rgb) ++index;
            if (index == sizeof palette / sizeof *palette || read<unsigned char>(b + 0x7c) != index) return false;
        }
    } else if (old_color != new_color) return false;
    if (!same_variant(a + 0x30, b + 0x30) || !same_variant(a + 0x50, b + 0x50)) return false;
    const uintptr_t text_a = word(a + 0x70), text_b = word(b + 0x70);
    const char *expected = delta == CueDelta::Comment ? comment : text_a ? reinterpret_cast<const char *>(text_a) : "";
    return expected && !std::strcmp(expected, text_b ? reinterpret_cast<const char *>(text_b) : "");
}
} // namespace
Profile native_profile(int build, uintptr_t bias) noexcept {
    Profile p;
#if defined(__aarch64__) && !defined(AZSDK_CUE_STORE_TESTING)
    if (build != 1 && build != 2) return p;
    azcue_profile profile {};
    if (!azsdk_cue_main_profile(build, &profile)) return p;
    // Caller bias must name the same main image that passed byte validation.
    azcue_profile nominal {};
    if (!azsdk_cue_profile_get(build, bias, &nominal) || nominal.ctor.address != profile.ctor.address) return p;
#define NATIVE(field, index) p.field = reinterpret_cast<decltype(p.field)>(profile.store[index].address)
    NATIVE(copy_source, 0);
    NATIVE(copy_content, 1);
    NATIVE(release_ref, 2);
    NATIVE(position, 3);
    NATIVE(get_cue, 4);
    NATIVE(destroy_cue, 5);
    NATIVE(convert, 6);
    NATIVE(destroy_property, 7);
    NATIVE(has_cue, 8);
    NATIVE(get_current_cue, 9);
    NATIVE(copy_property, 10);
    NATIVE(serialize_property, 11);
    NATIVE(where_from, 12);
#undef NATIVE
#else
    (void) build;
    (void) bias;
#endif
    return p;
}
void OwnedCue::reset() noexcept {
    if (destroy_) destroy_(&native);
    destroy_ = nullptr;
    std::memset(&native, 0, sizeof native);
}
void Property::reset() noexcept {
    if (destroy_) destroy_(&native);
    destroy_ = nullptr;
    copy_ = nullptr;
    std::memset(&native, 0, sizeof native);
}
bool Property::copy_to(Property &out) const noexcept {
    if (&out == this) return valid();
    out.reset();
    if (!destroy_ || !copy_) return false;
    copy_(&out.native, &native);
    out.destroy_ = destroy_;
    out.copy_ = copy_;
    return true;
}
void Snapshot::reset() noexcept {
    if (content_) p_.release_ref(&content_);
    if (hot_content_) p_.release_ref(&hot_content_);
    if (memory_set_) p_.release_ref(&memory_set_);
    if (hot_set_) p_.release_ref(&hot_set_);
    if (where_from_) p_.release_ref(&where_from_);
    if (source_) p_.release_ref(&source_);
    if (player_) method<void (*)(void *)>(player_, 1)(reinterpret_cast<void *>(player_));
    player_ = source_ = where_from_ = memory_set_ = hot_set_ = content_ = hot_content_ = pack_ = 0;
    count_ = memory_count_ = 0;
    revision_ = 0;
    fingerprint_ = 0;
    std::memset(source_id_, 0, sizeof source_id_);
    std::memset(track_id_, 0, sizeof track_id_);
}
bool Snapshot::open(const Profile &p, void *manager, uint64_t generation,
                    const unsigned char expected[12], RevisionState &state) noexcept {
    reset();
    p_ = p;
    if (!complete(p) || !manager || !generation || (expected && !nonzero(expected, 12))) { diagnostic("profile"); return false; }
    const uintptr_t helper = reinterpret_cast<uintptr_t>(manager) + 0x150;
    const uintptr_t player_interface = word(helper + 8), service = word(helper + 0x10);
    if (!player_interface || !service) { diagnostic("interfaces"); return false; }
    NativePlayer owned = method<NativePlayer (*)(void *)>(player_interface, 2)(reinterpret_cast<void *>(player_interface));
    player_ = owned.value;
    if (!player_) { diagnostic("player"); return false; }
    const uintptr_t position = reinterpret_cast<uintptr_t>(p.position(reinterpret_cast<const void *>(player_)));
    if (!position || !nonzero(reinterpret_cast<const unsigned char *>(position + 8), 12) ||
        (expected && std::memcmp(reinterpret_cast<const void *>(position + 8), expected, 12))) {
        diagnostic("position", position);
        reset();
        return false;
    }
    std::memcpy(source_id_, reinterpret_cast<const void *>(position + 8), 12);
    p.copy_source(&source_, reinterpret_cast<const uintptr_t *>(position + 0x30));
    if (!source_ || !word(source_ + 0x58)) {
        diagnostic("source", position, source_);
        reset();
        return false;
    }
    NativePlayer where = p.where_from(reinterpret_cast<const void *>(player_));
    where_from_ = where.value;
    if (!where_from_ || where_from_ != word(source_ + 0x58)) {
        diagnostic("where-from", position, source_);
        reset();
        return false;
    }
    pack_ = reinterpret_cast<uintptr_t>(method<void *(*) (void *, const void *)>(service, 6)(reinterpret_cast<void *>(service), &where_from_));
    if (!pack_) {
        diagnostic("pack", position, source_);
        reset();
        return false;
    }
    /* Native SourceID constructors copy 8 + 4 identity bytes. The final four
     * bytes of a position's 16-byte storage are padding and can be uninitialized.
     * Retain the Pack storage for native receipts, but compare/hash identity12. */
    std::memcpy(track_id_, reinterpret_cast<const void *>(pack_ + 0x80), 16);
    if (!nonzero(track_id_, 12) || std::memcmp(track_id_, reinterpret_cast<const void *>(position + 8), 12)) {
        diagnostic("track16", position, source_, pack_);
        reset();
        return false;
    }
    const uintptr_t publication = word(pack_ + 0xe8), hot_publication = word(pack_ + 0xe0);
    if (!publication && !hot_publication) diagnostic("pub-empty", position, source_, pack_);
    if (publication) p.copy_content(&memory_set_, reinterpret_cast<const uintptr_t *>(pack_ + 0xe8));
    if (hot_publication) p.copy_content(&hot_set_, reinterpret_cast<const uintptr_t *>(pack_ + 0xe0));
    if ((publication && !memory_set_) || (hot_publication && !hot_set_)) { reset(); return false; }
    if (memory_set_) p.copy_content(&content_, reinterpret_cast<const uintptr_t *>(memory_set_ + 0x28));
    if (hot_set_) p.copy_content(&hot_content_, reinterpret_cast<const uintptr_t *>(hot_set_ + 0x28));
    if ((publication && word(publication + 0x28) && !content_) ||
        (hot_publication && word(hot_publication + 0x28) && !hot_content_)) { diagnostic("content", position, source_, pack_); reset(); return false; }
    uintptr_t begin = 0, end = 0, hot_begin = 0, hot_end = 0;
    if (content_) { begin = word(content_ + 0x28); end = word(content_ + 0x30); }
    if (hot_content_) { hot_begin = word(hot_content_ + 0x28); hot_end = word(hot_content_ + 0x30); }
    if (end < begin || (end - begin) % 0x80 || (end - begin) / 0x80 > 64 || (end != begin && !begin) ||
        hot_end < hot_begin || (hot_end - hot_begin) % 0x80 || (hot_end - hot_begin) / 0x80 > 8 || (hot_end != hot_begin && !hot_begin)) {
        diagnostic("vector", position, source_, pack_);
        reset();
        return false;
    }
    memory_count_ = (end - begin) / 0x80;
    count_ = memory_count_ + (hot_end - hot_begin) / 0x80;
    if (count_ > 64) { diagnostic("vector-total", position, source_, pack_); reset(); return false; }
    uint64_t fingerprint = hash(1469598103934665603ULL, track_id_, 12);
    const uint64_t memory_size = memory_count_;
    fingerprint = hash(fingerprint, &memory_size, sizeof memory_size);
    fingerprint = hash(fingerprint, reinterpret_cast<const void *>(begin), memory_count_ * 0x80);
    fingerprint = hash(fingerprint, reinterpret_cast<const void *>(hot_begin), (count_ - memory_count_) * 0x80);
    if (state.load_generation != generation || state.content != content_ || state.hot_content != hot_content_ || state.fingerprint != fingerprint || std::memcmp(state.track, track_id_, 12)) {
        if (state.revision == std::numeric_limits<uint64_t>::max()) {
            reset();
            return false;
        }
        ++state.revision;
        state.load_generation = generation;
        state.content = content_;
        state.hot_content = hot_content_;
        state.fingerprint = fingerprint;
        std::memcpy(state.track, track_id_, 16);
    }
    revision_ = state.revision;
    fingerprint_ = fingerprint;
    return true;
}
bool Snapshot::row(size_t ordinal, OwnedCue &out) const noexcept {
    out.reset();
    if (!revision_ || ordinal >= count_ || std::memcmp(reinterpret_cast<const void *>(pack_ + 0x80), track_id_, 12)) return false;
    const uintptr_t *collection = ordinal < memory_count_ ? &content_ : &hot_content_;
    const size_t index = ordinal < memory_count_ ? ordinal : ordinal - memory_count_;
    out.native = p_.get_cue(collection, index);
    out.destroy_ = p_.destroy_cue;
    return true;
}
bool Snapshot::property(uint32_t entry_id, uint64_t expected_revision, Property &out) const noexcept {
    out.reset();
    if (!entry_id || expected_revision != revision_) return false;
    OwnedCue cue;
    if (!row(entry_id - 1, cue)) return false;
    out.native = p_.convert(&where_from_, reinterpret_cast<const void *>(pack_), reinterpret_cast<const void *>(source_ + 0x28), &cue.native);
    out.destroy_ = p_.destroy_property;
    out.copy_ = p_.copy_property;
    /* The converted point must belong to the exact source admitted above. */
    if (std::memcmp(out.native.bytes + 0x30, source_id_, 12)) {
        out.reset();
        return false;
    }
    return true;
}
bool Snapshot::current_property(unsigned kind, Property &out) const noexcept {
    out.reset();
    if (!revision_ || kind > 8 || !p_.has_cue || !p_.get_current_cue || !p_.copy_property ||
        std::memcmp(reinterpret_cast<const void *>(pack_ + 0x80), track_id_, 12) ||
        !p_.has_cue(reinterpret_cast<const void *>(player_), static_cast<int>(kind))) return false;
    const auto *cue = static_cast<const unsigned char *>(p_.get_current_cue(reinterpret_cast<const void *>(player_), static_cast<int>(kind)));
    if (!cue || std::memcmp(cue + 0x30, source_id_, 12)) return false;
    p_.copy_property(&out.native, cue);
    out.destroy_ = p_.destroy_property;
    out.copy_ = p_.copy_property;
    return true;
}
bool Snapshot::canonical_property(unsigned kind, const Property &original, Property &out) const noexcept {
    if (&out == &original) return false;
    if (!revision_ || kind > 8 || !original.valid() || !p_.serialize_property ||
        std::memcmp(original.native.bytes + 0x30, source_id_, 12) ||
        std::memcmp(reinterpret_cast<const void *>(pack_ + 0x80), track_id_, 12)) {
        out.reset();
        return false;
    }
    OwnedCue cue;
    /* Native CueStoreMaintainer::request sets W2 to (kind != 0). Both native
     * conversions retain strings and source refs through their own constructors. */
    cue.native = p_.serialize_property(kind, &original.native, kind != 0);
    cue.destroy_ = p_.destroy_cue;
    out.reset();
    out.native = p_.convert(&where_from_, reinterpret_cast<const void *>(pack_),
                            reinterpret_cast<const void *>(source_ + 0x28), &cue.native);
    out.destroy_ = p_.destroy_property;
    out.copy_ = p_.copy_property;
    if (std::memcmp(out.native.bytes + 0x30, source_id_, 12) ||
        std::memcmp(reinterpret_cast<const void *>(pack_ + 0x80), track_id_, 12)) {
        out.reset();
        return false;
    }
    return true;
}
bool Snapshot::content_witness(uint64_t expected_revision, azcuepersist_witness *out) const noexcept {
    if (!out) return false;
    *out = {};
    if (!revision_ || expected_revision != revision_ || !memory_set_ || !hot_set_ ||
        std::memcmp(reinterpret_cast<const void *>(pack_ + 0x80), track_id_, 12)) return false;
    if (word(pack_ + 0xe8) != memory_set_ || word(pack_ + 0xe0) != hot_set_ ||
        (memory_set_ && word(memory_set_ + 0x28) != content_) ||
        (hot_set_ && word(hot_set_ + 0x28) != hot_content_)) return false;
    const uintptr_t begin = content_ ? word(content_ + 0x28) : 0, end = content_ ? word(content_ + 0x30) : 0;
    const uintptr_t hot_begin = hot_content_ ? word(hot_content_ + 0x28) : 0, hot_end = hot_content_ ? word(hot_content_ + 0x30) : 0;
    if (end < begin || (end - begin) % 0x80 || (end - begin) / 0x80 != memory_count_ ||
        memory_count_ > 64 || (end != begin && !begin) ||
        hot_end < hot_begin || (hot_end - hot_begin) % 0x80 || (hot_end - hot_begin) / 0x80 != count_ - memory_count_ ||
        count_ - memory_count_ > 8 || (hot_end != hot_begin && !hot_begin)) return false;
    const uint64_t count = memory_count_;
    uint64_t current = hash(1469598103934665603ULL, source_id_, 12);
    current = hash(current, &count, sizeof count);
    current = hash(current, reinterpret_cast<const void *>(begin), memory_count_ * 0x80);
    current = hash(current, reinterpret_cast<const void *>(hot_begin), (count_ - memory_count_) * 0x80);
    if (current != fingerprint_) return false;
    out->source_info = source_;
    out->memory_set = memory_set_;
    out->hot_set = hot_set_;
    out->memory_content = content_;
    out->hot_content = hot_content_;
    std::memcpy(out->source_id, source_id_, 12);
    out->fingerprint = current;
    out->revision = revision_;
    return true;
}
bool Snapshot::auto_only_changed(const azcuepersist_witness &before, uint32_t memory_entry_id, bool desired, uint32_t source_rate) const noexcept {
    azcuepersist_witness fresh {};
    if (!before.revision || !before.source_info || !before.memory_set || !before.hot_set ||
        !memory_entry_id || !source_rate || !content_witness(revision_, &fresh) || std::memcmp(before.source_id, source_id_, 12)) return false;
    CueVector memory, hot;
    if (!cue_vector(before.memory_content, 64, memory) || !cue_vector(before.hot_content, 8, hot) ||
        memory.count != memory_count_ || hot.count != count_ - memory_count_ ||
        memory.count + hot.count > 64 || memory_entry_id > memory.count) return false;
    const uint64_t memory_size = memory.count;
    uint64_t baseline = hash(1469598103934665603ULL, before.source_id, 12);
    baseline = hash(baseline, &memory_size, sizeof memory_size);
    baseline = hash(baseline, reinterpret_cast<const void *>(memory.begin), memory.count * 0x80);
    baseline = hash(baseline, reinterpret_cast<const void *>(hot.begin), hot.count * 0x80);
    if (baseline != before.fingerprint) return false;
    const uintptr_t current_memory = content_ ? word(content_ + 0x28) : 0;
    const uintptr_t current_hot = hot_content_ ? word(hot_content_ + 0x28) : 0;
    for (size_t i = 0; i < memory.count; ++i)
        if (!same_cue(memory.begin + i * 0x80, current_memory + i * 0x80, i + 1 == memory_entry_id ? CueDelta::Auto : CueDelta::None, desired, source_rate)) return false;
    for (size_t i = 0; i < hot.count; ++i)
        if (!same_cue(hot.begin + i * 0x80, current_hot + i * 0x80, CueDelta::None, desired, source_rate)) return false;
    return true;
}
bool Snapshot::metadata_only_changed(const azcuepersist_witness &before, unsigned native_kind, uint32_t entry_id,
                                     const char *comment, uint32_t rgb, uint32_t source_rate) const noexcept {
    azcuepersist_witness fresh {};
    if (!before.revision || !before.source_info || !before.memory_set || !before.hot_set || !entry_id ||
        native_kind > 8 || (native_kind && native_kind != entry_id) || rgb > 0xffffffu || !source_rate ||
        !content_witness(revision_, &fresh) || std::memcmp(before.source_id, source_id_, 12)) return false;
    CueVector memory, hot;
    if (!cue_vector(before.memory_content, 64, memory) || !cue_vector(before.hot_content, 8, hot) ||
        memory.count != memory_count_ || hot.count != count_ - memory_count_ ||
        memory.count + hot.count > 64 || (!native_kind && entry_id > memory.count)) return false;
    const uint64_t memory_size = memory.count;
    uint64_t baseline = hash(1469598103934665603ULL, before.source_id, 12);
    baseline = hash(baseline, &memory_size, sizeof memory_size);
    baseline = hash(baseline, reinterpret_cast<const void *>(memory.begin), memory.count * 0x80);
    baseline = hash(baseline, reinterpret_cast<const void *>(hot.begin), hot.count * 0x80);
    if (baseline != before.fingerprint) return false;
    const CueDelta edit = comment ? CueDelta::Comment : CueDelta::Color;
    unsigned matches = 0;
    const uintptr_t current_memory = content_ ? word(content_ + 0x28) : 0;
    const uintptr_t current_hot = hot_content_ ? word(hot_content_ + 0x28) : 0;
    for (size_t i = 0; i < memory.count; ++i) {
        bool selected = !native_kind && i + 1 == entry_id;
        if (selected) ++matches;
        if (!same_cue(memory.begin + i * 0x80, current_memory + i * 0x80, selected ? edit : CueDelta::None, false, source_rate, comment, rgb)) return false;
    }
    for (size_t i = 0; i < hot.count; ++i) {
        bool selected = native_kind && read<unsigned char>(hot.begin + i * 0x80) == native_kind;
        if (selected) ++matches;
        if (!same_cue(hot.begin + i * 0x80, current_hot + i * 0x80, selected ? edit : CueDelta::None, false, source_rate, comment, rgb)) return false;
    }
    return matches == 1;
}
} // namespace azcuestore
