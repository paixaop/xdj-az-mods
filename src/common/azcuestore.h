/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUESTORE_H
#define AZCUESTORE_H
#include <cstddef>
#include <cstdint>
struct azcuepersist_witness;
/* Native owner-thread API. Keep the manager's owner gate throughout a snapshot's
 * lifetime. These native objects must never cross the plugin bridge. */
namespace azcuestore {
struct NativePlayer {
    uintptr_t value = 0;
    ~NativePlayer() {}
};
struct NativeCue {
    alignas(8) unsigned char bytes[0x80];
};
struct NativeProperty {
    alignas(8) unsigned char bytes[0xf8];
};
struct NativeString {
    void *data = nullptr;
    ~NativeString() {}
};
struct Profile {
    void (*copy_source)(uintptr_t *, const uintptr_t *) = nullptr;
    void (*copy_content)(uintptr_t *, const uintptr_t *) = nullptr;
    void (*release_ref)(uintptr_t *) = nullptr;
    const void *(*position)(const void *) = nullptr;
    NativeCue (*get_cue)(const uintptr_t *, size_t) = nullptr;
    void (*destroy_cue)(void *) = nullptr;
    NativeProperty (*convert)(const uintptr_t *, const void *, const void *, const void *) = nullptr;
    void (*destroy_property)(void *) = nullptr;
    bool (*has_cue)(const void *, int) = nullptr;
    const void *(*get_current_cue)(const void *, int) = nullptr;
    void (*copy_property)(void *, const void *) = nullptr;
    NativeCue (*serialize_property)(unsigned, const void *, bool) = nullptr;
    /* Nontrivial 8-byte return storage enforces the native hidden-X8 ABI.
     * This result owns a counted WhereFrom ref, not a PlayerState object. */
    NativePlayer (*where_from)(const void *) = nullptr;
    NativeString (*string_allocate)(uint64_t) = nullptr;
    void (*string_destroy)(void *) = nullptr;
    void (*color_construct)(void *, unsigned, unsigned, unsigned) = nullptr;
};
Profile native_profile(int build, uintptr_t bias) noexcept;
struct RevisionState {
    uint64_t revision = 0, load_generation = 0, fingerprint = 0;
    uintptr_t content = 0, hot_content = 0;
    unsigned char track[16] = {};
};
class OwnedCue {
   public:
    NativeCue native = {};
    OwnedCue() = default;
    ~OwnedCue() { reset(); }
    OwnedCue(const OwnedCue &) = delete;
    OwnedCue &operator=(const OwnedCue &) = delete;
    void reset() noexcept;
   private:
    friend class Snapshot;
    void (*destroy_)(void *) = nullptr;
};
class Property {
   public:
    NativeProperty native = {};
    Property() = default;
    ~Property() { reset(); }
    Property(const Property &) = delete;
    Property &operator=(const Property &) = delete;
    void reset() noexcept;
    bool valid() const noexcept { return destroy_ != nullptr; }
    bool copy_to(Property &) const noexcept;
   private:
    friend class Snapshot;
    void (*destroy_)(void *) = nullptr;
    void (*copy_)(void *, const void *) = nullptr;
};
class Snapshot {
   public:
    Snapshot() = default;
    ~Snapshot() { reset(); }
    Snapshot(const Snapshot &) = delete;
    Snapshot &operator=(const Snapshot &) = delete;
    /* Null expected_source captures the owned PlayerState identity. The caller
     * must still fence its native load generation before and after this call. */
    bool open(const Profile &, void *manager, uint64_t load_generation,
              const unsigned char expected_source[12], RevisionState &) noexcept;
    void reset() noexcept;
    size_t count() const noexcept { return count_; }
    size_t memory_count() const noexcept { return memory_count_; }
    uint64_t revision() const noexcept { return revision_; }
    const unsigned char *source_id() const noexcept { return source_id_; }
    const unsigned char *track_id() const noexcept { return track_id_; }
    bool row(size_t ordinal, OwnedCue &) const noexcept;
    bool property(uint32_t entry_id, uint64_t expected_revision, Property &) const noexcept;
    bool current_property(unsigned kind, Property &) const noexcept;
    uint32_t source_rate() const noexcept;
    /* Build fresh empty seek variants, then let the native stored-Cue converter
     * qualify the source and construct any compressed-format seek accessor. */
    bool timed_property(unsigned kind, double in_s, double out_s, bool loop,
                        uint32_t rgb, const char *comment, Property &) const noexcept;
    bool creation_only_changed(const azcuepersist_witness &before, unsigned native_kind,
                               const Property &expected, uint32_t source_rate,
                               const char *comment = "", uint32_t rgb = 0) const noexcept;
    /* Exact native persisted timestamp projection; the original stays intact. */
    bool canonical_property(unsigned kind, const Property &, Property &) const noexcept;
    /* Internal borrowed roots/contents, pinned by this Snapshot. The submitter
     * must take its own checked counted refs before this Snapshot is destroyed. */
    bool content_witness(uint64_t expected_revision, azcuepersist_witness *, bool allow_absent = false) const noexcept;
    /* Persistence owns all five before refs through this call. Unknown seek
     * representations fail closed; no truncated public strings are compared. */
    bool auto_only_changed(const azcuepersist_witness &before, uint32_t memory_entry_id, bool desired, uint32_t source_rate) const noexcept;
    /* Full stored-value comparison without converting every seek property.
     * A non-null comment selects a comment edit; null selects an RGB edit. */
    bool metadata_only_changed(const azcuepersist_witness &before, unsigned native_kind, uint32_t entry_id,
                               const char *comment, uint32_t rgb, uint32_t source_rate) const noexcept;
   private:
    Profile p_ = {};
    uintptr_t player_ = 0, source_ = 0, where_from_ = 0, memory_set_ = 0, hot_set_ = 0, content_ = 0, hot_content_ = 0, pack_ = 0;
    size_t count_ = 0, memory_count_ = 0;
    uint64_t revision_ = 0, fingerprint_ = 0;
    unsigned char source_id_[12] = {}, track_id_[16] = {};
};
} // namespace azcuestore
#endif
