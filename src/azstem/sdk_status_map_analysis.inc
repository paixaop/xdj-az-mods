/* SPDX-License-Identifier: GPL-3.0-only */
/* Firmware 2.00 TrackPhrase::fromAtom and its owned value ABI. The firmware
 * creates Content, parses its rows and supplies its native reference count.
 * This adapter only constructs the plain input value and transfers the counted
 * result to an unchanged native holder. No Content/vtable is synthesized. */
struct AnalysisPhrase;
struct AnalysisAtomRef;
struct AnalysisMemory {
    uintptr_t data = 0;
    uint64_t size = 0;
    ~AnalysisMemory();
};
struct AnalysisOps {
    AnalysisPhrase (*make)(const AnalysisAtomRef *) = nullptr;
    void (*memory)(AnalysisMemory *, const void *, size_t) = nullptr;
    void (*memory_drop)(AnalysisMemory *) = nullptr;
    unsigned char (*grant)(uintptr_t) = nullptr;
    unsigned char (*release)(uintptr_t) = nullptr;
    void (*trash)(uintptr_t, uintptr_t) = nullptr;
} analysis_ops;
std::atomic<bool> analysis_ready {false};
void analysis_drop(uintptr_t content) {
    if (content && analysis_ops.release(content)) analysis_ops.trash(read_value<uintptr_t>(content, 0x18), content);
}
AnalysisMemory::~AnalysisMemory() {
    if (data) analysis_ops.memory_drop(this);
}
/* A nontrivial return is intentional: AAPCS64 passes its result address in x8.
 * The native call at 0xe929c8 supplies x0=shared atom reference, x8=result. */
struct AnalysisPhrase {
    uintptr_t content = 0;
    AnalysisPhrase() = default;
    AnalysisPhrase(const AnalysisPhrase &) = delete;
    AnalysisPhrase &operator=(const AnalysisPhrase &) = delete;
    AnalysisPhrase(AnalysisPhrase &&from) noexcept : content(from.content) { from.content = 0; }
    AnalysisPhrase &operator=(AnalysisPhrase &&from) noexcept {
        if (this != &from) { analysis_drop(content); content = from.content; from.content = 0; }
        return *this;
    }
    ~AnalysisPhrase() { analysis_drop(content); }
};
struct AnalysisAtom {
    uint64_t header = 32;
    uint32_t prefix = 18;
    uint16_t mood = 0, end = 0;
    uint64_t stride = 24;
    uint32_t count = 0, reserved = 0;
    AnalysisMemory rows, raw;
};
struct AnalysisAtomRef { const AnalysisAtom *value; uintptr_t control; };
static_assert(sizeof(AnalysisPhrase) == 8 && sizeof(AnalysisMemory) == 16 && sizeof(AnalysisAtom) == 64,
              "firmware 2.00 phrase value ABI");
static_assert(offsetof(AnalysisAtom, mood) == 0xc && offsetof(AnalysisAtom, end) == 0xe &&
              offsetof(AnalysisAtom, stride) == 0x10 && offsetof(AnalysisAtom, count) == 0x18 &&
              offsetof(AnalysisAtom, rows) == 0x20 && offsetof(AnalysisAtom, raw) == 0x30,
              "firmware 2.00 phrase atom fields");
void configure_analysis(int build, uintptr_t bias) {
    if (analysis_ready.load(std::memory_order_acquire) || build != 2 || bias) return;
    struct Site { uintptr_t address; uint32_t words[4]; };
    constexpr Site sites[] = {
        {0xdafa38, {0xa9a47bfd, 0x910003fd, 0xa90363f7, 0xf9400017}},
        {0x2477df0, {0xa900081f, 0xb5000042, 0xd65f03c0, 0xa9bd7bfd}},
        {0x2477e70, {0xf9400000, 0x177eb92b, 0xd503201f, 0xd503201f}},
        {0x6f2728, {0xa9bb7bfd, 0x5289eb03, 0x91003002, 0x910003fd}},
        {0x6f2848, {0xa9bc7bfd, 0x5289eb01, 0x72aa4861, 0x910003fd}},
        {0x2371890, {0xa9b77bfd, 0x910003fd, 0x910203e8, 0xa90153f3}}};
    for (const auto &site : sites)
        if (std::memcmp(reinterpret_cast<const void *>(site.address), site.words, sizeof site.words)) return;
    analysis_ops.make = reinterpret_cast<decltype(analysis_ops.make)>(sites[0].address);
    analysis_ops.memory = reinterpret_cast<decltype(analysis_ops.memory)>(sites[1].address);
    analysis_ops.memory_drop = reinterpret_cast<decltype(analysis_ops.memory_drop)>(sites[2].address);
    analysis_ops.grant = reinterpret_cast<decltype(analysis_ops.grant)>(sites[3].address);
    analysis_ops.release = reinterpret_cast<decltype(analysis_ops.release)>(sites[4].address);
    analysis_ops.trash = reinterpret_cast<decltype(analysis_ops.trash)>(sites[5].address);
    analysis_ready.store(true, std::memory_order_release);
}
bool analysis_supported() {
    return enabled.load(std::memory_order_acquire) && profile == &profiles[1] &&
           analysis_ready.load(std::memory_order_acquire) && map_writer_installed == 15;
}
int analysis_current(unsigned deck, uint64_t load, const aznm_packet *expected) {
    if (!expected || deck >= 4 || !load) return AZ_INVALID;
    if (!analysis_supported()) return AZ_UNSUPPORTED;
    return azn_maps_current(deck, load, expected) ? AZ_OK : AZ_STALE;
}
int analysis_capture(unsigned deck, uint64_t load, aznm_packet *out) {
    if (!out || deck >= 4 || !load) return AZ_INVALID;
    if (!analysis_supported()) return AZ_UNSUPPORTED;
    azn_pack_view view {};
    if (!azn_pack_acquire(deck, &view)) return AZ_STALE;
    struct Drop { uint64_t pin; ~Drop() { azn_pin_drop(pin); } } drop {view.token};
    if (view.load != load || !azn_pack_current(deck, &view)) return AZ_STALE;
    aznm_packet result {};
    {
        Slot &slot = cache[bucket(view.object)];
        Lock lock(slot);
        if (!lock.held) return AZ_BUSY;
        const Pair &pair = slot.pair;
        if (pair.pack.address != view.object || !sample_matches(pair) || !pair.packet.owner ||
            !pair.content[0].token || !pair.content[1].token) return AZ_BUSY;
        if (!copy_packet(pair, result)) return AZ_STALE;
    }
    uint32_t count = 0;
    int rc = read_rows(&result.grid, AZNM_GRID, nullptr, 0, &count);
    if (rc != AZ_OK || !count || !current_identity(deck, view, result)) {
        release_packet(&result);
        return rc != AZ_OK ? rc : !count ? AZ_BUSY : AZ_STALE;
    }
    *out = result;
    return AZ_OK;
}
unsigned analysis_u16(const uint8_t *p) { return unsigned(p[0]) << 8 | p[1]; }
uint32_t analysis_u32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool analysis_pssi(const uint8_t *p, uint32_t size, uint32_t beats) {
    if (!p || size < 56 || size > 32 + 1024 * 24 || std::memcmp(p, "PSSI", 4) ||
        analysis_u32(p + 4) != 32 || analysis_u32(p + 8) != size || analysis_u16(p + 12) != 0 ||
        analysis_u16(p + 14) != 24) return false;
    unsigned count = analysis_u16(p + 16), mood = analysis_u16(p + 18), end = analysis_u16(p + 26);
    if (!count || count > 1024 || size != 32 + count * 24 || mood < 1 || mood > 3 || end > beats) return false;
    unsigned prior = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned start = analysis_u16(p + 32 + i * 24 + 2);
        if (!start || start <= prior || start >= end) return false;
        prior = start;
    }
    return true;
}
int analysis_publish(unsigned deck, uint64_t load, const aznm_packet *expected, const void *data, uint32_t size) {
    if (!expected || deck >= 4 || !load || !data) return AZ_INVALID;
    if (!analysis_supported()) return AZ_UNSUPPORTED;
    uint32_t beats = 0, phrases = 0;
    int rc = read_rows(&expected->grid, AZNM_GRID, nullptr, 0, &beats);
    if (rc != AZ_OK) return rc;
    rc = read_rows(&expected->phrases, AZNM_PHRASES, nullptr, 0, &phrases);
    if (rc != AZ_OK) return rc;
    if (phrases) return AZ_NOT_FOUND;
    const auto *pssi = static_cast<const uint8_t *>(data);
    if (!analysis_pssi(pssi, size, beats)) return AZ_INVALID;
    if (analysis_current(deck, load, expected) != AZ_OK) return AZ_STALE;
    AnalysisPhrase made;
    try {
        AnalysisAtom atom;
        atom.mood = uint16_t(analysis_u16(pssi + 18));
        atom.end = uint16_t(analysis_u16(pssi + 26));
        atom.count = analysis_u16(pssi + 16);
        analysis_ops.memory(&atom.rows, pssi + 32, size - 32);
        analysis_ops.memory(&atom.raw, pssi, size);
        const AnalysisAtomRef ref {&atom, 0};
        made = analysis_ops.make(&ref);
    } catch (...) { return AZ_FAILED; }
    if (!made.content || !typed(made.content, profile->phrase_content) ||
        read_value<uint32_t>(made.content, 0x48) != analysis_u16(pssi + 16)) return AZ_FAILED;
    std::lock_guard<std::recursive_mutex> mutation(map_mutation);
    azn_pack_view view {};
    if (!azn_pack_acquire(deck, &view)) return AZ_STALE;
    struct Drop { uint64_t pin; ~Drop() { azn_pin_drop(pin); } } drop {view.token};
    if (view.load != load || !current_identity(deck, view, *expected)) return AZ_STALE;
    uintptr_t holder = 0, prior = 0;
    Drop holder_pin {0};
    {
        Slot &slot = cache[bucket(view.object)];
        Lock lock(slot);
        if (!lock.held) return AZ_BUSY;
        const Pair &pair = slot.pair;
        if (pair.pack.address != view.object || pair.packet.owner != expected->owner ||
            !azn_pin_hold(pair.holder[1].token, &holder)) return AZ_STALE;
        holder_pin.pin = pair.holder[1].token;
    }
    if (!typed(holder, profile->phrase_holder) || !azn_pin_hold(expected->phrases.content, &prior)) return AZ_STALE;
    Drop prior_pin {expected->phrases.content};
    if (read_value<uintptr_t>(holder, 0x28) != prior || read_value<uint32_t>(prior, 0x48)) return AZ_STALE;
    HolderEdit edit(1, holder);
    if (!(edit.mask & (1u << deck)) || !edit.bracket || !current_identity(deck, view, *expected)) return AZ_BUSY;
    if (!analysis_ops.grant(made.content)) return AZ_FAILED;
    uintptr_t compare = prior;
    if (!__atomic_compare_exchange_n(reinterpret_cast<uintptr_t *>(holder + 0x28), &compare, made.content,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        analysis_drop(made.content);
        return AZ_STALE;
    }
    analysis_drop(prior); /* release the holder's replaced reference */
    edit.finish(made.content);
    /* Native listeners schedule their own owner-thread work. The firmware's
     * normal registry invokes the same broadcast from its repository worker. */
    reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(map_writer_original[3])(holder, 0);
    return AZ_OK;
}
