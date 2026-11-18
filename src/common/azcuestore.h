/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCUESTORE_H
#define AZCUESTORE_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
struct azcuepersist_witness;
/* Native owner-thread API. Keep the manager's owner gate throughout a snapshot's
 * lifetime. These native objects must never cross the plugin bridge. Owned
 * records start zeroed and must be reset exactly once; do not copy them. */
typedef struct {
    uintptr_t value;
} azcs_native_ref;
typedef struct {
    _Alignas(8) unsigned char bytes[0x80];
} azcs_native_cue;
typedef struct {
    _Alignas(8) unsigned char bytes[0xf8];
} azcs_native_property;
typedef struct {
    void *data;
} azcs_native_string;
typedef struct {
    void (*copy_source)(uintptr_t *, const uintptr_t *);
    void (*copy_content)(uintptr_t *, const uintptr_t *);
    void (*release_ref)(uintptr_t *);
    const void *(*position)(const void *);
    azcs_native_cue (*get_cue)(const uintptr_t *, size_t);
    void (*destroy_cue)(void *);
    azcs_native_property (*convert)(const uintptr_t *, const void *, const void *, const void *);
    void (*destroy_property)(void *);
    bool (*has_cue)(const void *, int);
    const void *(*get_current_cue)(const void *, int);
    void (*copy_property)(void *, const void *);
    azcs_native_cue (*serialize_property)(unsigned, const void *, bool);
    /* Native functions return their one-word counted value through hidden X8.
     * Invoke through az_native_result_word, never a C small-aggregate return. */
    void *where_from;
    void *string_allocate;
    void (*string_destroy)(void *);
    void (*color_construct)(void *, unsigned, unsigned, unsigned);
} azcs_profile;
azcs_profile azcs_native_profile(int build, uintptr_t bias);
typedef struct {
    uint64_t revision, load_generation, fingerprint;
    uintptr_t content, hot_content;
    unsigned char track[16];
} azcs_revision_state;
typedef struct {
    azcs_native_cue native;
    void (*destroy_)(void *);
} azcs_owned_cue;
typedef struct {
    azcs_native_property native;
    void (*destroy_)(void *);
    void (*copy_)(void *, const void *);
} azcs_property;
typedef struct {
    azcs_profile p_;
    uintptr_t player_, source_, where_from_, memory_set_, hot_set_, content_, hot_content_, pack_;
    size_t count_, memory_count_;
    uint64_t revision_, fingerprint_;
    unsigned char source_id_[12], track_id_[16];
} azcs_snapshot;
void azcs_owned_cue_reset(azcs_owned_cue *);
void azcs_property_reset(azcs_property *);
bool azcs_property_valid(const azcs_property *);
bool azcs_property_copy_to(const azcs_property *, azcs_property *);
/* Null expected_source captures the owned PlayerState identity. The caller
 * must still fence its native load generation before and after this call. */
bool azcs_snapshot_open(azcs_snapshot *, const azcs_profile *, void *manager,
                        uint64_t load_generation, const unsigned char expected_source[12], azcs_revision_state *);
void azcs_snapshot_reset(azcs_snapshot *);
size_t azcs_snapshot_count(const azcs_snapshot *);
size_t azcs_snapshot_memory_count(const azcs_snapshot *);
uint64_t azcs_snapshot_revision(const azcs_snapshot *);
const unsigned char *azcs_snapshot_source_id(const azcs_snapshot *);
const unsigned char *azcs_snapshot_track_id(const azcs_snapshot *);
bool azcs_snapshot_row(const azcs_snapshot *, size_t ordinal, azcs_owned_cue *);
bool azcs_snapshot_property(const azcs_snapshot *, uint32_t entry_id, uint64_t expected_revision, azcs_property *);
bool azcs_snapshot_current_property(const azcs_snapshot *, unsigned kind, azcs_property *);
uint32_t azcs_snapshot_source_rate(const azcs_snapshot *);
/* Build empty seek variants, then let the native converter qualify the source
 * and construct any compressed-format seek accessor. */
bool azcs_snapshot_timed_property(const azcs_snapshot *, unsigned kind, double in_s, double out_s,
                                  bool loop, uint32_t rgb, const char *comment, azcs_property *);
/* Exact native persisted timestamp projection; the original stays intact. */
bool azcs_snapshot_canonical_property(const azcs_snapshot *, unsigned kind, const azcs_property *, azcs_property *);
/* Borrowed roots pinned by the snapshot. A submitter takes its own counted refs. */
bool azcs_snapshot_content_witness(const azcs_snapshot *, uint64_t expected_revision,
                                   struct azcuepersist_witness *, bool allow_absent);
/* Persistence owns all five baseline refs. Unknown seek representations fail
 * closed; full stored values are compared, never truncated public strings. */
bool azcs_snapshot_creation_only_changed(const azcs_snapshot *, const struct azcuepersist_witness *, unsigned native_kind,
                                         const azcs_property *expected, uint32_t source_rate, const char *comment, uint32_t rgb);
bool azcs_snapshot_auto_only_changed(const azcs_snapshot *, const struct azcuepersist_witness *, uint32_t memory_entry_id,
                                     bool desired, uint32_t source_rate);
/* Non-null comment selects a text edit; null selects an RGB edit. */
bool azcs_snapshot_metadata_only_changed(const azcs_snapshot *, const struct azcuepersist_witness *, unsigned native_kind,
                                         uint32_t entry_id, const char *comment, uint32_t rgb, uint32_t source_rate);
#endif
