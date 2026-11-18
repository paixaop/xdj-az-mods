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
static int find_image(struct dl_phdr_info *info, size_t unused, void *context) {
    (void) unused;
    __auto_type s = &(*((ImageSearch *) (context)));
    if (s->main ? (info->dlpi_name && *info->dlpi_name) : info->dlpi_addr != s->image->base) return 0;
    s->image->base = info->dlpi_addr;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        __auto_type p = &(info->dlpi_phdr[i]);
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_R)) continue;
        if (s->image->count == 32 || p->p_vaddr > UINTPTR_MAX - info->dlpi_addr ||
            p->p_memsz > UINTPTR_MAX - info->dlpi_addr - p->p_vaddr) {
            s->image->count = 0;
            return 1;
        }
        s->image->ranges[s->image->count++] = (Range) {info->dlpi_addr + p->p_vaddr,
                                                       info->dlpi_addr + p->p_vaddr + p->p_memsz, p->p_flags};
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
static uintptr_t checked_vtable(uintptr_t object, const char *name, unsigned slots) {
    if (!canonical(object)) return 0;
    __auto_type table = load(uintptr_t, object);
    if (!canonical(table) || table < 16 ||
        !image_contains(&source_state().main_image, table - 16, (slots + 2u) * 8u, kRead) ||
        load(uintptr_t, table - 16)) return 0;
    __auto_type type = load(uintptr_t, table - 8);
    if (!canonical(type) || !image_contains(&source_state().main_image, type, 16, kRead)) return 0;
    __auto_type text = load(uintptr_t, type + 8);
    if (!image_contains(&source_state().main_image, text, 1, kRead)) return 0;
    if (*((const char *) (text)) == '*') ++text;
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
static bool owned_chain(uintptr_t server, const Build *build, OwnedChain *out) {
    __auto_type dispatcher = load(uintptr_t, server + build->dispatcher);
    if (!canonical(dispatcher)) return false;
    __auto_type handler = load(uintptr_t, dispatcher + 0x20);
    if (!canonical(handler)) return false;
    __auto_type updater = load(uintptr_t, handler + 0x30);
    if (!checked_vtable(updater, "N13music_library18InformationUpdaterE", 1)) return false;
    __auto_type mount = load(uintptr_t, updater + 8);
    if (!checked_vtable(mount, "N13music_library16SqliteMountPointE", 1)) return false;
    __auto_type database = load(uintptr_t, mount + 0x30);
    if (!checked_vtable(database, "N13music_library14SqliteDataBaseE", 1)) return false;
    __auto_type media = load(uint16_t, database + 8);
    __auto_type connection = load(uintptr_t, database + 0x38);
    if (!media || load(uint16_t, mount + 8) != media || !canonical(connection) ||
        !canonical(load(uintptr_t, updater + 0x10)) ||
        !canonical(load(uintptr_t, mount + 0x38))) return false;
    /* Borrow the REAL source references in the verified serialized native worker scope.
     * Never memcpy ownership words into a shared/weak object or fabricate an owner. */
    __auto_type m = &(*((const StrongOwner *) (updater + 8)));
    __auto_type d = &(*((const StrongOwner *) (mount + 0x30)));
    if (((uintptr_t) (m->object)) != mount || ((m->control) ? __atomic_load_n(&m->control->strong, __ATOMIC_RELAXED) : 0) <= 0 ||
        ((uintptr_t) (d->object)) != database || ((d->control) ? __atomic_load_n(&d->control->strong, __ATOMIC_RELAXED) : 0) <= 0) return false;
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
static int open_metadata(const char *path, int flags) { return open(path, flags); }
static const MetadataOps metadata_ops = {fstat, open_metadata, read, close};
/* Complete bounded fdinfo text only: reject duplicate/truncated/overflowed mount IDs. */
static bool parse_mount_id(const char *data, size_t size, uint64_t *out) {
    bool found = false;
    uint64_t value = 0;
    for (size_t start = 0; start < size;) {
        size_t end = start;
        while (end < size && data[end] != '\n') ++end;
        if (end == size) return false;
        if (end - start >= 7 && !memcmp(data + start, "mnt_id:", 7)) {
            if (found) return false;
            size_t at = start + 7;
            while (at < end && (data[at] == ' ' || data[at] == '\t')) ++at;
            if (at == end) return false;
            value = 0;
            for (; at < end && data[at] >= '0' && data[at] <= '9'; ++at) {
                unsigned digit = ((unsigned) (data[at] - '0'));
                if (value > (UINT64_MAX - digit) / 10) return false;
                value = value * 10 + digit;
            }
            while (at < end && (data[at] == ' ' || data[at] == '\t')) ++at;
            if (at != end || !value) return false;
            found = true;
        }
        start = end + 1;
    }
    if (found) (*out) = value;
    return found;
}
static bool descriptor_mount_id(int native_fd, const MetadataOps *ops, uint64_t *out) {
    char path[64], text[4096];
    int length = snprintf(path, sizeof path, "/proc/self/fdinfo/%d", native_fd);
    if (length < 0 || ((size_t) (length)) >= sizeof path) return false;
    int fd = ops->open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    size_t used = 0;
    bool complete = false;
    /* At most 8 reads and 4096 bytes. No unbounded EINTR/short-read retry. */
    for (unsigned attempt = 0; attempt < 8 && used < sizeof text; ++attempt) {
        ssize_t n = ops->read(fd, text + used, sizeof text - used);
        if (n < 0) break;
        if (!n) {
            complete = true;
            break;
        }
        if (((size_t) (n)) > sizeof text - used) break;
        used += ((size_t) (n));
    }
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
static int capture_physical(uintptr_t connection, const SqlProfile *p, const MetadataOps *ops, struct azsdk_source_sample *out, int *descriptor) {
    if (!p->ready || !p->file_control) return AZS_PHYSICAL_PROFILE;
    NativeFile *file = NULL;
    NativeVFS *vfs = NULL;
    if (p->file_control(((struct sqlite3 *) (connection)), "main", 7, &file) || !file ||
        p->file_control(((struct sqlite3 *) (connection)), "main", 27, &vfs) || !vfs)
        return AZS_PHYSICAL_API;
    /* File liveness comes from the owned native callback. VFS/table/name/code are bounded
     * to the exact hashed DSO; wrappers and unrelated SQLite modules fail closed. */
    if (!canonical(((uintptr_t) (file))) ||
        !canonical(((uintptr_t) (vfs))) ||
        !image_contains(&p->image, ((uintptr_t) (vfs)), sizeof *vfs, kRead) ||
        vfs->version != 3 || vfs->os_size != 120 || vfs->max_path != 512 ||
        !supported_name(p, vfs->name) ||
        !module_function(p, ((uintptr_t) (vfs->open)), p->open_rva))
        return AZS_PHYSICAL_VFS;
    const NativeIO *io = file->methods;
    __auto_type table = ((uintptr_t) (io));
    if (!canonical(table) || !image_contains(&p->image, table, sizeof *io, kRead) || table < p->image.base)
        return AZS_PHYSICAL_METHODS;
    __auto_type relative = table - p->image.base;
    unsigned slot = 3;
    for (unsigned i = 0; i < 3; ++i)
        if (relative == p->io_rvas[i]) {
            slot = i;
            break;
        }
    if (slot == 3 || io->version != (slot == 2 ? 1 : 3) ||
        !module_function(p, ((uintptr_t) (io->read)), p->read_rva) ||
        !module_function(p, ((uintptr_t) (io->size)), p->size_rva) ||
        !module_function(p, ((uintptr_t) (io->control)), p->control_rva))
        return AZS_PHYSICAL_METHODS;
    __auto_type address = ((uintptr_t) (file));
    __auto_type embedded = load(uintptr_t, address + 8);
    int native_fd = load(int32_t, address + 0x18);
    if (embedded != ((uintptr_t) (vfs)) || native_fd < 0)
        return AZS_PHYSICAL_DESCRIPTOR;
    struct stat before, after;
    int64_t size = -1;
    uint64_t mount = 0;
    if (ops->stat(native_fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        !descriptor_mount_id(native_fd, &((*ops)), &(mount)) || io->size(file, &size) ||
        ops->stat(native_fd, &after)) return AZS_PHYSICAL_METADATA;
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || size != before.st_size ||
        load(int32_t, address + 0x18) != native_fd || file->methods != io ||
        load(uintptr_t, address + 8) != embedded) return AZS_PHYSICAL_CHANGED;
    out->device = ((uint64_t) (before.st_dev));
    out->inode = ((uint64_t) (before.st_ino));
    out->mount_id = mount;
    out->file_size = ((uint64_t) (size));
    out->physical_observed_ns = now_ns();
    out->flags |= AZS_PHYSICAL;
    if (descriptor) *descriptor = native_fd;
    return AZS_PHYSICAL_OK;
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
static int observe(uintptr_t server, const Build *build, const SqlProfile *sql,
#if defined(__linux__)
                   const MetadataOps *ops,
#endif
                   void *out, size_t out_size) {
    if (!worker_scope(server, &((*build)))) return AZS_GUARD;
    OwnedChain chain = {0};
    bool supported = owned_chain(server, &((*build)), &(chain));
    /* Stable local pins survive capture reentrancy; the holders themselves are
     * freshly resolved below, never retained across a native callback. Actual
     * complete-receiver lifetime and serialization remain caller preconditions. */
    StrongOwner mount __attribute__((cleanup(az_native_owner_reset))) = {0};
    StrongOwner database __attribute__((cleanup(az_native_owner_reset))) = {0};
    if (supported) {
        az_native_owner_copy(&mount, &(*chain.mount));
        az_native_owner_copy(&database, &(*chain.database));
    }
    AZ_MUTEX_GUARD(lock, source_state().registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    if (!atomic_load_explicit(&(source_state().enabled), memory_order_acquire)) return AZS_UNSUPPORTED;
    if (!supported) {
        retire_server(server);
        return AZS_GUARD;
    }
    __auto_type observed_generation = connection_generation(chain.connection);
    __auto_type status_generation = az_connection_status_generation(&source_state().connection_epochs,
                                                                    ((const void *) (chain.connection)));
    Binding *binding = NULL;
    {
        for (unsigned range_index_5 = 0; range_index_5 < sizeof(source_state().bindings) / sizeof(source_state().bindings)[0]; ++range_index_5) {
            __auto_type b = &(source_state().bindings[range_index_5]);
            if (b->server == server) {
                binding = b;
                break;
            }
        }
    }
    if (!binding) {
        for (unsigned range_index_6 = 0; range_index_6 < sizeof(source_state().bindings) / sizeof(source_state().bindings)[0]; ++range_index_6) {
            __auto_type b = &(source_state().bindings[range_index_6]);
            if (!b->server) {
                binding = b;
                break;
            }
        }
    }
    Entry *entry = NULL, *empty = NULL;
    bool was_observed = false, status_was_observed = false;
    {
        for (unsigned range_index_7 = 0; range_index_7 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_7) {
            __auto_type e = &(source_state().entries[range_index_7]);

            if (!e->sample.owner_epoch) {
                if (!empty) empty = e;
                continue;
            }
            if (e->mount_address != chain.mount_address ||
                e->database_address != chain.database_address || e->connection != chain.connection ||
                e->sample.native_media_id != chain.media) continue;
            if (!same_owner(&(e->mount), &(mount)) || !same_owner(&(e->database), &(database))) continue;
            if (e->connection_generation) was_observed = true;
            if (e->status_connection_generation) status_was_observed = true;
            if (!entry && e->connection_generation == observed_generation &&
                e->status_connection_generation == status_generation && owner_bound(e)) entry = e;
        }
    }
    if ((!observed_generation && was_observed) || (!status_generation && status_was_observed)) {
        /* Unavailability cannot downgrade a known incarnation or permanently
         * retire its key. It may be unrelated active lifecycle work. */
        return AZS_STALE;
    }
    if (!binding || (!entry && (!empty || source_state().next_epoch == UINT64_MAX)) || source_state().next_revision == UINT64_MAX) {
        /* A valid but unrecorded owner must never become a later 'unique' media match. */
        set_bit(source_state().seen_media, chain.media);
        set_bit(source_state().ambiguous_media, chain.media);
        retire_server(server);
        return AZS_LIMIT;
    }
    Entry candidate __attribute__((cleanup(entry_drop))) = {0};
    if (!entry) {
        az_native_weak_copy(&candidate.mount, &(mount)); /* Genuine weak construction, no persistent strong retention. */
        az_native_weak_copy(&candidate.database, &(database));
        candidate.mount_address = chain.mount_address;
        candidate.database_address = chain.database_address;
        candidate.connection = chain.connection;
        candidate.connection_generation = observed_generation;
        candidate.status_connection_generation = status_generation;
    }
    struct azsdk_source_sample sample = {0};
    sample.struct_size = sizeof sample;
    sample.flags = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE;
    sample.owner_epoch = entry ? entry->sample.owner_epoch : source_state().next_epoch + 1;
    sample.revision = source_state().next_revision + 1;
    sample.observed_ns = now_ns();
    sample.native_media_id = chain.media;
    sample.physical_status = AZS_PHYSICAL_DISABLED;
    if (bit(source_state().ambiguous_media, chain.media)) sample.flags |= AZS_MEDIA_AMBIGUOUS;
    if (atomic_load_explicit(&(source_state().capture_policy), memory_order_relaxed) & AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER) {
#if defined(__linux__)
        sample.physical_status = capture_physical(chain.connection, sql, ops, &(sample), NULL);
#else
        (void) (*sql);
        sample.physical_status = AZS_PHYSICAL_PROFILE;
#endif
    }
    /* Physical capture can execute native callbacks. Re-resolve the actual
     * holders after it, compare against the stable pins, and preserve the
     * original incarnation stamp instead of adopting the latest return stamp.
     * Registry identity/binding/sample publication has not occurred yet. */
    OwnedChain current = {0};
    if (!worker_scope(server, &((*build))) || !owned_chain(server, &((*build)), &(current)) ||
        current.mount_address != chain.mount_address || current.database_address != chain.database_address ||
        current.connection != chain.connection || current.media != chain.media ||
        (current.mount->control != mount.control) || (mount.control != current.mount->control) ||
        (current.database->control != database.control) || (database.control != current.database->control) ||
        !atomic_load_explicit(&(source_state().enabled), memory_order_acquire)) {
        retire_server(server);
        return AZS_STALE;
    }
    /* A receipt-only refusal is transient/unknown, not retirement evidence.
     * Actual close/reopen can never make the old positive stamp current again;
     * live() continues to reject it without discarding another source's key. */
    if (connection_generation(chain.connection) != observed_generation ||
        az_connection_status_generation(&source_state().connection_epochs, ((const void *) (chain.connection))) !=
            status_generation) return AZS_STALE;
    if (!entry) {
        if (bit(source_state().seen_media, chain.media)) set_bit(source_state().ambiguous_media, chain.media);
        set_bit(source_state().seen_media, chain.media);
        candidate.sample = sample;
        entry = empty;
        entry_drop(entry);
        *entry = candidate;
        candidate = (Entry) {0};
        source_state().next_epoch = sample.owner_epoch;
    }
    if (bit(source_state().ambiguous_media, chain.media)) sample.flags |= AZS_MEDIA_AMBIGUOUS;
    entry->sample = sample;
    source_state().next_revision = sample.revision;
    binding->server = server;
    binding->epoch = sample.owner_epoch;
    retire_unused();
    copy_output(out, out_size, &(sample));
    return AZS_OK;
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
static bool cache_source_is(const SourceBorrow *source, const CachePins *pins, bool for_status) {
    bool bound = false;
    {
        for (unsigned range_index_9 = 0; range_index_9 < sizeof(source_state().bindings) / sizeof(source_state().bindings)[0]; ++range_index_9) {
            __auto_type binding = &(source_state().bindings[range_index_9]);
            if (binding->server == ((uintptr_t) (source->server)) &&
                binding->epoch == source->source_epoch) {
                bound = true;
                break;
            }
        }
    }
    const Entry *entry = cache_source_entry(source->source_epoch);
    if (!bound || !entry || (for_status ? !status_live(entry) : (!entry->connection_generation || !live(entry))) ||
        !pins->mount.object || !pins->database.object ||
        entry->mount_address != ((uintptr_t) (pins->mount.object)) ||
        entry->database_address != ((uintptr_t) (pins->database.object)) ||
        !same_owner(&(entry->mount), &(pins->mount)) || !same_owner(&(entry->database), &(pins->database))) return false;
    /* Matching control blocks do not freeze the DB's mutable source tuple.
     * Read only the native prefixes of these actual callback-owned pins, after
     * exact owner/alias validation, and compare the tuple used by observe().
     * No connection is followed and no epoch is renewed by this check. */
    __auto_type mount = ((uintptr_t) (pins->mount.object));
    __auto_type database = ((uintptr_t) (pins->database.object));
    return entry->connection == load(uintptr_t, database + 0x38) &&
           entry->sample.native_media_id == load(uint16_t, database + 8) &&
           entry->sample.native_media_id == load(uint16_t, mount + 8) &&
           connection_generation(entry->connection) == entry->connection_generation;
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
static int cache_prepare_source(const SourceBorrow *source, const Build *profile, const StrongOwner *collector, CachePins *pins, uint64_t *generation) {
    __auto_type s = &(source_state());
    if (!atomic_load_explicit(&(s->enabled), memory_order_acquire) || source->build < 1 || source->build > 2 ||
        (int) (source->build) != atomic_load_explicit(&(s->bound_build), memory_order_acquire)) return AZS_UNSUPPORTED;
    if (!source->source_epoch || !source->mount || !source->database || !collector ||
        !cache_worker_is(source, &((*profile)))) return AZS_GUARD;
    /* These pointers are explicitly owned borrows from the current callback;
     * canonical-address/RTTI tests cannot make arbitrary shared_ptr memory safe. */
    if (!source->mount->object || !source->database->object || !collector->object || !cache_stamp(generation)) return AZS_GUARD;
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
struct cache_birth_impl_guard_1 {
    const BirthBorrow **capture_borrow;
    const Build **capture_profile;
    CachePins *capture_pins;
    uint64_t *capture_before;
    int *capture_status;
    StrongOwner *capture_creator;
};
static void cache_birth_impl_guard_1_call(void *opaque) {
    struct cache_birth_impl_guard_1 *guard = opaque;
    (void) guard;

    (*guard->capture_status) = cache_prepare_source(&((*guard->capture_borrow)->source), &((*(*guard->capture_profile))), (*guard->capture_borrow)->collector, &((*guard->capture_pins)), &((*guard->capture_before)));
    if ((*guard->capture_status) == AZS_OK) {
        if (!(*guard->capture_borrow)->creator_database || !(*guard->capture_borrow)->creator_database->object || !(*guard->capture_borrow)->native_result ||
            guard->capture_pins->collector.object != (*guard->capture_borrow)->receiver_collector) (*guard->capture_status) = AZS_GUARD;
        else {
            az_native_owner_copy(&(*guard->capture_creator), &(*(*guard->capture_borrow)->creator_database));
            if (guard->capture_creator->object != guard->capture_pins->database.object || (guard->capture_creator->control != guard->capture_pins->database.control) ||
                (guard->capture_pins->database.control != guard->capture_creator->control)) (*guard->capture_status) = AZS_GUARD;
        }
    }
}
struct cache_birth_impl_guard_2 {
    const BirthBorrow **capture_borrow;
    const Build **capture_profile;
    CachePins *capture_pins;
    CacheBirthScope *capture_construction;
    StrongOwner *capture_creator;
    Observation **capture_out;
    int result;
};
static int cache_birth_impl_guard_2_body(struct cache_birth_impl_guard_2 *guard) {
    (void) guard;

    __auto_type generation = guard->capture_construction->generation;
        /* Preserve the actual final-write stamp through factory cleanup and
         * native shared-owner publication. Never adopt the latest return stamp. */
    if (!guard->capture_construction->completed || guard->capture_construction->poisoned || !cache_stamp_is(generation) ||
        !cache_worker_is(&((*guard->capture_borrow)->source), &((*(*guard->capture_profile)))) ||
        !cache_borrow_source_unchanged(&((*guard->capture_borrow)->source), &((*guard->capture_pins))) ||
        !(*guard->capture_borrow)->collector->object || (*guard->capture_borrow)->collector->object != guard->capture_pins->collector.object ||
        ((*guard->capture_borrow)->collector->control != guard->capture_pins->collector.control) || (guard->capture_pins->collector.control != (*guard->capture_borrow)->collector->control) ||
        (*guard->capture_borrow)->creator_database->object != guard->capture_creator->object ||
        ((*guard->capture_borrow)->creator_database->control != guard->capture_creator->control) || (guard->capture_creator->control != (*guard->capture_borrow)->creator_database->control) ||
        !(*guard->capture_borrow)->native_result->object) return AZS_STALE;
    az_native_owner_copy(&guard->capture_pins->cache, &(*(*guard->capture_borrow)->native_result)); // actual successfully constructed native owner
    if (guard->capture_pins->cache.object != guard->capture_construction->raw_result) return AZS_STALE;
    __auto_type s = &(source_state());
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    if (!cache_stamp_is(generation) || !cache_source_is(&((*guard->capture_borrow)->source), &((*guard->capture_pins)), false)) return AZS_STALE;
    CacheBinding *slot = NULL;
    for (unsigned i = 0; i < s->cache_limit; ++i) {
        __auto_type binding = &(s->cache_bindings[i]);
        if (binding->birth_epoch && same_owner(&(binding->cache), &(guard->capture_pins->cache))) return AZS_STALE;
        if (!slot && !cache_binding_retained(binding, generation)) slot = binding;
    }
    if (!slot || s->next_cache_birth == UINT64_MAX) return AZS_LIMIT;
    CacheBinding candidate __attribute__((cleanup(cache_binding_drop))) = {0};
    az_native_weak_copy(&candidate.mount, &(guard->capture_pins->mount));
    az_native_weak_copy(&candidate.database, &(guard->capture_pins->database));
    az_native_weak_copy(&candidate.collector, &(guard->capture_pins->collector));
    az_native_weak_copy(&candidate.cache, &(guard->capture_pins->cache));
    candidate.source_epoch = (*guard->capture_borrow)->source.source_epoch;
    candidate.generation = generation;
    candidate.birth_epoch = s->next_cache_birth + 1;
    candidate.factory_kind = guard->capture_construction->factory_kind;
    if (!cache_stamp_is(generation) || !cache_source_is(&((*guard->capture_borrow)->source), &((*guard->capture_pins)), false)) return AZS_STALE;
    cache_binding_drop(slot);
    *slot = candidate;
    candidate = (CacheBinding) {0}; // reclaim stale weak controls only in this owned worker
    s->next_cache_birth = slot->birth_epoch;
    if ((*guard->capture_out)) {
        azcb_observation_drop((*guard->capture_out));
        *(*guard->capture_out) = cache_observation(slot);
    }
    return AZS_OK;
}
static void cache_birth_impl_guard_2_call(void *opaque) {
    struct cache_birth_impl_guard_2 *guard = opaque;
    guard->result = cache_birth_impl_guard_2_body(guard);
}
static int cache_birth_impl(const BirthBorrow *borrow, const Build *profile, Forward original, Observation *out) {
    if (!original.call) return AZS_INVALID;
    CachePins pins __attribute__((cleanup(cache_pins_drop))) = {0};
    StrongOwner creator __attribute__((cleanup(az_native_owner_reset))) = {0};
    uint64_t before = 0;
    int status = AZS_GUARD;
    struct cache_birth_impl_guard_1 guarded_0 = {.capture_borrow = &borrow, .capture_profile = &profile, .capture_pins = &pins, .capture_before = &before, .capture_status = &status, .capture_creator = &creator};
    if (!az_native_try(cache_birth_impl_guard_1_call, &guarded_0)) { status = AZS_GUARD; }

    /* Native exceptions are deliberately outside the observer catch boundary.
     * The closure alone constructs the real native result at its original sret
     * destination. No reinterpretation as a plain pointer-return function. */
    CacheBirthScope construction __attribute__((cleanup(birth_scope_drop)));
    birth_scope_init(&construction, status == AZS_OK, &((*borrow)));
    original.call(original.context);
    if (status != AZS_OK) return status;
    struct cache_birth_impl_guard_2 guarded_1 = {.capture_borrow = &borrow, .capture_profile = &profile, .capture_pins = &pins, .capture_construction = &construction, .capture_creator = &creator, .capture_out = &out};
    if (!az_native_try(cache_birth_impl_guard_2_call, &guarded_1)) { return AZS_GUARD; }
    return guarded_1.result;
}
struct cache_query_impl_guard_1 {
    SelectionInvocation *capture_selection;
    const QueryBorrow **capture_borrow;
    const Build **capture_profile;
    CachePins *capture_pins;
    uint64_t *capture_generation;
    int *capture_status;
    CopyRowScalar *capture_copy;
    Observation *capture_candidate;
};
static void cache_query_impl_guard_1_call(void *opaque) {
    struct cache_query_impl_guard_1 *guard = opaque;
    (void) guard;

    (*guard->capture_status) = azsel_ready(&(*guard->capture_selection)) ? cache_prepare_source(&((*guard->capture_borrow)->source), &((*(*guard->capture_profile))), (*guard->capture_borrow)->collector, &((*guard->capture_pins)), &((*guard->capture_generation))) : AZS_GUARD;
    if ((*guard->capture_status) == AZS_OK) {
        if (!(*guard->capture_borrow)->selected_cache || !(*guard->capture_borrow)->selected_cache->object || !guard->capture_copy->copy ||
            (*guard->capture_borrow)->selected_cache->object != (*guard->capture_borrow)->receiver_cache) (*guard->capture_status) = AZS_GUARD;
        else {
            az_native_owner_copy(&guard->capture_pins->cache, &(*(*guard->capture_borrow)->selected_cache));
            __auto_type s = &(source_state());
            AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
            if (!lock.held) (*guard->capture_status) = AZS_BUSY;
            else {
                (*guard->capture_status) = AZS_UNKNOWN;
                if (!cache_stamp_is((*guard->capture_generation)) || !cache_source_is(&((*guard->capture_borrow)->source), &((*guard->capture_pins)), false)) (*guard->capture_status) = AZS_STALE;
                else {
                    for (unsigned range_index_10 = 0; range_index_10 < sizeof(s->cache_bindings) / sizeof(s->cache_bindings)[0]; ++range_index_10) {
                        __auto_type binding = &(s->cache_bindings[range_index_10]);

                        if (cache_binding_current(binding, (*guard->capture_generation)) &&
                            binding->source_epoch == (*guard->capture_borrow)->source.source_epoch &&
                            cache_owner_is(&(binding->cache), &(guard->capture_pins->cache)) &&
                            cache_owner_is(&(binding->collector), &(guard->capture_pins->collector)) &&
                            cache_owner_is(&(binding->mount), &(guard->capture_pins->mount)) &&
                            cache_owner_is(&(binding->database), &(guard->capture_pins->database))) {
                            azcb_observation_drop(&(*guard->capture_candidate));
                            (*guard->capture_candidate) = cache_observation(binding);
                            (*guard->capture_status) = AZS_OK;
                            break;
                        }
                    }
                }
            }
        }
    }
}
struct cache_query_impl_guard_2 {
    SelectionInvocation *capture_selection;
    uint64_t *capture_generation;
    const QueryBorrow **capture_borrow;
    const Build **capture_profile;
    CachePins *capture_pins;
    CopyRowScalar *capture_copy;
    Observation *capture_candidate;
    Observation **capture_out;
    int result;
};
static int cache_query_impl_guard_2_body(struct cache_query_impl_guard_2 *guard) {
    (void) guard;

    if (!azsel_ready(&(*guard->capture_selection)) || !cache_stamp_is((*guard->capture_generation)) || !cache_worker_is(&((*guard->capture_borrow)->source), &((*(*guard->capture_profile)))) ||
        !cache_query_borrows_unchanged((*guard->capture_borrow), &((*guard->capture_pins)))) return AZS_STALE;
    uint32_t value = 0;
    if (!guard->capture_copy->copy(guard->capture_copy->context, &value)) return AZS_GUARD;
    if (!azsel_ready(&(*guard->capture_selection)) || !cache_worker_is(&((*guard->capture_borrow)->source), &((*(*guard->capture_profile))))) return AZS_GUARD;
    __auto_type s = &(source_state());
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
        /* The scalar copier may reenter native work. Recheck every actual
         * borrow and the pinned source tuple after it, before publication.
         * Check the mutation stamp first, before any holder storage reread. */
    if (!azsel_ready(&(*guard->capture_selection)) || !cache_stamp_is((*guard->capture_generation)) || !cache_query_borrows_unchanged((*guard->capture_borrow), &((*guard->capture_pins))) ||
        !cache_source_is(&((*guard->capture_borrow)->source), &((*guard->capture_pins)), false)) return AZS_STALE;
    bool retained = false;
    {
        for (unsigned range_index_11 = 0; range_index_11 < sizeof(s->cache_bindings) / sizeof(s->cache_bindings)[0]; ++range_index_11) {
            __auto_type binding = &(s->cache_bindings[range_index_11]);
            if (binding->birth_epoch == guard->capture_candidate->birth_epoch &&
                cache_binding_current(binding, (*guard->capture_generation)) &&
                cache_owner_is(&(binding->cache), &(guard->capture_pins->cache)) && cache_owner_is(&(binding->collector), &(guard->capture_pins->collector))) {
                retained = true;
                break;
            }
        }
    }
    if (!azsel_ready(&(*guard->capture_selection)) || !retained || !cache_source_is(&((*guard->capture_borrow)->source), &((*guard->capture_pins)), false)) return AZS_STALE;
    guard->capture_candidate->row_value = value;
    if ((*guard->capture_out)) azcb_observation_copy((*guard->capture_out), &(*guard->capture_candidate));
    return AZS_OK;
}
static void cache_query_impl_guard_2_call(void *opaque) {
    struct cache_query_impl_guard_2 *guard = opaque;
    guard->result = cache_query_impl_guard_2_body(guard);
}
static int cache_query_impl(const QueryBorrow *borrow, const Build *profile, Forward original, CopyRowScalar copy, Observation *out) {
    if (!original.call) return AZS_INVALID;
    SelectionInvocation selection __attribute__((cleanup(azsel_invocation_drop)));
    azsel_invocation_init(&selection, &source_state().cache_selection);
    CachePins pins __attribute__((cleanup(cache_pins_drop))) = {0};
    Observation candidate __attribute__((cleanup(azcb_observation_drop))) = {0};
    uint64_t generation = 0;
    int status = AZS_GUARD;
    struct cache_query_impl_guard_1 guarded_0 = {.capture_selection = &selection, .capture_borrow = &borrow, .capture_profile = &profile, .capture_pins = &pins, .capture_generation = &generation, .capture_status = &status, .capture_copy = &copy, .capture_candidate = &candidate};
    if (!az_native_try(cache_query_impl_guard_1_call, &guarded_0)) { status = AZS_GUARD; }

    original.call(original.context); // preserve destination, ownership and native exception
    if (status != AZS_OK) return status;
    struct cache_query_impl_guard_2 guarded_1 = {.capture_selection = &selection, .capture_generation = &generation, .capture_borrow = &borrow, .capture_profile = &profile, .capture_pins = &pins, .capture_copy = &copy, .capture_candidate = &candidate, .capture_out = &out};
    if (!az_native_try(cache_query_impl_guard_2_call, &guarded_1)) { return AZS_GUARD; }
    return guarded_1.result;
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
struct library_scope_init_guard_1 {
    CacheLibraryScope **capture_ctx;
    void **capture_server;
};
static void library_scope_init_guard_1_call(void *opaque) {
    struct library_scope_init_guard_1 *guard = opaque;
    (void) guard;

    if (!cache_worker_is(&((*guard->capture_ctx)->source), &((*guard->capture_ctx)->profile))) return;
    OwnedChain chain = {0};
    if (!owned_chain(((uintptr_t) ((*guard->capture_server))), &((*guard->capture_ctx)->profile), &(chain))) return;
    az_native_owner_copy(&(*guard->capture_ctx)->pins.mount, &(*chain.mount));
    az_native_owner_copy(&(*guard->capture_ctx)->pins.database, &(*chain.database));
    __auto_type s = &(source_state());
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    if (!lock.held) return;
    {
        for (unsigned range_index_12 = 0; range_index_12 < sizeof(s->bindings) / sizeof(s->bindings)[0]; ++range_index_12) {
            __auto_type binding = &(s->bindings[range_index_12]);
            if (binding->server == ((uintptr_t) ((*guard->capture_server)))) {
                (*guard->capture_ctx)->source.source_epoch = binding->epoch;
                (*guard->capture_ctx)->ready = cache_source_is(&((*guard->capture_ctx)->source), &((*guard->capture_ctx)->pins), false);
                (*guard->capture_ctx)->status_ready = cache_source_is(&((*guard->capture_ctx)->source), &((*guard->capture_ctx)->pins), true);
                break;
            }
        }
    }
}
static void library_scope_init(CacheLibraryScope *ctx, unsigned build, void *server, const Build *value) {
    (void) ctx;
    *ctx = (CacheLibraryScope) {.row_selection = 0, .ready = false, .status_ready = false, .row_valid = false, .parent = cache_library_scope, .source = make_source(server, build, &ctx->pins.mount, &ctx->pins.database), .profile = (*value)};

    ctx->depth = ctx->parent ? (ctx->parent->depth < kSlots ? ctx->parent->depth + 1 : kSlots + 1) : 1;
    cache_library_scope = ctx;
    if (ctx->depth > kSlots || !azcm_current(server, &ctx->message) ||
        (ctx->message.kind != 0x2102 && ctx->message.kind != 0x2002 && ctx->message.kind != 0x3000)) return;
    struct library_scope_init_guard_1 guarded_0 = {.capture_ctx = &ctx, .capture_server = &server};
    if (!az_native_try(library_scope_init_guard_1_call, &guarded_0)) { ctx->ready = false; }
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
struct selection_scope_init_guard_1 {
    CacheSelectionScope **capture_ctx;
    const struct azce_call **capture_call;
};
static void selection_scope_init_guard_1_call(void *opaque) {
    struct selection_scope_init_guard_1 *guard = opaque;
    (void) guard;

            /* These are the reviewed selection entry's live native receiver
             * and owning Creator reference, not addresses recovered from TLS.
             * Only read them before the original; all later holders are local. */
    __auto_type root = ((uintptr_t) ((*guard->capture_call)->args[0]));
    az_native_owner_copy(&(*guard->capture_ctx)->pins.mount, &(*((const StrongOwner *) (root + 0x28))));
    az_native_owner_copy(&(*guard->capture_ctx)->pins.collector, &(*((const StrongOwner *) (root + 0x40))));
    if (!(*guard->capture_ctx)->pins.mount.object || !(*guard->capture_ctx)->pins.collector.object ||
        !cache_owner_is(&((*guard->capture_ctx)->library->pins.mount), &((*guard->capture_ctx)->pins.mount))) return;
    __auto_type mount = ((uintptr_t) ((*guard->capture_ctx)->pins.mount.object));
    az_native_owner_copy(&(*guard->capture_ctx)->pins.database, &(*((const StrongOwner *) (mount + 0x30))));
    if (!(*guard->capture_ctx)->pins.database.object || !cache_owner_is(&((*guard->capture_ctx)->library->pins.database), &((*guard->capture_ctx)->pins.database))) return;
    (*guard->capture_ctx)->creator = load(const void *, (*guard->capture_call)->args[3]);
    if (!(*guard->capture_ctx)->creator) return;
    (*guard->capture_ctx)->source.server = (*guard->capture_ctx)->library->source.server;
    (*guard->capture_ctx)->source.build = (*guard->capture_ctx)->library->source.build;
    (*guard->capture_ctx)->source.source_epoch = (*guard->capture_ctx)->library->source.source_epoch;
    __auto_type s = &(source_state());
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    (*guard->capture_ctx)->ready = lock.held && cache_source_is(&((*guard->capture_ctx)->source), &((*guard->capture_ctx)->pins), false);
}
static void selection_scope_init(CacheSelectionScope *ctx, const struct azce_call *call) {
    (void) ctx;
    *ctx = (CacheSelectionScope) {.creator = NULL, .ready = false, .parent = cache_selection_scope, .library = cache_library_scope, .source = make_source(NULL, 0, &ctx->pins.mount, &ctx->pins.database)};

    cache_selection_scope = ctx;
    cache_poison_scopes(NULL); // a nested selection cannot preserve an older factory claim
    if (!ctx->library || !ctx->library->ready || ctx->library->depth > kSlots ||
        (ctx->library->message.kind != 0x2102 && ctx->library->message.kind != 0x2002) ||
        ((unsigned char) (call->args[1])) != ctx->library->message.selector ||
        ((uint32_t) (call->args[2])) != ctx->library->message.client ||
        !canonical(call->args[0]) || !canonical(call->args[3])) return;
    struct selection_scope_init_guard_1 guarded_0 = {.capture_ctx = &ctx, .capture_call = &call};
    if (!az_native_try(selection_scope_init_guard_1_call, &guarded_0)) { ctx->ready = false; }
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
struct query_scope_init_guard_1 {
    CacheQueryScope **capture_ctx;
    const struct azce_call **capture_call;
};
static void query_scope_init_guard_1_call(void *opaque) {
    struct query_scope_init_guard_1 *guard = opaque;
    (void) guard;

    (*guard->capture_ctx)->native_owned = true; // actual reviewed QUERY entry's live receiver, on the owned library worker
    SelectionBorrow borrow = {(*guard->capture_ctx)->library->source.build,
                              ((const void *) ((*guard->capture_call)->args[0])), (*guard->capture_ctx), query_scope_owned, query_scope_mount};
    struct azsel_resolution resolved __attribute__((cleanup(azsel_resolution_drop))) = {0};
    azsel_resolve_owned(&(*guard->capture_ctx)->selection, &borrow, (struct azsel_position) {(*guard->capture_ctx)->client, (*guard->capture_ctx)->selector}, &resolved);
    if (resolved.status != AZSEL_OK) return;
    az_native_owner_copy(&(*guard->capture_ctx)->pins.mount, &(resolved.pins.mount));
    az_native_owner_copy(&(*guard->capture_ctx)->pins.database, &(resolved.pins.database));
    az_native_owner_copy(&(*guard->capture_ctx)->pins.collector, &(resolved.pins.collector));
    az_native_owner_copy(&(*guard->capture_ctx)->pins.cache, &(resolved.pins.cache));
    if (!cache_owner_is(&((*guard->capture_ctx)->library->pins.mount), &((*guard->capture_ctx)->pins.mount)) ||
        !cache_owner_is(&((*guard->capture_ctx)->library->pins.database), &((*guard->capture_ctx)->pins.database))) return;
    (*guard->capture_ctx)->source.server = (*guard->capture_ctx)->library->source.server;
    (*guard->capture_ctx)->source.build = (*guard->capture_ctx)->library->source.build;
    (*guard->capture_ctx)->source.source_epoch = (*guard->capture_ctx)->library->source.source_epoch;
    __auto_type s = &(source_state());
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    (*guard->capture_ctx)->ready = lock.held && azsel_ready(&(*guard->capture_ctx)->selection) && cache_source_is(&((*guard->capture_ctx)->source), &((*guard->capture_ctx)->pins), false);
}
static void query_scope_init(CacheQueryScope *ctx, const struct azce_call *call) {
    (void) ctx;
    *ctx = (CacheQueryScope) {.start = 0, .count = 0, .client = 0, .selector = 0, .ready = false, .native_owned = false, .adapter_active = false, .adapter_done = false, .poisoned = false, .published = false, .parent = cache_query_scope, .library = cache_library_scope, .source = make_source(NULL, 0, &ctx->pins.mount, &ctx->pins.database)};

    azsel_invocation_init(&ctx->selection, &source_state().cache_selection);
    ctx->depth = ctx->parent ? (ctx->parent->depth < kSlots ? ctx->parent->depth + 1 : kSlots + 1) : 1;
    cache_query_scope = ctx;
    for (__auto_type older = ctx->parent; older; older = older->parent)
        if (older->adapter_active) older->poisoned = true;
    if (ctx->library) ctx->library->row_valid = false;
    if (!ctx->library || !ctx->library->ready || ctx->depth > kSlots || !azsel_ready(&ctx->selection) ||
        ctx->library->message.kind != 0x3000 || ctx->library->message.fields[3] != 0 ||
        ((unsigned char) (call->args[1])) != ctx->library->message.client ||
        ((uint32_t) (call->args[2])) != ctx->library->message.selector ||
        ((uint32_t) (call->args[3])) != ctx->library->message.fields[1] ||
        ((uint32_t) (call->args[4])) != ctx->library->message.fields[2] ||
        !canonical(call->args[0]) || !canonical(call->args[8])) return;
    ctx->start = ((uint32_t) (call->args[3]));
    ctx->count = ((uint32_t) (call->args[4]));
    ctx->client = ctx->library->message.client;
    ctx->selector = ctx->library->message.selector;
    if (ctx->start > INT32_MAX || !ctx->count || ctx->count > 16) return;
    struct query_scope_init_guard_1 guarded_0 = {.capture_ctx = &ctx, .capture_call = &call};
    if (!az_native_try(query_scope_init_guard_1_call, &guarded_0)) { ctx->ready = false; }
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

static void cache_forward_query_adapter(struct azce_call *call, Forward original) {
    __auto_type scope = cache_query_scope;
    if (!scope || !scope->ready || scope->poisoned || scope->adapter_active || scope->adapter_done ||
        scope->library != cache_library_scope || !azsel_ready(&scope->selection) ||
        !canonical(call->args[0]) || !canonical(call->args[3]) || !canonical(call->args[8]) ||
        ((uint32_t) (call->args[1])) != scope->start ||
        ((uint32_t) (call->args[2])) != scope->count) {
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
    const unsigned family = call->kind == AZCE_KIND_GENERIC ? AZCB_PRIMARY_FACTORY : AZCB_STANDARD_FALLBACK_FACTORY;
    if (result == AZS_OK && !scope->poisoned && azsel_ready(&scope->selection) && row.factory_kind == family) {
        azcb_observation_copy(&scope->row, &row);
        scope->adapter_done = true;
    } else scope->poisoned = true;
}
static bool cache_row_binding_current(const Evidence *row) {
    {
        for (unsigned range_index_13 = 0; range_index_13 < sizeof(source_state().cache_bindings) / sizeof(source_state().cache_bindings)[0]; ++range_index_13) {
            __auto_type binding = &(source_state().cache_bindings[range_index_13]);
            if (binding->birth_epoch == row->cache.birth_epoch && binding->source_epoch == row->cache.source_epoch &&
                binding->generation == row->cache.layout_generation && cache_binding_current(binding, binding->generation) &&
                !(binding->cache.control != row->cache.cache.control) && !(row->cache.cache.control != binding->cache.control) &&
                !(binding->collector.control != row->cache.collector.control) && !(row->cache.collector.control != binding->collector.control)) return true;
        }
    }
    return false;
}
struct cache_forward_query_guard_1 {
    CacheQueryScope *capture_scope;
    struct azce_call **capture_call;
};
static void cache_forward_query_guard_1_call(void *opaque) {
    struct cache_forward_query_guard_1 *guard = opaque;
    (void) guard;

    if (!guard->capture_scope->ready || guard->capture_scope->poisoned || !guard->capture_scope->adapter_done || !azsel_ready(&guard->capture_scope->selection) ||
        guard->capture_scope->library != cache_library_scope || !cache_stamp_is(guard->capture_scope->row.layout_generation)) return;
    uint32_t value = 0;
    if (!azcrow_copy_owned_tag23(((const void *) ((*guard->capture_call)->args[8])), &value) ||
        value != guard->capture_scope->row.row_value) return;
    __auto_type s = &(source_state());
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    if (!lock.held || !azsel_ready(&guard->capture_scope->selection) || !cache_source_is(&(guard->capture_scope->source), &(guard->capture_scope->pins), false) ||
        !cache_stamp_is(guard->capture_scope->row.layout_generation)) return;
    __auto_type row = &(guard->capture_scope->library->row);
    azcb_observation_copy(&row->cache, &guard->capture_scope->row);
    row->sequence = guard->capture_scope->library->message.sequence;
    row->client = guard->capture_scope->client;
    row->selector = guard->capture_scope->selector;
    row->start = guard->capture_scope->start;
    row->count = guard->capture_scope->count;
    guard->capture_scope->library->row_selection = guard->capture_scope->selection.revision;
    guard->capture_scope->library->row_valid = cache_row_binding_current(row);
    guard->capture_scope->published = guard->capture_scope->library->row_valid;
}
static void cache_forward_query(struct azce_call *call, Forward original) {
    CacheQueryScope scope __attribute__((cleanup(query_scope_drop)));
    query_scope_init(&scope, &(*call));
    original.call(original.context);
    struct cache_forward_query_guard_1 guarded_0 = {.capture_scope = &scope, .capture_call = &call};
    if (!az_native_try(cache_forward_query_guard_1_call, &guarded_0)) {
        if (scope.library) scope.library->row_valid = false;
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
    packet_key = 0;
    original.call(original.context); // no SDK lock or observer catch across native work
}
static void cache_forward_library_route(unsigned build, void *server, const Build *profile, Forward original) {
    const bool admitted = azsdk_cache_capture_ready && azsdk_cache_capture_ready() == 1;
    // Always push a masking frame, including while a partial installation is
    // forwarding. An older reentrant source scope cannot authorize this call.
    cache_forward_library_impl(admitted ? build : 0, server, &((*profile)), original);
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
    __auto_type selected = cache_selection_scope;
    if (call->kind != AZCE_KIND_CREATE || !selected || !selected->ready ||
        selected->library != cache_library_scope || !cache_library_scope->ready ||
        call->args[0] != ((uintptr_t) (selected->pins.collector.object)) ||
        !canonical(call->args[1]) || !canonical(call->args[8])) {
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



struct azcq_current_owned_guard_1 {
    Evidence **capture_out;
    bool result;
};
static bool azcq_current_owned_guard_1_body(struct azcq_current_owned_guard_1 *guard) {
    (void) guard;

    __auto_type scope = cache_library_scope;
    if (cache_query_scope || !scope || !scope->ready || !scope->row_valid || scope->message.kind != 0x3000 ||
        !cache_worker_is(&(scope->source), &(scope->profile))) return false;
    SelectionInvocation selection __attribute__((cleanup(azsel_invocation_drop)));
    azsel_invocation_init(&selection, &source_state().cache_selection);
    if (!azsel_ready(&selection) || selection.revision != scope->row_selection ||
        !cache_stamp_is(scope->row.cache.layout_generation)) return false;
    __auto_type s = &(source_state());
    AZ_MUTEX_GUARD(lock, s->registry_mutex, true);
    if (!lock.held || !cache_source_is(&(scope->source), &(scope->pins), false) ||
        !cache_row_binding_current(&(scope->row)) || !azsel_ready(&selection) ||
        !cache_stamp_is(scope->row.cache.layout_generation)) return false;
    azcq_evidence_copy((*guard->capture_out), &scope->row);
    return true;
}
static void azcq_current_owned_guard_1_call(void *opaque) {
    struct azcq_current_owned_guard_1 *guard = opaque;
    guard->result = azcq_current_owned_guard_1_body(guard);
}
bool azcq_current_owned(Evidence *out) {
    struct azcq_current_owned_guard_1 guarded_0 = {.capture_out = &out};
    if (!az_native_try(azcq_current_owned_guard_1_call, &guarded_0)) { return false; }
    return guarded_0.result;
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

void azcb_observe_factory_owned(const FactoryBorrow *borrow, Forward original) {
    if (!original.call) return;
    __auto_type frame = cache_birth_scope;
    cache_poison_scopes(frame);
    const bool ready = frame && frame->eligible && !frame->poisoned &&
                       !frame->factory_active && !frame->completed && borrow->receiver_creator == frame->creator &&
                       borrow->borrowed_pool == frame->pool && borrow->native_result &&
                       (borrow->kind == AZCB_PRIMARY_FACTORY || borrow->kind == AZCB_STANDARD_FALLBACK_FACTORY) &&
        /* The exact standard factory is supported only for Creator mode0.
         * Read it before forwarding, only after the actual owned Collector
         * scope matched this Creator. An arbitrary factory entry cannot
         * confer permission to follow its receiver. */
                       (borrow->kind != AZCB_STANDARD_FALLBACK_FACTORY ||
                        load(unsigned char, ((uintptr_t) (borrow->receiver_creator)) + 0x44) == 0) &&
                       cache_stamp(&(frame->generation));
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
    if (frame->poisoned || !cache_stamp_is(frame->generation) || !*borrow->native_result ||
        (borrow->kind == AZCB_STANDARD_FALLBACK_FACTORY && (!frame->setup_seen || frame->setters != 16 ||
                                                            *borrow->native_result != frame->raw_result))) {
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

void azcb_observe_construction_write_owned(const WriteBorrow *borrow, Forward original) {
    if (!original.call) return;
    __auto_type frame = cache_birth_scope;
    cache_poison_scopes(frame);
    const bool setup = borrow->kind == AZCB_SETUP_WRITE;
    const bool ready = frame && frame->eligible && !frame->poisoned && frame->factory_active &&
                       frame->factory_kind == AZCB_STANDARD_FALLBACK_FACTORY && !frame->write_active &&
                       borrow->receiver_cache && borrow->native_status && cache_stamp_is(frame->generation) &&
                       ((setup && !frame->setup_seen && borrow->argument == 16 && borrow->borrowed_pool == frame->pool) ||
                        (borrow->kind == AZCB_LAYOUT_WRITE && frame->setup_seen && frame->setters < 16 &&
                         borrow->argument == frame->setters && borrow->receiver_cache == frame->raw_result));
    if (frame) {
        if (!ready) frame->poisoned = true;
        frame->write_active = true;
    }
    WriteGuard guard __attribute__((cleanup(write_guard_drop)));
    write_guard_init(&guard, frame);
    __auto_type before = ready ? frame->generation : 0;
    {
        CacheMutation mutation __attribute__((cleanup(cache_mutation_drop)));
        cache_mutation_init(&mutation);
        original.call(original.context);
    }
    guard.returned = true;
    if (!ready) return;
    uint64_t after = 0;
    if (frame->poisoned || !cache_stamp(&(after)) || before == UINT64_MAX || after != before + 1 ||
        *borrow->native_status != 0) {
        frame->poisoned = true;
        return;
    }
    frame->generation = after;
    if (setup) {
        frame->setup_seen = true;
        frame->raw_result = borrow->receiver_cache;
    } else ++frame->setters;
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


struct azsdk_sources_configure_guard_1 {
    int *capture_enable;
    int *capture_build_id;
    unsigned *capture_policy;
    int result;
};
static int azsdk_sources_configure_guard_1_body(struct azsdk_sources_configure_guard_1 *guard) {
    (void) guard;

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
    if (!(*guard->capture_enable) || (*guard->capture_build_id) < 1 || (*guard->capture_build_id) > 2 || ((*guard->capture_policy) & ~kKnownPolicy)) return AZS_UNSUPPORTED;
    int prior = atomic_load_explicit(&(source_state().bound_build), memory_order_relaxed);
    if (prior && prior != (*guard->capture_build_id)) return AZS_UNSUPPORTED;
    pthread_once(&source_state().image_once, prepare_image);
    if (!source_state().main_image.count) return AZS_UNSUPPORTED;
    atomic_store_explicit(&(source_state().bound_build), (*guard->capture_build_id), memory_order_relaxed);
    if ((*guard->capture_policy) & AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER)
        pthread_once(&source_state().sql_once, prepare_sqlcipher);
    atomic_store_explicit(&(source_state().capture_policy), (*guard->capture_policy), memory_order_relaxed);
    atomic_store_explicit(&(source_state().enabled), 1, memory_order_release);
    return AZS_OK;
}
static void azsdk_sources_configure_guard_1_call(void *opaque) {
    struct azsdk_sources_configure_guard_1 *guard = opaque;
    guard->result = azsdk_sources_configure_guard_1_body(guard);
}
int azsdk_sources_configure(int enable, int build_id, unsigned policy) {
    struct azsdk_sources_configure_guard_1 guarded_0 = {.capture_enable = &enable, .capture_build_id = &build_id, .capture_policy = &policy};
    if (!az_native_try(azsdk_sources_configure_guard_1_call, &guarded_0)) { return AZS_UNSUPPORTED; }
    return guarded_0.result;
}
struct azsdk_sources_observe_owned_message_guard_1 {
    void **capture_server;
    void **capture_out;
    int *capture_build;
    size_t *capture_size;
    int result;
};
static int azsdk_sources_observe_owned_message_guard_1_body(struct azsdk_sources_observe_owned_message_guard_1 *guard) {
    (void) guard;

    return observe(((uintptr_t) ((*guard->capture_server))), &(builds[(*guard->capture_build)]), &(source_state().sql_profile),
#if defined(__linux__)
                   &metadata_ops,
#endif
                   (*guard->capture_out), (*guard->capture_size));
}
static void azsdk_sources_observe_owned_message_guard_1_call(void *opaque) {
    struct azsdk_sources_observe_owned_message_guard_1 *guard = opaque;
    guard->result = azsdk_sources_observe_owned_message_guard_1_body(guard);
}
int azsdk_sources_observe_owned_message(void *server, void *out,
                                        size_t size) {
    if (!output_ok(out, size, true)) return AZS_INVALID;
    if (!atomic_load_explicit(&(source_state().enabled), memory_order_acquire)) return AZS_UNSUPPORTED;
    int build = atomic_load_explicit(&(source_state().bound_build), memory_order_relaxed);
    if (build < 1 || build > 2) return AZS_UNSUPPORTED;
    struct azsdk_sources_observe_owned_message_guard_1 guarded_0 = {.capture_server = &server, .capture_out = &out, .capture_build = &build, .capture_size = &size};
    if (!az_native_try(azsdk_sources_observe_owned_message_guard_1_call, &guarded_0)) { return AZS_GUARD; }
    return guarded_0.result;
}
struct azsdk_sources_read_guard_1 {
    uint64_t *capture_epoch;
    void **capture_out;
    size_t *capture_size;
    int result;
};
static int azsdk_sources_read_guard_1_body(struct azsdk_sources_read_guard_1 *guard) {
    (void) guard;

    AZ_MUTEX_GUARD(lock, source_state().registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    {
        for (unsigned range_index_15 = 0; range_index_15 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_15) {
            __auto_type e = &(source_state().entries[range_index_15]);
            if (e->sample.owner_epoch == (*guard->capture_epoch)) {
                __auto_type s = scalar_sample(e);
                copy_output((*guard->capture_out), (*guard->capture_size), &(s));
                return (s.flags & AZS_CURRENT) ? AZS_OK : AZS_STALE;
            }
        }
    }
    return AZS_UNKNOWN;
}
static void azsdk_sources_read_guard_1_call(void *opaque) {
    struct azsdk_sources_read_guard_1 *guard = opaque;
    guard->result = azsdk_sources_read_guard_1_body(guard);
}
int azsdk_sources_read(uint64_t epoch, void *out, size_t size) {
    if (!epoch || !output_ok(out, size, false)) return AZS_INVALID;
    struct azsdk_sources_read_guard_1 guarded_0 = {.capture_epoch = &epoch, .capture_out = &out, .capture_size = &size};
    if (!az_native_try(azsdk_sources_read_guard_1_call, &guarded_0)) { return AZS_BUSY; }
    return guarded_0.result;
}
struct azsdk_sources_status_read_guard_1 {
    uint64_t *capture_epoch;
    void **capture_out;
    size_t *capture_size;
    int result;
};
static int azsdk_sources_status_read_guard_1_body(struct azsdk_sources_status_read_guard_1 *guard) {
    (void) guard;

    AZ_MUTEX_GUARD(lock, source_state().registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    {
        for (unsigned range_index_16 = 0; range_index_16 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_16) {
            __auto_type entry = &(source_state().entries[range_index_16]);
            if (entry->sample.owner_epoch == (*guard->capture_epoch)) {
                if (!status_live(entry)) return AZS_STALE;
                copy_output((*guard->capture_out), (*guard->capture_size), &(entry->sample));
                return AZS_OK;
            }
        }
    }
    return AZS_UNKNOWN;
}
static void azsdk_sources_status_read_guard_1_call(void *opaque) {
    struct azsdk_sources_status_read_guard_1 *guard = opaque;
    guard->result = azsdk_sources_status_read_guard_1_body(guard);
}
int azsdk_sources_status_read(uint64_t epoch, void *out, size_t size) {
    if (!epoch || !output_ok(out, size, false)) return AZS_INVALID;
    struct azsdk_sources_status_read_guard_1 guarded_0 = {.capture_epoch = &epoch, .capture_out = &out, .capture_size = &size};
    if (!az_native_try(azsdk_sources_status_read_guard_1_call, &guarded_0)) { return AZS_BUSY; }
    return guarded_0.result;
}
struct azsdk_sources_candidate_guard_1 {
    uint16_t *capture_media;
    void **capture_out;
    size_t *capture_size;
    int result;
};
static int azsdk_sources_candidate_guard_1_body(struct azsdk_sources_candidate_guard_1 *guard) {
    (void) guard;

    AZ_MUTEX_GUARD(lock, source_state().registry_mutex, true);
    if (!lock.held) return AZS_BUSY;
    if (bit(source_state().ambiguous_media, (*guard->capture_media))) {
        struct azsdk_source_sample s = {0};
        s.struct_size = sizeof s;
        s.native_media_id = (*guard->capture_media);
        s.flags = AZS_HISTORY_INCOMPLETE | AZS_MEDIA_AMBIGUOUS;
        copy_output((*guard->capture_out), (*guard->capture_size), &(s));
        return AZS_AMBIGUOUS;
    }
    {
        for (unsigned range_index_17 = 0; range_index_17 < sizeof(source_state().entries) / sizeof(source_state().entries)[0]; ++range_index_17) {
            __auto_type e = &(source_state().entries[range_index_17]);
            if (e->sample.native_media_id == (*guard->capture_media)) {
                __auto_type s = scalar_sample(e);
                if (!(s.flags & AZS_CURRENT)) continue;
                copy_output((*guard->capture_out), (*guard->capture_size), &(s));
                return AZS_OK;
            }
        }
    }
    return AZS_UNKNOWN;
}
static void azsdk_sources_candidate_guard_1_call(void *opaque) {
    struct azsdk_sources_candidate_guard_1 *guard = opaque;
    guard->result = azsdk_sources_candidate_guard_1_body(guard);
}
int azsdk_sources_candidate(uint16_t media, void *out, size_t size) {
    if (!media || !output_ok(out, size, false)) return AZS_INVALID;
    struct azsdk_sources_candidate_guard_1 guarded_0 = {.capture_media = &media, .capture_out = &out, .capture_size = &size};
    if (!az_native_try(azsdk_sources_candidate_guard_1_call, &guarded_0)) { return AZS_BUSY; }
    return guarded_0.result;
}
struct azsdk_sources_sweep_guard_1 {
    unsigned result;
};
static unsigned azsdk_sources_sweep_guard_1_body(struct azsdk_sources_sweep_guard_1 *guard) {
    (void) guard;

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
static void azsdk_sources_sweep_guard_1_call(void *opaque) {
    struct azsdk_sources_sweep_guard_1 *guard = opaque;
    guard->result = azsdk_sources_sweep_guard_1_body(guard);
}
unsigned azsdk_sources_sweep() {
    struct azsdk_sources_sweep_guard_1 guarded_0 = {0};
    if (!az_native_try(azsdk_sources_sweep_guard_1_call, &guarded_0)) { return 0; }
    return guarded_0.result;
}
int azsdk_sources_physical_ready() {
    return atomic_load_explicit(&(source_state().enabled), memory_order_acquire) && atomic_load_explicit(&(source_state().physical_prepared), memory_order_acquire);
}
int azsdk_sources_descriptor_identity(int descriptor, uint64_t mount_id,
                                      uint64_t device, uint64_t inode) {
#if defined(__linux__)
    struct stat descriptor_before, descriptor_after;
    uint64_t actual_mount = 0;
    return descriptor >= 0 && mount_id && inode && !metadata_ops.stat(descriptor, &descriptor_before) &&
           S_ISREG(descriptor_before.st_mode) && ((uint64_t) (descriptor_before.st_dev)) == device &&
           ((uint64_t) (descriptor_before.st_ino)) == inode &&
           descriptor_mount_id(descriptor, &(metadata_ops), &(actual_mount)) && actual_mount == mount_id &&
           !metadata_ops.stat(descriptor, &descriptor_after) && descriptor_before.st_dev == descriptor_after.st_dev && descriptor_before.st_ino == descriptor_after.st_ino;
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
        actual.mount_id == mount_id && actual.device == device && actual.inode == inode) return fd;
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
static bool reply_code_profile(ReplyCodeProfile *out) {
    if (!reply_capture_enabled()) return false;
    const unsigned build = (unsigned) (atomic_load_explicit(&(source_state().bound_build), memory_order_acquire));
    ReplyCodeProfile code = {0};
#ifdef AZSDK_CACHE_BIRTH_TESTING
    if (reply_test_code.build) code = reply_test_code;
    else
#endif
    {
        struct azsdk_cache_reply_code generated;
        if (!azsdk_cache_reply_code_profile((int) (build), &generated)) return false;
        __auto_type base = source_state().main_image.base;
        if (generated.begin_rva > UINTPTR_MAX - base || generated.return_rva > UINTPTR_MAX - base) return false;
        code.build = build;
        code.begin = base + generated.begin_rva;
        code.return_pc = base + generated.return_rva;
        code.length = generated.length;
        memcpy(code.sha256, generated.sha256, sizeof code.sha256);
    }
    if (code.build != build || !code.begin || (code.begin & 3u) || (code.return_pc & 3u) ||
        code.length < 4 || code.length > 65536 || code.length > UINTPTR_MAX - code.begin ||
        code.return_pc < code.begin + 4 || code.return_pc > code.begin + code.length ||
        code.sha256[64] || !image_contains(&source_state().main_image, code.begin, code.length, kRead | kExecute)) return false;
    char digest[65];
    azsha256_hex(((const void *) (code.begin)), code.length, digest);
    if (memcmp(digest, code.sha256, sizeof digest)) return false;
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

void azcr_forward_ticket_owned(struct azce_call *call, Forward original) {
    if (!original.call) return;
    ReplyWeakAbi empty __attribute__((cleanup(az_native_weak_reset))) = {0};
    const ReplyWeakAbi *receiver = &empty;
    Request request = {0};
    uintptr_t return_pc = 0;
    ReplyCodeProfile code = {0};
    __auto_type ticket = call ? (uintptr_t) (call->args[0]) : 0;
    /* The reviewed native process entry is invoked with a live owning-queue
     * Ticket. The metadata checks below constrain that borrow; they acquire no
     * liveness. Every native field is copied BEFORE forwarding and is never
     * reread after processing can destroy/mutate its packet or Ticket. */
    if (call && !call->reserved && !call->caller_reserved && reply_code_profile(&(code)) &&
        checked_vtable(ticket, "N13music_library21LoadInfoRequestTicketE", 14) &&
        load(unsigned char, ticket + 0x80) == 2) {
        __auto_type packet = load(uintptr_t, ticket + 0x28);
        if (canonical(packet)) {
            memcpy(request.request_id, ((const void *) (ticket + 8)), sizeof request.request_id);
            request.sequence = load(uint32_t, packet + 8);
            request.reply_kind = load(uint16_t, packet);
            receiver = ((const ReplyWeakAbi *) (ticket + 0x88));
            return_pc = code.return_pc;
        }
    }
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

void azcr_forward_reception_owned(struct azce_call *call, Forward original) {
    if (!original.call) return;
    ReplyCallbackScope callback __attribute__((cleanup(reply_scope_drop)));
    reply_scope_init(&callback); // foreign/reentrant callbacks mask older receipts
    ReplyCodeProfile code = {0};
    __auto_type receiver = call ? (uintptr_t) (call->args[0]) : 0;
    if (call && !call->reserved && !call->caller_reserved && reply_code_profile(&(code)) &&
        call->caller_link == code.return_pc) {
        Promotion native = {0};
        native.control = call->caller_x21;
        native.caller_link = call->caller_link;
        if (azcr_receive_current_owned(&native, ((const void *) (receiver)), &callback.receipt.selected) &&
            checked_vtable(receiver, "N17trackinfo_stocker22LoadInfoRequestHandler9ReceptionE", 3)) {
            /* Native slot13's checked code keeps its actual X21 strong lease
             * until after this callback returns. The helper's temporary lock
             * is already released. These two copied fields confer no Stocker
             * ownership and are not a player load ID/source association. */
            callback.receipt.pack_id = load(uint32_t, receiver + 8);
            callback.receipt.borrowed_stocker = load(uintptr_t, receiver + 0x10);
            callback.invocation = azcr_current_invocation();
            callback.ready = true;
        }
    }
    if (azjs_reception && call && checked_vtable((uintptr_t) (call->args[0]), "N17trackinfo_stocker22LoadInfoRequestHandler9ReceptionE", 3)) {
        __auto_type receiver = (uintptr_t) (call->args[0]);
        /* Foreign/unpromoted replies revoke any older pending claim for that pack. */
        azjs_reception(callback.ready ? receiver : 0, load(uint32_t, receiver + 8),
                       ((const void *) (call->args[1])));
    }
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
