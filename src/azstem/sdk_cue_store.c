/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcuestore.h"
#include "../common/azcueprofile.h"
#include "../common/azcuepersist.h"
#include <string.h>
#include <math.h>
#include <limits.h>
#include "../common/aznative_c.h"
#ifdef AZSDK_CUE_DIAGNOSTIC
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#endif


/* Diagnostic builds only: at most one native snapshot diagnostic per second,
 * and at most 128 for the lifetime of the process. Never inspect request data. */
#ifdef AZSDK_CUE_DIAGNOSTIC
static bool diagnostic_admitted(void) {
    static _Atomic(uint64_t) last = 0;
    static _Atomic(unsigned) emitted = 0;
    struct timespec clock = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &clock) || atomic_load_explicit(&(emitted), memory_order_relaxed) >= 128) return false;
    const uint64_t now = (uint64_t) (clock.tv_sec) * 1000000000ULL + (uint64_t) (clock.tv_nsec);
    uint64_t previous = atomic_load_explicit(&(last), memory_order_relaxed);
    if (previous && compare_timestamps(now, previous, 1000000000, 1, 1) == AZ_TIME_EQUAL) return false;
    if (!atomic_compare_exchange_strong_explicit(&(last), &(previous), now, memory_order_relaxed, memory_order_relaxed)) return false;
    if (atomic_fetch_add_explicit(&(emitted), 1, memory_order_relaxed) >= 128) return false;
    return true;
}
static void diagnostic_hex(const char *stage, uintptr_t position, uintptr_t pack, char position_hex[33], char pack_hex[33]) {
    const char digits[] = "0123456789abcdef";
    if (position && pack && !strcmp(stage, "track16")) {
        __auto_type a = ((const unsigned char *) (position + 8));
        __auto_type b = ((const unsigned char *) (pack + 0x80));
        for (unsigned i = 0; i < 16; ++i) {
            position_hex[i * 2] = digits[a[i] >> 4];
            position_hex[i * 2 + 1] = digits[a[i] & 15];
            pack_hex[i * 2] = digits[b[i] >> 4];
            pack_hex[i * 2 + 1] = digits[b[i] & 15];
        }
    }
}
#endif
static void diagnostic(const char *stage, uintptr_t position, uintptr_t source, uintptr_t pack) {
#ifdef AZSDK_CUE_DIAGNOSTIC
    if (!diagnostic_admitted()) return;
    char position_hex[33] = {0}, pack_hex[33] = {0};
    diagnostic_hex(stage, position, pack, position_hex, pack_hex);
    fprintf(stderr, "SDK_CUE_STORE stage=%s native_position=%p native_source=%p native_pack=%p position16=%s pack16=%s\n",
            stage, ((void *) (position)), ((void *) (source)), ((void *) (pack)), position_hex, pack_hex);
#else
    (void) stage;
    (void) position;
    (void) source;
    (void) pack;
#endif
}
#define read(T, address) ({ T read_value; memcpy(&read_value, (const void *)(uintptr_t)(address), sizeof read_value); read_value; })
static uintptr_t word(uintptr_t a) { return read(uintptr_t, a); }
#define method(F, object, slot) ((F) word(word(object) + (slot) * sizeof(uintptr_t)))
static bool nonzero(const unsigned char *p, size_t n) {
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) v |= p[i];
    return v != 0;
}
static uint64_t hash(uint64_t h, const void *p, size_t n) {
    __auto_type b = ((const unsigned char *) (p));
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}
static bool complete(const azcs_profile *p) { return p->copy_source && p->copy_content && p->release_ref && p->position && p->get_cue && p->destroy_cue && p->convert && p->destroy_property && p->where_from; }
typedef struct CueVector {
    uintptr_t begin;
    size_t count;
} CueVector;
static bool cue_vector(uintptr_t content, size_t limit, CueVector *out) {
    (*out) = (CueVector) {0};
    if (!content) return true;
    const uintptr_t begin = word(content + 0x28), end = word(content + 0x30);
    if (end < begin || (end - begin) % 0x80 || (end - begin) / 0x80 > limit || (end != begin && !begin)) return false;
    out->begin = begin;
    out->count = (end - begin) / 0x80;
    return true;
}
static bool variant_payload_equal(uintptr_t a, uintptr_t b, int32_t tag) {
    const uintptr_t value_a = word(a + 8), value_b = word(b + 8);
    return value_a && value_b && !memcmp((const void *) value_a, (const void *) value_b, tag == 2 ? 8 : 20);
}
static bool same_variant(uintptr_t a, uintptr_t b) {
    const int32_t tag = read(int32_t, a);
    if (tag != read(int32_t, b)) return false;
    if (tag == 0) return true; /* Native copy does not read the inactive union. */
    if (read(uint32_t, a + 0x18) != read(uint32_t, b + 0x18)) return false;
    /* Native stored-Cue serializer 1177a00 reads a single MPEG point (tag 2)
     * as two u32 values, or a single FLAC point (tag 4) as two u64 and one u32.
     * Ref/control addresses and FLAC tail padding are not values. Full seek
     * arrays (tags 1/3) and unproved lengths must fail closed. */
    if ((tag != 2 && tag != 4) || read(uint32_t, a + 0x18) != 1) return false;
    return variant_payload_equal(a, b, tag);
}
typedef enum {
    CueDelta_None,
    CueDelta_Auto,
    CueDelta_Comment,
    CueDelta_Color,
    CueDelta_Creation
} CueDelta;
static bool same_cue_identity(uintptr_t a, uintptr_t b, uint32_t rate) {
    return read(unsigned char, a) == read(unsigned char, b) && read(uint32_t, a + 4) == read(uint32_t, b + 4) &&
           (read(unsigned char, a) == 0xff || azcue_timestamps_equal(azcue_read_timestamps((const void *) a), azcue_read_timestamps((const void *) b), rate)) &&
           read(uint32_t, a + 0x2a) == read(uint32_t, b + 0x2a);
}
static unsigned memory_palette_index(uint32_t rgb) {
    const uint32_t palette[] = {0, 0xff00c6, 0xff0000, 0xff7100, 0xffff00, 0x00e300, 0x00c3ff, 0x0051ff, 0x9c08ff};
    unsigned index = 0;
    while (index < sizeof palette / sizeof *palette && palette[index] != rgb) ++index;
    return index;
}
static bool edited_cue_color(uintptr_t b, uint32_t rgb, bool memory_color) {
    uint32_t new_color = read(uint32_t, b + 0x78);
    if (new_color != (0xff000000u | rgb) && !(memory_color && !rgb && !new_color)) return false;
    if (memory_color) {
        unsigned index = memory_palette_index(rgb);
        if (index == 9 || read(unsigned char, b + 0x7c) != index) return false;
    }
    return true;
}
static bool same_cue_color(uintptr_t a, uintptr_t b, CueDelta delta, uint32_t rgb) {
    bool color_edit = delta == CueDelta_Color || delta == CueDelta_Creation;
    bool memory_color = color_edit && read(unsigned char, a) == 0;
    if (!memory_color && read(unsigned char, a + 0x7c) != read(unsigned char, b + 0x7c)) return false;
    if (color_edit) return edited_cue_color(b, rgb, memory_color);
    return read(uint32_t, a + 0x78) == read(uint32_t, b + 0x78);
}
static bool same_cue_comment(uintptr_t a, uintptr_t b, CueDelta delta, const char *comment) {
    const uintptr_t text_a = word(a + 0x70), text_b = word(b + 0x70);
    const char *expected = delta == CueDelta_Comment || delta == CueDelta_Creation ? comment : text_a ? (const char *) text_a
                                                                                                      : "";
    return expected && !strcmp(expected, text_b ? (const char *) text_b : "");
}
static bool same_cue(uintptr_t a, uintptr_t b, CueDelta delta, bool desired, uint32_t source_rate, const char *comment, uint32_t rgb) {
    if (!same_cue_identity(a, b, source_rate)) return false;
    if (delta == CueDelta_Auto ? read(unsigned char, b + 0x28) != (unsigned) desired : read(unsigned char, a + 0x28) != read(unsigned char, b + 0x28)) return false;
    if (!same_cue_color(a, b, delta, rgb)) return false;
    if (!same_variant(a + 0x30, b + 0x30) || !same_variant(a + 0x50, b + 0x50)) return false;
    return same_cue_comment(a, b, delta, comment);
}

static bool coordinates(uint32_t rate, double seconds, uint64_t *sample, uint64_t *usec) {
    double rounded = round(seconds * rate);
        // Preserve the origin window accepted above, including a tiny negative
        // input, without converting a negative frame to unsigned storage.
    if (signbit(rounded)) rounded = 0;
    const int sample_bound = compare_timestamps(rounded, 9007199254740992., rate, rate, 1);
    if (sample_bound == AZ_TIME_INVALID || sample_bound != AZ_TIME_BEFORE) return false;
    (*sample) = ((uint64_t) (rounded));
        // Stock Property-to-Cue serialization floors cached_usec * Fs / 1e6.
        // Ceil the exact source-frame time so that this roundtrip retains the
        // selected frame instead of introducing another truncation toward zero.
    const __uint128_t cached = ((__uint128_t) (*sample) * 1000000 + rate - 1) / rate;
    const int cache_bound = compare_timestamps(cached, (__uint128_t) (1) << 53, 1000000, rate, 1);
    if (cache_bound == AZ_TIME_INVALID || cache_bound != AZ_TIME_BEFORE) return false;
    (*usec) = ((uint64_t) (cached));
    return true;
}
static bool absent(uintptr_t row) {
    const uint64_t marker = UINT64_C(0x7fffffffffffffff);
    return read(unsigned char, row) == 0xff || read(uint64_t, row + 8) == marker || read(uint64_t, row + 16) == marker;
}
static bool match_unchanged_cue(uintptr_t old, CueVector b, bool matched[10], uint32_t rate) {
    for (size_t j = 0; j < b.count; ++j)
        if (!matched[j] && same_cue(old, b.begin + j * 0x80, CueDelta_None, false, rate, NULL, 0)) {
            matched[j] = true;
            return true;
        }
    return false;
}
static bool match_previous_cues(CueVector a, CueVector b, bool matched[10], bool changed, unsigned kind, uint32_t rate, unsigned *removed_empty) {
    for (size_t i = 0; i < a.count; ++i) {
        uintptr_t old = a.begin + i * 0x80;
        if (!match_unchanged_cue(old, b, matched, rate)) {
            if (!changed || !kind || !absent(old) || ++*removed_empty > 1) return false;
        }
    }
    return true;
}
static bool creation_counts_match(CueVector a, CueVector b, unsigned kind, unsigned removed_empty, unsigned additions) {
    return additions == 1 && (kind ? b.count + removed_empty == a.count + 1 : b.count == a.count + 1);
}
static bool unchanged(CueVector a, CueVector b, bool changed, unsigned kind, uint32_t rate, const azcs_owned_cue *wanted, const char *comment, uint32_t rgb) {
    bool matched[10] = {0};
    unsigned additions = 0, removed_empty = 0;
    if (!match_previous_cues(a, b, matched, changed, kind, rate, &removed_empty)) return false;
    for (size_t j = 0; j < b.count; ++j)
        if (!matched[j]) {
            if (!changed || ++additions > 1 || !same_cue((uintptr_t) &wanted->native, b.begin + j * 0x80, CueDelta_Creation, false, rate, comment, rgb)) return false;
        }
    return changed ? creation_counts_match(a, b, kind, removed_empty, additions) : !additions;
}

struct string_release {
    azcs_native_string *text;
    az_native_string_destroy_fn destroy;
};
static void string_release_drop(struct string_release *r) { r->destroy(r->text); }
azcs_profile azcs_native_profile(int build, uintptr_t bias) {
    azcs_profile p = {0};
#if defined(__aarch64__) && !defined(AZSDK_CUE_STORE_TESTING)
    if (build != 1 && build != 2) return p;
    struct azcue_profile profile = {0};
    if (!azsdk_cue_main_profile(build, &profile)) return p;
    // Caller bias must name the same main image that passed byte validation.
    struct azcue_profile nominal = {0};
    if (!azsdk_cue_profile_get(build, bias, &nominal) || nominal.ctor.address != profile.ctor.address) return p;
#define NATIVE(field, index) p.field = ((__typeof__(p.field)) (profile.store[index].address))
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
    p.string_allocate = ((__typeof__(p.string_allocate)) (profile.persist.string_allocate));
    p.string_destroy = ((__typeof__(p.string_destroy)) (profile.persist.string_destroy));
    p.color_construct = ((__typeof__(p.color_construct)) (profile.persist.color_construct));
#undef NATIVE
#else
    (void) build;
    (void) bias;
#endif
    return p;
}
void azcs_owned_cue_reset(azcs_owned_cue *self) {
    if (self->destroy_) self->destroy_(&self->native);
    self->destroy_ = NULL;
    memset(&self->native, 0, sizeof self->native);
}
void azcs_property_reset(azcs_property *self) {
    if (self->destroy_) self->destroy_(&self->native);
    self->destroy_ = NULL;
    self->copy_ = NULL;
    memset(&self->native, 0, sizeof self->native);
}
bool azcs_property_copy_to(const azcs_property *self, azcs_property *out) {
    if (out == self) return azcs_property_valid(self);
    azcs_property_reset(out);
    if (!self->destroy_ || !self->copy_) return false;
    self->copy_(&out->native, &self->native);
    out->destroy_ = self->destroy_;
    out->copy_ = self->copy_;
    return true;
}
void azcs_snapshot_reset(azcs_snapshot *self) {
    if (self->content_) self->p_.release_ref(&self->content_);
    if (self->hot_content_) self->p_.release_ref(&self->hot_content_);
    if (self->memory_set_) self->p_.release_ref(&self->memory_set_);
    if (self->hot_set_) self->p_.release_ref(&self->hot_set_);
    if (self->where_from_) self->p_.release_ref(&self->where_from_);
    if (self->source_) self->p_.release_ref(&self->source_);
    if (self->player_) method(void (*)(void *), self->player_, 1)(((void *) (self->player_)));
    self->player_ = self->source_ = self->where_from_ = self->memory_set_ = self->hot_set_ = self->content_ = self->hot_content_ = self->pack_ = 0;
    self->count_ = self->memory_count_ = 0;
    self->revision_ = 0;
    self->fingerprint_ = 0;
    memset(self->source_id_, 0, sizeof self->source_id_);
    memset(self->track_id_, 0, sizeof self->track_id_);
}
static bool snapshot_player(azcs_snapshot *self, void *manager, uintptr_t *service) {
    uintptr_t helper = (uintptr_t) manager + 0x150, player_interface = word(helper + 8);
    *service = word(helper + 0x10);
    if (!player_interface || !*service) {
        diagnostic("interfaces", 0, 0, 0);
        return false;
    }
    az_native_player_acquire_fn acquire = (az_native_player_acquire_fn) word(word(player_interface) + 2 * sizeof(uintptr_t));
    self->player_ = (uintptr_t) az_native_player_acquire(acquire, (struct az_native_player_interface *) player_interface);
    if (!self->player_) {
        diagnostic("player", 0, 0, 0);
        return false;
    }
    return true;
}
static bool snapshot_position(azcs_snapshot *self, const unsigned char expected[12], uintptr_t *position) {
    *position = (uintptr_t) self->p_.position((const void *) self->player_);
    if (!*position || !nonzero((const unsigned char *) (*position + 8), 12) ||
        (expected && memcmp((const void *) (*position + 8), expected, 12))) {
        diagnostic("position", *position, 0, 0);
        return false;
    }
    memcpy(self->source_id_, (const void *) (*position + 8), 12);
    return true;
}
static bool snapshot_source(azcs_snapshot *self, uintptr_t position) {
    self->p_.copy_source(&self->source_, (const uintptr_t *) (position + 0x30));
    if (!self->source_ || !word(self->source_ + 0x58)) {
        diagnostic("source", position, self->source_, 0);
        return false;
    }
    self->where_from_ = (uintptr_t) az_native_location_acquire(self->p_.where_from, (const struct az_native_player *) self->player_);
    if (!self->where_from_ || self->where_from_ != word(self->source_ + 0x58)) {
        diagnostic("where-from", position, self->source_, 0);
        return false;
    }
    return true;
}
static bool snapshot_pack(azcs_snapshot *self, uintptr_t service, uintptr_t position) {
    self->pack_ = (uintptr_t) method(void *(*) (void *, const void *), service, 6)((void *) service, &self->where_from_);
    if (!self->pack_) {
        diagnostic("pack", position, self->source_, 0);
        return false;
    }
    /* The identity is 12 bytes; the native pack retains four trailing padding bytes. */
    memcpy(self->track_id_, (const void *) (self->pack_ + 0x80), 16);
    if (!nonzero(self->track_id_, 12) || memcmp(self->track_id_, (const void *) (position + 8), 12)) {
        diagnostic("track16", position, self->source_, self->pack_);
        return false;
    }
    return true;
}
static bool snapshot_publications(azcs_snapshot *self, uintptr_t publication, uintptr_t hot_publication) {
    if (publication) self->p_.copy_content(&self->memory_set_, (const uintptr_t *) (self->pack_ + 0xe8));
    if (hot_publication) self->p_.copy_content(&self->hot_set_, (const uintptr_t *) (self->pack_ + 0xe0));
    return !((publication && !self->memory_set_) || (hot_publication && !self->hot_set_));
}
static bool missing_content(uintptr_t publication, uintptr_t content) {
    return publication && word(publication + 0x28) && !content;
}
static bool snapshot_contents(azcs_snapshot *self, uintptr_t position) {
    uintptr_t publication = word(self->pack_ + 0xe8), hot_publication = word(self->pack_ + 0xe0);
    if (!publication && !hot_publication) diagnostic("pub-empty", position, self->source_, self->pack_);
    if (!snapshot_publications(self, publication, hot_publication)) return false;
    if (self->memory_set_) self->p_.copy_content(&self->content_, (const uintptr_t *) (self->memory_set_ + 0x28));
    if (self->hot_set_) self->p_.copy_content(&self->hot_content_, (const uintptr_t *) (self->hot_set_ + 0x28));
    if (missing_content(publication, self->content_) || missing_content(hot_publication, self->hot_content_)) {
        diagnostic("content", position, self->source_, self->pack_);
        return false;
    }
    return true;
}
static bool snapshot_vectors(azcs_snapshot *self, uintptr_t position, CueVector *memory, CueVector *hot) {
    if (!cue_vector(self->content_, 64, memory) || !cue_vector(self->hot_content_, 8, hot)) {
        diagnostic("vector", position, self->source_, self->pack_);
        return false;
    }
    self->memory_count_ = memory->count;
    self->count_ = memory->count + hot->count;
    if (self->count_ > 64) {
        diagnostic("vector-total", position, self->source_, self->pack_);
        return false;
    }
    return true;
}
static uint64_t content_fingerprint(const unsigned char identity[12], CueVector memory, CueVector hot) {
    uint64_t fingerprint = hash(1469598103934665603ULL, identity, 12);
    const uint64_t memory_size = memory.count;
    fingerprint = hash(fingerprint, &memory_size, sizeof memory_size);
    fingerprint = hash(fingerprint, (const void *) memory.begin, memory.count * 0x80);
    return hash(fingerprint, (const void *) hot.begin, hot.count * 0x80);
}
static bool snapshot_changed(const azcs_snapshot *self, const azcs_revision_state *state, uint64_t generation, uint64_t fingerprint) {
    return state->load_generation != generation || state->content != self->content_ ||
           state->hot_content != self->hot_content_ || state->fingerprint != fingerprint || memcmp(state->track, self->track_id_, 12);
}
static bool snapshot_revision(azcs_snapshot *self, azcs_revision_state *state, uint64_t generation, uint64_t fingerprint) {
    if (snapshot_changed(self, state, generation, fingerprint)) {
        if (state->revision == UINT64_MAX) return false;
        ++state->revision;
        state->load_generation = generation;
        state->content = self->content_;
        state->hot_content = self->hot_content_;
        state->fingerprint = fingerprint;
        memcpy(state->track, self->track_id_, 16);
    }
    self->revision_ = state->revision;
    self->fingerprint_ = fingerprint;
    return true;
}
static bool snapshot_acquire(azcs_snapshot *self, void *manager, const unsigned char expected[12], uint64_t generation, azcs_revision_state *state) {
    uintptr_t service, position;
    CueVector memory, hot;
    if (!snapshot_player(self, manager, &service) || !snapshot_position(self, expected, &position) ||
        !snapshot_source(self, position) || !snapshot_pack(self, service, position) ||
        !snapshot_contents(self, position) || !snapshot_vectors(self, position, &memory, &hot)) return false;
    return snapshot_revision(self, state, generation, content_fingerprint(self->track_id_, memory, hot));
}
bool azcs_snapshot_open(azcs_snapshot *self, const azcs_profile *p, void *manager, uint64_t generation, const unsigned char expected[12], azcs_revision_state *state) {
    azcs_snapshot_reset(self);
    self->p_ = *p;
    if (!complete(p) || !manager || !generation || (expected && !nonzero(expected, 12))) {
        diagnostic("profile", 0, 0, 0);
        return false;
    }
    if (!snapshot_acquire(self, manager, expected, generation, state)) {
        azcs_snapshot_reset(self);
        return false;
    }
    return true;
}
bool azcs_snapshot_row(const azcs_snapshot *self, size_t ordinal, azcs_owned_cue *out) {
    azcs_owned_cue_reset(out);
    if (!self->revision_ || ordinal >= self->count_ || memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12)) return false;
    const uintptr_t *collection = ordinal < self->memory_count_ ? &self->content_ : &self->hot_content_;
    const size_t index = ordinal < self->memory_count_ ? ordinal : ordinal - self->memory_count_;
    out->native = self->p_.get_cue(collection, index);
    out->destroy_ = self->p_.destroy_cue;
    return true;
}
bool azcs_snapshot_property(const azcs_snapshot *self, uint32_t entry_id, uint64_t expected_revision, azcs_property *out) {
    azcs_property_reset(out);
    if (!entry_id || expected_revision != self->revision_) return false;
    azcs_owned_cue cue __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    if (!azcs_snapshot_row(self, entry_id - 1, &(cue))) return false;
    out->native = self->p_.convert(&self->where_from_, ((const void *) (self->pack_)), ((const void *) (self->source_ + 0x28)), &cue.native);
    out->destroy_ = self->p_.destroy_property;
    out->copy_ = self->p_.copy_property;
    /* The converted point must belong to the exact source admitted above. */
    if (memcmp(out->native.bytes + 0x30, self->source_id_, 12)) {
        azcs_property_reset(out);
        return false;
    }
    return true;
}
static bool current_property_ready(const azcs_snapshot *self, unsigned kind) {
    return !(!self->revision_ || kind > 8 || !self->p_.has_cue || !self->p_.get_current_cue || !self->p_.copy_property ||
             memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12) ||
             !self->p_.has_cue(((const void *) (self->player_)), ((int) (kind))));
}
bool azcs_snapshot_current_property(const azcs_snapshot *self, unsigned kind, azcs_property *out) {
    azcs_property_reset(out);
    if (!current_property_ready(self, kind)) return false;
    __auto_type cue = ((const unsigned char *) (self->p_.get_current_cue(((const void *) (self->player_)), ((int) (kind)))));
    if (!cue || memcmp(cue + 0x30, self->source_id_, 12)) return false;
    self->p_.copy_property(&out->native, cue);
    out->destroy_ = self->p_.destroy_property;
    out->copy_ = self->p_.copy_property;
    return true;
}
uint32_t azcs_snapshot_source_rate(const azcs_snapshot *self) {
    if (!self->revision_ || !self->player_ || !self->pack_ || memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12)) return 0;
    // Both stock PlayerState::getPositionTime implementations divide the
    // selected source-position samples by this owned snapshot's Hertz value.
    // UnitStateServer publishes the same Core sampling-rate getter used by
    // the stock cue builder. Pack+0xbe is musical key data, not a clock.
    uint32_t rate = read(uint32_t, self->player_ + 0x18);
    return rate && rate < 0x7fffffffu ? rate : 0;
}
static bool property_builders_ready(const azcs_snapshot *self) {
    return self->p_.string_allocate && self->p_.string_destroy && self->p_.color_construct && self->p_.copy_property;
}
static bool timed_property_input(const azcs_snapshot *self, uint32_t rate, unsigned kind, uint32_t rgb, const char *comment, double in_s, double out_s) {
    return rate && kind <= 8 && rgb <= 0xffffff && comment && property_builders_ready(self) && isfinite(in_s) && isfinite(out_s);
}
static bool valid_start_time(int start, int bound, int us_bound) {
    return start != AZ_TIME_INVALID && start != AZ_TIME_BEFORE && bound != AZ_TIME_INVALID && bound == AZ_TIME_BEFORE &&
           us_bound != AZ_TIME_INVALID && us_bound == AZ_TIME_BEFORE;
}
static bool valid_end_time(int length, int end_bound, int end_us_bound) {
    return length != AZ_TIME_INVALID && length == AZ_TIME_AFTER && end_bound != AZ_TIME_INVALID && end_bound == AZ_TIME_BEFORE &&
           end_us_bound != AZ_TIME_INVALID && end_us_bound == AZ_TIME_BEFORE;
}
static bool timed_coordinates_valid(uint32_t rate, double in_s, double out_s, bool loop) {
    int start = compare_timestamps(in_s, 0., 1, rate, 1);
    int length = compare_timestamps(out_s, in_s, 1, rate, 1);
    int bound = compare_timestamps(in_s * rate, 9007199254740992., rate, rate, 1);
    int end_bound = compare_timestamps(out_s * rate, 9007199254740992., rate, rate, 1);
    int us_bound = compare_timestamps(in_s * 1000000., 9007199254740992., 1000000, rate, 1);
    int end_us_bound = compare_timestamps(out_s * 1000000., 9007199254740992., 1000000, rate, 1);
    return valid_start_time(start, bound, us_bound) && (!loop || valid_end_time(length, end_bound, end_us_bound));
}
static bool timed_cue_color(const azcs_snapshot *self, azcs_native_cue *cue, unsigned kind, uint32_t rgb) {
    self->p_.color_construct(cue->bytes + 0x78, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    if (!kind) {
        unsigned index = memory_palette_index(rgb);
        if (index == 9) return false;
        cue->bytes[0x7c] = (unsigned char) index;
    }
    return true;
}
static bool timed_cue_coordinates(azcs_native_cue *cue, uint32_t rate, double in_s, double out_s, bool loop) {
    uint64_t in_us = 0, in_sample = 0;
    uint64_t out_us = UINT64_C(0x7fffffffffffffff), out_sample = out_us;
    if (!coordinates(rate, in_s, &in_sample, &in_us) || (loop && !coordinates(rate, out_s, &out_sample, &out_us))) return false;
    memcpy(cue->bytes + 4, &rate, sizeof rate);
    memcpy(cue->bytes + 8, &in_us, sizeof in_us);
    memcpy(cue->bytes + 16, &in_sample, sizeof in_sample);
    memcpy(cue->bytes + 24, &out_us, sizeof out_us);
    memcpy(cue->bytes + 32, &out_sample, sizeof out_sample);
    return true;
}
static bool property_endpoint_matches(const azcs_snapshot *self, const azcs_property *out, unsigned offset, uint32_t rate, double seconds) {
    if (memcmp(out->native.bytes + 0x30 + offset, self->source_id_, 12) ||
        read(uint32_t, (uintptr_t) (out->native.bytes + 0x60 + offset)) != rate) return false;
    double actual = (double) read(uint64_t, (uintptr_t) (out->native.bytes + 0x40 + offset)) / rate;
    int order = compare_timestamps(actual, seconds, 1, rate, 1);
    if (order == AZ_TIME_INVALID) return false;
    return order == AZ_TIME_EQUAL;
}
static bool native_comment(const azcs_snapshot *self, const char *comment, azcs_native_string *text) {
    size_t length = 0;
    while (length < 128 && comment[length]) ++length;
    if (length == 128) return false;
    *text = az_native_string_allocate(self->p_.string_allocate, length + 1);
    if (!text->data) return false;
    memcpy(text->data, comment, length + 1);
    return true;
}
static bool timed_property_matches(const azcs_snapshot *self, const azcs_property *out, uint32_t rate, double in_s, double out_s, bool loop) {
    return property_endpoint_matches(self, out, 0, rate, in_s) &&
           (!loop || property_endpoint_matches(self, out, 0x58, rate, out_s)) &&
           !memcmp((const void *) (self->pack_ + 0x80), self->track_id_, 12);
}
static bool timed_property_impl(const azcs_snapshot *self, unsigned kind, double in_s, double out_s, bool loop, uint32_t rgb, const char *comment, azcs_property *out) {
    azcs_property_reset(out);
    uint32_t rate = azcs_snapshot_source_rate(self);
    if (!timed_property_input(self, rate, kind, rgb, comment, in_s, out_s) || !timed_coordinates_valid(rate, in_s, out_s, loop)) return false;
    azcs_native_string text = {0};
    if (!native_comment(self, comment, &text)) return false;
    struct string_release release __attribute__((cleanup(string_release_drop))) = {&text, self->p_.string_destroy};
    (void) release; /* cleanup attribute owns the scope exit */
    // Cue80 is a value carrier, not a polymorphic firmware object. Its two
    // tagged seek variants are empty (tag=0); no counted reference is forged.
    // Native conversion owns every object it constructs from this borrowed row.
    azcs_native_cue cue = {0};
    cue.bytes[0] = ((unsigned char) (kind));


    if (!timed_cue_coordinates(&cue, rate, in_s, out_s, loop)) return false;
    memcpy(cue.bytes + 0x70, &text.data, sizeof text.data);
    if (!timed_cue_color(self, &cue, kind, rgb)) return false;
    out->native = self->p_.convert(&self->where_from_, ((const void *) (self->pack_)),
                                   ((const void *) (self->source_ + 0x28)), &cue);
    out->destroy_ = self->p_.destroy_property;
    out->copy_ = self->p_.copy_property;
    if (!timed_property_matches(self, out, rate, in_s, out_s, loop)) {
        azcs_property_reset(out);
        return false;
    }
    return true;
}
bool azcs_snapshot_timed_property(const azcs_snapshot *self, unsigned kind, double in_s, double out_s, bool loop, uint32_t rgb, const char *comment, azcs_property *out) {
    azcs_property value __attribute__((cleanup(azcs_property_reset))) = {0};
    azcs_property_reset(out);
    if (!timed_property_impl(self, kind, in_s, out_s, loop, rgb, comment, &value)) return false;
    *out = value;
    value = (azcs_property) {0};
    return true;
}
static bool canonical_property_ready(const azcs_snapshot *self, unsigned kind, const azcs_property *original) {
    return !(!self->revision_ || kind > 8 || !azcs_property_valid(original) || !self->p_.serialize_property ||
             memcmp(original->native.bytes + 0x30, self->source_id_, 12) ||
             memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12));
}
bool azcs_snapshot_canonical_property(const azcs_snapshot *self, unsigned kind, const azcs_property *original, azcs_property *out) {
    if (out == original) return false;
    if (!canonical_property_ready(self, kind, original)) {
        azcs_property_reset(out);
        return false;
    }
    azcs_owned_cue cue __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    /* Native CueStoreMaintainer::request sets W2 to (kind != 0). Both native
     * conversions retain strings and source refs through their own constructors. */
    cue.native = self->p_.serialize_property(kind, &original->native, kind != 0);
    cue.destroy_ = self->p_.destroy_cue;
    azcs_property_reset(out);
    out->native = self->p_.convert(&self->where_from_, ((const void *) (self->pack_)),
                                   ((const void *) (self->source_ + 0x28)), &cue.native);
    out->destroy_ = self->p_.destroy_property;
    out->copy_ = self->p_.copy_property;
    if (memcmp(out->native.bytes + 0x30, self->source_id_, 12) ||
        memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12)) {
        azcs_property_reset(out);
        return false;
    }
    return true;
}
static bool snapshot_publication_current(const azcs_snapshot *self) {
    return word(self->pack_ + 0xe8) == self->memory_set_ && word(self->pack_ + 0xe0) == self->hot_set_ &&
           (!self->memory_set_ || word(self->memory_set_ + 0x28) == self->content_) &&
           (!self->hot_set_ || word(self->hot_set_ + 0x28) == self->hot_content_);
}
static bool witness_snapshot_ready(const azcs_snapshot *self, uint64_t expected_revision, bool allow_absent) {
    return self->revision_ && expected_revision == self->revision_ &&
           (allow_absent || (self->memory_set_ && self->hot_set_)) &&
           !memcmp((const void *) (self->pack_ + 0x80), self->track_id_, 12);
}
static bool snapshot_vector_counts(const azcs_snapshot *self, CueVector memory, CueVector hot) {
    return memory.count == self->memory_count_ && hot.count == self->count_ - self->memory_count_;
}
bool azcs_snapshot_content_witness(const azcs_snapshot *self, uint64_t expected_revision, struct azcuepersist_witness *out, bool allow_absent) {
    if (!out) return false;
    *out = (struct azcuepersist_witness) {0};
    if (!witness_snapshot_ready(self, expected_revision, allow_absent) || !snapshot_publication_current(self)) return false;
    CueVector memory, hot;
    if (!cue_vector(self->content_, 64, &memory) || !cue_vector(self->hot_content_, 8, &hot) || !snapshot_vector_counts(self, memory, hot)) return false;
    uint64_t current = content_fingerprint(self->source_id_, memory, hot);
    if (current != self->fingerprint_) return false;
    out->source_info = self->source_;
    out->memory_set = self->memory_set_;
    out->hot_set = self->hot_set_;
    out->memory_content = self->content_;
    out->hot_content = self->hot_content_;
    memcpy(out->source_id, self->source_id_, 12);
    out->fingerprint = current;
    out->revision = self->revision_;
    return true;
}
static bool creation_input(unsigned kind, const azcs_property *expected, uint32_t rate, const char *comment, uint32_t rgb) {
    return rate && kind <= 8 && comment && rgb <= 0xffffffu && azcs_property_valid(expected);
}
static bool witness_origin(const azcs_snapshot *self, const struct azcuepersist_witness *before) {
    return before->revision && before->source_info && !memcmp(before->source_id, self->source_id_, 12);
}
static bool creation_vectors(const azcs_snapshot *self, const struct azcuepersist_witness *before, CueVector *old_memory, CueVector *old_hot, CueVector *memory, CueVector *hot) {
    struct azcuepersist_witness fresh = {0};
    return azcs_snapshot_content_witness(self, self->revision_, &fresh, true) &&
           cue_vector(before->memory_content, 10, old_memory) && cue_vector(before->hot_content, 8, old_hot) &&
           cue_vector(self->content_, 10, memory) && cue_vector(self->hot_content_, 8, hot);
}
bool azcs_snapshot_creation_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, unsigned kind, const azcs_property *expected, uint32_t rate, const char *comment, uint32_t rgb) {
    CueVector old_memory, old_hot, memory, hot;
    if (!creation_input(kind, expected, rate, comment, rgb) || !witness_origin(self, before) ||
        !creation_vectors(self, before, &old_memory, &old_hot, &memory, &hot)) return false;
    if (content_fingerprint(before->source_id, old_memory, old_hot) != before->fingerprint) return false;
    azcs_owned_cue wanted __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    wanted.native = self->p_.serialize_property(kind, &expected->native, kind != 0);
    wanted.destroy_ = self->p_.destroy_cue;


    return unchanged(old_memory, memory, !kind, kind, rate, &(wanted), comment, rgb) && unchanged(old_hot, hot, kind != 0, kind, rate, &(wanted), comment, rgb);
}
static bool witness_refs(const struct azcuepersist_witness *before) {
    return before->revision && before->source_info && before->memory_set && before->hot_set;
}
static bool existing_content_current(const azcs_snapshot *self, const struct azcuepersist_witness *before) {
    struct azcuepersist_witness fresh = {0};
    return azcs_snapshot_content_witness(self, self->revision_, &fresh, false) && !memcmp(before->source_id, self->source_id_, 12);
}
static bool previous_vectors(const azcs_snapshot *self, const struct azcuepersist_witness *before, CueVector *memory, CueVector *hot) {
    return cue_vector(before->memory_content, 64, memory) && cue_vector(before->hot_content, 8, hot) &&
           snapshot_vector_counts(self, *memory, *hot) && memory->count + hot->count <= 64;
}
static bool auto_memory_matches(CueVector memory, uintptr_t current_memory, uint32_t entry, bool desired, uint32_t rate) {
    for (size_t i = 0; i < memory.count; i++)
        if (!same_cue(memory.begin + i * 0x80, current_memory + i * 0x80, i + 1 == entry ? CueDelta_Auto : CueDelta_None, desired, rate, NULL, 0)) return false;
    return true;
}
static bool auto_hot_matches(CueVector hot, uintptr_t current_hot, bool desired, uint32_t rate) {
    for (size_t i = 0; i < hot.count; i++)
        if (!same_cue(hot.begin + i * 0x80, current_hot + i * 0x80, CueDelta_None, desired, rate, NULL, 0)) return false;
    return true;
}
static uintptr_t cue_begin(uintptr_t content) { return content ? word(content + 0x28) : 0; }
bool azcs_snapshot_auto_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, uint32_t memory_entry_id, bool desired, uint32_t source_rate) {
    if (!witness_refs(before) || !memory_entry_id || !source_rate || !existing_content_current(self, before)) return false;
    CueVector memory, hot;
    if (!previous_vectors(self, before, &memory, &hot) || memory_entry_id > memory.count) return false;
    if (content_fingerprint(before->source_id, memory, hot) != before->fingerprint) return false;
    uintptr_t current_memory = cue_begin(self->content_);
    uintptr_t current_hot = cue_begin(self->hot_content_);
    return auto_memory_matches(memory, current_memory, memory_entry_id, desired, source_rate) &&
           auto_hot_matches(hot, current_hot, desired, source_rate);
}
typedef struct CueEdit {
    unsigned kind;
    uint32_t entry, rgb, rate;
    const char *comment;
    CueDelta delta;
} CueEdit;
static bool metadata_input(const struct azcuepersist_witness *before, CueEdit edit) {
    return witness_refs(before) && edit.entry && edit.kind <= 8 && (!edit.kind || edit.kind == edit.entry) &&
           edit.rgb <= 0xffffffu && edit.rate;
}
static bool metadata_memory_matches(CueVector memory, uintptr_t current, CueEdit edit, unsigned *matches) {
    for (size_t i = 0; i < memory.count; i++) {
        bool selected = !edit.kind && i + 1 == edit.entry;
        if (selected) ++*matches;
        if (!same_cue(memory.begin + i * 0x80, current + i * 0x80, selected ? edit.delta : CueDelta_None, false, edit.rate, edit.comment, edit.rgb)) return false;
    }
    return true;
}
static bool metadata_hot_matches(CueVector hot, uintptr_t current, CueEdit edit, unsigned *matches) {
    for (size_t i = 0; i < hot.count; i++) {
        bool selected = edit.kind && read(unsigned char, hot.begin + i * 0x80) == edit.kind;
        if (selected) ++*matches;
        if (!same_cue(hot.begin + i * 0x80, current + i * 0x80, selected ? edit.delta : CueDelta_None, false, edit.rate, edit.comment, edit.rgb)) return false;
    }
    return true;
}
static bool metadata_vectors(const azcs_snapshot *self, const struct azcuepersist_witness *before, CueEdit edit, CueVector *memory, CueVector *hot) {
    return previous_vectors(self, before, memory, hot) && (edit.kind || edit.entry <= memory->count);
}
bool azcs_snapshot_metadata_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, unsigned native_kind, uint32_t entry_id, const char *comment, uint32_t rgb, uint32_t source_rate) {
    CueEdit edit = {native_kind, entry_id, rgb, source_rate, comment, comment ? CueDelta_Comment : CueDelta_Color};
    if (!metadata_input(before, edit) || !existing_content_current(self, before)) return false;
    CueVector memory, hot;
    if (!metadata_vectors(self, before, edit, &memory, &hot)) return false;
    if (content_fingerprint(before->source_id, memory, hot) != before->fingerprint) return false;
    unsigned matches = 0;
    uintptr_t current_memory = cue_begin(self->content_);
    uintptr_t current_hot = cue_begin(self->hot_content_);
    if (!metadata_memory_matches(memory, current_memory, edit, &matches) || !metadata_hot_matches(hot, current_hot, edit, &matches)) return false;
    return matches == 1;
}


bool azcs_property_valid(const azcs_property *self) { return self->destroy_ != NULL; }
size_t azcs_snapshot_count(const azcs_snapshot *self) { return self->count_; }
size_t azcs_snapshot_memory_count(const azcs_snapshot *self) { return self->memory_count_; }
uint64_t azcs_snapshot_revision(const azcs_snapshot *self) { return self->revision_; }
const unsigned char *azcs_snapshot_source_id(const azcs_snapshot *self) { return self->source_id_; }
const unsigned char *azcs_snapshot_track_id(const azcs_snapshot *self) { return self->track_id_; }
