/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "source_import_analyzer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Profile for the exact 2.00 EP147 image. The library worker owns the qualified
 * database throughout this call. AnalyzeManager copies its arguments and owns
 * the subsequent analysis, repository reads, and fileset publication. */
#define ANALYZER_FIRMWARE_200 2 /* EPB_V200 */
#define ANALYZER_DATABASE_VTABLE ((uintptr_t) 0x02b9d368)
#define ANALYZER_DIGGER_VTABLE ((uintptr_t) 0x02b991d0)
#define ANALYZER_MANAGER_VTABLE ((uintptr_t) 0x02ab8970)
#define ANALYZER_REQUEST ((uintptr_t) 0x00d00f08)
#define ANALYZER_FOLDER_REREAD ((uintptr_t) 0x0122f860)
#define ANALYZER_GUI_KEY ((uintptr_t) 0x02106d18)
#define ANALYZER_MAPPED_LINK ((uintptr_t) 0x00a687c8)
#define ANALYZER_REQUESTER_CLASS ((uintptr_t) 0x00d02cf0)
#define ANALYZER_STRING_CTOR ((uintptr_t) 0x024a1180)
#define ANALYZER_STRING_DTOR ((uintptr_t) 0x02469aa0)
#define ANALYZER_FILE_CTOR ((uintptr_t) 0x024bd310)

struct analyzer_entry {
    uint32_t id, parent;
    uint8_t directory, depth, enumerated;
    const char *basename;
};

struct analyzer_track {
    uint32_t local_device;
    uint16_t media;
    uint8_t kind, reserved;
    uint32_t entry;
    uint32_t padding;
};

struct analyzer_requester {
    uint32_t owner_class, kind, value, padding;
};

_Static_assert(offsetof(struct analyzer_entry, basename) == 0x10, "FolderEntry basename ABI");
_Static_assert(sizeof(struct analyzer_entry) == 0x18, "FolderEntry ABI");
_Static_assert(offsetof(struct analyzer_track, padding) == AZSPI_NATIVE_TRACK_BYTES, "TrackID ABI");
_Static_assert(offsetof(struct analyzer_requester, padding) == AZSPI_NATIVE_REQUESTER_BYTES, "AnalyzeRequesterID ABI");

static _Atomic uint64_t analyzer_serial;

static int analyzer_component_ok(const char *name, size_t size) {
    if (!size || size > 255)
        return 0;
    return name[0] != '.' || (size > 1 && (name[1] != '.' || size > 2));
}

static int analyzer_path_components(const char *audio) {
    unsigned depth = 0;
    const char *part = audio + 1;
    for (const char *p = part;; ++p) {
        if (*p != '/' && *p)
            continue;
        if (!analyzer_component_ok(part, (size_t) (p - part)) || ++depth > 9)
            return 0;
        if (!*p)
            return 1;
        part = p + 1;
    }
}

static int analyzer_ascii_path(const char *audio) {
    if (!audio || audio[0] != '/' || strnlen(audio, AZ_SOURCE_PATH_BYTES) == AZ_SOURCE_PATH_BYTES)
        return 0;
    for (const unsigned char *p = (const unsigned char *) audio + 1; *p; ++p) {
        if (*p < ' ' || *p > '~' || *p == '\\')
            return 0;
    }
    return analyzer_path_components(audio);
}

static int analyzer_staged_path(const struct st_source *source, const char *audio,
                                char path[AZ_SOURCE_PATH_BYTES]) {
    return source && analyzer_ascii_path(audio) && st_source_path(source->mount, audio, path);
}

static uint32_t analyzer_path_hash(const char *audio) {
    /* On ASCII paths the UTF-16 code units are identical to these bytes. */
    uint32_t hash = 0;
    for (const unsigned char *p = (const unsigned char *) audio; *p; ++p)
        hash = *p * UINT32_C(0x93b6) + hash * UINT32_C(0x34f5501d);
    return hash % UINT32_C(0x30d43);
}

static unsigned analyzer_hash_bucket(uint32_t hash) {
    static const unsigned bit_indices[] = {0, 2, 6, 7, 9, 13, 16};
    unsigned bucket = 0;
    for (unsigned i = 0; i < sizeof(bit_indices) / sizeof(bit_indices[0]); ++i)
        bucket |= ((hash >> bit_indices[i]) & 1u) << i;
    return bucket;
}

int azspi_analyzer_locator(const char *audio, char out[AZ_SOURCE_PATH_BYTES]) {
    if (!out || !analyzer_ascii_path(audio))
        return AZ_INVALID;
    unsigned hash = analyzer_path_hash(audio);
    snprintf(out, AZ_SOURCE_PATH_BYTES, "/PIONEER/USBANLZ/P0%02X/%08X/ANLZ0000.DAT",
             analyzer_hash_bucket(hash), hash);
    return AZ_OK;
}

static uintptr_t analyzer_word(const void *object, size_t offset) {
    uintptr_t value;
    memcpy(&value, (const unsigned char *) object + offset, sizeof(value));
    return value;
}

static int analyzer_vtable(const void *object, uintptr_t vtable) {
    return object && analyzer_word(object, 0) == vtable;
}

static int analyzer_medium_matches(const void *database, uint16_t media) {
    uint16_t actual;
    memcpy(&actual, (const unsigned char *) database + 8, sizeof(actual));
    uint16_t kind = media & 31u;
    return actual == media && (kind == 2 || kind == 3);
}

static void *analyzer_owned_digger(void *database, uint16_t media, const char *root) {
    if (!analyzer_vtable(database, ANALYZER_DATABASE_VTABLE) || !analyzer_medium_matches(database, media))
        return NULL;
    void *digger = (void *) analyzer_word(database, 0x40);
    if (!analyzer_vtable(digger, ANALYZER_DIGGER_VTABLE))
        return NULL;
    const char *native_root = (const char *) analyzer_word(digger, 0x28);
    return native_root && !strcmp(native_root, root) ? digger : NULL;
}

static int analyzer_same_entry(const struct analyzer_entry *entry, uint32_t parent,
                               const char *basename, size_t length) {
    return entry && entry->parent == parent && entry->basename &&
           !strncmp(entry->basename, basename, length) && !entry->basename[length];
}

static int analyzer_vector_count(uintptr_t begin, uintptr_t end, size_t *count) {
    if (!begin || end < begin || (end - begin) % sizeof(void *))
        return 0;
    *count = (end - begin) / sizeof(void *);
    return *count <= 100000;
}

static const struct analyzer_entry *analyzer_find_entry(void *digger, uint32_t parent,
                                                        const char *basename, size_t length) {
    uintptr_t begin = analyzer_word(digger, 0x30), end = analyzer_word(digger, 0x38);
    size_t count = 0;
    if (!analyzer_vector_count(begin, end, &count))
        return NULL;
    const struct analyzer_entry *const *entries = (const void *) begin;
    for (size_t i = 0; i < count; ++i) {
        if (analyzer_same_entry(entries[i], parent, basename, length))
            return entries[i];
    }
    return NULL;
}

static uint32_t analyzer_walk_folders(void *digger, const char *audio, int (*reread)(void *, uint32_t)) {
    uint32_t parent = 0;
    for (const char *part = audio + 1;;) {
        if (reread(digger, parent))
            return 0;
        size_t length = strcspn(part, "/");
        const struct analyzer_entry *entry = analyzer_find_entry(digger, parent, part, length);
        if (!entry || entry->directory != (part[length] == '/'))
            return 0;
        if (!part[length])
            return entry->id;
        parent = entry->id;
        part += length + 1;
    }
}

static uint32_t analyzer_folder_track(void *digger, const char *audio) {
    int (*reread)(void *, uint32_t) = (void *) ANALYZER_FOLDER_REREAD;
    return analyzer_walk_folders(digger, audio, reread);
}

static void *analyzer_manager(void) {
    uintptr_t (*key)(const char *) = (void *) ANALYZER_GUI_KEY;
    unsigned (*link)(uintptr_t *, unsigned) = (void *) ANALYZER_MAPPED_LINK;
    uintptr_t mapped[2] = {0, key("GUI->IAnalyzeManager")};
    if (!link(mapped, 0))
        return NULL;
    void *owner = (void *) mapped[0];
    if (!analyzer_vtable(owner, ANALYZER_MANAGER_VTABLE))
        return NULL;
    return analyzer_word((void *) ANALYZER_MANAGER_VTABLE, 0x10) == ANALYZER_REQUEST ? owner : NULL;
}

static int analyzer_request_owner(struct analyzer_requester *out) {
    uint32_t (*requester_class)(void) = (void *) ANALYZER_REQUESTER_CLASS;
    uint64_t serial = atomic_fetch_add_explicit(&analyzer_serial, 1, memory_order_relaxed);
    if (serial >= 0x3fffffffu)
        return 0;
    /* Kind 1 belongs to Decks 1–4. The native Preview kind 2 uses payload 0.
     * The analyzer compares the full kind and payload for requester ownership. */
    *out = (struct analyzer_requester) {requester_class(), 2, 0x40000000u | (uint32_t) serial, 0};
    return 1;
}

static int analyzer_arm(const struct analyzer_track *track, const struct analyzer_requester *requester,
                        azspi_analyzer_arm_fn arm, void *context) {
    return arm ? arm((const uint8_t *) track, (const uint8_t *) requester, context) : AZ_OK;
}

static int analyzer_request(void *manager, const struct analyzer_track *track, const char *path,
                            azspi_analyzer_arm_fn arm, void *context) {
    void (*make_string)(uintptr_t *, const char *) = (void *) ANALYZER_STRING_CTOR;
    void (*make_file)(uintptr_t *, const uintptr_t *) = (void *) ANALYZER_FILE_CTOR;
    void (*release_string)(uintptr_t *) = (void *) ANALYZER_STRING_DTOR;
    void (*request)(void *, const struct analyzer_track *, const uintptr_t *, const struct analyzer_requester *) =
        (void *) ANALYZER_REQUEST;
    struct analyzer_requester requester;
    if (!analyzer_request_owner(&requester))
        return AZ_LIMIT;
    int status = analyzer_arm(track, &requester, arm, context);
    if (status != AZ_OK)
        return status;
    uintptr_t file, string;
    make_string(&string, path);
    make_file(&file, &string);
    release_string(&string);
    request(manager, track, &file, &requester);
    /* juce::File owns one juce::String at offset zero. */
    release_string(&file);
    return AZ_PENDING;
}

int azspi_analyzer_submit_watched(int firmware, void *database, uint16_t media,
                                  const struct st_source *source, const char *audio,
                                  uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES], azspi_analyzer_arm_fn arm, void *context) {
    if (firmware != ANALYZER_FIRMWARE_200)
        return AZ_UNSUPPORTED;
    char path[AZ_SOURCE_PATH_BYTES];
    if (!raw_id || !analyzer_staged_path(source, audio, path))
        return AZ_INVALID;
    void *digger = analyzer_owned_digger(database, media, source->mount);
    if (!digger)
        return AZ_STALE;
    void *manager = analyzer_manager();
    if (!manager)
        return AZ_BUSY;
    uint32_t entry = analyzer_folder_track(digger, audio);
    if (!entry)
        return AZ_NOT_FOUND;
    /* For the qualified local USB worker, the native local device identity is
     * 1 and type 2 addresses its native Folder namespace. Padding permits the
     * core's existing request observer to read its aligned two-word snapshot. */
    const struct analyzer_track track = {1, media, 2, 0, entry, 0};
    int status = analyzer_request(manager, &track, path, arm, context);
    if (status == AZ_PENDING)
        memcpy(raw_id, &track, AZSPI_NATIVE_TRACK_BYTES);
    return status;
}

int azspi_analyzer_submit(int firmware, void *database, uint16_t media,
                          const struct st_source *source, const char *audio,
                          uint8_t raw_id[AZSPI_NATIVE_TRACK_BYTES]) {
    return azspi_analyzer_submit_watched(firmware, database, media, source, audio, raw_id, NULL, NULL);
}
