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
static void diagnostic(const char *stage, uintptr_t position, uintptr_t source, uintptr_t pack) {
#ifdef AZSDK_CUE_DIAGNOSTIC
    static _Atomic(uint64_t) last = 0;
    static _Atomic(unsigned) emitted = 0;
    struct timespec clock = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &clock) || atomic_load_explicit(&(emitted), memory_order_relaxed) >= 128) return;
    const uint64_t now = (uint64_t) (clock.tv_sec) * 1000000000ULL + (uint64_t) (clock.tv_nsec);
    uint64_t previous = atomic_load_explicit(&(last), memory_order_relaxed);
    if (previous && compare_timestamps(now, previous, 1000000000, 1, 1) == AZ_TIME_EQUAL) return;
    if (!atomic_compare_exchange_strong_explicit(&(last), &(previous), now, memory_order_relaxed, memory_order_relaxed)) return;
    if (atomic_fetch_add_explicit(&(emitted), 1, memory_order_relaxed) >= 128) return;
    char position_hex[33] = {0}, pack_hex[33] = {0};
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
    const uintptr_t value_a = word(a + 8), value_b = word(b + 8);
    return value_a && value_b && !memcmp(((const void *) (value_a)), ((const void *) (value_b)), tag == 2 ? 8 : 20);
}
typedef enum {
    CueDelta_None,
    CueDelta_Auto,
    CueDelta_Comment,
    CueDelta_Color,
    CueDelta_Creation
} CueDelta;
static bool same_cue(uintptr_t a, uintptr_t b, CueDelta delta, bool desired, uint32_t source_rate, const char *comment, uint32_t rgb) {
    if (read(unsigned char, a) != read(unsigned char, b) || read(uint32_t, a + 4) != read(uint32_t, b + 4) ||
        (read(unsigned char, a) != 0xff && !azcue_timestamps_equal(azcue_read_timestamps(((const void *) (a))), azcue_read_timestamps(((const void *) (b))), source_rate)) ||
        read(uint32_t, a + 0x2a) != read(uint32_t, b + 0x2a)) return false;
    if (delta == CueDelta_Auto ? read(unsigned char, b + 0x28) != (unsigned) (desired)
                               : read(unsigned char, a + 0x28) != read(unsigned char, b + 0x28)) return false;
    const uint32_t old_color = read(uint32_t, a + 0x78), new_color = read(uint32_t, b + 0x78);
    const bool color_edit = delta == CueDelta_Color || delta == CueDelta_Creation;
    const bool memory_color = color_edit && read(unsigned char, a) == 0;
    if (!memory_color && read(unsigned char, a + 0x7c) != read(unsigned char, b + 0x7c)) return false;
    if (color_edit) {
        // A Memory RGB edit also updates the native palette index. The native
        // opaque color constructor uses 0xff; uncolored Memory black can be 0.
        if (new_color != (0xff000000u | rgb) && !(memory_color && !rgb && !new_color)) return false;
        if (memory_color) {
            const uint32_t palette[] = {0, 0xff00c6, 0xff0000, 0xff7100, 0xffff00, 0x00e300, 0x00c3ff, 0x0051ff, 0x9c08ff};
            unsigned index = 0;
            while (index < sizeof palette / sizeof *palette && palette[index] != rgb) ++index;
            if (index == sizeof palette / sizeof *palette || read(unsigned char, b + 0x7c) != index) return false;
        }
    } else if (old_color != new_color) return false;
    if (!same_variant(a + 0x30, b + 0x30) || !same_variant(a + 0x50, b + 0x50)) return false;
    const uintptr_t text_a = word(a + 0x70), text_b = word(b + 0x70);
    const char *expected = delta == CueDelta_Comment || delta == CueDelta_Creation ? comment : text_a ? ((const char *) (text_a))
                                                                                                      : "";
    return expected && !strcmp(expected, text_b ? ((const char *) (text_b)) : "");
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
static bool unchanged(CueVector a, CueVector b, bool changed, unsigned kind, uint32_t rate, const azcs_owned_cue *wanted, const char *comment, uint32_t rgb) {
    bool matched[10] = {0};
    unsigned additions = 0, removed_empty = 0;
    for (size_t i = 0; i < a.count; ++i) {
        uintptr_t old = a.begin + i * 0x80;
        bool found = false;
        for (size_t j = 0; j < b.count; ++j)
            if (!matched[j] &&
                same_cue(old, b.begin + j * 0x80, CueDelta_None, false, rate, NULL, 0)) {
                matched[j] = true;
                found = true;
                break;
            }
        if (!found) {
            if (!changed || !kind || !absent(old) || ++removed_empty > 1) return false;
        }
    }
    for (size_t j = 0; j < b.count; ++j)
        if (!matched[j]) {
            if (!changed || ++additions > 1 || !same_cue(((uintptr_t) (&wanted->native)), b.begin + j * 0x80, CueDelta_Creation, false, rate, comment, rgb)) return false;
        }
    return changed ? additions == 1 && (kind ? b.count + removed_empty == a.count + 1 : b.count == a.count + 1) : !additions;
}

struct string_release {
    azcs_native_string *text;
    void (*destroy)(void *);
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
bool azcs_snapshot_open(azcs_snapshot *self, const azcs_profile *p, void *manager, uint64_t generation, const unsigned char expected[12], azcs_revision_state *state) {
    azcs_snapshot_reset(self);
    self->p_ = (*p);
    if (!complete(p) || !manager || !generation || (expected && !nonzero(expected, 12))) {
        diagnostic("profile", 0, 0, 0);
        return false;
    }
    const uintptr_t helper = ((uintptr_t) (manager)) + 0x150;
    const uintptr_t player_interface = word(helper + 8), service = word(helper + 0x10);
    if (!player_interface || !service) {
        diagnostic("interfaces", 0, 0, 0);
        return false;
    }
    azcs_native_ref owned = {0};
    az_native_result_word((void *) word(word(player_interface) + 2 * sizeof(uintptr_t)), &owned, (void *) player_interface);
    self->player_ = owned.value;
    if (!self->player_) {
        diagnostic("player", 0, 0, 0);
        return false;
    }
    const uintptr_t position = ((uintptr_t) (p->position(((const void *) (self->player_)))));
    if (!position || !nonzero(((const unsigned char *) (position + 8)), 12) ||
        (expected && memcmp(((const void *) (position + 8)), expected, 12))) {
        diagnostic("position", position, 0, 0);
        azcs_snapshot_reset(self);
        return false;
    }
    memcpy(self->source_id_, ((const void *) (position + 8)), 12);
    p->copy_source(&self->source_, ((const uintptr_t *) (position + 0x30)));
    if (!self->source_ || !word(self->source_ + 0x58)) {
        diagnostic("source", position, self->source_, 0);
        azcs_snapshot_reset(self);
        return false;
    }
    azcs_native_ref where = {0};
    az_native_result_word(p->where_from, &where, (const void *) self->player_);
    self->where_from_ = where.value;
    if (!self->where_from_ || self->where_from_ != word(self->source_ + 0x58)) {
        diagnostic("where-from", position, self->source_, 0);
        azcs_snapshot_reset(self);
        return false;
    }
    self->pack_ = ((uintptr_t) (method(void *(*) (void *, const void *), service, 6)(((void *) (service)), &self->where_from_)));
    if (!self->pack_) {
        diagnostic("pack", position, self->source_, 0);
        azcs_snapshot_reset(self);
        return false;
    }
    /* Native SourceID constructors copy 8 + 4 identity bytes. The final four
     * bytes of a position's 16-byte storage are padding and can be uninitialized.
     * Retain the Pack storage for native receipts, but compare/hash identity12. */
    memcpy(self->track_id_, ((const void *) (self->pack_ + 0x80)), 16);
    if (!nonzero(self->track_id_, 12) || memcmp(self->track_id_, ((const void *) (position + 8)), 12)) {
        diagnostic("track16", position, self->source_, self->pack_);
        azcs_snapshot_reset(self);
        return false;
    }
    const uintptr_t publication = word(self->pack_ + 0xe8), hot_publication = word(self->pack_ + 0xe0);
    if (!publication && !hot_publication) diagnostic("pub-empty", position, self->source_, self->pack_);
    if (publication) p->copy_content(&self->memory_set_, ((const uintptr_t *) (self->pack_ + 0xe8)));
    if (hot_publication) p->copy_content(&self->hot_set_, ((const uintptr_t *) (self->pack_ + 0xe0)));
    if ((publication && !self->memory_set_) || (hot_publication && !self->hot_set_)) {
        azcs_snapshot_reset(self);
        return false;
    }
    if (self->memory_set_) p->copy_content(&self->content_, ((const uintptr_t *) (self->memory_set_ + 0x28)));
    if (self->hot_set_) p->copy_content(&self->hot_content_, ((const uintptr_t *) (self->hot_set_ + 0x28)));
    if ((publication && word(publication + 0x28) && !self->content_) ||
        (hot_publication && word(hot_publication + 0x28) && !self->hot_content_)) {
        diagnostic("content", position, self->source_, self->pack_);
        azcs_snapshot_reset(self);
        return false;
    }
    uintptr_t begin = 0, end = 0, hot_begin = 0, hot_end = 0;
    if (self->content_) {
        begin = word(self->content_ + 0x28);
        end = word(self->content_ + 0x30);
    }
    if (self->hot_content_) {
        hot_begin = word(self->hot_content_ + 0x28);
        hot_end = word(self->hot_content_ + 0x30);
    }
    if (end < begin || (end - begin) % 0x80 || (end - begin) / 0x80 > 64 || (end != begin && !begin) ||
        hot_end < hot_begin || (hot_end - hot_begin) % 0x80 || (hot_end - hot_begin) / 0x80 > 8 || (hot_end != hot_begin && !hot_begin)) {
        diagnostic("vector", position, self->source_, self->pack_);
        azcs_snapshot_reset(self);
        return false;
    }
    self->memory_count_ = (end - begin) / 0x80;
    self->count_ = self->memory_count_ + (hot_end - hot_begin) / 0x80;
    if (self->count_ > 64) {
        diagnostic("vector-total", position, self->source_, self->pack_);
        azcs_snapshot_reset(self);
        return false;
    }
    uint64_t fingerprint = hash(1469598103934665603ULL, self->track_id_, 12);
    const uint64_t memory_size = self->memory_count_;
    fingerprint = hash(fingerprint, &memory_size, sizeof memory_size);
    fingerprint = hash(fingerprint, ((const void *) (begin)), self->memory_count_ * 0x80);
    fingerprint = hash(fingerprint, ((const void *) (hot_begin)), (self->count_ - self->memory_count_) * 0x80);
    if (state->load_generation != generation || state->content != self->content_ || state->hot_content != self->hot_content_ || state->fingerprint != fingerprint || memcmp(state->track, self->track_id_, 12)) {
        if (state->revision == UINT64_MAX) {
            azcs_snapshot_reset(self);
            return false;
        }
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
bool azcs_snapshot_current_property(const azcs_snapshot *self, unsigned kind, azcs_property *out) {
    azcs_property_reset(out);
    if (!self->revision_ || kind > 8 || !self->p_.has_cue || !self->p_.get_current_cue || !self->p_.copy_property ||
        memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12) ||
        !self->p_.has_cue(((const void *) (self->player_)), ((int) (kind)))) return false;
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
static bool timed_property_impl(const azcs_snapshot *self, unsigned kind, double in_s, double out_s, bool loop, uint32_t rgb, const char *comment, azcs_property *out) {
    azcs_property_reset(out);
    uint32_t rate = azcs_snapshot_source_rate(self);
    if (!rate || kind > 8 || rgb > 0xffffff || !comment || !self->p_.string_allocate ||
        !self->p_.string_destroy || !self->p_.color_construct || !self->p_.copy_property ||
        !isfinite(in_s) || !isfinite(out_s)) return false;
    int start = compare_timestamps(in_s, 0., 1, rate, 1);
    int length = compare_timestamps(out_s, in_s, 1, rate, 1);
    int bound = compare_timestamps(in_s * rate, 9007199254740992., rate, rate, 1);
    int end_bound = compare_timestamps(out_s * rate, 9007199254740992., rate, rate, 1);
    int us_bound = compare_timestamps(in_s * 1000000., 9007199254740992., 1000000, rate, 1);
    int end_us_bound = compare_timestamps(out_s * 1000000., 9007199254740992., 1000000, rate, 1);
    if (start == AZ_TIME_INVALID || start == AZ_TIME_BEFORE || bound == AZ_TIME_INVALID || bound != AZ_TIME_BEFORE ||
        us_bound == AZ_TIME_INVALID || us_bound != AZ_TIME_BEFORE ||
        (loop && (length == AZ_TIME_INVALID || length != AZ_TIME_AFTER || end_bound == AZ_TIME_INVALID || end_bound != AZ_TIME_BEFORE || end_us_bound == AZ_TIME_INVALID || end_us_bound != AZ_TIME_BEFORE))) return false;
    size_t length_bytes = 0;
    while (length_bytes < 128 && comment[length_bytes]) ++length_bytes;
    if (length_bytes == 128) return false;
    azcs_native_string text = {0};
    az_native_result_word(self->p_.string_allocate, &text, (const void *) (uintptr_t) (length_bytes + 1));
    if (!text.data) return false;
    struct string_release release __attribute__((cleanup(string_release_drop))) = {&text, self->p_.string_destroy};
    memcpy(text.data, comment, length_bytes + 1);
    // Cue80 is a value carrier, not a polymorphic firmware object. Its two
    // tagged seek variants are empty (tag=0); no counted reference is forged.
    // Native conversion owns every object it constructs from this borrowed row.
    azcs_native_cue cue = {0};
    cue.bytes[0] = ((unsigned char) (kind));


    uint64_t in_us = 0, in_sample = 0;
    uint64_t out_us = UINT64_C(0x7fffffffffffffff), out_sample = out_us;
    if (!coordinates(rate, in_s, &(in_sample), &(in_us)) || (loop && !coordinates(rate, out_s, &(out_sample), &(out_us)))) return false;
    memcpy(cue.bytes + 4, &rate, sizeof rate);
    memcpy(cue.bytes + 8, &in_us, sizeof in_us);
    memcpy(cue.bytes + 16, &in_sample, sizeof in_sample);
    memcpy(cue.bytes + 24, &out_us, sizeof out_us);
    memcpy(cue.bytes + 32, &out_sample, sizeof out_sample);
    memcpy(cue.bytes + 0x70, &text.data, sizeof text.data);
    self->p_.color_construct(cue.bytes + 0x78, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    if (!kind) {
        const uint32_t palette[] = {0, 0xff00c6, 0xff0000, 0xff7100, 0xffff00, 0x00e300, 0x00c3ff, 0x0051ff, 0x9c08ff};
        unsigned index = 0;
        while (index < sizeof palette / sizeof *palette && palette[index] != rgb) ++index;
        if (index == sizeof palette / sizeof *palette) return false;
        cue.bytes[0x7c] = ((unsigned char) (index));
    }
    out->native = self->p_.convert(&self->where_from_, ((const void *) (self->pack_)),
                                   ((const void *) (self->source_ + 0x28)), &cue);
    out->destroy_ = self->p_.destroy_property;
    out->copy_ = self->p_.copy_property;
    if (memcmp(out->native.bytes + 0x30, self->source_id_, 12) ||
        read(uint32_t, ((uintptr_t) (out->native.bytes + 0x60))) != rate ||
        compare_timestamps((double) (read(uint64_t, ((uintptr_t) (out->native.bytes + 0x40)))) / rate, in_s, 1, rate, 1) != AZ_TIME_EQUAL ||
        (loop && (memcmp(out->native.bytes + 0x88, self->source_id_, 12) ||
                  read(uint32_t, ((uintptr_t) (out->native.bytes + 0xb8))) != rate ||
                  compare_timestamps((double) (read(uint64_t, ((uintptr_t) (out->native.bytes + 0x98)))) / rate, out_s, 1, rate, 1) != AZ_TIME_EQUAL)) ||
        memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12)) {
        azcs_property_reset(out);
        return false;
    }
    return true;
}
struct timed_property_call {
    const azcs_snapshot *self;
    unsigned kind;
    double in_s, out_s;
    bool loop;
    uint32_t rgb;
    const char *comment;
    azcs_property *out;
    bool result;
};
static void timed_property_call(void *opaque) {
    struct timed_property_call *c = opaque;
    c->result = timed_property_impl(c->self, c->kind, c->in_s, c->out_s, c->loop, c->rgb, c->comment, c->out);
}
bool azcs_snapshot_timed_property(const azcs_snapshot *self, unsigned kind, double in_s, double out_s, bool loop, uint32_t rgb, const char *comment, azcs_property *out) {
    struct timed_property_call c = {self, kind, in_s, out_s, loop, rgb, comment, out, false};
    if (!az_native_try(timed_property_call, &c)) {
        azcs_property_reset(out);
        return false;
    }
    return c.result;
}
bool azcs_snapshot_canonical_property(const azcs_snapshot *self, unsigned kind, const azcs_property *original, azcs_property *out) {
    if (out == original) return false;
    if (!self->revision_ || kind > 8 || !azcs_property_valid(original) || !self->p_.serialize_property ||
        memcmp(original->native.bytes + 0x30, self->source_id_, 12) ||
        memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12)) {
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
bool azcs_snapshot_content_witness(const azcs_snapshot *self, uint64_t expected_revision, struct azcuepersist_witness *out, bool allow_absent) {
    if (!out) return false;
    *out = (struct azcuepersist_witness) {0};
    if (!self->revision_ || expected_revision != self->revision_ || (!allow_absent && (!self->memory_set_ || !self->hot_set_)) ||
        memcmp(((const void *) (self->pack_ + 0x80)), self->track_id_, 12)) return false;
    if (word(self->pack_ + 0xe8) != self->memory_set_ || word(self->pack_ + 0xe0) != self->hot_set_ ||
        (self->memory_set_ && word(self->memory_set_ + 0x28) != self->content_) ||
        (self->hot_set_ && word(self->hot_set_ + 0x28) != self->hot_content_)) return false;
    const uintptr_t begin = self->content_ ? word(self->content_ + 0x28) : 0, end = self->content_ ? word(self->content_ + 0x30) : 0;
    const uintptr_t hot_begin = self->hot_content_ ? word(self->hot_content_ + 0x28) : 0, hot_end = self->hot_content_ ? word(self->hot_content_ + 0x30) : 0;
    if (end < begin || (end - begin) % 0x80 || (end - begin) / 0x80 != self->memory_count_ ||
        self->memory_count_ > 64 || (end != begin && !begin) ||
        hot_end < hot_begin || (hot_end - hot_begin) % 0x80 || (hot_end - hot_begin) / 0x80 != self->count_ - self->memory_count_ ||
        self->count_ - self->memory_count_ > 8 || (hot_end != hot_begin && !hot_begin)) return false;
    const uint64_t count = self->memory_count_;
    uint64_t current = hash(1469598103934665603ULL, self->source_id_, 12);
    current = hash(current, &count, sizeof count);
    current = hash(current, ((const void *) (begin)), self->memory_count_ * 0x80);
    current = hash(current, ((const void *) (hot_begin)), (self->count_ - self->memory_count_) * 0x80);
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
bool azcs_snapshot_creation_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, unsigned kind, const azcs_property *expected, uint32_t rate, const char *comment, uint32_t rgb) {
    struct azcuepersist_witness fresh = {0};
    CueVector old_memory, old_hot, memory, hot;
    if (!rate || kind > 8 || !comment || rgb > 0xffffffu || !azcs_property_valid(expected) || !before->revision || !before->source_info ||
        memcmp(before->source_id, self->source_id_, 12) ||
        !azcs_snapshot_content_witness(self, self->revision_, &fresh, true) || !cue_vector(before->memory_content, 10, &(old_memory)) ||
        !cue_vector(before->hot_content, 8, &(old_hot)) || !cue_vector(self->content_, 10, &(memory)) || !cue_vector(self->hot_content_, 8, &(hot))) return false;
    const uint64_t count = old_memory.count;
    uint64_t baseline = hash(1469598103934665603ULL, before->source_id, 12);
    baseline = hash(baseline, &count, sizeof count);
    baseline = hash(baseline, ((const void *) (old_memory.begin)), old_memory.count * 0x80);
    baseline = hash(baseline, ((const void *) (old_hot.begin)), old_hot.count * 0x80);
    if (baseline != before->fingerprint) return false;
    azcs_owned_cue wanted __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    wanted.native = self->p_.serialize_property(kind, &expected->native, kind != 0);
    wanted.destroy_ = self->p_.destroy_cue;


    return unchanged(old_memory, memory, !kind, kind, rate, &(wanted), comment, rgb) && unchanged(old_hot, hot, kind != 0, kind, rate, &(wanted), comment, rgb);
}
bool azcs_snapshot_auto_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, uint32_t memory_entry_id, bool desired, uint32_t source_rate) {
    struct azcuepersist_witness fresh = {0};
    if (!before->revision || !before->source_info || !before->memory_set || !before->hot_set ||
        !memory_entry_id || !source_rate || !azcs_snapshot_content_witness(self, self->revision_, &fresh, false) || memcmp(before->source_id, self->source_id_, 12)) return false;
    CueVector memory, hot;
    if (!cue_vector(before->memory_content, 64, &(memory)) || !cue_vector(before->hot_content, 8, &(hot)) ||
        memory.count != self->memory_count_ || hot.count != self->count_ - self->memory_count_ ||
        memory.count + hot.count > 64 || memory_entry_id > memory.count) return false;
    const uint64_t memory_size = memory.count;
    uint64_t baseline = hash(1469598103934665603ULL, before->source_id, 12);
    baseline = hash(baseline, &memory_size, sizeof memory_size);
    baseline = hash(baseline, ((const void *) (memory.begin)), memory.count * 0x80);
    baseline = hash(baseline, ((const void *) (hot.begin)), hot.count * 0x80);
    if (baseline != before->fingerprint) return false;
    const uintptr_t current_memory = self->content_ ? word(self->content_ + 0x28) : 0;
    const uintptr_t current_hot = self->hot_content_ ? word(self->hot_content_ + 0x28) : 0;
    for (size_t i = 0; i < memory.count; ++i)
        if (!same_cue(memory.begin + i * 0x80, current_memory + i * 0x80, i + 1 == memory_entry_id ? CueDelta_Auto : CueDelta_None, desired, source_rate, NULL, 0)) return false;
    for (size_t i = 0; i < hot.count; ++i)
        if (!same_cue(hot.begin + i * 0x80, current_hot + i * 0x80, CueDelta_None, desired, source_rate, NULL, 0)) return false;
    return true;
}
bool azcs_snapshot_metadata_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, unsigned native_kind, uint32_t entry_id, const char *comment, uint32_t rgb, uint32_t source_rate) {
    struct azcuepersist_witness fresh = {0};
    if (!before->revision || !before->source_info || !before->memory_set || !before->hot_set || !entry_id ||
        native_kind > 8 || (native_kind && native_kind != entry_id) || rgb > 0xffffffu || !source_rate ||
        !azcs_snapshot_content_witness(self, self->revision_, &fresh, false) || memcmp(before->source_id, self->source_id_, 12)) return false;
    CueVector memory, hot;
    if (!cue_vector(before->memory_content, 64, &(memory)) || !cue_vector(before->hot_content, 8, &(hot)) ||
        memory.count != self->memory_count_ || hot.count != self->count_ - self->memory_count_ ||
        memory.count + hot.count > 64 || (!native_kind && entry_id > memory.count)) return false;
    const uint64_t memory_size = memory.count;
    uint64_t baseline = hash(1469598103934665603ULL, before->source_id, 12);
    baseline = hash(baseline, &memory_size, sizeof memory_size);
    baseline = hash(baseline, ((const void *) (memory.begin)), memory.count * 0x80);
    baseline = hash(baseline, ((const void *) (hot.begin)), hot.count * 0x80);
    if (baseline != before->fingerprint) return false;
    const CueDelta edit = comment ? CueDelta_Comment : CueDelta_Color;
    unsigned matches = 0;
    const uintptr_t current_memory = self->content_ ? word(self->content_ + 0x28) : 0;
    const uintptr_t current_hot = self->hot_content_ ? word(self->hot_content_ + 0x28) : 0;
    for (size_t i = 0; i < memory.count; ++i) {
        bool selected = !native_kind && i + 1 == entry_id;
        if (selected) ++matches;
        if (!same_cue(memory.begin + i * 0x80, current_memory + i * 0x80, selected ? edit : CueDelta_None, false, source_rate, comment, rgb)) return false;
    }
    for (size_t i = 0; i < hot.count; ++i) {
        bool selected = native_kind && read(unsigned char, hot.begin + i * 0x80) == native_kind;
        if (selected) ++matches;
        if (!same_cue(hot.begin + i * 0x80, current_hot + i * 0x80, selected ? edit : CueDelta_None, false, source_rate, comment, rgb)) return false;
    }
    return matches == 1;
}


bool azcs_property_valid(const azcs_property *self) { return self->destroy_ != NULL; }
size_t azcs_snapshot_count(const azcs_snapshot *self) { return self->count_; }
size_t azcs_snapshot_memory_count(const azcs_snapshot *self) { return self->memory_count_; }
uint64_t azcs_snapshot_revision(const azcs_snapshot *self) { return self->revision_; }
const unsigned char *azcs_snapshot_source_id(const azcs_snapshot *self) { return self->source_id_; }
const unsigned char *azcs_snapshot_track_id(const azcs_snapshot *self) { return self->track_id_; }
