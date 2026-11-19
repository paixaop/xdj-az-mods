/* SPDX-License-Identifier: GPL-3.0-only */
/* Firmware 2.00 TrackPhrase::fromAtom and its owned value ABI. The firmware
 * creates Content, parses its rows and supplies its native reference count.
 * This adapter only constructs the plain input value and transfers the counted
 * result to an unchanged native holder. No Content/vtable is synthesized. */
struct AnalysisMemory {
    uintptr_t data;
    uint64_t size;
};
struct AnalysisOps {
    void *make; /* native nontrivial result: hidden destination, not a C value return */
    void (*memory)(struct AnalysisMemory *, const void *, size_t);
    void (*memory_drop)(struct AnalysisMemory *);
    unsigned char (*grant)(uintptr_t);
    unsigned char (*release)(uintptr_t);
    void (*trash)(uintptr_t, uintptr_t);
};
static struct AnalysisOps analysis_ops;
static _Atomic(bool) analysis_ready;
static void analysis_drop(uintptr_t content) {
    if (content && analysis_ops.release(content)) analysis_ops.trash(read_ptr(content, 0x18), content);
}
struct AnalysisAtom {
    uint64_t header;
    uint32_t prefix;
    uint16_t mood, end;
    uint64_t stride;
    uint32_t count, reserved;
    struct AnalysisMemory rows, raw;
};
struct AnalysisAtomRef {
    const struct AnalysisAtom *value;
    uintptr_t control;
};
static void analysis_atom_drop(struct AnalysisAtom *atom) {
    /* Native aggregate destruction runs in reverse member order. */
    if (atom->raw.data) analysis_ops.memory_drop(&atom->raw);
    if (atom->rows.data) analysis_ops.memory_drop(&atom->rows);
}
_Static_assert(sizeof(uintptr_t) == 8 && sizeof(struct AnalysisMemory) == 16 && sizeof(struct AnalysisAtom) == 64,
               "firmware 2.00 phrase value ABI");
_Static_assert(offsetof(struct AnalysisAtom, mood) == 0xc && offsetof(struct AnalysisAtom, end) == 0xe &&
                   offsetof(struct AnalysisAtom, stride) == 0x10 && offsetof(struct AnalysisAtom, count) == 0x18 &&
                   offsetof(struct AnalysisAtom, rows) == 0x20 && offsetof(struct AnalysisAtom, raw) == 0x30,
               "firmware 2.00 phrase atom fields");
struct AnalysisMake {
    uintptr_t content;
    const uint8_t *pssi;
    uint32_t size;
};
static void analysis_make_drop(struct AnalysisMake *call) { analysis_drop(call->content); }
static void analysis_make(void *opaque) {
    struct AnalysisMake *call = opaque;
    struct AnalysisAtom atom __attribute__((cleanup(analysis_atom_drop))) = {.header = 32, .prefix = 18, .stride = 24};
    atom.mood = (uint16_t) ((unsigned) call->pssi[18] << 8 | call->pssi[19]);
    atom.end = (uint16_t) ((unsigned) call->pssi[26] << 8 | call->pssi[27]);
    atom.count = (unsigned) call->pssi[16] << 8 | call->pssi[17];
    analysis_ops.memory(&atom.rows, call->pssi + 32, call->size - 32);
    analysis_ops.memory(&atom.raw, call->pssi, call->size);
    const struct AnalysisAtomRef ref = {&atom, 0};
    az_native_result_word(analysis_ops.make, &call->content, &ref);
}
static void configure_analysis(int build, uintptr_t bias) {
    if (atomic_load_explicit(&(analysis_ready), memory_order_acquire) || build != 2 || bias) return;
    struct Site {
        uintptr_t address;
        uint32_t words[4];
    };
    const struct Site sites[] = {
        {0xdafa38, {0xa9a47bfd, 0x910003fd, 0xa90363f7, 0xf9400017}},
        {0x2477df0, {0xa900081f, 0xb5000042, 0xd65f03c0, 0xa9bd7bfd}},
        {0x2477e70, {0xf9400000, 0x177eb92b, 0xd503201f, 0xd503201f}},
        {0x6f2728, {0xa9bb7bfd, 0x5289eb03, 0x91003002, 0x910003fd}},
        {0x6f2848, {0xa9bc7bfd, 0x5289eb01, 0x72aa4861, 0x910003fd}},
        {0x2371890, {0xa9b77bfd, 0x910003fd, 0x910203e8, 0xa90153f3}}};
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; ++i)
        if (memcmp(((const void *) (sites[i].address)), sites[i].words, sizeof sites[i].words)) return;
    analysis_ops.make = ((void *) (sites[0].address));
    analysis_ops.memory = ((__typeof__(analysis_ops.memory)) (sites[1].address));
    analysis_ops.memory_drop = ((__typeof__(analysis_ops.memory_drop)) (sites[2].address));
    analysis_ops.grant = ((__typeof__(analysis_ops.grant)) (sites[3].address));
    analysis_ops.release = ((__typeof__(analysis_ops.release)) (sites[4].address));
    analysis_ops.trash = ((__typeof__(analysis_ops.trash)) (sites[5].address));
    atomic_store_explicit(&(analysis_ready), true, memory_order_release);
}
static bool analysis_supported() {
    return atomic_load_explicit(&(enabled), memory_order_acquire) && profile == &profiles[1] &&
           atomic_load_explicit(&(analysis_ready), memory_order_acquire) && map_writer_installed == 15;
}
static bool analysis_request_valid(unsigned deck, uint64_t load, const void *packet) {
    return packet && deck < 4 && load;
}
static int analysis_current(unsigned deck, uint64_t load, const struct aznm_packet *expected) {
    if (!analysis_request_valid(deck, load, expected)) return AZ_INVALID;
    if (!analysis_supported()) return AZ_UNSUPPORTED;
    return azn_maps_current(deck, load, expected) ? AZ_OK : AZ_STALE;
}
static bool analysis_pair_ready(const Pair *pair, uintptr_t object) {
    return pair->pack.address == object && sample_matches(pair) && pair->packet.owner && pair->content[0].token && pair->content[1].token;
}
static int analysis_capture_pair(uintptr_t object, struct aznm_packet *out) {
    Slot *slot = &cache[bucket(object)];
    SLOT_LOCK(lock, *slot);
    if (!lock.held) return AZ_BUSY;
    if (!analysis_pair_ready(&slot->pair, object)) return AZ_BUSY;
    return copy_packet(&slot->pair, out) ? AZ_OK : AZ_STALE;
}
static int analysis_capture_current(unsigned deck, const struct azn_pack_view *view, const struct aznm_packet *result) {
    uint32_t count = 0;
    int rc = read_rows(&result->grid, AZNM_GRID, NULL, 0, &count);
    if (rc != AZ_OK) return rc;
    if (!count) return AZ_BUSY;
    return current_identity(deck, view, result) ? AZ_OK : AZ_STALE;
}
static int analysis_capture(unsigned deck, uint64_t load, struct aznm_packet *out) {
    if (!analysis_request_valid(deck, load, out)) return AZ_INVALID;
    if (!analysis_supported()) return AZ_UNSUPPORTED;
    struct azn_pack_view view = {0};
    if (!azn_pack_acquire(deck, &view)) return AZ_STALE;
    PIN_DROP(drop, view.token);
    if (view.load != load || !azn_pack_current(deck, &view)) return AZ_STALE;
    struct aznm_packet result = {0};
    int rc = analysis_capture_pair(view.object, &result);
    if (rc != AZ_OK) return rc;
    rc = analysis_capture_current(deck, &view, &result);
    if (rc != AZ_OK) {
        release_packet(&result);
        return rc;
    }
    *out = result;
    return AZ_OK;
}
static unsigned analysis_u16(const uint8_t *p) { return (unsigned) (p[0]) << 8 | p[1]; }
static uint32_t analysis_u32(const uint8_t *p) { return (uint32_t) (p[0]) << 24 | (uint32_t) (p[1]) << 16 | (uint32_t) (p[2]) << 8 | p[3]; }
static bool analysis_pssi_header(const uint8_t *p, uint32_t size) {
    return p && size >= 56 && size <= 32 + 1024 * 24 && !memcmp(p, "PSSI", 4) &&
           analysis_u32(p + 4) == 32 && analysis_u32(p + 8) == size && analysis_u16(p + 12) == 0 && analysis_u16(p + 14) == 24;
}
static bool analysis_pssi_rows(const uint8_t *p, unsigned count, unsigned end) {
    unsigned prior = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned start = analysis_u16(p + 32 + i * 24 + 2);
        if (!start || start <= prior || start >= end) return false;
        prior = start;
    }
    return true;
}
static bool analysis_pssi(const uint8_t *p, uint32_t size, uint32_t beats) {
    if (!analysis_pssi_header(p, size)) return false;
    unsigned count = analysis_u16(p + 16), mood = analysis_u16(p + 18), end = analysis_u16(p + 26);
    if (!count || count > 1024 || size != 32 + count * 24 || mood < 1 || mood > 3 || end > beats) return false;
    return analysis_pssi_rows(p, count, end);
}
static int analysis_publish_input(const struct aznm_packet *expected, const void *data, uint32_t size) {
    uint32_t beats = 0, phrases = 0;
    int rc = read_rows(&expected->grid, AZNM_GRID, NULL, 0, &beats);
    if (rc != AZ_OK) return rc;
    rc = read_rows(&expected->phrases, AZNM_PHRASES, NULL, 0, &phrases);
    if (rc != AZ_OK) return rc;
    if (phrases) return AZ_NOT_FOUND;
    return analysis_pssi(data, size, beats) ? AZ_OK : AZ_INVALID;
}
static bool analysis_content_valid(const struct AnalysisMake *call) {
    return call->content && typed(call->content, profile->phrase_content) && read_u32(call->content, 0x48) == analysis_u16(call->pssi + 16);
}
static int analysis_hold_holder(uintptr_t object, uint64_t owner, uintptr_t *holder, uint64_t *token) {
    Slot *slot = &cache[bucket(object)];
    SLOT_LOCK(lock, *slot);
    if (!lock.held) return AZ_BUSY;
    const Pair *pair = &slot->pair;
    if (pair->pack.address != object || pair->packet.owner != owner || !azn_pin_hold(pair->holder[1].token, holder)) return AZ_STALE;
    *token = pair->holder[1].token;
    return AZ_OK;
}
static int analysis_replace(unsigned deck, const struct azn_pack_view *view, const struct aznm_packet *expected,
                            uintptr_t holder, uintptr_t prior, uintptr_t made) {
    if (read_ptr(holder, 0x28) != prior || read_u32(prior, 0x48)) return AZ_STALE;
    HOLDER_EDIT(edit, 1, holder);
    if (!(edit.mask & (1u << deck)) || !edit.bracket || !current_identity(deck, view, expected)) return AZ_BUSY;
    if (!analysis_ops.grant(made)) return AZ_FAILED;
    uintptr_t compare = prior;
    if (!__atomic_compare_exchange_n(((uintptr_t *) (holder + 0x28)), &compare, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        analysis_drop(made);
        return AZ_STALE;
    }
    analysis_drop(prior); /* release the holder's replaced reference */
    holder_finish(&edit, made, false);
    /* Native listeners schedule their own owner-thread work. The firmware's
     * normal registry invokes the same broadcast from its repository worker. */
    ((uintptr_t (*)(uintptr_t, uintptr_t))(map_writer_original[3]))(holder, 0);
    return AZ_OK;
}
static int analysis_publish_commit(unsigned deck, uint64_t load, const struct aznm_packet *expected, uintptr_t made) {
    MAP_MUTATION(mutation);
    struct azn_pack_view view = {0};
    if (!azn_pack_acquire(deck, &view)) return AZ_STALE;
    PIN_DROP(drop, view.token);
    if (view.load != load || !current_identity(deck, &view, expected)) return AZ_STALE;
    uintptr_t holder = 0, prior = 0;
    PIN_DROP(holder_pin, 0);
    int rc = analysis_hold_holder(view.object, expected->owner, &holder, &holder_pin);
    if (rc != AZ_OK) return rc;
    if (!typed(holder, profile->phrase_holder) || !azn_pin_hold(expected->phrases.content, &prior)) return AZ_STALE;
    PIN_DROP(prior_pin, expected->phrases.content);
    return analysis_replace(deck, &view, expected, holder, prior, made);
}
static int analysis_publish(unsigned deck, uint64_t load, const struct aznm_packet *expected, const void *data, uint32_t size) {
    if (!analysis_request_valid(deck, load, expected) || !data) return AZ_INVALID;
    if (!analysis_supported()) return AZ_UNSUPPORTED;
    int rc = analysis_publish_input(expected, data, size);
    if (rc != AZ_OK) return rc;
    if (analysis_current(deck, load, expected) != AZ_OK) return AZ_STALE;
    struct AnalysisMake call __attribute__((cleanup(analysis_make_drop))) = {.pssi = data, .size = size};
    analysis_make(&call);
    if (!analysis_content_valid(&call)) return AZ_FAILED;
    return analysis_publish_commit(deck, load, expected, call.content);
}
