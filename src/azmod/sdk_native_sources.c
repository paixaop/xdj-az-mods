/* SPDX-License-Identifier: GPL-3.0-only */
/* Owned native source capture, GNU libstdc++ ABI only. See CORE-SDK-NATIVE-SOURCES.md.
 * Evidence RVAs are comparisons against checked API/vtable results, never independent
 * entry addresses. No native DB, FD or object is dereferenced after the owned callback. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../common/azstatus_source.h"
/* Source-stage observers are optional in the standalone owner/ABI fixtures. */
#pragma weak azjs_server
#pragma weak azjs_ticket
#pragma weak azjs_ticket_return
#pragma weak azjs_reception
#include "../common/azsources.h"
#include "../common/azcachebirth.h"
#include "../common/azcachepin.h"
#include "../common/azcacheentry.h"
#include "../common/azcachemessage.h"
#include "../common/azcacheselection.h"
#include "../common/azcacherow.h"
#include "../common/azcachequery.h"
#include "../common/azcachereplynative.h"
#include "../common/azcachereplycode.h"
#include "../common/azsha256.h"
/* Ownership-only standalone fixtures omit the machine entry table. Production
 * links its real process-lifetime admission flag from sdk_cache_entry.c. */
int azsdk_cache_capture_ready(void) __attribute__((weak));
#include "../common/azconnectionhooks.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


#include "../common/aznative_sync.h"
#include <limits.h>

#include <time.h>

#if defined(__linux__)

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


typedef struct az_native_owner StrongOwner, WeakOwner;
typedef struct azcb_source_borrow SourceBorrow;
typedef struct azcb_forward Forward;
typedef struct azcb_birth_borrow BirthBorrow;
typedef struct azcb_factory_borrow FactoryBorrow;
typedef struct azcb_write_borrow WriteBorrow;
typedef struct azcb_query_borrow QueryBorrow;
typedef struct azcb_copy_row CopyRowScalar;
typedef struct azcb_observation Observation;
typedef struct azcq_evidence Evidence;
typedef struct azcm_envelope Envelope;
typedef struct azsel_invocation SelectionInvocation;
typedef struct azsel_mutation SelectionMutation;
typedef struct azsel_state SelectionState;
typedef struct azsel_borrow SelectionBorrow;
typedef struct azcp_pins BirthPins;
typedef struct az_sql_profile NativeSqlProfile;
typedef struct az_connection_state ConnectionState;
typedef struct az_connection_receipt ConnectionReceipt;
typedef struct azcr_callback_receipt CallbackReceipt;
typedef struct azcr_scope ReplyScope;
typedef struct azcr_request Request;
typedef struct azcr_promotion Promotion;


enum {
    kRead = 4,
    kExecute = 1,
    kSlots = 32
};
static const unsigned kKnownPolicy = AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER;
_Static_assert(sizeof(uintptr_t) == 8, "native source ABI requires 64 bits");
typedef struct Range Range;
struct Range {
    uintptr_t begin, end;
    unsigned flags;
};
typedef struct Image Image;
struct Image {
    Range ranges[32];
    unsigned count;
    uintptr_t base;
};
static bool image_contains(const Image *ctx, uintptr_t p, size_t n, unsigned flags);
static bool image_contains(const Image *ctx, uintptr_t p, size_t n, unsigned flags) {
    (void) ctx;

    if (n > UINTPTR_MAX - p) return false;
    for (unsigned i = 0; i < ctx->count; ++i)
        if ((ctx->ranges[i].flags & flags) == flags && p >= ctx->ranges[i].begin &&
            p + n <= ctx->ranges[i].end) return true;
    return false;
}

/* Exact public SQLite ABI prefix. The Unix file's descriptor offset is a PRIVATE verified
 * layout, enabled only after matching the actual loaded SQLCipher text and method RVAs. */
typedef struct NativeFile NativeFile;
typedef struct NativeIO NativeIO;
struct NativeIO {
    int version;
    int (*close)(NativeFile *);
    int (*read)(NativeFile *, void *, int, int64_t);
    int (*write)(NativeFile *, const void *, int, int64_t);
    int (*truncate)(NativeFile *, int64_t);
    int (*sync)(NativeFile *, int);
    int (*size)(NativeFile *, int64_t *);
    int (*lock)(NativeFile *, int);
    int (*unlock)(NativeFile *, int);
    int (*reserved)(NativeFile *, int *);
    int (*control)(NativeFile *, int, void *);
    int (*sector)(NativeFile *);
    int (*characteristics)(NativeFile *);
};
typedef struct NativeFile NativeFile;
struct NativeFile {
    const NativeIO *methods;
};
typedef struct NativeVFS NativeVFS;
struct NativeVFS {
    int version, os_size, max_path;
    void *next;
    const char *name;
    void *app;
    int (*open)(NativeVFS *, const char *, NativeFile *, int, int *);
};
_Static_assert(offsetof(NativeIO, size) == 0x30 && offsetof(NativeIO, control) == 0x50 &&
                   offsetof(NativeVFS, name) == 0x18 && offsetof(NativeVFS, open) == 0x28,
               "SQLite public 64-bit ABI");
typedef struct SqlProfile SqlProfile;
struct SqlProfile {
    Image image;
    az_sql_file_control file_control;
    uintptr_t io_rvas[3];
    uintptr_t open_rva, read_rva, size_rva, control_rva;
    bool ready;
    /* Local physical-capture view, also constructible by explicit synthetic
     * fixtures. Only the sealed shared proof prepares the production view.
     * That proof retains the module; neither view owns a native connection. */
    const NativeSqlProfile *proof;
};
typedef struct Build Build;
struct Build {
    size_t dispatcher;
    uintptr_t affinity_rva;
};
static const Build builds[] = {{0, 0}, {0x1c0, 0x78a2b0}, {0x1c8, 0x7cc2f0}};
typedef struct Entry Entry;
struct Entry {
    WeakOwner mount;
    WeakOwner database;
    uintptr_t mount_address, database_address, connection;
    /* Immutable part of this source key. Zero is an incomplete owner
     * diagnostic, never an observed SQLite connection incarnation. */
    uint64_t connection_generation;
    uint64_t status_connection_generation;
    struct azsdk_source_sample sample;
    bool retired;
};
typedef struct Binding Binding;
struct Binding {
    uintptr_t server;
    uint64_t epoch;
};
typedef struct CacheBinding CacheBinding;
struct CacheBinding {
    WeakOwner mount;
    WeakOwner database;
    WeakOwner collector;
    WeakOwner cache;
    uint64_t source_epoch, birth_epoch, generation;
    unsigned factory_kind;
};
typedef struct SourceState SourceState;
struct SourceState {
    Image main_image;
    pthread_once_t image_once, sql_once;
    _Atomic(int) enabled, bound_build;
    _Atomic(unsigned) capture_policy;
    _Atomic(bool) physical_prepared;
    pthread_mutex_t registry_mutex;
    uint64_t next_epoch, next_revision;
    /* Never erase these tombstones. Native MediaID discriminators wrap after 255 mounts. */
    unsigned char seen_media[65536 / 8], ambiguous_media[65536 / 8];
    Entry entries[kSlots];
    Binding bindings[kSlots];
    /* Cache provenance shares the existing source registry and worker. It is
     * not a source lookup by media/content/raw address or a native task queue.
     * Weak controls are reclaimed only under the registry mutex on that worker.
     * Mutator entries touch only the atomics and never release weak owners. */
    CacheBinding cache_bindings[kSlots];
    uint64_t next_cache_birth;
    unsigned cache_limit;
    _Atomic(uint64_t) cache_generation;
    _Atomic(unsigned) cache_active, cache_depth_limit;
    _Atomic(bool) cache_saturated;
    SelectionState cache_selection;
    ConnectionState connection_epochs;
    SqlProfile sql_profile; /* Prepared once under the registry mutex before publication. */
};
static SourceState sources = {.image_once = PTHREAD_ONCE_INIT, .sql_once = PTHREAD_ONCE_INIT, .registry_mutex = PTHREAD_MUTEX_INITIALIZER, .cache_limit = 32, .cache_generation = 1, .cache_depth_limit = 32, .cache_selection = AZSEL_STATE_INIT, .connection_epochs = AZ_CONNECTION_STATE_INIT};
#define source_state() (sources)

static bool canonical(uintptr_t p) {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
#define load(T, p) ({ T value; memcpy(&value,(const void *)(p),sizeof value);value; })
static uint64_t now_ns() {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
        t.tv_nsec >= 1000000000L) return 0;
    __auto_type s = ((uint64_t) (t.tv_sec));
    if (s > (UINT64_MAX - ((unsigned long) (t.tv_nsec))) / UINT64_C(1000000000)) return 0;
    return s * UINT64_C(1000000000) + ((unsigned long) (t.tv_nsec));
}
static bool bit(const unsigned char *set, unsigned media) {
    return (set[media / 8] & (1u << (media % 8))) != 0;
}
static void set_bit(unsigned char *set, unsigned media) {
    set[media / 8] |= ((unsigned char) (1u << (media % 8)));
}
static bool output_ok(void *out, size_t size, bool optional) {
    return out ? size >= 8 : optional && size == 0;
}
static void copy_output(void *out, size_t size, const struct azsdk_source_sample *sample) {
    if (out) memcpy(out, sample, size < sizeof(*sample) ? size : sizeof(*sample));
}
#if defined(__linux__)
typedef struct ImageSearch ImageSearch;
struct ImageSearch {
    Image *image;
    bool main;
};
static bool append_image_segment(Image *image, uintptr_t base, const ElfW(Phdr) * p) {
    if (image->count == 32 || p->p_vaddr > UINTPTR_MAX - base || p->p_memsz > UINTPTR_MAX - base - p->p_vaddr) {
        image->count = 0;
        return false;
    }
    image->ranges[image->count++] = (Range) {base + p->p_vaddr, base + p->p_vaddr + p->p_memsz, p->p_flags};
    return true;
}
static int find_image(struct dl_phdr_info *info, size_t unused, void *context) {
    (void) unused;
    __auto_type s = &(*((ImageSearch *) (context)));
    if (s->main ? (info->dlpi_name && *info->dlpi_name) : info->dlpi_addr != s->image->base) return 0;
    s->image->base = info->dlpi_addr;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        __auto_type p = &(info->dlpi_phdr[i]);
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_R)) continue;
        if (!append_image_segment(s->image, info->dlpi_addr, p)) return 1;
    }
    return 1;
}
#endif
static void prepare_image() {
#if defined(__linux__)
    ImageSearch s = {&source_state().main_image, true};
    dl_iterate_phdr(find_image, &s);
#endif
}
/* These range checks constrain metadata, never acquire object liveness. The caller owns
 * the receiver/chain. Heap pointers cannot be made safe by a canonical-address check. */
static bool image_string_equal(const Image *image, uintptr_t address, const char *expected) {
    for (size_t i = 0;; ++i) {
        if (i > UINTPTR_MAX - address || !image_contains(image, address + i, 1, kRead)) return false;
        /* An ELF range bounds the module, not this string object. Scalar volatile
         * reads prevent widening past an earlier mismatch or the terminating NUL. */
        char actual = *((const volatile char *) (address + i));
        if (actual != expected[i]) return false;
        if (!actual) return true;
    }
}
static uintptr_t checked_type_name(uintptr_t table) {
    __auto_type type = load(uintptr_t, table - 8);
    if (!canonical(type) || !image_contains(&source_state().main_image, type, 16, kRead)) return 0;
    __auto_type text = load(uintptr_t, type + 8);
    if (!image_contains(&source_state().main_image, text, 1, kRead)) return 0;
    if (*((const char *) (text)) == '*') ++text;
    return text;
}
static uintptr_t checked_vtable(uintptr_t object, const char *name, unsigned slots) {
    if (!canonical(object)) return 0;
    __auto_type table = load(uintptr_t, object);
    if (!canonical(table) || table < 16 ||
        !image_contains(&source_state().main_image, table - 16, (slots + 2u) * 8u, kRead) ||
        load(uintptr_t, table - 16)) return 0;
    uintptr_t text = checked_type_name(table);
    if (!text) return 0;
    return image_string_equal(&(source_state().main_image), text, name) ? table : 0;
}
static bool worker_scope(uintptr_t server, const Build *build) {
    __auto_type table = checked_vtable(server, "N13music_library18MusicLibraryServerE", 11);
    if (!table || load(unsigned char, server + 0xd2) != 1 ||
        !canonical(load(uintptr_t, server + 0x28))) return false;
    __auto_type entry = load(uintptr_t, table + 5 * 8);
    if (!image_contains(&source_state().main_image, entry, 4, kRead | kExecute) || entry < source_state().main_image.base ||
        entry - source_state().main_image.base != build->affinity_rva) return false;
    /* Only the checked native virtual result is invoked, never a computed address. */
    typedef bool (*NeedsAsync)(void *);
    return !((NeedsAsync) (entry))(((void *) (server)));
}
typedef struct OwnedChain OwnedChain;
struct OwnedChain {
    const StrongOwner *mount;
    const StrongOwner *database;
    uintptr_t mount_address, database_address, connection;
    uint16_t media;
};
static uintptr_t owned_updater(uintptr_t server, const Build *build) {
    uintptr_t dispatcher = load(uintptr_t, server + build->dispatcher);
    if (!canonical(dispatcher)) return 0;
    uintptr_t handler = load(uintptr_t, dispatcher + 0x20);
    if (!canonical(handler)) return 0;
    uintptr_t updater = load(uintptr_t, handler + 0x30);
    return checked_vtable(updater, "N13music_library18InformationUpdaterE", 1) ? updater : 0;
}
static bool owned_reference_live(const StrongOwner *owner, uintptr_t object) {
    return (uintptr_t) owner->object == object && owner->control &&
           __atomic_load_n(&owner->control->strong, __ATOMIC_RELAXED) > 0;
}
static bool owned_chain_tuple(uintptr_t updater, uintptr_t mount, uint16_t media, uintptr_t connection) {
    return media && load(uint16_t, mount + 8) == media && canonical(connection) &&
           canonical(load(uintptr_t, updater + 0x10)) && canonical(load(uintptr_t, mount + 0x38));
}
static bool owned_chain(uintptr_t server, const Build *build, OwnedChain *out) {
    uintptr_t updater = owned_updater(server, build);
    if (!updater) return false;
    __auto_type mount = load(uintptr_t, updater + 8);
    if (!checked_vtable(mount, "N13music_library16SqliteMountPointE", 1)) return false;
    __auto_type database = load(uintptr_t, mount + 0x30);
    if (!checked_vtable(database, "N13music_library14SqliteDataBaseE", 1)) return false;
    __auto_type media = load(uint16_t, database + 8);
    __auto_type connection = load(uintptr_t, database + 0x38);
    if (!owned_chain_tuple(updater, mount, media, connection)) return false;
    /* Borrow the REAL source references in the verified serialized native worker scope.
     * Never memcpy ownership words into a shared/weak object or fabricate an owner. */
    __auto_type m = &(*((const StrongOwner *) (updater + 8)));
    __auto_type d = &(*((const StrongOwner *) (mount + 0x30)));
    if (!owned_reference_live(m, mount) || !owned_reference_live(d, database)) return false;
    out->mount = m;
    out->database = d;
    out->mount_address = mount;
    out->database_address = database;
    out->connection = connection;
    out->media = media;
    return true;
}

static bool module_function(const SqlProfile *p, uintptr_t f, uintptr_t rva) {
    return f >= p->image.base && f - p->image.base == rva && image_contains(&p->image, f, 4, kRead | kExecute);
}
static void prepare_sqlcipher() {
    /* The same immutable startup proof will serve lifecycle installation.
     * It is minted before the first redirect and reused afterward, so physical
     * preparation never rehashes text modified by our own accepted hooks. */
    __auto_type proof = az_connection_prepare_profile();
    if (!proof || !az_sql_profile_range_count(proof) || az_sql_profile_range_count(proof) > 32 || !az_sql_profile_file_control(proof)) return;
    SqlProfile p = {0};
    p.proof = proof;
    p.image.base = az_sql_profile_base(proof);
    p.image.count = az_sql_profile_range_count(proof);
    for (unsigned i = 0; i < p.image.count; ++i) {
        __auto_type range = az_sql_profile_range(proof, i);
        if (!range) return;
        p.image.ranges[i] = (Range) {range->begin, range->end, range->flags};
    }
    p.file_control = az_sql_profile_file_control(proof);
    for (unsigned i = 0; i < 3; ++i) p.io_rvas[i] = az_sql_profile_io_rva(proof, i);
    p.open_rva = az_sql_profile_vfs_open_rva(proof);
    p.read_rva = az_sql_profile_read_rva(proof);
    p.size_rva = az_sql_profile_size_rva(proof);
    p.control_rva = az_sql_profile_control_rva(proof);
    p.ready = true;
    source_state().sql_profile = p;
    atomic_store_explicit(&(source_state().physical_prepared), true, memory_order_release);
}

#if defined(__linux__)
typedef struct MetadataOps MetadataOps;
struct MetadataOps {
    int (*stat)(int, struct stat *);
    int (*open)(const char *, int);
    ssize_t (*read)(int, void *, size_t);
    int (*close)(int);
};
static int open_metadata(const char *path, int flags) { return open(path, flags | O_CLOEXEC); }
static const MetadataOps metadata_ops = {fstat, open_metadata, read, close};
/* Complete bounded fdinfo text only: reject duplicate/truncated/overflowed mount IDs. */
static size_t skip_mount_space(const char *data, size_t at, size_t end) {
    while (at < end && (data[at] == ' ' || data[at] == '\t')) ++at;
    return at;
}
static bool parse_mount_value(const char *data, size_t start, size_t end, uint64_t *out) {
    size_t at = skip_mount_space(data, start, end);
    if (at == end) return false;
    uint64_t value = 0;
    for (; at < end && data[at] >= '0' && data[at] <= '9'; ++at) {
        unsigned digit = (unsigned) (data[at] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (skip_mount_space(data, at, end) != end || !value) return false;
    *out = value;
    return true;
}
static size_t mount_line_end(const char *data, size_t start, size_t size) {
    while (start < size && data[start] != '\n') ++start;
    return start;
}
static bool parse_mount_id(const char *data, size_t size, uint64_t *out) {
    bool found = false;
    uint64_t value = 0;
    for (size_t start = 0; start < size;) {
        size_t end = mount_line_end(data, start, size);
        if (end == size) return false;
        if (end - start >= 7 && !memcmp(data + start, "mnt_id:", 7)) {
            if (found) return false;
            if (!parse_mount_value(data, start + 7, end, &value)) return false;
            found = true;
        }
        start = end + 1;
    }
    if (found) (*out) = value;
    return found;
}
static bool read_mount_text(int fd, const MetadataOps *ops, char *text, size_t capacity, size_t *used) {

    bool complete = false;
    /* At most 8 reads and 4096 bytes. No unbounded EINTR/short-read retry. */
    for (unsigned attempt = 0; attempt < 8 && *used < capacity; ++attempt) {
        ssize_t n = ops->read(fd, text + *used, capacity - *used);
        if (n < 0) break;
        if (!n) {
            complete = true;
            break;
        }
        if (((size_t) (n)) > capacity - *used) break;
        *used += ((size_t) (n));
    }
    return complete;
}
static bool descriptor_mount_id(int native_fd, const MetadataOps *ops, uint64_t *out) {
    char path[64], text[4096];
    int length = snprintf(path, sizeof path, "/proc/self/fdinfo/%d", native_fd);
    if (length < 0 || ((size_t) (length)) >= sizeof path) return false;
    int fd = ops->open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    size_t used = 0;
    bool complete = read_mount_text(fd, ops, text, sizeof text, &used);
    int closed = ops->close(fd); /* Only our fdinfo FD; NEVER the SQLite descriptor. */
    return !closed && complete && parse_mount_id(text, used, &((*out)));
}
static bool supported_name(const SqlProfile *p, const char *name) {
    static const char *const names[] = {"unix", "unix-excl", "unix-none", "unix-dotfile"};
    __auto_type address = ((uintptr_t) (name));
    {
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(names) / sizeof(names)[0]; ++range_index_1) {
            __auto_type expected = names[range_index_1];
            if (image_string_equal(&(p->image), address, expected)) return true;
        }
    }
    return false;
}
static bool physical_vfs_matches(const SqlProfile *p, NativeFile *file, NativeVFS *vfs) {
    return canonical((uintptr_t) file) && canonical((uintptr_t) vfs) &&
           image_contains(&p->image, (uintptr_t) vfs, sizeof *vfs, kRead) &&
           vfs->version == 3 && vfs->os_size == 120 && vfs->max_path == 512 &&
           supported_name(p, vfs->name) && module_function(p, (uintptr_t) vfs->open, p->open_rva);
}
static unsigned physical_methods_slot(const SqlProfile *p, uintptr_t relative) {
    for (unsigned i = 0; i < 3; ++i)
        if (relative == p->io_rvas[i]) return i;
    return 3;
}
static bool physical_methods_match(const SqlProfile *p, const NativeIO *io) {
    uintptr_t table = (uintptr_t) io;
    if (!canonical(table) || !image_contains(&p->image, table, sizeof *io, kRead) || table < p->image.base) return false;
    unsigned slot = physical_methods_slot(p, table - p->image.base);
    return slot != 3 && io->version == (slot == 2 ? 1 : 3) &&
           module_function(p, (uintptr_t) io->read, p->read_rva) &&
           module_function(p, (uintptr_t) io->size, p->size_rva) &&
           module_function(p, (uintptr_t) io->control, p->control_rva);
}
static bool physical_metadata_changed(const struct stat *before, const struct stat *after, int64_t size) {
    return before->st_dev != after->st_dev || before->st_ino != after->st_ino ||
           before->st_size != after->st_size || size != before->st_size;
}
static bool physical_descriptor_changed(NativeFile *file, const NativeIO *io, uintptr_t embedded, int native_fd) {
    uintptr_t address = (uintptr_t) file;
    return load(int32_t, address + 0x18) != native_fd || file->methods != io || load(uintptr_t, address + 8) != embedded;
}
static bool read_physical_metadata(int native_fd, NativeFile *file, const NativeIO *io, const MetadataOps *ops,
                                   struct stat *before, struct stat *after, int64_t *size, uint64_t *mount) {
    return !ops->stat(native_fd, before) && S_ISREG(before->st_mode) && before->st_size >= 0 &&
           descriptor_mount_id(native_fd, ops, mount) && !io->size(file, size) && !ops->stat(native_fd, after);
}
static int capture_descriptor(NativeFile *file, NativeVFS *vfs, const MetadataOps *ops,
                              struct azsdk_source_sample *out, int *descriptor) {
    const NativeIO *io = file->methods;
    __auto_type address = ((uintptr_t) (file));
    __auto_type embedded = load(uintptr_t, address + 8);
    int native_fd = load(int32_t, address + 0x18);
    if (embedded != ((uintptr_t) (vfs)) || native_fd < 0)
        return AZS_PHYSICAL_DESCRIPTOR;
    struct stat before, after;
    int64_t size = -1;
    uint64_t mount = 0;
    if (!read_physical_metadata(native_fd, file, io, ops, &before, &after, &size, &mount)) return AZS_PHYSICAL_METADATA;
    if (physical_metadata_changed(&before, &after, size) || physical_descriptor_changed(file, io, embedded, native_fd))
        return AZS_PHYSICAL_CHANGED;
    out->device = ((uint64_t) (before.st_dev));
    out->inode = ((uint64_t) (before.st_ino));
    out->mount_id = mount;
    out->file_size = ((uint64_t) (size));
    out->physical_observed_ns = now_ns();
    out->flags |= AZS_PHYSICAL;
    if (descriptor) *descriptor = native_fd;
    return AZS_PHYSICAL_OK;
}
static int capture_physical(uintptr_t connection, const SqlProfile *p, const MetadataOps *ops, struct azsdk_source_sample *out, int *descriptor) {
    if (!p->ready || !p->file_control) return AZS_PHYSICAL_PROFILE;
    NativeFile *file = NULL;
    NativeVFS *vfs = NULL;
    if (p->file_control(((struct sqlite3 *) (connection)), "main", 7, &file) || !file ||
        p->file_control(((struct sqlite3 *) (connection)), "main", 27, &vfs) || !vfs)
        return AZS_PHYSICAL_API;
    /* The callback owns file liveness; the exact DSO bounds its metadata. */
    if (!physical_vfs_matches(p, file, vfs)) return AZS_PHYSICAL_VFS;
    if (!physical_methods_match(p, file->methods)) return AZS_PHYSICAL_METHODS;
    return capture_descriptor(file, vfs, ops, out, descriptor);
}
#endif
static bool has_binding(uint64_t epoch) {
    {
        for (unsigned range_index_2 = 0; range_index_2 < sizeof(source_state().bindings) / sizeof(source_state().bindings)[0]; ++range_index_2) {
            __auto_type b = &(source_state().bindings[range_index_2]);
            if (b->server && b->epoch == epoch) return true;
        }
    }
    return false;
}
static void retire_unused() {
    {
        for (unsigned range_index_3 = 0; range_index_3 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_3) {
            __auto_type e = &(source_state().entries[range_index_3]);
            if (e->sample.owner_epoch && !has_binding(e->sample.owner_epoch)) e->retired = true;
        }
    }
}
static void retire_server(uintptr_t server) {
    {
        for (unsigned range_index_4 = 0; range_index_4 < sizeof(source_state().bindings) / sizeof(source_state().bindings)[0]; ++range_index_4) {
            __auto_type b = &(source_state().bindings[range_index_4]);
            if (b->server == server) (*b) = (Binding) {0};
        }
    }
    retire_unused();
}
static uint64_t connection_generation(uintptr_t connection) {
    /* Only installed, armed native forwarding mints production receipts in
     * this embedded State. A failed read has no diagnostic cause: it includes
     * unknown/retired, active lifecycle work and saturation. No native pointer
     * is followed, and no receipt acquires a connection lease. Lock order is
     * registry_mutex -> EpochState; EpochState releases its lock before native
     * forwarding and must never call back into this registry while locked. */
    ConnectionReceipt receipt = {0};
    return az_connection_read(&source_state().connection_epochs, ((const void *) (connection)), &receipt) ? receipt.generation : 0;
}
static bool owner_bound(const Entry *e) {
    /* Registry/weak-owner validity is separate from a receipt observation.
     * EpochState can temporarily refuse every read during one source's native
     * lifecycle work; that cannot retire another source's binding/history. */
    return atomic_load_explicit(&(source_state().enabled), memory_order_acquire) && e->sample.owner_epoch && !e->retired &&
           !(!az_native_alive(e->mount.control)) && !(!az_native_alive(e->database.control)) &&
           has_binding(e->sample.owner_epoch);
}
static bool live(const Entry *e) {
    /* Zero-generation records retain only the incomplete owner diagnostic.
     * Cache provenance separately requires an observed nonzero generation. */
    return owner_bound(e) &&
           (!e->connection_generation || connection_generation(e->connection) == e->connection_generation);
}
static bool status_live(const Entry *e) {
    return owner_bound(e) && e->status_connection_generation &&
           az_connection_status_generation(&source_state().connection_epochs, ((const void *) (e->connection))) ==
               e->status_connection_generation;
}
static struct azsdk_source_sample scalar_sample(const Entry *e) {
    struct azsdk_source_sample s = e->sample;
    if (bit(source_state().ambiguous_media, s.native_media_id)) s.flags |= AZS_MEDIA_AMBIGUOUS;
    if (live(e)) s.flags |= AZS_CURRENT;
    else {
        s.flags &= ~(AZS_CURRENT | AZS_PHYSICAL);
        s.device = s.inode = s.mount_id = s.file_size = s.physical_observed_ns = 0;
    }
    return s;
}
static bool same_owner(const WeakOwner *weak, const StrongOwner *strong) { return weak->control == strong->control; }
static void entry_drop(Entry *p) {
    az_native_weak_reset(&p->database);
    az_native_weak_reset(&p->mount);
}
/* Build/SqlProfile arguments also allow synthetic GNU fixtures to exercise the complete
 * implementation without masquerading as a real firmware or relaxing production guards. */
typedef struct SourceLookup {
    Binding *binding;
    Entry *entry, *empty;
    uint64_t observed_generation, status_generation;
    bool was_observed, status_was_observed;
} SourceLookup;
static Binding *source_binding(uintptr_t server) {
    for (unsigned i = 0; i < kSlots; ++i)
        if (sources.bindings[i].server == server) return &sources.bindings[i];
    for (unsigned i = 0; i < kSlots; ++i)
        if (!sources.bindings[i].server) return &sources.bindings[i];
    return NULL;
}
static bool entry_matches_chain(const Entry *entry, const OwnedChain *chain, const StrongOwner *mount, const StrongOwner *database) {
    return entry->mount_address == chain->mount_address && entry->database_address == chain->database_address &&
           entry->connection == chain->connection && entry->sample.native_media_id == chain->media &&
           same_owner(&entry->mount, mount) && same_owner(&entry->database, database);
}
static bool source_entry_current(const Entry *entry, const SourceLookup *lookup) {
    return !lookup->entry && entry->connection_generation == lookup->observed_generation &&
           entry->status_connection_generation == lookup->status_generation && owner_bound(entry);
}
static void find_source_entry(SourceLookup *lookup, const OwnedChain *chain, const StrongOwner *mount, const StrongOwner *database) {
    for (unsigned i = 0; i < kSlots; ++i) {
        Entry *entry = &sources.entries[i];
        if (!entry->sample.owner_epoch) {
            if (!lookup->empty) lookup->empty = entry;
            continue;
        }
        if (!entry_matches_chain(entry, chain, mount, database)) continue;
        if (entry->connection_generation) lookup->was_observed = true;
        if (entry->status_connection_generation) lookup->status_was_observed = true;
        if (source_entry_current(entry, lookup)) lookup->entry = entry;
    }
}
static bool source_generation_unavailable(const SourceLookup *lookup) {
    return (!lookup->observed_generation && lookup->was_observed) || (!lookup->status_generation && lookup->status_was_observed);
}
static bool source_registry_full(const SourceLookup *lookup) {
    return !lookup->binding || (!lookup->entry && (!lookup->empty || sources.next_epoch == UINT64_MAX)) ||
           sources.next_revision == UINT64_MAX;
}
static void source_candidate_init(Entry *candidate, const SourceLookup *lookup, const OwnedChain *chain,
                                  const StrongOwner *mount, const StrongOwner *database) {
    if (lookup->entry) return;
    /* Genuine weak construction, with no persistent strong retention. */
    az_native_weak_copy(&candidate->mount, mount);
    az_native_weak_copy(&candidate->database, database);
    candidate->mount_address = chain->mount_address;
    candidate->database_address = chain->database_address;
    candidate->connection = chain->connection;
    candidate->connection_generation = lookup->observed_generation;
    candidate->status_connection_generation = lookup->status_generation;
}
static struct azsdk_source_sample source_sample_init(const SourceLookup *lookup, uint16_t media) {
    struct azsdk_source_sample sample = {0};
    sample.struct_size = sizeof sample;
    sample.flags = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE;
    sample.owner_epoch = lookup->entry ? lookup->entry->sample.owner_epoch : sources.next_epoch + 1;
    sample.revision = sources.next_revision + 1;
    sample.observed_ns = now_ns();
    sample.native_media_id = media;
    sample.physical_status = AZS_PHYSICAL_DISABLED;
    if (bit(sources.ambiguous_media, media)) sample.flags |= AZS_MEDIA_AMBIGUOUS;
    return sample;
}
static bool source_chain_unchanged(const OwnedChain *current, const OwnedChain *prior,
                                   const StrongOwner *mount, const StrongOwner *database) {
    return current->mount_address == prior->mount_address && current->database_address == prior->database_address &&
           current->connection == prior->connection && current->media == prior->media &&
           current->mount->control == mount->control && current->database->control == database->control;
}
static bool source_capture_current(uintptr_t server, const Build *build, const OwnedChain *chain,
                                   const StrongOwner *mount, const StrongOwner *database) {
    OwnedChain current = {0};
    return worker_scope(server, build) && owned_chain(server, build, &current) &&
           source_chain_unchanged(&current, chain, mount, database) && atomic_load_explicit(&sources.enabled, memory_order_acquire);
}
static bool source_receipts_current(const OwnedChain *chain, const SourceLookup *lookup) {
    return connection_generation(chain->connection) == lookup->observed_generation &&
           az_connection_status_generation(&sources.connection_epochs, (const void *) chain->connection) == lookup->status_generation;
}
static void source_publish(SourceLookup *lookup, Entry *candidate, struct azsdk_source_sample *sample, uintptr_t server) {
    uint16_t media = sample->native_media_id;
    if (!lookup->entry) {
        if (bit(sources.seen_media, media)) set_bit(sources.ambiguous_media, media);
        set_bit(sources.seen_media, media);
        candidate->sample = *sample;
        lookup->entry = lookup->empty;
        entry_drop(lookup->entry);
        *lookup->entry = *candidate;
        *candidate = (Entry) {0};
        sources.next_epoch = sample->owner_epoch;
    }
    if (bit(sources.ambiguous_media, media)) sample->flags |= AZS_MEDIA_AMBIGUOUS;
    lookup->entry->sample = *sample;
    sources.next_revision = sample->revision;
    lookup->binding->server = server;
    lookup->binding->epoch = sample->owner_epoch;
    retire_unused();
}
static int source_lookup_admit(SourceLookup *lookup, uintptr_t server, const OwnedChain *chain,
                               const StrongOwner *mount, const StrongOwner *database) {
    lookup->observed_generation = connection_generation(chain->connection);
    lookup->status_generation = az_connection_status_generation(&sources.connection_epochs, (const void *) chain->connection);
    lookup->binding = source_binding(server);
    find_source_entry(lookup, chain, mount, database);
    /* Temporary receipt refusal cannot downgrade or retire a known incarnation. */
    if (source_generation_unavailable(lookup)) return AZS_STALE;
    if (source_registry_full(lookup)) {
        /* An unrecorded valid owner must never become a later unique media match. */
        set_bit(sources.seen_media, chain->media);
        set_bit(sources.ambiguous_media, chain->media);
        retire_server(server);
        return AZS_LIMIT;
    }
    return AZS_OK;
}
static int observe_pinned_source(uintptr_t server, const Build *build, const SqlProfile *sql,
#if defined(__linux__)
                                 const MetadataOps *ops,
#endif
                                 const OwnedChain *chain, const StrongOwner *mount, const StrongOwner *database,
                                 void *out, size_t out_size) {
    SourceLookup lookup = {0};
    int status = source_lookup_admit(&lookup, server, chain, mount, database);
    if (status != AZS_OK) return status;
    Entry candidate __attribute__((cleanup(entry_drop))) = {0};
    source_candidate_init(&candidate, &lookup, chain, mount, database);
    struct azsdk_source_sample sample = source_sample_init(&lookup, chain->media);
    if (atomic_load_explicit(&sources.capture_policy, memory_order_relaxed) & AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER) {
#if defined(__linux__)
        sample.physical_status = capture_physical(chain->connection, sql, ops, &sample, NULL);
#else
        (void) sql;
        sample.physical_status = AZS_PHYSICAL_PROFILE;
#endif
    }
    /* Native callbacks may change the actual holders. Recheck before any publication. */
    if (!source_capture_current(server, build, chain, mount, database)) {
        retire_server(server);
        return AZS_STALE;
    }
    if (!source_receipts_current(chain, &lookup)) return AZS_STALE;
    source_publish(&lookup, &candidate, &sample, server);
    copy_output(out, out_size, &sample);
    return AZS_OK;
}
static int observe(uintptr_t server, const Build *build, const SqlProfile *sql,
#if defined(__linux__)
                   const MetadataOps *ops,
#endif
                   void *out, size_t out_size) {
    if (!worker_scope(server, build)) return AZS_GUARD;
    OwnedChain chain = {0};
    bool supported = owned_chain(server, build, &chain);
    /* Stable pins survive capture reentrancy; actual holders are resolved again below. */
    StrongOwner mount __attribute__((cleanup(az_native_owner_reset))) = {0};
    StrongOwner database __attribute__((cleanup(az_native_owner_reset))) = {0};
    if (supported) {
        az_native_owner_copy(&mount, chain.mount);
        az_native_owner_copy(&database, chain.database);
    }
    AZ_MUTEX_GUARD(lock, sources.registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    if (!atomic_load_explicit(&sources.enabled, memory_order_acquire)) return AZS_UNSUPPORTED;
    if (!supported) {
        retire_server(server);
        return AZS_GUARD;
    }
    return observe_pinned_source(server, build, sql,
#if defined(__linux__)
                                 ops,
#endif
                                 &chain, &mount, &database, out, out_size);
}


/* Native query pins die in reverse order: cache before Collector, so returning
 * the cache's reserved pages never follows a pool already freed by Collector.
 * No strong owner is saved in SourceState or in an observation. */
typedef struct CachePins CachePins;
struct CachePins {
    StrongOwner mount, database, collector, cache;
};
static void cache_pins_drop(CachePins *p) {
    az_native_owner_reset(&p->cache);
    az_native_owner_reset(&p->collector);
    az_native_owner_reset(&p->database);
    az_native_owner_reset(&p->mount);
}

static void cache_binding_drop(CacheBinding *p) {
    az_native_weak_reset(&p->cache);
    az_native_weak_reset(&p->collector);
    az_native_weak_reset(&p->database);
    az_native_weak_reset(&p->mount);
}
static bool cache_stamp(uint64_t *generation) {
    __auto_type s = &(source_state());
    if (atomic_load_explicit(&(s->cache_active), memory_order_acquire) ||
        atomic_load_explicit(&(s->cache_saturated), memory_order_acquire)) return false;
    (*generation) = atomic_load_explicit(&(s->cache_generation), memory_order_acquire);
    return (*generation) && !atomic_load_explicit(&(s->cache_active), memory_order_acquire) &&
           !atomic_load_explicit(&(s->cache_saturated), memory_order_acquire);
}
static bool cache_stamp_is(uint64_t generation) {
    uint64_t current = 0;
    return cache_stamp(&(current)) && current == generation;
}
static bool cache_owner_is(const WeakOwner *weak, const StrongOwner *owned) {
    StrongOwner pin __attribute__((cleanup(az_native_owner_reset))) = az_native_lock(weak);
    return weak->control == owned->control && pin.object == owned->object;
}
const Entry *cache_source_entry(uint64_t epoch) {
    {
        for (unsigned range_index_8 = 0; range_index_8 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_8) {
            __auto_type entry = &(source_state().entries[range_index_8]);
            if (entry->sample.owner_epoch == epoch) return entry;
        }
    }
    return NULL;
}
static bool cache_source_bound(const SourceBorrow *source) {
    for (unsigned i = 0; i < kSlots; ++i) {
        const Binding *binding = &sources.bindings[i];
        if (binding->server == (uintptr_t) source->server && binding->epoch == source->source_epoch) return true;
    }
    return false;
}
static bool cache_source_pins_match(const Entry *entry, const CachePins *pins) {
    return pins->mount.object && pins->database.object && entry->mount_address == (uintptr_t) pins->mount.object &&
           entry->database_address == (uintptr_t) pins->database.object && same_owner(&entry->mount, &pins->mount) &&
           same_owner(&entry->database, &pins->database);
}
static bool cache_source_tuple_matches(const Entry *entry, const CachePins *pins) {
    /* Read the mutable tuple only after exact owner/alias validation. A control
     * block match alone cannot freeze the native database fields. */
    uintptr_t mount = (uintptr_t) pins->mount.object, database = (uintptr_t) pins->database.object;
    return entry->connection == load(uintptr_t, database + 0x38) &&
           entry->sample.native_media_id == load(uint16_t, database + 8) &&
           entry->sample.native_media_id == load(uint16_t, mount + 8) &&
           connection_generation(entry->connection) == entry->connection_generation;
}
static bool cache_source_is(const SourceBorrow *source, const CachePins *pins, bool for_status) {
    bool bound = cache_source_bound(source);
    const Entry *entry = cache_source_entry(source->source_epoch);
    if (!bound || !entry || (for_status ? !status_live(entry) : (!entry->connection_generation || !live(entry))) ||
        !cache_source_pins_match(entry, pins)) return false;
    return cache_source_tuple_matches(entry, pins);
}

static bool cache_binding_retained(const CacheBinding *binding, uint64_t generation) {
    const Entry *entry = cache_source_entry(binding->source_epoch);
    return binding->birth_epoch && binding->generation == generation && entry &&
           entry->connection_generation && owner_bound(entry) &&
           !(!az_native_alive(binding->cache.control)) && !(!az_native_alive(binding->collector.control)) &&
           !(!az_native_alive(binding->mount.control)) && !(!az_native_alive(binding->database.control));
}
/* Receipt availability gates qualification, never slot reclamation. A native
 * operation on another connection can temporarily hide this still-owned key. */
static bool cache_binding_current(const CacheBinding *binding, uint64_t generation) {
    const Entry *entry = cache_source_entry(binding->source_epoch);
    return cache_binding_retained(binding, generation) && entry && live(entry);
}

static bool cache_worker_is(const SourceBorrow *source, const Build *profile) {
    __auto_type s = &(source_state());
    return atomic_load_explicit(&(s->enabled), memory_order_acquire) && source->build >= 1 && source->build <= 2 &&
           (int) (source->build) == atomic_load_explicit(&(s->bound_build), memory_order_acquire) && source->server &&
           worker_scope(((uintptr_t) (source->server)), &((*profile)));
}
static bool cache_build_enabled(const SourceBorrow *source) {
    return atomic_load_explicit(&sources.enabled, memory_order_acquire) && source->build >= 1 && source->build <= 2 &&
           (int) source->build == atomic_load_explicit(&sources.bound_build, memory_order_acquire);
}
static bool cache_source_borrow_valid(const SourceBorrow *source, const Build *profile, const StrongOwner *collector) {
    if (!source->source_epoch || !source->mount || !source->database || !collector || !cache_worker_is(source, profile)) return false;
    /* These are actual callback-owned borrows, never arbitrary holder addresses. */
    return source->mount->object && source->database->object && collector->object;
}
static int cache_prepare_source(const SourceBorrow *source, const Build *profile, const StrongOwner *collector, CachePins *pins, uint64_t *generation) {
    __auto_type s = &(source_state());
    if (!cache_build_enabled(source)) return AZS_UNSUPPORTED;
    if (!cache_source_borrow_valid(source, profile, collector) || !cache_stamp(generation)) return AZS_GUARD;
    az_native_owner_copy(&pins->mount, &(*source->mount));
    az_native_owner_copy(&pins->database, &(*source->database));
    az_native_owner_copy(&pins->collector, collector);
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    return cache_stamp_is((*generation)) && cache_source_is(source, &((*pins)), false) ? AZS_OK : AZS_STALE;
}
static bool cache_borrow_source_unchanged(const SourceBorrow *source, const CachePins *pins) {
    return source->mount->object == pins->mount.object && source->database->object == pins->database.object &&
           !(source->mount->control != pins->mount.control) && !(pins->mount.control != source->mount->control) &&
           !(source->database->control != pins->database.control) && !(pins->database.control != source->database->control);
}
static bool cache_query_borrows_unchanged(const QueryBorrow *borrow, const CachePins *pins) {
    return cache_borrow_source_unchanged(&(borrow->source), &((*pins))) &&
           borrow->collector->object == pins->collector.object &&
           !(borrow->collector->control != pins->collector.control) && !(pins->collector.control != borrow->collector->control) &&
           borrow->selected_cache->object == pins->cache.object &&
           !(borrow->selected_cache->control != pins->cache.control) && !(pins->cache.control != borrow->selected_cache->control);
}
static Observation cache_observation(const CacheBinding *binding) {
    Observation result = {0};
    result.birth_epoch = binding->birth_epoch;
    result.layout_generation = binding->generation;
    result.source_epoch = binding->source_epoch;
    result.factory_kind = binding->factory_kind;
    az_native_weak_copy(&result.mount, &(binding->mount));
    az_native_weak_copy(&result.database, &(binding->database));
    az_native_weak_copy(&result.collector, &(binding->collector));
    az_native_weak_copy(&result.cache, &(binding->cache));
    return result;
}
/* Bounded stack scopes tie factory completion to the same real Collector
 * invocation. A raw new result is never a persistent lookup key. No native
 * call, allocation or owning-result cleanup happens in this bookkeeping. */
struct CacheBirthScope;
static _Thread_local struct CacheBirthScope *cache_birth_scope = NULL;
_Thread_local unsigned cache_birth_depth = 0;
typedef struct CacheBirthScope CacheBirthScope;
struct CacheBirthScope {
    CacheBirthScope *parent;
    const void *creator;
    const void *pool;
    const void *raw_result;
    uint64_t generation;
    unsigned factory_kind;
    unsigned setters;
    bool pushed;
    bool eligible;
    bool poisoned;
    bool factory_active;
    bool write_active;
    bool setup_seen;
    bool completed;
};
static void birth_scope_init(CacheBirthScope *ctx, bool ready, const BirthBorrow *borrow);
static void birth_scope_drop(CacheBirthScope *ctx);
static void birth_scope_init(CacheBirthScope *ctx, bool ready, const BirthBorrow *borrow) {
    (void) ctx;
    *ctx = (CacheBirthScope) {.raw_result = NULL, .generation = 0, .factory_kind = 0, .setters = 0, .pushed = false, .poisoned = false, .factory_active = false, .write_active = false, .setup_seen = false, .completed = false, .parent = cache_birth_scope, .creator = borrow->receiver_creator, .pool = borrow->borrowed_pool, .eligible = ready && borrow->receiver_creator && borrow->borrowed_pool};

    for (__auto_type ancestor = ctx->parent; ancestor; ancestor = ancestor->parent)
        if (ancestor->factory_active) ancestor->poisoned = true;
    if (cache_birth_depth >= kSlots) {
        atomic_store_explicit(&(source_state().cache_saturated), true, memory_order_release);
        ctx->eligible = false;
        return;
    }
    ++cache_birth_depth;
    cache_birth_scope = ctx;
    ctx->pushed = true;
}
static void birth_scope_drop(CacheBirthScope *ctx) {
    (void) ctx;

    if (ctx->pushed) {
        cache_birth_scope = ctx->parent;
        --cache_birth_depth;
    }
}

static void cache_poison_scopes(CacheBirthScope *except) {
    for (__auto_type scope = cache_birth_scope; scope; scope = scope->parent)
        if (scope != except) scope->poisoned = true;
}
static bool cache_creator_pin(const BirthBorrow *borrow, const CachePins *pins, StrongOwner *creator) {
    if (!borrow->creator_database || !borrow->creator_database->object || !borrow->native_result ||
        pins->collector.object != borrow->receiver_collector) return false;
    az_native_owner_copy(creator, borrow->creator_database);
    return creator->object == pins->database.object && creator->control == pins->database.control;
}
static bool cache_birth_holders_unchanged(const BirthBorrow *borrow, const CachePins *pins, const StrongOwner *creator) {
    return cache_borrow_source_unchanged(&borrow->source, pins) && borrow->collector->object &&
           borrow->collector->object == pins->collector.object && borrow->collector->control == pins->collector.control &&
           borrow->creator_database->object == creator->object && borrow->creator_database->control == creator->control &&
           borrow->native_result->object;
}
static bool cache_birth_return_valid(const BirthBorrow *borrow, const Build *profile, const CacheBirthScope *construction,
                                     const CachePins *pins, const StrongOwner *creator) {
    return construction->completed && !construction->poisoned && cache_stamp_is(construction->generation) &&
           cache_worker_is(&borrow->source, profile) && cache_birth_holders_unchanged(borrow, pins, creator);
}
static int cache_birth_slot(const CachePins *pins, uint64_t generation, CacheBinding **out) {
    SourceState *s = &sources;
    for (unsigned i = 0; i < s->cache_limit; ++i) {
        CacheBinding *binding = &s->cache_bindings[i];
        if (binding->birth_epoch && same_owner(&binding->cache, &pins->cache)) return AZS_STALE;
        if (!*out && !cache_binding_retained(binding, generation)) *out = binding;
    }
    return !*out || s->next_cache_birth == UINT64_MAX ? AZS_LIMIT : AZS_OK;
}
static int cache_birth_publish(const BirthBorrow *borrow, const CachePins *pins, const CacheBirthScope *construction, Observation *out) {
    uint64_t generation = construction->generation;
    SourceState *s = &sources;
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    if (!cache_stamp_is(generation) || !cache_source_is(&borrow->source, pins, false)) return AZS_STALE;
    CacheBinding *slot = NULL;
    int status = cache_birth_slot(pins, generation, &slot);
    if (status != AZS_OK) return status;
    CacheBinding candidate __attribute__((cleanup(cache_binding_drop))) = {0};
    az_native_weak_copy(&candidate.mount, &pins->mount);
    az_native_weak_copy(&candidate.database, &pins->database);
    az_native_weak_copy(&candidate.collector, &pins->collector);
    az_native_weak_copy(&candidate.cache, &pins->cache);
    candidate.source_epoch = borrow->source.source_epoch;
    candidate.generation = generation;
    candidate.birth_epoch = s->next_cache_birth + 1;
    candidate.factory_kind = construction->factory_kind;
    if (!cache_stamp_is(generation) || !cache_source_is(&borrow->source, pins, false)) return AZS_STALE;
    cache_binding_drop(slot);
    *slot = candidate;
    candidate = (CacheBinding) {0};
    (void) candidate; /* The cleanup guard no longer owns the transferred weak references. */
    s->next_cache_birth = slot->birth_epoch;
    if (out) {
        azcb_observation_drop(out);
        *out = cache_observation(slot);
    }
    return AZS_OK;
}
static int cache_birth_impl(const BirthBorrow *borrow, const Build *profile, Forward original, Observation *out) {
    if (!original.call) return AZS_INVALID;
    CachePins pins __attribute__((cleanup(cache_pins_drop))) = {0};
    StrongOwner creator __attribute__((cleanup(az_native_owner_reset))) = {0};
    uint64_t before = 0;
    int status = AZS_GUARD;
    status = cache_prepare_source(&(borrow->source), &((*profile)), borrow->collector, &(pins), &(before));
    if (status == AZS_OK && !cache_creator_pin(borrow, &pins, &creator)) status = AZS_GUARD;

    /* The original constructs the real native result at its original sret
     * destination. No reinterpretation as a plain pointer-return function. */
    CacheBirthScope construction __attribute__((cleanup(birth_scope_drop)));
    birth_scope_init(&construction, status == AZS_OK, &((*borrow)));
    original.call(original.context);
    if (status != AZS_OK) return status;
    /* Preserve the final-write stamp through factory cleanup and publication. */
    if (!cache_birth_return_valid(borrow, profile, &construction, &pins, &creator)) return AZS_STALE;
    az_native_owner_copy(&pins.cache, borrow->native_result);
    if (pins.cache.object != construction.raw_result) return AZS_STALE;
    return cache_birth_publish(borrow, &pins, &construction, out);
}

static bool cache_binding_matches_pins(const CacheBinding *binding, const CachePins *pins, uint64_t generation, uint64_t epoch) {
    return cache_binding_current(binding, generation) && binding->source_epoch == epoch &&
           cache_owner_is(&binding->cache, &pins->cache) && cache_owner_is(&binding->collector, &pins->collector) &&
           cache_owner_is(&binding->mount, &pins->mount) && cache_owner_is(&binding->database, &pins->database);
}
static bool cache_query_input_valid(const QueryBorrow *borrow, CopyRowScalar copy) {
    return borrow->selected_cache && borrow->selected_cache->object && copy.copy && borrow->selected_cache->object == borrow->receiver_cache;
}
static int cache_query_select(const QueryBorrow *borrow, CachePins *pins, uint64_t generation, CopyRowScalar copy, Observation *candidate) {
    if (!cache_query_input_valid(borrow, copy)) return AZS_GUARD;
    az_native_owner_copy(&pins->cache, borrow->selected_cache);
    AZ_MUTEX_GUARD(lock, sources.registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    if (!cache_stamp_is(generation) || !cache_source_is(&borrow->source, pins, false)) return AZS_STALE;
    for (unsigned i = 0; i < kSlots; ++i) {
        const CacheBinding *binding = &sources.cache_bindings[i];
        if (!cache_binding_matches_pins(binding, pins, generation, borrow->source.source_epoch)) continue;
        azcb_observation_drop(candidate);
        *candidate = cache_observation(binding);
        return AZS_OK;
    }
    return AZS_UNKNOWN;
}
static bool cache_query_binding_retained(const CachePins *pins, uint64_t generation, uint64_t birth_epoch) {
    for (unsigned i = 0; i < kSlots; ++i) {
        const CacheBinding *binding = &sources.cache_bindings[i];
        if (binding->birth_epoch == birth_epoch && cache_binding_current(binding, generation) &&
            cache_owner_is(&binding->cache, &pins->cache) && cache_owner_is(&binding->collector, &pins->collector)) return true;
    }
    return false;
}
static bool cache_query_capture_current(const QueryBorrow *borrow, const CachePins *pins, uint64_t generation,
                                        const SelectionInvocation *selection) {
    /* Check the mutation stamp before rereading holders after the copier callback. */
    return azsel_ready(selection) && cache_stamp_is(generation) && cache_query_borrows_unchanged(borrow, pins) &&
           cache_source_is(&borrow->source, pins, false);
}
static int cache_query_publish(const QueryBorrow *borrow, const CachePins *pins, uint64_t generation,
                               const SelectionInvocation *selection, Observation *candidate, uint32_t value, Observation *out) {
    AZ_MUTEX_GUARD(lock, sources.registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    if (!cache_query_capture_current(borrow, pins, generation, selection)) return AZS_STALE;
    bool retained = cache_query_binding_retained(pins, generation, candidate->birth_epoch);
    if (!azsel_ready(selection) || !retained || !cache_source_is(&borrow->source, pins, false)) return AZS_STALE;
    candidate->row_value = value;
    if (out) azcb_observation_copy(out, candidate);
    return AZS_OK;
}
static int cache_query_capture(const QueryBorrow *borrow, const Build *profile, const CachePins *pins, uint64_t generation,
                               const SelectionInvocation *selection, Observation *candidate, CopyRowScalar copy, Observation *out) {
    if (!azsel_ready(selection) || !cache_stamp_is(generation) || !cache_worker_is(&borrow->source, profile) ||
        !cache_query_borrows_unchanged(borrow, pins)) return AZS_STALE;
    uint32_t value = 0;
    if (!copy.copy(copy.context, &value)) return AZS_GUARD;
    if (!azsel_ready(selection) || !cache_worker_is(&borrow->source, profile)) return AZS_GUARD;
    return cache_query_publish(borrow, pins, generation, selection, candidate, value, out);
}
static int cache_query_impl(const QueryBorrow *borrow, const Build *profile, Forward original, CopyRowScalar copy, Observation *out) {
    if (!original.call) return AZS_INVALID;
    SelectionInvocation selection __attribute__((cleanup(azsel_invocation_drop)));
    azsel_invocation_init(&selection, &sources.cache_selection);
    CachePins pins __attribute__((cleanup(cache_pins_drop))) = {0};
    Observation candidate __attribute__((cleanup(azcb_observation_drop))) = {0};
    uint64_t generation = 0;
    int status = azsel_ready(&selection) ? cache_prepare_source(&borrow->source, profile, borrow->collector, &pins, &generation) : AZS_GUARD;
    if (status == AZS_OK) status = cache_query_select(borrow, &pins, generation, copy, &candidate);
    original.call(original.context); /* Preserve the destination, ownership and native exception. */
    if (status != AZS_OK) return status;
    return cache_query_capture(borrow, profile, &pins, generation, &selection, &candidate, copy, out);
}
typedef struct CacheMutation CacheMutation;
struct CacheMutation {
    bool active_;
};
static void cache_mutation_init(CacheMutation *ctx);
static void cache_mutation_drop(CacheMutation *ctx);
static void cache_mutation_init(CacheMutation *ctx) {
    (void) ctx;
    *ctx = (CacheMutation) {.active_ = false};

    __auto_type s = &(source_state());
    unsigned active = atomic_load_explicit(&(s->cache_active), memory_order_acquire);
    const unsigned limit = atomic_load_explicit(&(s->cache_depth_limit), memory_order_relaxed);
    while (active < limit) {
        if (atomic_compare_exchange_weak_explicit(&(s->cache_active), &(active), active + 1, memory_order_acq_rel, memory_order_acquire)) {
            ctx->active_ = true;
            break;
        }
    }
    if (!ctx->active_) atomic_store_explicit(&(s->cache_saturated), true, memory_order_release);
    __auto_type generation = atomic_load_explicit(&(s->cache_generation), memory_order_acquire);
    for (;;) {
        if (generation == UINT64_MAX) {
            atomic_store_explicit(&(s->cache_saturated), true, memory_order_release);
            break;
        }
        if (atomic_compare_exchange_weak_explicit(&(s->cache_generation), &(generation), generation + 1, memory_order_acq_rel, memory_order_acquire)) break;
    }
}
static void cache_mutation_drop(CacheMutation *ctx) {
    (void) ctx;

    if (ctx->active_) atomic_fetch_sub_explicit(&(source_state().cache_active), 1, memory_order_release);
}


/* The MusicLibraryServer caller already owns its complete receiver/chain on
 * its worker. Copy actual source owners before forwarding. The TLS stack only
 * carries these local pins through nested native calls; it grants no lifetime.
 * Unsupported and overflow frames hide any earlier frame. */
struct CacheLibraryScope;
static _Thread_local struct CacheLibraryScope *cache_library_scope = NULL;
static SourceBorrow make_source(void *server, unsigned build, StrongOwner *mount, StrongOwner *database) { return (SourceBorrow) {server, build, 0, mount, database}; }
typedef struct CacheLibraryScope CacheLibraryScope;
struct CacheLibraryScope {
    CacheLibraryScope *parent;
    CachePins pins;
    SourceBorrow source;
    Build profile;
    Envelope message;
    Evidence row;
    uint64_t row_selection;
    unsigned depth;
    bool ready;
    bool status_ready;
    bool row_valid;
};
static void library_scope_init(CacheLibraryScope *ctx, unsigned build, void *server, const Build *value);
static void library_scope_drop(CacheLibraryScope *ctx);
static bool library_message_supported(CacheLibraryScope *ctx, void *server) {
    return ctx->depth <= kSlots && azcm_current(server, &ctx->message) &&
           (ctx->message.kind == 0x2102 || ctx->message.kind == 0x2002 || ctx->message.kind == 0x3000);
}
static void library_source_bind(CacheLibraryScope *ctx, void *server) {
    for (unsigned i = 0; i < kSlots; ++i) {
        const Binding *binding = &sources.bindings[i];
        if (binding->server != (uintptr_t) server) continue;
        ctx->source.source_epoch = binding->epoch;
        ctx->ready = cache_source_is(&ctx->source, &ctx->pins, false);
        ctx->status_ready = cache_source_is(&ctx->source, &ctx->pins, true);
        break;
    }
}
static void library_scope_init(CacheLibraryScope *ctx, unsigned build, void *server, const Build *value) {
    (void) ctx;
    *ctx = (CacheLibraryScope) {.row_selection = 0, .ready = false, .status_ready = false, .row_valid = false, .parent = cache_library_scope, .source = make_source(server, build, &ctx->pins.mount, &ctx->pins.database), .profile = (*value)};

    ctx->depth = ctx->parent ? (ctx->parent->depth < kSlots ? ctx->parent->depth + 1 : kSlots + 1) : 1;
    cache_library_scope = ctx;
    if (!library_message_supported(ctx, server)) return;
    {

        if (!cache_worker_is(&(ctx->source), &(ctx->profile))) return;
        OwnedChain chain = {0};
        if (!owned_chain(((uintptr_t) (server)), &(ctx->profile), &(chain))) return;
        az_native_owner_copy(&ctx->pins.mount, &(*chain.mount));
        az_native_owner_copy(&ctx->pins.database, &(*chain.database));
        __auto_type s = &(source_state());
        AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
        if (!lock.held) return;
        library_source_bind(ctx, server);
    }
}
static void library_scope_drop(CacheLibraryScope *ctx) {
    (void) ctx;
    cache_library_scope = ctx->parent;
    azcq_evidence_drop(&ctx->row);
    cache_pins_drop(&ctx->pins);
}

struct CacheSelectionScope;
static _Thread_local struct CacheSelectionScope *cache_selection_scope = NULL;
typedef struct CacheSelectionScope CacheSelectionScope;
struct CacheSelectionScope {
    CacheSelectionScope *parent;
    CacheLibraryScope *library;
    CachePins pins;
    SourceBorrow source;
    const void *creator;
    bool ready;
};
static void selection_scope_init(CacheSelectionScope *ctx, const struct azce_call *call);
static void selection_scope_drop(CacheSelectionScope *ctx);
static bool selection_call_matches(const CacheLibraryScope *library, const struct azce_call *call) {
    return (library->message.kind == 0x2102 || library->message.kind == 0x2002) &&
           (unsigned char) call->args[1] == library->message.selector && (uint32_t) call->args[2] == library->message.client &&
           canonical(call->args[0]) && canonical(call->args[3]);
}
static bool selection_scope_admitted(const CacheSelectionScope *ctx, const struct azce_call *call) {
    return ctx->library && ctx->library->ready && ctx->library->depth <= kSlots && selection_call_matches(ctx->library, call);
}
static void selection_scope_init(CacheSelectionScope *ctx, const struct azce_call *call) {
    (void) ctx;
    *ctx = (CacheSelectionScope) {.creator = NULL, .ready = false, .parent = cache_selection_scope, .library = cache_library_scope, .source = make_source(NULL, 0, &ctx->pins.mount, &ctx->pins.database)};

    cache_selection_scope = ctx;
    cache_poison_scopes(NULL); // a nested selection cannot preserve an older factory claim
    if (!selection_scope_admitted(ctx, call)) return;
    {

            /* These are the reviewed selection entry's live native receiver
             * and owning Creator reference, not addresses recovered from TLS.
             * Only read them before the original; all later holders are local. */
        __auto_type root = ((uintptr_t) (call->args[0]));
        az_native_owner_copy(&ctx->pins.mount, &(*((const StrongOwner *) (root + 0x28))));
        az_native_owner_copy(&ctx->pins.collector, &(*((const StrongOwner *) (root + 0x40))));
        if (!ctx->pins.mount.object || !ctx->pins.collector.object ||
            !cache_owner_is(&(ctx->library->pins.mount), &(ctx->pins.mount))) return;
        __auto_type mount = ((uintptr_t) (ctx->pins.mount.object));
        az_native_owner_copy(&ctx->pins.database, &(*((const StrongOwner *) (mount + 0x30))));
        if (!ctx->pins.database.object || !cache_owner_is(&(ctx->library->pins.database), &(ctx->pins.database))) return;
        ctx->creator = load(const void *, call->args[3]);
        if (!ctx->creator) return;
        ctx->source.server = ctx->library->source.server;
        ctx->source.build = ctx->library->source.build;
        ctx->source.source_epoch = ctx->library->source.source_epoch;
        __auto_type s = &(source_state());
        AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
        ctx->ready = lock.held && cache_source_is(&(ctx->source), &(ctx->pins), false);
    }
}
static void selection_scope_drop(CacheSelectionScope *ctx) {
    (void) ctx;
    cache_selection_scope = ctx->parent;
    cache_pins_drop(&ctx->pins);
}

struct CacheQueryScope;
static _Thread_local struct CacheQueryScope *cache_query_scope = NULL;
typedef struct CacheQueryScope CacheQueryScope;
struct CacheQueryScope {
    CacheQueryScope *parent;
    CacheLibraryScope *library;
    SelectionInvocation selection;
    CachePins pins;
    SourceBorrow source;
    Observation row;
    uint32_t start;
    uint32_t count;
    uint32_t client;
    uint32_t selector;
    unsigned depth;
    bool ready;
    bool native_owned;
    bool adapter_active;
    bool adapter_done;
    bool poisoned;
    bool published;
};
static bool query_scope_owned(void *context);
static bool query_scope_mount(void *context, const void *candidate);
static void query_scope_init(CacheQueryScope *ctx, const struct azce_call *call);
static void query_scope_drop(CacheQueryScope *ctx);
static bool query_scope_owned(void *context) {

    __auto_type self = &(*((CacheQueryScope *) (context)));
    return self->native_owned && azsel_ready(&self->selection) && self->library == cache_library_scope &&
           self->library->ready && cache_worker_is(&(self->library->source), &(self->library->profile));
}
static bool query_scope_mount(void *context, const void *candidate) {

    __auto_type self = &(*((CacheQueryScope *) (context)));
        // This exact Mount already has a genuine, RTTI-checked source pin.
    return candidate == self->library->pins.mount.object;
}
static bool query_call_matches(const CacheLibraryScope *library, const struct azce_call *call) {
    return library->message.kind == 0x3000 && library->message.fields[3] == 0 &&
           (unsigned char) call->args[1] == library->message.client && (uint32_t) call->args[2] == library->message.selector &&
           (uint32_t) call->args[3] == library->message.fields[1] && (uint32_t) call->args[4] == library->message.fields[2] &&
           canonical(call->args[0]) && canonical(call->args[8]);
}
static bool query_scope_admitted(const CacheQueryScope *ctx, const struct azce_call *call) {
    return ctx->library && ctx->library->ready && ctx->depth <= kSlots && azsel_ready(&ctx->selection) &&
           query_call_matches(ctx->library, call);
}
static void query_poison_older(CacheQueryScope *parent) {
    for (; parent; parent = parent->parent)
        if (parent->adapter_active) parent->poisoned = true;
}
static void query_scope_resolve(CacheQueryScope *ctx, const struct azce_call *call) {
    {

        ctx->native_owned = true; // actual reviewed QUERY entry's live receiver, on the owned library worker
        SelectionBorrow borrow = {ctx->library->source.build,
                                  ((const void *) (call->args[0])), ctx, query_scope_owned, query_scope_mount};
        struct azsel_resolution resolved __attribute__((cleanup(azsel_resolution_drop))) = {0};
        azsel_resolve_owned(&ctx->selection, &borrow, (struct azsel_position) {ctx->client, ctx->selector}, &resolved);
        if (resolved.status != AZSEL_OK) return;
        az_native_owner_copy(&ctx->pins.mount, &(resolved.pins.mount));
        az_native_owner_copy(&ctx->pins.database, &(resolved.pins.database));
        az_native_owner_copy(&ctx->pins.collector, &(resolved.pins.collector));
        az_native_owner_copy(&ctx->pins.cache, &(resolved.pins.cache));
        if (!cache_owner_is(&(ctx->library->pins.mount), &(ctx->pins.mount)) ||
            !cache_owner_is(&(ctx->library->pins.database), &(ctx->pins.database))) return;
        ctx->source.server = ctx->library->source.server;
        ctx->source.build = ctx->library->source.build;
        ctx->source.source_epoch = ctx->library->source.source_epoch;
        __auto_type s = &(source_state());
        AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
        ctx->ready = lock.held && azsel_ready(&ctx->selection) && cache_source_is(&(ctx->source), &(ctx->pins), false);
    }
}
static void query_scope_init(CacheQueryScope *ctx, const struct azce_call *call) {
    (void) ctx;
    *ctx = (CacheQueryScope) {.start = 0, .count = 0, .client = 0, .selector = 0, .ready = false, .native_owned = false, .adapter_active = false, .adapter_done = false, .poisoned = false, .published = false, .parent = cache_query_scope, .library = cache_library_scope, .source = make_source(NULL, 0, &ctx->pins.mount, &ctx->pins.database)};

    azsel_invocation_init(&ctx->selection, &source_state().cache_selection);
    ctx->depth = ctx->parent ? (ctx->parent->depth < kSlots ? ctx->parent->depth + 1 : kSlots + 1) : 1;
    cache_query_scope = ctx;
    query_poison_older(ctx->parent);
    if (ctx->library) ctx->library->row_valid = false;
    if (!query_scope_admitted(ctx, call)) return;
    ctx->start = ((uint32_t) (call->args[3]));
    ctx->count = ((uint32_t) (call->args[4]));
    ctx->client = ctx->library->message.client;
    ctx->selector = ctx->library->message.selector;
    if (ctx->start > INT32_MAX || !ctx->count || ctx->count > 16) return;
    query_scope_resolve(ctx, call);
}
static void query_scope_drop(CacheQueryScope *ctx) {
    (void) ctx;

        // A nested query may have completed while this outer original was
        // still active. A refused/throwing outer call cannot lend that inner
        // evidence to the eventual reply of the outer invocation.
    if (!ctx->published && ctx->library && ctx->library == cache_library_scope) ctx->library->row_valid = false;
    cache_query_scope = ctx->parent;
    azcb_observation_drop(&ctx->row);
    cache_pins_drop(&ctx->pins);
    azsel_invocation_drop(&ctx->selection);
}

static bool cache_copy_native_row(void *carrier, uint32_t *value) {
    return azcrow_copy_owned_tag23(carrier, value);
}
typedef struct AdapterGuard AdapterGuard;
struct AdapterGuard {
    CacheQueryScope *scope;
    bool returned;
};
static void adapter_guard_init(AdapterGuard *ctx, CacheQueryScope *value);
static void adapter_guard_drop(AdapterGuard *ctx);
static void adapter_guard_init(AdapterGuard *ctx, CacheQueryScope *value) {
    (void) ctx;
    *ctx = (AdapterGuard) {.returned = false, .scope = value};
}
static void adapter_guard_drop(AdapterGuard *ctx) {
    (void) ctx;
    ctx->scope->adapter_active = false;
    if (!ctx->returned) ctx->scope->poisoned = true;
}

static bool query_adapter_scope_ready(const CacheQueryScope *scope) {
    return scope && scope->ready && !scope->poisoned && !scope->adapter_active && !scope->adapter_done &&
           scope->library == cache_library_scope && azsel_ready(&scope->selection);
}
static bool query_adapter_call_matches(const CacheQueryScope *scope, const struct azce_call *call) {
    return canonical(call->args[0]) && canonical(call->args[3]) && canonical(call->args[8]) &&
           (uint32_t) call->args[1] == scope->start && (uint32_t) call->args[2] == scope->count;
}
static void query_adapter_complete(CacheQueryScope *scope, const struct azce_call *call, int result, const Observation *row) {
    unsigned family = call->kind == AZCE_KIND_GENERIC ? AZCB_PRIMARY_FACTORY : AZCB_STANDARD_FALLBACK_FACTORY;
    if (result == AZS_OK && !scope->poisoned && azsel_ready(&scope->selection) && row->factory_kind == family) {
        azcb_observation_copy(&scope->row, row);
        scope->adapter_done = true;
    } else scope->poisoned = true;
}
static void cache_forward_query_adapter(struct azce_call *call, Forward original) {
    __auto_type scope = cache_query_scope;
    if (!query_adapter_scope_ready(scope) || !query_adapter_call_matches(scope, call)) {
        if (scope) scope->poisoned = true;
        original.call(original.context);
        return;
    }
    // Both exact adapter entries borrow their selected cache at+8 and receive
    // the real Mount owner in X3. Copy that holder only before native work.
    __auto_type receiver = load(const void *, call->args[0] + 8);
    StrongOwner mount __attribute__((cleanup(az_native_owner_reset))) = {0};
    az_native_owner_copy(&mount, ((const StrongOwner *) (call->args[3])));
    if (receiver != scope->pins.cache.object || !mount.object ||
        !cache_owner_is(&(scope->pins.mount), &(mount))) {
        scope->poisoned = true;
        original.call(original.context);
        return;
    }
    scope->adapter_active = true;
    AdapterGuard guard __attribute__((cleanup(adapter_guard_drop)));
    adapter_guard_init(&guard, scope);
    QueryBorrow borrow = {scope->source, &scope->pins.collector, &scope->pins.cache, receiver};
    Observation row __attribute__((cleanup(azcb_observation_drop))) = {0};
    __auto_type result = cache_query_impl(&(borrow), &(scope->library->profile), original,
                                          (CopyRowScalar) {cache_copy_native_row, ((void *) (call->args[8]))}, &row);
    guard.returned = true;
    query_adapter_complete(scope, call, result, &row);
}
static bool cache_row_binding_matches(const CacheBinding *binding, const Evidence *row) {
    return binding->birth_epoch == row->cache.birth_epoch && binding->source_epoch == row->cache.source_epoch &&
           binding->generation == row->cache.layout_generation && cache_binding_current(binding, binding->generation) &&
           binding->cache.control == row->cache.cache.control && binding->collector.control == row->cache.collector.control;
}
static bool cache_row_binding_current(const Evidence *row) {
    {
        for (unsigned range_index_13 = 0; range_index_13 < sizeof(source_state().cache_bindings) / sizeof(source_state().cache_bindings)[0]; ++range_index_13) {
            __auto_type binding = &(source_state().cache_bindings[range_index_13]);
            if (cache_row_binding_matches(binding, row)) return true;
        }
    }
    return false;
}
static bool query_return_ready(const CacheQueryScope *scope) {
    return scope->ready && !scope->poisoned && scope->adapter_done && azsel_ready(&scope->selection) &&
           scope->library == cache_library_scope && cache_stamp_is(scope->row.layout_generation);
}
static void cache_forward_query(struct azce_call *call, Forward original) {
    CacheQueryScope scope __attribute__((cleanup(query_scope_drop)));
    query_scope_init(&scope, &(*call));
    original.call(original.context);
    {

        if (!query_return_ready(&scope)) return;
        uint32_t value = 0;
        if (!azcrow_copy_owned_tag23(((const void *) (call->args[8])), &value) ||
            value != scope.row.row_value) return;
        __auto_type s = &(source_state());
        AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
        if (!lock.held || !azsel_ready(&scope.selection) || !cache_source_is(&(scope.source), &(scope.pins), false) ||
            !cache_stamp_is(scope.row.layout_generation)) return;
        __auto_type row = &(scope.library->row);
        azcb_observation_copy(&row->cache, &scope.row);
        row->sequence = scope.library->message.sequence;
        row->client = scope.client;
        row->selector = scope.selector;
        row->start = scope.start;
        row->count = scope.count;
        scope.library->row_selection = scope.selection.revision;
        scope.library->row_valid = cache_row_binding_current(row);
        scope.published = scope.library->row_valid;
    }
}
typedef struct JoinScope JoinScope;
struct JoinScope {
    bool enabled;
};
static void join_scope_init(JoinScope *ctx, bool value);
static void join_scope_drop(JoinScope *ctx);
static void join_scope_init(JoinScope *ctx, bool value) {
    (void) ctx;
    *ctx = (JoinScope) {.enabled = value};
}
static void join_scope_drop(JoinScope *ctx) {
    (void) ctx;
    if (ctx->enabled) azjs_server(0, 0, 0, 0);
}

static void cache_forward_library_impl(unsigned build, void *server, const Build *profile, Forward original) {
    if (!original.call) return;
    CacheLibraryScope scope __attribute__((cleanup(library_scope_drop)));
    library_scope_init(&scope, build, server, &((*profile)));
    JoinScope joining __attribute__((cleanup(join_scope_drop)));
    join_scope_init(&joining, azjs_server != NULL);
    __auto_type packet_key = azcm_take_packet_key(server);
    if (joining.enabled) azjs_server(scope.status_ready ? packet_key : 0, scope.message.parameter,
                                     scope.status_ready ? scope.source.source_epoch : 0, 1);
    original.call(original.context); // no SDK lock or observer catch across native work
}
static void cache_forward_library_route(unsigned build, void *server, const Build *profile, Forward original) {
    const bool admitted = azsdk_cache_capture_ready && azsdk_cache_capture_ready() == 1;
    // Always push a masking frame, including while a partial installation is
    // forwarding. An older reentrant source scope cannot authorize this call.
    cache_forward_library_impl(admitted ? build : 0, server, &((*profile)), original);
}
static bool cache_create_call_matches(const CacheSelectionScope *selected, const struct azce_call *call) {
    return call->kind == AZCE_KIND_CREATE && selected && selected->ready && selected->library == cache_library_scope &&
           cache_library_scope->ready && call->args[0] == (uintptr_t) selected->pins.collector.object &&
           canonical(call->args[1]) && canonical(call->args[8]);
}
static void cache_forward_create(struct azce_call *call, Forward original) {
    __auto_type selected = cache_selection_scope;
    if (!cache_create_call_matches(selected, call)) {
        original.call(original.context);
        return;
    }
    BirthPins pins __attribute__((cleanup(azcp_pins_drop))) = {0};
    azcp_pins_init(&pins, &selected->source, &selected->pins.collector, ((const void *) (call->args[0])), ((const void *) (call->args[1])));
    __auto_type borrow = azcp_pins_borrow(&pins, ((const StrongOwner *) (call->args[8])));
    if (!azcp_pins_ready(&pins) || borrow.receiver_creator != selected->creator) {
        original.call(original.context);
        return;
    }
    /* Only local owner holders survive the original. Its genuine X8 result is
     * read by the existing birth helper after successful native construction. */
    (void) cache_birth_impl(&(borrow), &(selected->library->profile), original, NULL);
}
static void cache_forward_entry_impl(struct azce_call *call, Forward original) {
    if (!call || !original.call) return;
    if (call->kind == AZCE_KIND_QUERY) {
        cache_forward_query(call, original);
        return;
    }
    if (call->kind == AZCE_KIND_GENERIC || call->kind == AZCE_KIND_TRACKINFO) {
        cache_forward_query_adapter(call, original);
        return;
    }
    if (call->kind == AZCE_KIND_SELECTION) {
        CacheSelectionScope scope __attribute__((cleanup(selection_scope_drop)));
        selection_scope_init(&scope, &(*call));
        original.call(original.context);
        return;
    }
    cache_forward_create(call, original);
}



static bool cache_evidence_scope_ready(const CacheLibraryScope *scope) {
    return !cache_query_scope && scope && scope->ready && scope->row_valid && scope->message.kind == 0x3000 &&
           cache_worker_is(&scope->source, &scope->profile);
}
static bool cache_evidence_current(CacheLibraryScope *scope, const SelectionInvocation *selection) {
    return cache_source_is(&scope->source, &scope->pins, false) && cache_row_binding_current(&scope->row) &&
           azsel_ready(selection) && cache_stamp_is(scope->row.cache.layout_generation);
}
bool azcq_current_owned(Evidence *out) {
    {

        __auto_type scope = cache_library_scope;
        if (!cache_evidence_scope_ready(scope)) return false;
        SelectionInvocation selection __attribute__((cleanup(azsel_invocation_drop)));
        azsel_invocation_init(&selection, &source_state().cache_selection);
        if (!azsel_ready(&selection) || selection.revision != scope->row_selection ||
            !cache_stamp_is(scope->row.cache.layout_generation)) return false;
        __auto_type s = &(source_state());
        AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
        if (!lock.held || !cache_evidence_current(scope, &selection)) return false;
        azcq_evidence_copy(out, &scope->row);
        return true;
    }
}



void azcb_forward_library_owned(unsigned build, void *server, Forward original) {
    const Build unsupported = {0, 0};
    cache_forward_library_route(build, server, (build >= 1 && build <= 2 ? &builds[build] : &unsupported), original);
}
void azcb_observe_entry_owned(struct azce_call *call, Forward original) {
    cache_forward_entry_impl(call, original);
}
typedef struct FactoryGuard FactoryGuard;
struct FactoryGuard {
    CacheBirthScope *frame;
    bool returned;
};
static void factory_guard_init(FactoryGuard *ctx, CacheBirthScope *value);
static void factory_guard_drop(FactoryGuard *ctx);
static void factory_guard_init(FactoryGuard *ctx, CacheBirthScope *value) {
    (void) ctx;
    *ctx = (FactoryGuard) {.returned = false, .frame = value};
}
static void factory_guard_drop(FactoryGuard *ctx) {
    (void) ctx;
    ctx->frame->factory_active = false;
    if (!ctx->returned) ctx->frame->poisoned = true;
}

static bool cache_factory_scope_ready(const CacheBirthScope *frame) {
    return frame && frame->eligible && !frame->poisoned && !frame->factory_active && !frame->completed;
}
static bool cache_factory_borrow_matches(const CacheBirthScope *frame, const FactoryBorrow *borrow) {
    return borrow->receiver_creator == frame->creator && borrow->borrowed_pool == frame->pool && borrow->native_result &&
           (borrow->kind == AZCB_PRIMARY_FACTORY || borrow->kind == AZCB_STANDARD_FALLBACK_FACTORY);
}
static bool cache_factory_admit(CacheBirthScope *frame, const FactoryBorrow *borrow) {
    if (!cache_factory_scope_ready(frame) || !cache_factory_borrow_matches(frame, borrow)) return false;
    /* The exact fallback factory supports Creator mode 0. Read only after the
     * current owned Collector scope has established this Creator's liveness. */
    if (borrow->kind == AZCB_STANDARD_FALLBACK_FACTORY &&
        load(unsigned char, (uintptr_t) borrow->receiver_creator + 0x44) != 0) return false;
    return cache_stamp(&frame->generation);
}
static bool cache_factory_completed(const CacheBirthScope *frame, const FactoryBorrow *borrow) {
    if (frame->poisoned || !cache_stamp_is(frame->generation) || !*borrow->native_result) return false;
    return borrow->kind != AZCB_STANDARD_FALLBACK_FACTORY ||
           (frame->setup_seen && frame->setters == 16 && *borrow->native_result == frame->raw_result);
}
void azcb_observe_factory_owned(const FactoryBorrow *borrow, Forward original) {
    if (!original.call) return;
    __auto_type frame = cache_birth_scope;
    cache_poison_scopes(frame);
    const bool ready = cache_factory_admit(frame, borrow);
    if (!ready) {
        if (frame) frame->poisoned = true;
        original.call(original.context);
        return;
    }
    frame->factory_kind = borrow->kind;
    frame->factory_active = true;
    FactoryGuard guard __attribute__((cleanup(factory_guard_drop)));
    factory_guard_init(&guard, frame);
    original.call(original.context); // original raw pointer result and exception remain native
    guard.returned = true;
    if (!cache_factory_completed(frame, borrow)) {
        frame->poisoned = true;
        return;
    }
    frame->raw_result = *borrow->native_result;
    frame->completed = true;
}
typedef struct WriteGuard WriteGuard;
struct WriteGuard {
    CacheBirthScope *frame;
    bool returned;
};
static void write_guard_init(WriteGuard *ctx, CacheBirthScope *value);
static void write_guard_drop(WriteGuard *ctx);
static void write_guard_init(WriteGuard *ctx, CacheBirthScope *value) {
    (void) ctx;
    *ctx = (WriteGuard) {.returned = false, .frame = value};
}
static void write_guard_drop(WriteGuard *ctx) {
    (void) ctx;
    if (ctx->frame) {
        ctx->frame->write_active = false;
        if (!ctx->returned) ctx->frame->poisoned = true;
    }
}

static bool cache_write_scope_ready(const CacheBirthScope *frame) {
    return frame->eligible && !frame->poisoned && frame->factory_active &&
           frame->factory_kind == AZCB_STANDARD_FALLBACK_FACTORY && !frame->write_active;
}
static bool cache_write_sequence_matches(const CacheBirthScope *frame, const WriteBorrow *borrow) {
    if (borrow->kind == AZCB_SETUP_WRITE)
        return !frame->setup_seen && borrow->argument == 16 && borrow->borrowed_pool == frame->pool;
    return borrow->kind == AZCB_LAYOUT_WRITE && frame->setup_seen && frame->setters < 16 &&
           borrow->argument == frame->setters && borrow->receiver_cache == frame->raw_result;
}
static bool cache_write_admit(const CacheBirthScope *frame, const WriteBorrow *borrow) {
    return frame && cache_write_scope_ready(frame) && borrow->receiver_cache && borrow->native_status &&
           cache_stamp_is(frame->generation) && cache_write_sequence_matches(frame, borrow);
}
static void cache_write_complete(CacheBirthScope *frame, const WriteBorrow *borrow, uint64_t before) {
    uint64_t after = 0;
    if (frame->poisoned || !cache_stamp(&after) || before == UINT64_MAX || after != before + 1 || *borrow->native_status != 0) {
        frame->poisoned = true;
        return;
    }
    frame->generation = after;
    if (borrow->kind == AZCB_SETUP_WRITE) {
        frame->setup_seen = true;
        frame->raw_result = borrow->receiver_cache;
    } else ++frame->setters;
}
void azcb_observe_construction_write_owned(const WriteBorrow *borrow, Forward original) {
    if (!original.call) return;
    __auto_type frame = cache_birth_scope;
    cache_poison_scopes(frame);
    const bool ready = cache_write_admit(frame, borrow);
    if (frame) {
        if (!ready) frame->poisoned = true;
        frame->write_active = true;
    }
    WriteGuard guard __attribute__((cleanup(write_guard_drop)));
    write_guard_init(&guard, frame);
    __auto_type before = ready && frame ? frame->generation : 0;
    {
        CacheMutation mutation __attribute__((cleanup(cache_mutation_drop)));
        cache_mutation_init(&mutation);
        original.call(original.context);
    }
    guard.returned = true;
    if (!ready || !frame) return;
    cache_write_complete(frame, borrow, before);
}
int azcb_observe_birth_owned(const BirthBorrow *borrow, Forward original, Observation *out) {
    Build unsupported = {0, 0};
    const Build *profile = (borrow->source.build >= 1 && borrow->source.build <= 2 ? &builds[borrow->source.build] : &unsupported);
    return cache_birth_impl(borrow, &((*profile)), original, out);
}
int azcb_observe_query_owned(const QueryBorrow *borrow, Forward original, CopyRowScalar copy, Observation *out) {
    Build unsupported = {0, 0};
    const Build *profile = (borrow->source.build >= 1 && borrow->source.build <= 2 ? &builds[borrow->source.build] : &unsupported);
    return cache_query_impl(borrow, &((*profile)), original, copy, out);
}
void azcb_retire_forward(Forward original) {
    if (!original.call) return;
    cache_poison_scopes(NULL);
    CacheMutation mutation __attribute__((cleanup(cache_mutation_drop)));
    cache_mutation_init(&mutation);
    original.call(original.context);
}
void azcb_selection_forward(Forward original) {
    if (!original.call) return;
    SelectionMutation mutation __attribute__((cleanup(azsel_mutation_drop)));
    azsel_mutation_init(&mutation, &source_state().cache_selection, AZSEL_SELECTION_CHANGE);
    original.call(original.context);
}


static bool source_configuration_valid(int enable, int build_id, unsigned policy) {
    return enable && build_id >= 1 && build_id <= 2 && !(policy & ~kKnownPolicy);
}
int azsdk_sources_configure(int enable, int build_id, unsigned policy) {
    {

        AZ_MUTEX_GUARD(lock, source_state().registry_mutex, false);
        if (!lock.held) return AZS_UNSUPPORTED;
        atomic_store_explicit(&(source_state().enabled), 0, memory_order_release);
        {
            for (unsigned range_index_14 = 0; range_index_14 < sizeof(source_state().bindings) / sizeof(source_state().bindings)[0]; ++range_index_14) {
                __auto_type b = &(source_state().bindings[range_index_14]);
                (*b) = (Binding) {0};
            }
        }
        retire_unused();
        atomic_store_explicit(&(source_state().capture_policy), 0, memory_order_relaxed);
        if (!source_configuration_valid(enable, build_id, policy)) return AZS_UNSUPPORTED;
        int prior = atomic_load_explicit(&(source_state().bound_build), memory_order_relaxed);
        if (prior && prior != build_id) return AZS_UNSUPPORTED;
        pthread_once(&source_state().image_once, prepare_image);
        if (!source_state().main_image.count) return AZS_UNSUPPORTED;
        atomic_store_explicit(&(source_state().bound_build), build_id, memory_order_relaxed);
        if (policy & AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER)
            pthread_once(&source_state().sql_once, prepare_sqlcipher);
        atomic_store_explicit(&(source_state().capture_policy), policy, memory_order_relaxed);
        atomic_store_explicit(&(source_state().enabled), 1, memory_order_release);
        return AZS_OK;
    }
}
int azsdk_sources_observe_owned_message(void *server, void *out,
                                        size_t size) {
    if (!output_ok(out, size, true)) return AZS_INVALID;
    if (!atomic_load_explicit(&(source_state().enabled), memory_order_acquire)) return AZS_UNSUPPORTED;
    int build = atomic_load_explicit(&(source_state().bound_build), memory_order_relaxed);
    if (build < 1 || build > 2) return AZS_UNSUPPORTED;
    {

        return observe(((uintptr_t) (server)), &(builds[build]), &(source_state().sql_profile),
#if defined(__linux__)
                       &metadata_ops,
#endif
                       out, size);
    }
}
int azsdk_sources_read(uint64_t epoch, void *out, size_t size) {
    if (!epoch || !output_ok(out, size, false)) return AZS_INVALID;
    {

        AZ_MUTEX_GUARD(lock, source_state().registry_mutex, true);
        if (!lock.held) return AZS_BUSY;
        {
            for (unsigned range_index_15 = 0; range_index_15 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_15) {
                __auto_type e = &(source_state().entries[range_index_15]);
                if (e->sample.owner_epoch == epoch) {
                    __auto_type s = scalar_sample(e);
                    copy_output(out, size, &(s));
                    return (s.flags & AZS_CURRENT) ? AZS_OK : AZS_STALE;
                }
            }
        }
        return AZS_UNKNOWN;
    }
}
int azsdk_sources_status_read(uint64_t epoch, void *out, size_t size) {
    if (!epoch || !output_ok(out, size, false)) return AZS_INVALID;
    {

        AZ_MUTEX_GUARD(lock, source_state().registry_mutex, true);
        if (!lock.held) return AZS_BUSY;
        {
            for (unsigned range_index_16 = 0; range_index_16 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_16) {
                __auto_type entry = &(source_state().entries[range_index_16]);
                if (entry->sample.owner_epoch == epoch) {
                    if (!status_live(entry)) return AZS_STALE;
                    copy_output(out, size, &(entry->sample));
                    return AZS_OK;
                }
            }
        }
        return AZS_UNKNOWN;
    }
}
int azsdk_sources_candidate(uint16_t media, void *out, size_t size) {
    if (!media || !output_ok(out, size, false)) return AZS_INVALID;
    {

        AZ_MUTEX_GUARD(lock, source_state().registry_mutex, true);
        if (!lock.held) return AZS_BUSY;
        if (bit(source_state().ambiguous_media, media)) {
            struct azsdk_source_sample s = {0};
            s.struct_size = sizeof s;
            s.native_media_id = media;
            s.flags = AZS_HISTORY_INCOMPLETE | AZS_MEDIA_AMBIGUOUS;
            copy_output(out, size, &(s));
            return AZS_AMBIGUOUS;
        }
        {
            for (unsigned range_index_17 = 0; range_index_17 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_17) {
                __auto_type e = &(source_state().entries[range_index_17]);
                if (e->sample.native_media_id == media) {
                    __auto_type s = scalar_sample(e);
                    if (!(s.flags & AZS_CURRENT)) continue;
                    copy_output(out, size, &(s));
                    return AZS_OK;
                }
            }
        }
        return AZS_UNKNOWN;
    }
}
unsigned azsdk_sources_sweep() {
    {

        AZ_MUTEX_GUARD(lock, source_state().registry_mutex, false);
        if (!lock.held) return 0;
        unsigned reclaimed = 0;
        {
            for (unsigned range_index_18 = 0; range_index_18 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_18) {
                __auto_type e = &(source_state().entries[range_index_18]);
                if (e->sample.owner_epoch && !owner_bound(e)) {
                    __auto_type epoch = e->sample.owner_epoch;
                    {
                        for (unsigned range_index_19 = 0; range_index_19 < sizeof(source_state().bindings) / sizeof(source_state().bindings)[0]; ++range_index_19) {
                            __auto_type b = &(source_state().bindings[range_index_19]);
                            if (b->epoch == epoch) (*b) = (Binding) {0};
                        }
                    }
                    entry_drop(e);
                    (*e) = (Entry) {0}; /* Weak control blocks may be deallocated on this worker only. */
                    ++reclaimed;
                }
            }
        }
        return reclaimed;
    }
}
int azsdk_sources_physical_ready() {
    return atomic_load_explicit(&(source_state().enabled), memory_order_acquire) && atomic_load_explicit(&(source_state().physical_prepared), memory_order_acquire);
}
#if defined(__linux__)
static bool descriptor_stat_matches(int descriptor, uint64_t device, uint64_t inode, struct stat *out) {
    return !metadata_ops.stat(descriptor, out) && S_ISREG(out->st_mode) &&
           (uint64_t) out->st_dev == device && (uint64_t) out->st_ino == inode;
}
static bool descriptor_stat_unchanged(int descriptor, const struct stat *before) {
    struct stat after;
    return !metadata_ops.stat(descriptor, &after) && before->st_dev == after.st_dev && before->st_ino == after.st_ino;
}
static bool source_physical_identity(const struct azsdk_source_sample *sample, uint64_t mount, uint64_t device, uint64_t inode) {
    return sample->mount_id == mount && sample->device == device && sample->inode == inode;
}
#endif
int azsdk_sources_descriptor_identity(int descriptor, uint64_t mount_id,
                                      uint64_t device, uint64_t inode) {
#if defined(__linux__)
    struct stat before;
    uint64_t actual_mount = 0;
    return descriptor >= 0 && mount_id && inode && descriptor_stat_matches(descriptor, device, inode, &before) &&
           descriptor_mount_id(descriptor, &metadata_ops, &actual_mount) && actual_mount == mount_id &&
           descriptor_stat_unchanged(descriptor, &before);
#else
    (void) descriptor;
    (void) mount_id;
    (void) device;
    (void) inode;
    return 0;
#endif
}
int azsdk_sources_reader_fd(void *connection, uint64_t mount_id,
                            uint64_t device, uint64_t inode) {
#if defined(__linux__)
    if (!connection || !mount_id || !inode || !azsdk_sources_physical_ready()) return -1;
    struct azsdk_source_sample actual = {0};
    int fd = -1;
    if (capture_physical(((uintptr_t) (connection)), &(source_state().sql_profile), &(metadata_ops), &(actual), &fd) == AZS_PHYSICAL_OK &&
        source_physical_identity(&actual, mount_id, device, inode)) return fd;
#else
    (void) connection;
    (void) mount_id;
    (void) device;
    (void) inode;
#endif
    return -1;
}
int azsdk_sources_install_connections(
    azsdk_source_connection_installer installer, void *context) {
    __auto_type state = &(source_state());
    const int build = atomic_load_explicit(&(state->bound_build), memory_order_acquire);
    if (!installer || build < 1 || build > 2) return 0;
    /* install() prepares/reuses the sealed exact-module proof before either
     * redirect. Never hold registry_mutex across installer/native callbacks.
     * This canonical State has process lifetime; source configure/disable does
     * not reset it or turn off an already reachable close observer. */
    return az_connection_install(&state->connection_epochs, (struct az_connection_installer) {installer, context});
}

#if defined(AZSDK_CACHE_BIRTH_TESTING)
/* Test-only adapter for the fixture contract declared before this translation
 * unit. Every ownership and generation decision stays in the real helpers. */
struct CacheTestConnection {
    void *value, *output;
};
static int cache_test_open(void *v) {
    struct CacheTestConnection *c = v;
    c->output = c->value;
    return 0;
}
static int cache_test_close(void *v) {
    (void) v;
    return 0;
}
void azsdk_cache_test_connection_event(void *connection, int open) {
    if (open) {
        struct CacheTestConnection c = {connection, NULL};
        az_connection_forward_open(&source_state().connection_epochs, &c.output, cache_test_open, &c, NULL);
    } else az_connection_forward_close(&source_state().connection_epochs, connection, cache_test_close, NULL);
}
static SourceBorrow cache_test_source(const Scope *scope) {
    SourceBorrow source = {scope->owned_server, scope->build, scope->source_epoch, NULL, NULL};
    if (scope->list_server) {
        source.mount = NATIVE_OWNER(&scope->list_server->mount);
        if (source.mount->object) source.database = NATIVE_OWNER(&((SqliteMountPoint *) source.mount->object)->database);
    }
    return source;
}
static Build cache_test_profile(const Scope *scope) {
    if (!scope->owned_server || scope->build < 1 || scope->build > 2) return (Build) {0};
    uintptr_t table = load(uintptr_t, (uintptr_t) scope->owned_server);
    return (Build) {builds[scope->build].dispatcher, load(uintptr_t, table + 5 * 8) - source_state().main_image.base};
}
void azsdk_cache_test_forward_library(unsigned build, void *server, void (*original)(void *), void *context) {
    Scope scope = {build, 0, server, NULL, 0};
    Build profile = cache_test_profile(&scope);
    cache_forward_library_impl(build, server, &profile, (Forward) {original, context});
}
void azsdk_cache_test_forward_guarded_library(unsigned build, void *server, void (*original)(void *), void *context) {
    Scope scope = {build, 0, server, NULL, 0};
    Build profile = cache_test_profile(&scope);
    cache_forward_library_route(build, server, &profile, (Forward) {original, context});
}
static void cache_test_weak(CacheOwner *out, const WeakOwner *value) {
    CacheOwner view;
    memcpy(&view, value, sizeof view);
    weak_set(out, view);
}
static void cache_test_publish(const Observation *observed, Witness *out) {
    if (!out) return;
    out->birth_epoch = observed->birth_epoch;
    out->layout_generation = observed->layout_generation;
    out->source_epoch = observed->source_epoch;
    cache_test_weak(&out->mount, &observed->mount);
    cache_test_weak(&out->database, &observed->database);
    cache_test_weak(&out->collector, &observed->collector);
    cache_test_weak(&out->cache, &observed->cache);
    out->row_value = observed->row_value;
}
static void cache_test_forward_birth(void *v) { birth_call_forward(v); }
static void cache_test_forward_query(void *v) { query_call_forward(v); }
static bool cache_test_copy_row(void *v, uint32_t *value) {
    const QueryCall *call = v;
    const NativeRows *rows = call->result_storage;
    if (!rows->begin || rows->end != rows->begin + 1 || rows->capacity != rows->end || !rows->begin[0]) return false;
    *value = rows->begin[0]->value;
    if (call->copy_boundary) call->copy_boundary(call->copy_context);
    return true;
}
static void cache_test_reset(const Limits *limits) {
    SourceState *s = &source_state();
    AZ_MUTEX_GUARD(lock, s->registry_mutex, false);
    if (!lock.held) return;
    if (atomic_load_explicit(&s->cache_active, memory_order_acquire)) {
        atomic_store_explicit(&s->cache_saturated, true, memory_order_release);
        return;
    }
    for (size_t i = 0; i < kSlots; ++i) {
        cache_binding_drop(&s->cache_bindings[i]);
        s->cache_bindings[i] = (CacheBinding) {0};
    }
    s->cache_limit = limits->slots <= kSlots ? limits->slots : kSlots;
    s->next_cache_birth = limits->next_birth;
    atomic_store_explicit(&s->cache_generation, limits->generation, memory_order_release);
    atomic_store_explicit(&s->cache_active, 0, memory_order_release);
    atomic_store_explicit(&s->cache_depth_limit, limits->depth <= kSlots ? limits->depth : kSlots, memory_order_release);
    atomic_store_explicit(&s->cache_saturated, !limits->generation || !limits->depth || !limits->slots, memory_order_release);
}
static int cache_test_birth(const Scope *scope, const BirthCall *call, Witness *out) {
    BirthBorrow borrow = {cache_test_source(scope), scope->list_server ? NATIVE_OWNER(&scope->list_server->collector) : NULL,
                          call->creator && *call->creator ? NATIVE_OWNER(&(*call->creator)->database) : NULL,
                          call->collector, call->creator ? *call->creator : NULL, call->collector ? call->collector->pool : NULL, NATIVE_OWNER(call->result_storage)};
    Observation observed __attribute__((cleanup(azcb_observation_drop))) = {0};
    Build profile = cache_test_profile(scope);
    int result = cache_birth_impl(&borrow, &profile, (Forward) {cache_test_forward_birth, (void *) call}, &observed);
    if (result == AZS_OK) cache_test_publish(&observed, out);
    return result;
}
static int cache_test_query(const Scope *scope, const QueryCall *call, Witness *out) {
    const CacheOwner *selected = NULL;
    if (scope->list_server) {
        const ClientVector *clients = &scope->list_server->clients;
        if (clients->begin && scope->client_bank < (size_t) (clients->end - clients->begin)) {
            const OwnerVector *bank = clients->begin + scope->client_bank;
            if (bank->begin != bank->end) selected = bank->begin;
        }
    }
    QueryBorrow borrow = {cache_test_source(scope), scope->list_server ? NATIVE_OWNER(&scope->list_server->collector) : NULL, NATIVE_OWNER(selected), call->adapter ? call->adapter->cache : NULL};
    Observation observed __attribute__((cleanup(azcb_observation_drop))) = {0};
    Build profile = cache_test_profile(scope);
    int result = cache_query_impl(&borrow, &profile, (Forward) {cache_test_forward_query, (void *) call}, (CopyRowScalar) {cache_test_copy_row, (void *) call}, &observed);
    if (result == AZS_OK) cache_test_publish(&observed, out);
    return result;
}
static void cache_test_mutation(Mutation kind, void (*original)(void *), void *context) {
    (void) kind;
    azcb_retire_forward((Forward) {original, context});
}
static Counters cache_test_counters(void) {
    SourceState *s = &source_state();
    AZ_MUTEX_GUARD(lock, s->registry_mutex, false);
    Counters result = {0};
    if (!lock.held) return result;
    for (size_t i = 0; i < kSlots; ++i)
        if (s->cache_bindings[i].birth_epoch) ++result.retained;
    result.next_birth = s->next_cache_birth;
    result.generation = atomic_load_explicit(&s->cache_generation, memory_order_acquire);
    result.active = atomic_load_explicit(&s->cache_active, memory_order_acquire);
    result.saturated = atomic_load_explicit(&s->cache_saturated, memory_order_acquire);
    return result;
}
__attribute__((visibility("hidden"))) const Api *azsdk_cache_birth_test_api_v1(void) {
    static const Api api = {1, cache_test_reset, cache_test_birth, cache_test_query, cache_test_mutation, cache_test_counters};
    return &api;
}
#endif




typedef struct ReplyCodeProfile ReplyCodeProfile;
struct ReplyCodeProfile {
    unsigned build;
    uintptr_t begin, return_pc;
    size_t length;
    char sha256[65];
};
#ifdef AZSDK_CACHE_BIRTH_TESTING
_Thread_local ReplyCodeProfile reply_test_code;
#endif
static bool reply_capture_enabled() {
    return atomic_load_explicit(&(source_state().enabled), memory_order_acquire) &&
           azsdk_cache_capture_ready && azsdk_cache_capture_ready();
}
static bool reply_generated_code(unsigned build, ReplyCodeProfile *code) {
    struct azsdk_cache_reply_code generated;
    if (!azsdk_cache_reply_code_profile((int) build, &generated)) return false;
    uintptr_t base = sources.main_image.base;
    if (generated.begin_rva > UINTPTR_MAX - base || generated.return_rva > UINTPTR_MAX - base) return false;
    code->build = build;
    code->begin = base + generated.begin_rva;
    code->return_pc = base + generated.return_rva;
    code->length = generated.length;
    memcpy(code->sha256, generated.sha256, sizeof code->sha256);
    return true;
}
static bool reply_code_range_valid(const ReplyCodeProfile *code) {
    return code->begin && !(code->begin & 3u) && !(code->return_pc & 3u) && code->length >= 4 && code->length <= 65536 &&
           code->length <= UINTPTR_MAX - code->begin && code->return_pc >= code->begin + 4 && code->return_pc <= code->begin + code->length;
}
static bool reply_code_verified(const ReplyCodeProfile *code, unsigned build) {
    if (code->build != build || !reply_code_range_valid(code) || code->sha256[64] ||
        !image_contains(&sources.main_image, code->begin, code->length, kRead | kExecute)) return false;
    char digest[65];
    azsha256_hex((const void *) code->begin, code->length, digest);
    return !memcmp(digest, code->sha256, sizeof digest);
}
static bool reply_code_profile(ReplyCodeProfile *out) {
    if (!reply_capture_enabled()) return false;
    const unsigned build = (unsigned) (atomic_load_explicit(&(source_state().bound_build), memory_order_acquire));
    ReplyCodeProfile code = {0};
#ifdef AZSDK_CACHE_BIRTH_TESTING
    if (reply_test_code.build) code = reply_test_code;
    else
#endif
        if (!reply_generated_code(build, &code))
        return false;
    if (!reply_code_verified(&code, build)) return false;
    (*out) = code;
    return true;
}

/* This is a GNU weak-owner ABI view of the genuine tag2 ListenerReference
 * member, not a recovered weak<Reception> nominal type. Same-type copying of
 * this view preserves its alias/control and weak count without a converting
 * weak constructor's possible early promotion. The pointee is never accessed
 * through this erased type or retained as a raw pointer. */
struct ReplyWeakAbiPointee;
typedef WeakOwner ReplyWeakAbi;
_Static_assert(sizeof(ReplyWeakAbi) == 16 && _Alignof(ReplyWeakAbi) == 8,
               "reviewed native tag2 GNU weak owner ABI");

struct ReplyCallbackScope;
static _Thread_local struct ReplyCallbackScope *reply_callback = NULL;
typedef struct ReplyCallbackScope ReplyCallbackScope;
struct ReplyCallbackScope {
    ReplyCallbackScope *parent;
    const void *invocation;
    CallbackReceipt receipt;
    bool ready;
};
static void reply_scope_init(ReplyCallbackScope *ctx);
static void reply_scope_drop(ReplyCallbackScope *ctx);
static void reply_scope_init(ReplyCallbackScope *ctx) {
    (void) ctx;
    *ctx = (ReplyCallbackScope) {.invocation = NULL, .ready = false, .parent = reply_callback};
    reply_callback = ctx;
}
static void reply_scope_drop(ReplyCallbackScope *ctx) {
    (void) ctx;
    reply_callback = ctx->parent;
    azcr_callback_receipt_drop(&ctx->receipt);
}


typedef struct TicketJoin TicketJoin;
struct TicketJoin {
    bool enabled;
};
static void ticket_join_init(TicketJoin *ctx, bool value);
static void ticket_join_drop(TicketJoin *ctx);
static void ticket_join_init(TicketJoin *ctx, bool value) {
    (void) ctx;
    *ctx = (TicketJoin) {.enabled = value};
}
static void ticket_join_drop(TicketJoin *ctx) {
    (void) ctx;
    if (ctx->enabled) azjs_ticket(0, 0, 0);
}

static bool reply_call_valid(const struct azce_call *call) {
    return !call->reserved && !call->caller_reserved;
}
static const ReplyWeakAbi *reply_ticket_receiver(const struct azce_call *call, Request *request, uintptr_t *return_pc) {
    ReplyCodeProfile code = {0};
    if (!call || !reply_call_valid(call) || !reply_code_profile(&code)) return NULL;
    uintptr_t ticket = (uintptr_t) call->args[0];
    if (!checked_vtable(ticket, "N13music_library21LoadInfoRequestTicketE", 14) || load(unsigned char, ticket + 0x80) != 2) return NULL;
    uintptr_t packet = load(uintptr_t, ticket + 0x28);
    if (!canonical(packet)) return NULL;
    memcpy(request->request_id, (const void *) (ticket + 8), sizeof request->request_id);
    request->sequence = load(uint32_t, packet + 8);
    request->reply_kind = load(uint16_t, packet);
    *return_pc = code.return_pc;
    return (const ReplyWeakAbi *) (ticket + 0x88);
}
void azcr_forward_ticket_owned(struct azce_call *call, Forward original) {
    if (!original.call) return;
    ReplyWeakAbi empty __attribute__((cleanup(az_native_weak_reset))) = {0};
    const ReplyWeakAbi *receiver = &empty;
    Request request = {0};
    uintptr_t return_pc = 0;
    uintptr_t ticket = call ? (uintptr_t) call->args[0] : 0;
    /* Copy fields before forwarding, while the actual owning queue keeps the
     * Ticket live. Native work may destroy or replace both Ticket and packet. */
    const ReplyWeakAbi *borrowed = reply_ticket_receiver(call, &request, &return_pc);
    if (borrowed) receiver = borrowed;
    TicketJoin joining __attribute__((cleanup(ticket_join_drop)));
    ticket_join_init(&joining, azjs_ticket != NULL);
    if (joining.enabled) {
        uintptr_t packet = receiver == &empty ? 0 : load(uintptr_t, ticket + 0x28);
        azjs_ticket(ticket, packet, 1);
    }
    // Ineligible/nested entries still mask an outer scope while forwarding.
    ReplyScope selected __attribute__((cleanup(azcr_scope_drop)));
    azcr_scope_init(&selected, receiver, &request, return_pc);
    receiver = NULL;
    azcr_forward_owned(&selected, original);
    if (joining.enabled && call && azjs_ticket_return) azjs_ticket_return(ticket, call->result[0]);
}

static void reply_reception_capture(const struct azce_call *call, ReplyCallbackScope *callback) {
    ReplyCodeProfile code = {0};
    __auto_type receiver = call ? (uintptr_t) (call->args[0]) : 0;
    if (call && reply_call_valid(call) && reply_code_profile(&(code)) &&
        call->caller_link == code.return_pc) {
        Promotion native = {0};
        native.control = call->caller_x21;
        native.caller_link = call->caller_link;
        if (azcr_receive_current_owned(&native, ((const void *) (receiver)), &callback->receipt.selected) &&
            checked_vtable(receiver, "N17trackinfo_stocker22LoadInfoRequestHandler9ReceptionE", 3)) {
            /* Native slot13's checked code keeps its actual X21 strong lease
             * until after this callback returns. The helper's temporary lock
             * is already released. These two copied fields confer no Stocker
             * ownership and are not a player load ID/source association. */
            callback->receipt.pack_id = load(uint32_t, receiver + 8);
            callback->receipt.borrowed_stocker = load(uintptr_t, receiver + 0x10);
            callback->invocation = azcr_current_invocation();
            callback->ready = true;
        }
    }
}
static void reply_reception_join(const struct azce_call *call, const ReplyCallbackScope *callback) {
    if (azjs_reception && call && checked_vtable((uintptr_t) (call->args[0]), "N17trackinfo_stocker22LoadInfoRequestHandler9ReceptionE", 3)) {
        __auto_type receiver = (uintptr_t) (call->args[0]);
        /* Foreign/unpromoted replies revoke any older pending claim for that pack. */
        azjs_reception(callback->ready ? receiver : 0, load(uint32_t, receiver + 8),
                       ((const void *) (call->args[1])));
    }
}
void azcr_forward_reception_owned(struct azce_call *call, Forward original) {
    if (!original.call) return;
    ReplyCallbackScope callback __attribute__((cleanup(reply_scope_drop)));
    reply_scope_init(&callback); // foreign/reentrant callbacks mask older receipts
    reply_reception_capture(call, &callback);
    reply_reception_join(call, &callback);
    original.call(original.context);
}

bool azcr_current_callback_owned(CallbackReceipt *out) {
    __auto_type callback = reply_callback;
    if (!callback || !callback->ready || !reply_capture_enabled() ||
        callback->invocation != azcr_current_invocation()) return false;
    azcr_callback_receipt_copy(out, &callback->receipt);
    return true;
}


#ifdef AZSDK_CACHE_BIRTH_TESTING
__attribute__((visibility("hidden"))) void azsdk_cache_test_reply_profile(
    unsigned build, uintptr_t begin, uintptr_t return_pc, size_t length, const char *sha256) {
    reply_test_code = (ReplyCodeProfile) {0};
    if (!build || !sha256) return;
    reply_test_code.build = build;
    reply_test_code.begin = begin;
    reply_test_code.return_pc = return_pc;
    reply_test_code.length = length;
    memcpy(reply_test_code.sha256, sha256, sizeof reply_test_code.sha256);
}
#endif
