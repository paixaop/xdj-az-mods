/* SPDX-License-Identifier: GPL-3.0-only */
/* Owned native source capture, GNU libstdc++ ABI only. See CORE-SDK-NATIVE-SOURCES.md.
 * Evidence RVAs are comparisons against checked API/vtable results, never independent
 * entry addresses. No native DB, FD or object is dereferenced after the owned callback. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
extern "C" int azsdk_cache_capture_ready(void) __attribute__((weak));
#include "../common/azconnectionhooks.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <time.h>
#include <utility>
#if defined(__linux__)
#if !defined(__GLIBCXX__)
#error "Native source capture requires GNU libstdc++, never libc++"
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr unsigned kRead = 4, kExecute = 1, kSlots = 32;
constexpr unsigned kKnownPolicy = AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER;
static_assert(sizeof(std::uintptr_t) == 8, "native source ABI requires 64 bits");
static_assert(sizeof(std::shared_ptr<music_library::MountPoint>) == 16 &&
                  sizeof(std::weak_ptr<music_library::MountPoint>) == 16,
              "native source owners require GNU two-word shared/weak pointers");
struct Range {
    std::uintptr_t begin, end;
    unsigned flags;
};
struct Image {
    Range ranges[32] = {};
    unsigned count = 0;
    std::uintptr_t base = 0;
    bool contains(std::uintptr_t p, std::size_t n, unsigned flags) const noexcept {
        if (n > UINTPTR_MAX - p) return false;
        for (unsigned i = 0; i < count; ++i)
            if ((ranges[i].flags & flags) == flags && p >= ranges[i].begin &&
                p + n <= ranges[i].end) return true;
        return false;
    }
};
/* Exact public SQLite ABI prefix. The Unix file's descriptor offset is a PRIVATE verified
 * layout, enabled only after matching the actual loaded SQLCipher text and method RVAs. */
struct NativeFile;
struct NativeIO {
    int version;
    int (*close)(NativeFile *);
    int (*read)(NativeFile *, void *, int, std::int64_t);
    int (*write)(NativeFile *, const void *, int, std::int64_t);
    int (*truncate)(NativeFile *, std::int64_t);
    int (*sync)(NativeFile *, int);
    int (*size)(NativeFile *, std::int64_t *);
    int (*lock)(NativeFile *, int);
    int (*unlock)(NativeFile *, int);
    int (*reserved)(NativeFile *, int *);
    int (*control)(NativeFile *, int, void *);
    int (*sector)(NativeFile *);
    int (*characteristics)(NativeFile *);
};
struct NativeFile {
    const NativeIO *methods;
};
struct NativeVFS {
    int version, os_size, max_path;
    void *next;
    const char *name;
    void *app;
    int (*open)(NativeVFS *, const char *, NativeFile *, int, int *);
};
static_assert(offsetof(NativeIO, size) == 0x30 && offsetof(NativeIO, control) == 0x50 &&
                  offsetof(NativeVFS, name) == 0x18 && offsetof(NativeVFS, open) == 0x28,
              "SQLite public 64-bit ABI");
struct SqlProfile {
    Image image;
    azsdk_connection_hooks::FileControl file_control = nullptr;
    std::uintptr_t io_rvas[3] = {};
    std::uintptr_t open_rva = 0, read_rva = 0, size_rva = 0, control_rva = 0;
    bool ready = false;
    /* Local physical-capture view, also constructible by explicit synthetic
     * fixtures. Only the sealed shared proof prepares the production view.
     * That proof retains the module; neither view owns a native connection. */
    const azsdk_connection_hooks::SqlProfile *proof = nullptr;
};
struct Build {
    std::size_t dispatcher;
    std::uintptr_t affinity_rva;
};
constexpr Build builds[] = {{0, 0}, {0x1c0, 0x78a2b0}, {0x1c8, 0x7cc2f0}};
struct Entry {
    std::weak_ptr<music_library::MountPoint> mount;
    std::weak_ptr<music_library::SqliteDataBase> database;
    std::uintptr_t mount_address = 0, database_address = 0, connection = 0;
    /* Immutable part of this source key. Zero is an incomplete owner
     * diagnostic, never an observed SQLite connection incarnation. */
    std::uint64_t connection_generation = 0;
    azsdk_source_sample sample = {};
    bool retired = false;
};
struct Binding {
    std::uintptr_t server = 0;
    std::uint64_t epoch = 0;
};
struct CacheBinding {
    std::weak_ptr<music_library::MountPoint> mount;
    std::weak_ptr<music_library::SqliteDataBase> database;
    std::weak_ptr<music_library::ListCacheCollector> collector;
    std::weak_ptr<music_library::ListCache> cache;
    std::uint64_t source_epoch = 0, birth_epoch = 0, generation = 0;
    unsigned factory_kind = 0;
};
struct SourceState {
    Image main_image;
    std::once_flag image_once, sql_once;
    std::atomic<int> enabled {0}, bound_build {0};
    std::atomic<unsigned> capture_policy {0};
    std::atomic<bool> physical_prepared {false};
    std::mutex registry_mutex;
    std::uint64_t next_epoch = 0, next_revision = 0;
    /* Never erase these tombstones. Native MediaID discriminators wrap after 255 mounts. */
    unsigned char seen_media[65536 / 8] = {}, ambiguous_media[65536 / 8] = {};
    Entry entries[kSlots];
    Binding bindings[kSlots];
    /* Cache provenance shares the existing source registry and worker. It is
     * not a source lookup by media/content/raw address or a native task queue.
     * Weak controls are reclaimed only under the registry mutex on that worker.
     * Mutator entries touch only the atomics and never release weak owners. */
    CacheBinding cache_bindings[kSlots];
    std::uint64_t next_cache_birth = 0;
    unsigned cache_limit = kSlots;
    std::atomic<std::uint64_t> cache_generation {1};
    std::atomic<unsigned> cache_active {0}, cache_depth_limit {kSlots};
    std::atomic<bool> cache_saturated {false};
    azsdk_cache_selection::State cache_selection;
    azsdk_connection_hooks::EpochState connection_epochs;
    SqlProfile sql_profile; /* Prepared once under the registry mutex before publication. */
};
SourceState &source_state() noexcept {
    /* azmod_init can precede this TU's dynamic constructors. The guarded
     * placement construction initializes every weak owner before first use.
     * Native lifecycle hooks can also run after ordinary C++ atexit objects;
     * keep this one bounded state, its mutexes and weak controls alive until
     * process teardown. Storage owns bytes only and never destroys SourceState.
     * This is not a second registry or a native object-liveness acquisition. */
    struct ProcessStorage {
        alignas(SourceState) unsigned char bytes[sizeof(SourceState)];
        SourceState *state;
        ProcessStorage() noexcept : state(::new (static_cast<void *>(bytes)) SourceState) {}
    };
    static ProcessStorage storage;
    return *storage.state;
}

bool canonical(std::uintptr_t p) noexcept {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
template <class T>
T load(std::uintptr_t p) noexcept {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(p), sizeof value);
    return value;
}
std::uint64_t now_ns() noexcept {
    timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
        t.tv_nsec >= 1000000000L) return 0;
    auto s = static_cast<std::uint64_t>(t.tv_sec);
    if (s > (UINT64_MAX - static_cast<unsigned long>(t.tv_nsec)) / UINT64_C(1000000000)) return 0;
    return s * UINT64_C(1000000000) + static_cast<unsigned long>(t.tv_nsec);
}
bool bit(const unsigned char *set, unsigned media) noexcept {
    return (set[media / 8] & (1u << (media % 8))) != 0;
}
void set_bit(unsigned char *set, unsigned media) noexcept {
    set[media / 8] |= static_cast<unsigned char>(1u << (media % 8));
}
bool output_ok(void *out, std::size_t size, bool optional = false) noexcept {
    return out ? size >= 8 : optional && size == 0;
}
void copy_output(void *out, std::size_t size, const azsdk_source_sample &sample) noexcept {
    if (out) std::memcpy(out, &sample, size < sizeof sample ? size : sizeof sample);
}
#if defined(__linux__)
struct ImageSearch {
    Image *image;
    bool main;
};
int find_image(dl_phdr_info *info, std::size_t, void *context) noexcept {
    auto &s = *static_cast<ImageSearch *>(context);
    if (s.main ? (info->dlpi_name && *info->dlpi_name) : info->dlpi_addr != s.image->base) return 0;
    s.image->base = info->dlpi_addr;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const auto &p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD || !(p.p_flags & PF_R)) continue;
        if (s.image->count == 32 || p.p_vaddr > UINTPTR_MAX - info->dlpi_addr ||
            p.p_memsz > UINTPTR_MAX - info->dlpi_addr - p.p_vaddr) {
            s.image->count = 0;
            return 1;
        }
        s.image->ranges[s.image->count++] = {info->dlpi_addr + p.p_vaddr,
                                             info->dlpi_addr + p.p_vaddr + p.p_memsz, p.p_flags};
    }
    return 1;
}
#endif
void prepare_image() noexcept {
#if defined(__linux__)
    ImageSearch s {&source_state().main_image, true};
    dl_iterate_phdr(find_image, &s);
#endif
}
/* These range checks constrain metadata, never acquire object liveness. The caller owns
 * the receiver/chain. Heap pointers cannot be made safe by a canonical-address check. */
bool image_string_equal(const Image &image, std::uintptr_t address, const char *expected) noexcept {
    for (std::size_t i = 0;; ++i) {
        if (i > UINTPTR_MAX - address || !image.contains(address + i, 1, kRead)) return false;
        /* An ELF range bounds the module, not this string object. Scalar volatile
         * reads prevent widening past an earlier mismatch or the terminating NUL. */
        char actual = *reinterpret_cast<const volatile char *>(address + i);
        if (actual != expected[i]) return false;
        if (!actual) return true;
    }
}
std::uintptr_t checked_vtable(std::uintptr_t object, const char *name, unsigned slots) noexcept {
    if (!canonical(object)) return 0;
    auto table = load<std::uintptr_t>(object);
    if (!canonical(table) || table < 16 ||
        !source_state().main_image.contains(table - 16, (slots + 2u) * 8u, kRead) ||
        load<std::uintptr_t>(table - 16)) return 0;
    auto type = load<std::uintptr_t>(table - 8);
    if (!canonical(type) || !source_state().main_image.contains(type, 16, kRead)) return 0;
    auto text = load<std::uintptr_t>(type + 8);
    if (!source_state().main_image.contains(text, 1, kRead)) return 0;
    if (*reinterpret_cast<const char *>(text) == '*') ++text;
    return image_string_equal(source_state().main_image, text, name) ? table : 0;
}
bool worker_scope(std::uintptr_t server, const Build &build) {
    auto table = checked_vtable(server, "N13music_library18MusicLibraryServerE", 11);
    if (!table || load<unsigned char>(server + 0xd2) != 1 ||
        !canonical(load<std::uintptr_t>(server + 0x28))) return false;
    auto entry = load<std::uintptr_t>(table + 5 * 8);
    if (!source_state().main_image.contains(entry, 4, kRead | kExecute) || entry < source_state().main_image.base ||
        entry - source_state().main_image.base != build.affinity_rva) return false;
    /* Only the checked native virtual result is invoked, never a computed address. */
    using NeedsAsync = bool (*)(void *);
    return !reinterpret_cast<NeedsAsync>(entry)(reinterpret_cast<void *>(server));
}
struct OwnedChain {
    const std::shared_ptr<music_library::MountPoint> *mount = nullptr;
    const std::shared_ptr<music_library::SqliteDataBase> *database = nullptr;
    std::uintptr_t mount_address = 0, database_address = 0, connection = 0;
    std::uint16_t media = 0;
};
bool owned_chain(std::uintptr_t server, const Build &build, OwnedChain &out) noexcept {
    auto dispatcher = load<std::uintptr_t>(server + build.dispatcher);
    if (!canonical(dispatcher)) return false;
    auto handler = load<std::uintptr_t>(dispatcher + 0x20);
    if (!canonical(handler)) return false;
    auto updater = load<std::uintptr_t>(handler + 0x30);
    if (!checked_vtable(updater, "N13music_library18InformationUpdaterE", 1)) return false;
    auto mount = load<std::uintptr_t>(updater + 8);
    if (!checked_vtable(mount, "N13music_library16SqliteMountPointE", 1)) return false;
    auto database = load<std::uintptr_t>(mount + 0x30);
    if (!checked_vtable(database, "N13music_library14SqliteDataBaseE", 1)) return false;
    auto media = load<std::uint16_t>(database + 8);
    auto connection = load<std::uintptr_t>(database + 0x38);
    if (!media || load<std::uint16_t>(mount + 8) != media || !canonical(connection) ||
        !canonical(load<std::uintptr_t>(updater + 0x10)) ||
        !canonical(load<std::uintptr_t>(mount + 0x38))) return false;
    /* Borrow the REAL source references in the verified serialized native worker scope.
     * Never memcpy ownership words into a shared/weak object or fabricate an owner. */
    const auto &m = *reinterpret_cast<const std::shared_ptr<music_library::MountPoint> *>(updater + 8);
    const auto &d = *reinterpret_cast<const std::shared_ptr<music_library::SqliteDataBase> *>(mount + 0x30);
    if (reinterpret_cast<std::uintptr_t>(m.get()) != mount || m.use_count() <= 0 ||
        reinterpret_cast<std::uintptr_t>(d.get()) != database || d.use_count() <= 0) return false;
    out.mount = &m;
    out.database = &d;
    out.mount_address = mount;
    out.database_address = database;
    out.connection = connection;
    out.media = media;
    return true;
}

bool module_function(const SqlProfile &p, std::uintptr_t f, std::uintptr_t rva) noexcept {
    return f >= p.image.base && f - p.image.base == rva && p.image.contains(f, 4, kRead | kExecute);
}
void prepare_sqlcipher() noexcept {
    /* The same immutable startup proof will serve lifecycle installation.
     * It is minted before the first redirect and reused afterward, so physical
     * preparation never rehashes text modified by our own accepted hooks. */
    const auto *proof = azsdk_connection_hooks::prepare_profile();
    if (!proof || !proof->range_count() || proof->range_count() > 32 || !proof->file_control()) return;
    SqlProfile p;
    p.proof = proof;
    p.image.base = proof->base();
    p.image.count = proof->range_count();
    for (unsigned i = 0; i < p.image.count; ++i) {
        const auto *range = proof->range(i);
        if (!range) return;
        p.image.ranges[i] = {range->begin, range->end, range->flags};
    }
    p.file_control = proof->file_control();
    for (unsigned i = 0; i < 3; ++i) p.io_rvas[i] = proof->io_rva(i);
    p.open_rva = proof->vfs_open_rva();
    p.read_rva = proof->read_rva();
    p.size_rva = proof->size_rva();
    p.control_rva = proof->control_rva();
    p.ready = true;
    source_state().sql_profile = p;
    source_state().physical_prepared.store(true, std::memory_order_release);
}

#if defined(__linux__)
struct MetadataOps {
    int (*stat)(int, struct stat *);
    int (*open)(const char *, int);
    ssize_t (*read)(int, void *, std::size_t);
    int (*close)(int);
};
int open_metadata(const char *path, int flags) { return ::open(path, flags); }
const MetadataOps metadata_ops {::fstat, open_metadata, ::read, ::close};
/* Complete bounded fdinfo text only: reject duplicate/truncated/overflowed mount IDs. */
bool parse_mount_id(const char *data, std::size_t size, std::uint64_t &out) noexcept {
    bool found = false;
    std::uint64_t value = 0;
    for (std::size_t start = 0; start < size;) {
        std::size_t end = start;
        while (end < size && data[end] != '\n') ++end;
        if (end == size) return false;
        if (end - start >= 7 && !std::memcmp(data + start, "mnt_id:", 7)) {
            if (found) return false;
            std::size_t at = start + 7;
            while (at < end && (data[at] == ' ' || data[at] == '\t')) ++at;
            if (at == end) return false;
            value = 0;
            for (; at < end && data[at] >= '0' && data[at] <= '9'; ++at) {
                unsigned digit = static_cast<unsigned>(data[at] - '0');
                if (value > (UINT64_MAX - digit) / 10) return false;
                value = value * 10 + digit;
            }
            while (at < end && (data[at] == ' ' || data[at] == '\t')) ++at;
            if (at != end || !value) return false;
            found = true;
        }
        start = end + 1;
    }
    if (found) out = value;
    return found;
}
bool descriptor_mount_id(int native_fd, const MetadataOps &ops, std::uint64_t &out) noexcept {
    char path[64], text[4096];
    int length = std::snprintf(path, sizeof path, "/proc/self/fdinfo/%d", native_fd);
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof path) return false;
    int fd = ops.open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    std::size_t used = 0;
    bool complete = false;
    /* At most 8 reads and 4096 bytes. No unbounded EINTR/short-read retry. */
    for (unsigned attempt = 0; attempt < 8 && used < sizeof text; ++attempt) {
        ssize_t n = ops.read(fd, text + used, sizeof text - used);
        if (n < 0) break;
        if (!n) {
            complete = true;
            break;
        }
        if (static_cast<std::size_t>(n) > sizeof text - used) break;
        used += static_cast<std::size_t>(n);
    }
    int closed = ops.close(fd); /* Only our fdinfo FD; NEVER the SQLite descriptor. */
    return !closed && complete && parse_mount_id(text, used, out);
}
bool supported_name(const SqlProfile &p, const char *name) noexcept {
    static const char *const names[] = {"unix", "unix-excl", "unix-none", "unix-dotfile"};
    auto address = reinterpret_cast<std::uintptr_t>(name);
    for (auto expected : names)
        if (image_string_equal(p.image, address, expected)) return true;
    return false;
}
int capture_physical(std::uintptr_t connection, const SqlProfile &p, const MetadataOps &ops,
                     azsdk_source_sample &out) {
    if (!p.ready || !p.file_control) return AZS_PHYSICAL_PROFILE;
    NativeFile *file = nullptr;
    NativeVFS *vfs = nullptr;
    if (p.file_control(reinterpret_cast<sqlite3 *>(connection), "main", 7, &file) || !file ||
        p.file_control(reinterpret_cast<sqlite3 *>(connection), "main", 27, &vfs) || !vfs)
        return AZS_PHYSICAL_API;
    /* File liveness comes from the owned native callback. VFS/table/name/code are bounded
     * to the exact hashed DSO; wrappers and unrelated SQLite modules fail closed. */
    if (!canonical(reinterpret_cast<std::uintptr_t>(file)) ||
        !canonical(reinterpret_cast<std::uintptr_t>(vfs)) ||
        !p.image.contains(reinterpret_cast<std::uintptr_t>(vfs), sizeof *vfs, kRead) ||
        vfs->version != 3 || vfs->os_size != 120 || vfs->max_path != 512 ||
        !supported_name(p, vfs->name) ||
        !module_function(p, reinterpret_cast<std::uintptr_t>(vfs->open), p.open_rva))
        return AZS_PHYSICAL_VFS;
    const NativeIO *io = file->methods;
    auto table = reinterpret_cast<std::uintptr_t>(io);
    if (!canonical(table) || !p.image.contains(table, sizeof *io, kRead) || table < p.image.base)
        return AZS_PHYSICAL_METHODS;
    auto relative = table - p.image.base;
    unsigned slot = 3;
    for (unsigned i = 0; i < 3; ++i)
        if (relative == p.io_rvas[i]) {
            slot = i;
            break;
        }
    if (slot == 3 || io->version != (slot == 2 ? 1 : 3) ||
        !module_function(p, reinterpret_cast<std::uintptr_t>(io->read), p.read_rva) ||
        !module_function(p, reinterpret_cast<std::uintptr_t>(io->size), p.size_rva) ||
        !module_function(p, reinterpret_cast<std::uintptr_t>(io->control), p.control_rva))
        return AZS_PHYSICAL_METHODS;
    auto address = reinterpret_cast<std::uintptr_t>(file);
    auto embedded = load<std::uintptr_t>(address + 8);
    int native_fd = load<std::int32_t>(address + 0x18);
    if (embedded != reinterpret_cast<std::uintptr_t>(vfs) || native_fd < 0)
        return AZS_PHYSICAL_DESCRIPTOR;
    struct stat before, after;
    std::int64_t size = -1;
    std::uint64_t mount = 0;
    if (ops.stat(native_fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        !descriptor_mount_id(native_fd, ops, mount) || io->size(file, &size) ||
        ops.stat(native_fd, &after)) return AZS_PHYSICAL_METADATA;
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || size != before.st_size ||
        load<std::int32_t>(address + 0x18) != native_fd || file->methods != io ||
        load<std::uintptr_t>(address + 8) != embedded) return AZS_PHYSICAL_CHANGED;
    out.device = static_cast<std::uint64_t>(before.st_dev);
    out.inode = static_cast<std::uint64_t>(before.st_ino);
    out.mount_id = mount;
    out.file_size = static_cast<std::uint64_t>(size);
    out.physical_observed_ns = now_ns();
    out.flags |= AZS_PHYSICAL;
    return AZS_PHYSICAL_OK;
}
#endif
bool has_binding(std::uint64_t epoch) noexcept {
    for (const auto &b : source_state().bindings)
        if (b.server && b.epoch == epoch) return true;
    return false;
}
void retire_unused() noexcept {
    for (auto &e : source_state().entries)
        if (e.sample.owner_epoch && !has_binding(e.sample.owner_epoch)) e.retired = true;
}
void retire_server(std::uintptr_t server) noexcept {
    for (auto &b : source_state().bindings)
        if (b.server == server) b = {};
    retire_unused();
}
std::uint64_t connection_generation(std::uintptr_t connection) noexcept {
    /* Only installed, armed native forwarding mints production receipts in
     * this embedded State. A failed read has no diagnostic cause: it includes
     * unknown/retired, active lifecycle work and saturation. No native pointer
     * is followed, and no receipt acquires a connection lease. Lock order is
     * registry_mutex -> EpochState; EpochState releases its lock before native
     * forwarding and must never call back into this registry while locked. */
    azsdk_connection_epoch::Receipt receipt {};
    return source_state().connection_epochs.read(reinterpret_cast<const void *>(connection), receipt) ?
           receipt.generation : 0;
}
bool owner_bound(const Entry &e) noexcept {
    /* Registry/weak-owner validity is separate from a receipt observation.
     * EpochState can temporarily refuse every read during one source's native
     * lifecycle work; that cannot retire another source's binding/history. */
    return source_state().enabled.load(std::memory_order_acquire) && e.sample.owner_epoch && !e.retired &&
           !e.mount.expired() && !e.database.expired() &&
           has_binding(e.sample.owner_epoch);
}
bool live(const Entry &e) noexcept {
    /* Zero-generation records retain only the incomplete owner diagnostic.
     * Cache provenance separately requires an observed nonzero generation. */
    return owner_bound(e) &&
           (!e.connection_generation || connection_generation(e.connection) == e.connection_generation);
}
azsdk_source_sample scalar_sample(const Entry &e) noexcept {
    auto s = e.sample;
    if (bit(source_state().ambiguous_media, s.native_media_id)) s.flags |= AZS_MEDIA_AMBIGUOUS;
    if (live(e)) s.flags |= AZS_CURRENT;
    else {
        s.flags &= ~(AZS_CURRENT | AZS_PHYSICAL);
        s.device = s.inode = s.mount_id = s.file_size = s.physical_observed_ns = 0;
    }
    return s;
}
template <class T>
bool same_owner(const std::weak_ptr<T> &weak,
                const std::shared_ptr<T> &strong) noexcept {
    return !weak.owner_before(strong) && !strong.owner_before(weak);
}
/* Build/SqlProfile arguments also allow synthetic GNU fixtures to exercise the complete
 * implementation without masquerading as a real firmware or relaxing production guards. */
int observe(std::uintptr_t server, const Build &build, const SqlProfile &sql,
#if defined(__linux__)
            const MetadataOps &ops,
#endif
            void *out, std::size_t out_size) {
    if (!worker_scope(server, build)) return AZS_GUARD;
    OwnedChain chain;
    bool supported = owned_chain(server, build, chain);
    /* Stable local pins survive capture reentrancy; the holders themselves are
     * freshly resolved below, never retained across a native callback. Actual
     * complete-receiver lifetime and serialization remain caller preconditions. */
    std::shared_ptr<music_library::MountPoint> mount;
    std::shared_ptr<music_library::SqliteDataBase> database;
    if (supported) {
        mount = *chain.mount;
        database = *chain.database;
    }
    std::unique_lock<std::mutex> lock(source_state().registry_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZS_BUSY;
    if (!source_state().enabled.load(std::memory_order_acquire)) return AZS_UNSUPPORTED;
    if (!supported) {
        retire_server(server);
        return AZS_GUARD;
    }
    const auto observed_generation = connection_generation(chain.connection);
    Binding *binding = nullptr;
    for (auto &b : source_state().bindings)
        if (b.server == server) {
            binding = &b;
            break;
        }
    if (!binding)
        for (auto &b : source_state().bindings)
            if (!b.server) {
                binding = &b;
                break;
            }
    Entry *entry = nullptr, *empty = nullptr;
    bool was_observed = false;
    for (auto &e : source_state().entries) {
        if (!e.sample.owner_epoch) {
            if (!empty) empty = &e;
            continue;
        }
        if (e.mount_address != chain.mount_address ||
            e.database_address != chain.database_address || e.connection != chain.connection ||
            e.sample.native_media_id != chain.media) continue;
        if (!same_owner(e.mount, mount) || !same_owner(e.database, database)) continue;
        if (e.connection_generation) was_observed = true;
        if (!entry && e.connection_generation == observed_generation && owner_bound(e)) entry = &e;
    }
    if (!observed_generation && was_observed) {
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
    Entry candidate;
    if (!entry) {
        candidate.mount = mount; /* Genuine weak construction, no persistent strong retention. */
        candidate.database = database;
        candidate.mount_address = chain.mount_address;
        candidate.database_address = chain.database_address;
        candidate.connection = chain.connection;
        candidate.connection_generation = observed_generation;
    }
    azsdk_source_sample sample = {};
    sample.struct_size = sizeof sample;
    sample.flags = AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE;
    sample.owner_epoch = entry ? entry->sample.owner_epoch : source_state().next_epoch + 1;
    sample.revision = source_state().next_revision + 1;
    sample.observed_ns = now_ns();
    sample.native_media_id = chain.media;
    sample.physical_status = AZS_PHYSICAL_DISABLED;
    if (bit(source_state().ambiguous_media, chain.media)) sample.flags |= AZS_MEDIA_AMBIGUOUS;
    if (source_state().capture_policy.load(std::memory_order_relaxed) & AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER) {
#if defined(__linux__)
        sample.physical_status = capture_physical(chain.connection, sql, ops, sample);
#else
        (void) sql;
        sample.physical_status = AZS_PHYSICAL_PROFILE;
#endif
    }
    /* Physical capture can execute native callbacks. Re-resolve the actual
     * holders after it, compare against the stable pins, and preserve the
     * original incarnation stamp instead of adopting the latest return stamp.
     * Registry identity/binding/sample publication has not occurred yet. */
    OwnedChain current;
    if (!worker_scope(server, build) || !owned_chain(server, build, current) ||
        current.mount_address != chain.mount_address || current.database_address != chain.database_address ||
        current.connection != chain.connection || current.media != chain.media ||
        current.mount->owner_before(mount) || mount.owner_before(*current.mount) ||
        current.database->owner_before(database) || database.owner_before(*current.database) ||
        !source_state().enabled.load(std::memory_order_acquire)) {
        retire_server(server);
        return AZS_STALE;
    }
    /* A receipt-only refusal is transient/unknown, not retirement evidence.
     * Actual close/reopen can never make the old positive stamp current again;
     * live() continues to reject it without discarding another source's key. */
    if (connection_generation(chain.connection) != observed_generation) return AZS_STALE;
    if (!entry) {
        if (bit(source_state().seen_media, chain.media)) set_bit(source_state().ambiguous_media, chain.media);
        set_bit(source_state().seen_media, chain.media);
        candidate.sample = sample;
        entry = empty;
        *entry = std::move(candidate);
        source_state().next_epoch = sample.owner_epoch;
    }
    if (bit(source_state().ambiguous_media, chain.media)) sample.flags |= AZS_MEDIA_AMBIGUOUS;
    entry->sample = sample;
    source_state().next_revision = sample.revision;
    binding->server = server;
    binding->epoch = sample.owner_epoch;
    retire_unused();
    copy_output(out, out_size, sample);
    return AZS_OK;
}

namespace cb = azsdk_cache_birth;
/* Native query pins die in reverse order: cache before Collector, so returning
 * the cache's reserved pages never follows a pool already freed by Collector.
 * No strong owner is saved in SourceState or in an observation. */
struct CachePins {
    std::shared_ptr<music_library::MountPoint> mount;
    std::shared_ptr<music_library::SqliteDataBase> database;
    std::shared_ptr<music_library::ListCacheCollector> collector;
    std::shared_ptr<music_library::ListCache> cache;
};
bool cache_stamp(std::uint64_t &generation) noexcept {
    auto &s = source_state();
    if (s.cache_active.load(std::memory_order_acquire) ||
        s.cache_saturated.load(std::memory_order_acquire)) return false;
    generation = s.cache_generation.load(std::memory_order_acquire);
    return generation && !s.cache_active.load(std::memory_order_acquire) &&
           !s.cache_saturated.load(std::memory_order_acquire);
}
bool cache_stamp_is(std::uint64_t generation) noexcept {
    std::uint64_t current = 0;
    return cache_stamp(current) && current == generation;
}
template<class T>
bool cache_owner_is(const std::weak_ptr<T> &weak, const std::shared_ptr<T> &owned) noexcept {
    /* Control identity is the lookup key. Alias equality is an additional
     * scoped validation, never an address-derived owner or a persistent key. */
    return same_owner(weak, owned) && weak.lock().get() == owned.get();
}
const Entry *cache_source_entry(std::uint64_t epoch) noexcept {
    for (const auto &entry : source_state().entries)
        if (entry.sample.owner_epoch == epoch) return &entry;
    return nullptr;
}
bool cache_source_is(const cb::SourceBorrow &source, const CachePins &pins) noexcept {
    bool bound = false;
    for (const auto &binding : source_state().bindings)
        if (binding.server == reinterpret_cast<std::uintptr_t>(source.server) &&
            binding.epoch == source.source_epoch) { bound = true; break; }
    const Entry *entry = cache_source_entry(source.source_epoch);
    if (!bound || !entry || !entry->connection_generation || !live(*entry) || !pins.mount || !pins.database ||
        entry->mount_address != reinterpret_cast<std::uintptr_t>(pins.mount.get()) ||
        entry->database_address != reinterpret_cast<std::uintptr_t>(pins.database.get()) ||
        !same_owner(entry->mount, pins.mount) || !same_owner(entry->database, pins.database)) return false;
    /* Matching control blocks do not freeze the DB's mutable source tuple.
     * Read only the native prefixes of these actual callback-owned pins, after
     * exact owner/alias validation, and compare the tuple used by observe().
     * No connection is followed and no epoch is renewed by this check. */
    const auto mount = reinterpret_cast<std::uintptr_t>(pins.mount.get());
    const auto database = reinterpret_cast<std::uintptr_t>(pins.database.get());
    return entry->connection == load<std::uintptr_t>(database + 0x38) &&
           entry->sample.native_media_id == load<std::uint16_t>(database + 8) &&
           entry->sample.native_media_id == load<std::uint16_t>(mount + 8) &&
           connection_generation(entry->connection) == entry->connection_generation;
}
bool cache_binding_retained(const CacheBinding &binding, std::uint64_t generation) noexcept {
    const Entry *entry = cache_source_entry(binding.source_epoch);
    return binding.birth_epoch && binding.generation == generation && entry &&
           entry->connection_generation && owner_bound(*entry) &&
           !binding.cache.expired() && !binding.collector.expired() &&
           !binding.mount.expired() && !binding.database.expired();
}
/* Receipt availability gates qualification, never slot reclamation. A native
 * operation on another connection can temporarily hide this still-owned key. */
bool cache_binding_current(const CacheBinding &binding, std::uint64_t generation) noexcept {
    const Entry *entry = cache_source_entry(binding.source_epoch);
    return cache_binding_retained(binding, generation) && entry && live(*entry);
}

bool cache_worker_is(const cb::SourceBorrow &source, const Build &profile) {
    auto &s = source_state();
    return s.enabled.load(std::memory_order_acquire) && source.build >= 1 && source.build <= 2 &&
           int(source.build) == s.bound_build.load(std::memory_order_acquire) && source.server &&
           worker_scope(reinterpret_cast<std::uintptr_t>(source.server), profile);
}
int cache_prepare_source(const cb::SourceBorrow &source, const Build &profile,
                         const std::shared_ptr<music_library::ListCacheCollector> *collector,
                         CachePins &pins, std::uint64_t &generation) {
    auto &s = source_state();
    if (!s.enabled.load(std::memory_order_acquire) || source.build < 1 || source.build > 2 ||
        int(source.build) != s.bound_build.load(std::memory_order_acquire)) return AZS_UNSUPPORTED;
    if (!source.source_epoch || !source.mount || !source.database || !collector ||
        !cache_worker_is(source, profile)) return AZS_GUARD;
    /* These pointers are explicitly owned borrows from the current callback;
     * canonical-address/RTTI tests cannot make arbitrary shared_ptr memory safe. */
    if (!*source.mount || !*source.database || !*collector || !cache_stamp(generation)) return AZS_GUARD;
    pins.mount = *source.mount;
    pins.database = *source.database;
    pins.collector = *collector;
    std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return AZS_BUSY;
    return cache_stamp_is(generation) && cache_source_is(source, pins) ? AZS_OK : AZS_STALE;
}
bool cache_borrow_source_unchanged(const cb::SourceBorrow &source, const CachePins &pins) noexcept {
    return source.mount->get() == pins.mount.get() && source.database->get() == pins.database.get() &&
           !source.mount->owner_before(pins.mount) && !pins.mount.owner_before(*source.mount) &&
           !source.database->owner_before(pins.database) && !pins.database.owner_before(*source.database);
}
bool cache_query_borrows_unchanged(const cb::QueryBorrow &borrow, const CachePins &pins) noexcept {
    return cache_borrow_source_unchanged(borrow.source, pins) &&
           borrow.collector->get() == pins.collector.get() &&
           !borrow.collector->owner_before(pins.collector) && !pins.collector.owner_before(*borrow.collector) &&
           borrow.selected_cache->get() == pins.cache.get() &&
           !borrow.selected_cache->owner_before(pins.cache) && !pins.cache.owner_before(*borrow.selected_cache);
}
cb::Observation cache_observation(const CacheBinding &binding) noexcept {
    cb::Observation result;
    result.birth_epoch = binding.birth_epoch;
    result.layout_generation = binding.generation;
    result.source_epoch = binding.source_epoch;
    result.factory_kind = binding.factory_kind;
    result.mount = binding.mount;
    result.database = binding.database;
    result.collector = binding.collector;
    result.cache = binding.cache;
    return result;
}
/* Bounded stack scopes tie factory completion to the same real Collector
 * invocation. A raw new result is never a persistent lookup key. No native
 * call, allocation or owning-result cleanup happens in this bookkeeping. */
struct CacheBirthScope;
thread_local CacheBirthScope *cache_birth_scope = nullptr;
thread_local unsigned cache_birth_depth = 0;
struct CacheBirthScope {
    CacheBirthScope *parent = nullptr;
    const void *creator = nullptr, *pool = nullptr, *raw_result = nullptr;
    std::uint64_t generation = 0;
    unsigned factory_kind = 0, setters = 0;
    bool pushed = false, eligible = false, poisoned = false, factory_active = false;
    bool write_active = false, setup_seen = false, completed = false;
    CacheBirthScope(bool ready, const cb::BirthBorrow &borrow) noexcept
        : parent(cache_birth_scope), creator(borrow.receiver_creator), pool(borrow.borrowed_pool),
          eligible(ready && creator && pool) {
        for (auto ancestor = parent; ancestor; ancestor = ancestor->parent)
            if (ancestor->factory_active) ancestor->poisoned = true;
        if (cache_birth_depth >= kSlots) {
            source_state().cache_saturated.store(true, std::memory_order_release);
            eligible = false;
            return;
        }
        ++cache_birth_depth;
        cache_birth_scope = this;
        pushed = true;
    }
    ~CacheBirthScope() {
        if (pushed) { cache_birth_scope = parent; --cache_birth_depth; }
    }
    CacheBirthScope(const CacheBirthScope &) = delete;
    CacheBirthScope &operator=(const CacheBirthScope &) = delete;
};
void cache_poison_scopes(CacheBirthScope *except = nullptr) noexcept {
    for (auto scope = cache_birth_scope; scope; scope = scope->parent)
        if (scope != except) scope->poisoned = true;
}
int cache_birth_impl(const cb::BirthBorrow &borrow, const Build &profile,
                     cb::Forward original, cb::Observation *out) {
    if (!original.call) return AZS_INVALID;
    CachePins pins;
    std::shared_ptr<music_library::SqliteDataBase> creator;
    std::uint64_t before = 0;
    int status = AZS_GUARD;
    try {
        status = cache_prepare_source(borrow.source, profile, borrow.collector, pins, before);
        if (status == AZS_OK) {
            if (!borrow.creator_database || !*borrow.creator_database || !borrow.native_result ||
                pins.collector.get() != borrow.receiver_collector) status = AZS_GUARD;
            else {
                creator = *borrow.creator_database;
                if (creator.get() != pins.database.get() || creator.owner_before(pins.database) ||
                    pins.database.owner_before(creator)) status = AZS_GUARD;
            }
        }
    } catch (...) { status = AZS_GUARD; }
    /* Native exceptions are deliberately outside the observer catch boundary.
     * The closure alone constructs the real native result at its original sret
     * destination. No reinterpretation as a plain pointer-return function. */
    CacheBirthScope construction(status == AZS_OK, borrow);
    original.call(original.context);
    if (status != AZS_OK) return status;
    try {
        const auto generation = construction.generation;
        /* Preserve the actual final-write stamp through factory cleanup and
         * native shared-owner publication. Never adopt the latest return stamp. */
        if (!construction.completed || construction.poisoned || !cache_stamp_is(generation) ||
            !cache_worker_is(borrow.source, profile) ||
            !cache_borrow_source_unchanged(borrow.source, pins) ||
            !*borrow.collector || borrow.collector->get() != pins.collector.get() ||
            borrow.collector->owner_before(pins.collector) || pins.collector.owner_before(*borrow.collector) ||
            borrow.creator_database->get() != creator.get() ||
            borrow.creator_database->owner_before(creator) || creator.owner_before(*borrow.creator_database) ||
            !*borrow.native_result) return AZS_STALE;
        pins.cache = *borrow.native_result; // actual successfully constructed native owner
        if (pins.cache.get() != construction.raw_result) return AZS_STALE;
        auto &s = source_state();
        std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return AZS_BUSY;
        if (!cache_stamp_is(generation) || !cache_source_is(borrow.source, pins)) return AZS_STALE;
        CacheBinding *slot = nullptr;
        for (unsigned i = 0; i < s.cache_limit; ++i) {
            auto &binding = s.cache_bindings[i];
            if (binding.birth_epoch && same_owner(binding.cache, pins.cache)) return AZS_STALE;
            if (!slot && !cache_binding_retained(binding, generation)) slot = &binding;
        }
        if (!slot || s.next_cache_birth == UINT64_MAX) return AZS_LIMIT;
        CacheBinding candidate;
        candidate.mount = pins.mount;
        candidate.database = pins.database;
        candidate.collector = pins.collector;
        candidate.cache = pins.cache;
        candidate.source_epoch = borrow.source.source_epoch;
        candidate.generation = generation;
        candidate.birth_epoch = s.next_cache_birth + 1;
        candidate.factory_kind = construction.factory_kind;
        if (!cache_stamp_is(generation) || !cache_source_is(borrow.source, pins)) return AZS_STALE;
        *slot = candidate; // reclaim stale weak controls only in this owned worker
        s.next_cache_birth = candidate.birth_epoch;
        if (out) *out = cache_observation(candidate);
        return AZS_OK;
    } catch (...) { return AZS_GUARD; }
}
int cache_query_impl(const cb::QueryBorrow &borrow, const Build &profile, cb::Forward original,
                     cb::CopyRowScalar copy, cb::Observation *out) {
    if (!original.call) return AZS_INVALID;
    azsdk_cache_selection::Invocation selection(source_state().cache_selection);
    CachePins pins;
    cb::Observation candidate;
    std::uint64_t generation = 0;
    int status = AZS_GUARD;
    try {
        status = selection.ready() ?
            cache_prepare_source(borrow.source, profile, borrow.collector, pins, generation) : AZS_GUARD;
        if (status == AZS_OK) {
            if (!borrow.selected_cache || !*borrow.selected_cache || !copy.copy ||
                borrow.selected_cache->get() != borrow.receiver_cache) status = AZS_GUARD;
            else {
                pins.cache = *borrow.selected_cache;
                auto &s = source_state();
                std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
                if (!lock.owns_lock()) status = AZS_BUSY;
                else {
                    status = AZS_UNKNOWN;
                    if (!cache_stamp_is(generation) || !cache_source_is(borrow.source, pins)) status = AZS_STALE;
                    else for (const auto &binding : s.cache_bindings) {
                        if (cache_binding_current(binding, generation) &&
                            binding.source_epoch == borrow.source.source_epoch &&
                            cache_owner_is(binding.cache, pins.cache) &&
                            cache_owner_is(binding.collector, pins.collector) &&
                            cache_owner_is(binding.mount, pins.mount) &&
                            cache_owner_is(binding.database, pins.database)) {
                            candidate = cache_observation(binding);
                            status = AZS_OK;
                            break;
                        }
                    }
                }
            }
        }
    } catch (...) { status = AZS_GUARD; }
    original.call(original.context); // preserve destination, ownership and native exception
    if (status != AZS_OK) return status;
    try {
        if (!selection.ready() || !cache_stamp_is(generation) || !cache_worker_is(borrow.source, profile) ||
            !cache_query_borrows_unchanged(borrow, pins)) return AZS_STALE;
        std::uint32_t value = 0;
        if (!copy.copy(copy.context, value)) return AZS_GUARD;
        if (!selection.ready() || !cache_worker_is(borrow.source, profile)) return AZS_GUARD;
        auto &s = source_state();
        std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return AZS_BUSY;
        /* The scalar copier may reenter native work. Recheck every actual
         * borrow and the pinned source tuple after it, before publication.
         * Check the mutation stamp first, before any holder storage reread. */
        if (!selection.ready() || !cache_stamp_is(generation) || !cache_query_borrows_unchanged(borrow, pins) ||
            !cache_source_is(borrow.source, pins)) return AZS_STALE;
        bool retained = false;
        for (const auto &binding : s.cache_bindings)
            if (binding.birth_epoch == candidate.birth_epoch &&
                cache_binding_current(binding, generation) &&
                cache_owner_is(binding.cache, pins.cache) && cache_owner_is(binding.collector, pins.collector)) {
                retained = true; break;
            }
        if (!selection.ready() || !retained || !cache_source_is(borrow.source, pins)) return AZS_STALE;
        candidate.row_value = value;
        if (out) *out = candidate;
        return AZS_OK;
    } catch (...) { return AZS_GUARD; }
}
class CacheMutation {
    bool active_ = false;
public:
    CacheMutation() noexcept {
        auto &s = source_state();
        unsigned active = s.cache_active.load(std::memory_order_acquire);
        const unsigned limit = s.cache_depth_limit.load(std::memory_order_relaxed);
        while (active < limit) {
            if (s.cache_active.compare_exchange_weak(active, active + 1, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) { active_ = true; break; }
        }
        if (!active_) s.cache_saturated.store(true, std::memory_order_release);
        auto generation = s.cache_generation.load(std::memory_order_acquire);
        for (;;) {
            if (generation == UINT64_MAX) {
                s.cache_saturated.store(true, std::memory_order_release); break;
            }
            if (s.cache_generation.compare_exchange_weak(generation, generation + 1,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) break;
        }
    }
    ~CacheMutation() {
        if (active_) source_state().cache_active.fetch_sub(1, std::memory_order_release);
    }
    CacheMutation(const CacheMutation &) = delete;
    CacheMutation &operator=(const CacheMutation &) = delete;
};

/* The MusicLibraryServer caller already owns its complete receiver/chain on
 * its worker. Copy actual source owners before forwarding. The TLS stack only
 * carries these local pins through nested native calls; it grants no lifetime.
 * Unsupported and overflow frames hide any earlier frame. */
struct CacheLibraryScope;
thread_local CacheLibraryScope *cache_library_scope = nullptr;
struct CacheLibraryScope {
    CacheLibraryScope *parent;
    CachePins pins;
    cb::SourceBorrow source;
    Build profile;
    azsdk_cache_message::Envelope message;
    azsdk_cache_query::Evidence row;
    std::uint64_t row_selection = 0;
    unsigned depth;
    bool ready = false;
    bool row_valid = false;
    CacheLibraryScope(unsigned build, void *server, const Build &value) noexcept
        : parent(cache_library_scope), source {server, build, 0, &pins.mount, &pins.database}, profile(value),
          depth(parent ? (parent->depth < kSlots ? parent->depth + 1 : kSlots + 1) : 1) {
        cache_library_scope = this;
        if (depth > kSlots || !azsdk_cache_message::current(server, message) ||
            (message.kind != 0x2102 && message.kind != 0x2002 && message.kind != 0x3000)) return;
        try {
            if (!cache_worker_is(source, profile)) return;
            OwnedChain chain;
            if (!owned_chain(reinterpret_cast<std::uintptr_t>(server), profile, chain)) return;
            pins.mount = *chain.mount;
            pins.database = *chain.database;
            auto &s = source_state();
            std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
            if (!lock.owns_lock()) return;
            for (const auto &binding : s.bindings)
                if (binding.server == reinterpret_cast<std::uintptr_t>(server)) {
                    source.source_epoch = binding.epoch;
                    ready = cache_source_is(source, pins);
                    break;
                }
        } catch (...) { ready = false; }
    }
    ~CacheLibraryScope() { cache_library_scope = parent; }
    CacheLibraryScope(const CacheLibraryScope &) = delete;
    CacheLibraryScope &operator=(const CacheLibraryScope &) = delete;
};
struct CacheSelectionScope;
thread_local CacheSelectionScope *cache_selection_scope = nullptr;
struct CacheSelectionScope {
    CacheSelectionScope *parent;
    CacheLibraryScope *library;
    CachePins pins;
    cb::SourceBorrow source;
    const void *creator = nullptr;
    bool ready = false;
    explicit CacheSelectionScope(const azce_call &call) noexcept
        : parent(cache_selection_scope), library(cache_library_scope),
          source {nullptr, 0, 0, &pins.mount, &pins.database} {
        cache_selection_scope = this;
        cache_poison_scopes(); // a nested selection cannot preserve an older factory claim
        if (!library || !library->ready || library->depth > kSlots ||
            (library->message.kind != 0x2102 && library->message.kind != 0x2002) ||
            static_cast<unsigned char>(call.args[1]) != library->message.selector ||
            static_cast<std::uint32_t>(call.args[2]) != library->message.client ||
            !canonical(call.args[0]) || !canonical(call.args[3])) return;
        try {
            /* These are the reviewed selection entry's live native receiver
             * and owning Creator reference, not addresses recovered from TLS.
             * Only read them before the original; all later holders are local. */
            const auto root = static_cast<std::uintptr_t>(call.args[0]);
            pins.mount = *reinterpret_cast<const std::shared_ptr<music_library::MountPoint> *>(root + 0x28);
            pins.collector = *reinterpret_cast<const std::shared_ptr<music_library::ListCacheCollector> *>(root + 0x40);
            if (!pins.mount || !pins.collector ||
                !cache_owner_is(std::weak_ptr<music_library::MountPoint>(library->pins.mount), pins.mount)) return;
            const auto mount = reinterpret_cast<std::uintptr_t>(pins.mount.get());
            pins.database = *reinterpret_cast<const std::shared_ptr<music_library::SqliteDataBase> *>(mount + 0x30);
            if (!pins.database || !cache_owner_is(std::weak_ptr<music_library::SqliteDataBase>(library->pins.database), pins.database)) return;
            creator = load<const void *>(call.args[3]);
            if (!creator) return;
            source.server = library->source.server;
            source.build = library->source.build;
            source.source_epoch = library->source.source_epoch;
            auto &s = source_state();
            std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
            ready = lock.owns_lock() && cache_source_is(source, pins);
        } catch (...) { ready = false; }
    }
    ~CacheSelectionScope() { cache_selection_scope = parent; }
    CacheSelectionScope(const CacheSelectionScope &) = delete;
    CacheSelectionScope &operator=(const CacheSelectionScope &) = delete;
};
struct CacheQueryScope;
thread_local CacheQueryScope *cache_query_scope = nullptr;
struct CacheQueryScope {
    CacheQueryScope *parent;
    CacheLibraryScope *library;
    azsdk_cache_selection::Invocation selection;
    CachePins pins;
    cb::SourceBorrow source;
    cb::Observation row;
    std::uint32_t start = 0, count = 0, client = 0, selector = 0;
    unsigned depth;
    bool ready = false, native_owned = false, adapter_active = false, adapter_done = false, poisoned = false;
    bool published = false;
    static bool owned(void *context) {
        const auto &self = *static_cast<CacheQueryScope *>(context);
        return self.native_owned && self.selection.ready() && self.library == cache_library_scope &&
               self.library->ready && cache_worker_is(self.library->source, self.library->profile);
    }
    static bool mount(void *context, const void *candidate) {
        const auto &self = *static_cast<CacheQueryScope *>(context);
        // This exact Mount already has a genuine, RTTI-checked source pin.
        return candidate == self.library->pins.mount.get();
    }
    explicit CacheQueryScope(const azce_call &call) noexcept
        : parent(cache_query_scope), library(cache_library_scope), selection(source_state().cache_selection),
          source {nullptr, 0, 0, &pins.mount, &pins.database},
          depth(parent ? (parent->depth < kSlots ? parent->depth + 1 : kSlots + 1) : 1) {
        cache_query_scope = this;
        for (auto older = parent; older; older = older->parent)
            if (older->adapter_active) older->poisoned = true;
        if (library) library->row_valid = false;
        if (!library || !library->ready || depth > kSlots || !selection.ready() ||
            library->message.kind != 0x3000 || library->message.fields[3] != 0 ||
            static_cast<unsigned char>(call.args[1]) != library->message.client ||
            static_cast<std::uint32_t>(call.args[2]) != library->message.selector ||
            static_cast<std::uint32_t>(call.args[3]) != library->message.fields[1] ||
            static_cast<std::uint32_t>(call.args[4]) != library->message.fields[2] ||
            !canonical(call.args[0]) || !canonical(call.args[8])) return;
        start = static_cast<std::uint32_t>(call.args[3]);
        count = static_cast<std::uint32_t>(call.args[4]);
        client = library->message.client; selector = library->message.selector;
        if (start > INT32_MAX || !count || count > 16) return;
        try {
            native_owned = true; // actual reviewed QUERY entry's live receiver, on the owned library worker
            azsdk_cache_selection::Borrow borrow {library->source.build,
                reinterpret_cast<const void *>(call.args[0]), this, owned, mount};
            const auto resolved = azsdk_cache_selection::resolve_owned<
                music_library::MountPoint, music_library::SqliteDataBase,
                music_library::ListCacheCollector, music_library::ListCache>(selection, borrow, {client, selector});
            if (resolved.status != azsdk_cache_selection::Ok) return;
            pins.mount = resolved.pins.mount(); pins.database = resolved.pins.database();
            pins.collector = resolved.pins.collector(); pins.cache = resolved.pins.cache();
            if (!cache_owner_is(std::weak_ptr<music_library::MountPoint>(library->pins.mount), pins.mount) ||
                !cache_owner_is(std::weak_ptr<music_library::SqliteDataBase>(library->pins.database), pins.database)) return;
            source.server = library->source.server; source.build = library->source.build;
            source.source_epoch = library->source.source_epoch;
            auto &s = source_state();
            std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
            ready = lock.owns_lock() && selection.ready() && cache_source_is(source, pins);
        } catch (...) { ready = false; }
    }
    ~CacheQueryScope() {
        // A nested query may have completed while this outer original was
        // still active. A refused/throwing outer call cannot lend that inner
        // evidence to the eventual reply of the outer invocation.
        if (!published && library && library == cache_library_scope) library->row_valid = false;
        cache_query_scope = parent;
    }
    CacheQueryScope(const CacheQueryScope &) = delete;
    CacheQueryScope &operator=(const CacheQueryScope &) = delete;
};
bool cache_copy_native_row(void *carrier, std::uint32_t &value) {
    return azsdk_cache_row::copy_owned_tag23(carrier, value);
}
void cache_forward_query_adapter(azce_call *call, cb::Forward original) {
    auto scope = cache_query_scope;
    if (!scope || !scope->ready || scope->poisoned || scope->adapter_active || scope->adapter_done ||
        scope->library != cache_library_scope || !scope->selection.ready() ||
        !canonical(call->args[0]) || !canonical(call->args[3]) || !canonical(call->args[8]) ||
        static_cast<std::uint32_t>(call->args[1]) != scope->start ||
        static_cast<std::uint32_t>(call->args[2]) != scope->count) {
        if (scope) scope->poisoned = true;
        original.call(original.context); return;
    }
    // Both exact adapter entries borrow their selected cache at+8 and receive
    // the real Mount owner in X3. Copy that holder only before native work.
    const auto receiver = load<const void *>(call->args[0] + 8);
    const auto mount = *reinterpret_cast<const std::shared_ptr<music_library::MountPoint> *>(call->args[3]);
    if (receiver != scope->pins.cache.get() || !mount ||
        !cache_owner_is(std::weak_ptr<music_library::MountPoint>(scope->pins.mount), mount)) {
        scope->poisoned = true; original.call(original.context); return;
    }
    scope->adapter_active = true;
    struct AdapterGuard {
        CacheQueryScope *scope;
        bool returned = false;
        explicit AdapterGuard(CacheQueryScope *value) : scope(value) {}
        ~AdapterGuard() { scope->adapter_active = false; if (!returned) scope->poisoned = true; }
    } guard {scope};
    cb::QueryBorrow borrow {scope->source, &scope->pins.collector, &scope->pins.cache, receiver};
    cb::Observation row;
    const auto result = cache_query_impl(borrow, scope->library->profile, original,
        {cache_copy_native_row, reinterpret_cast<void *>(call->args[8])}, &row);
    guard.returned = true;
    const unsigned family = call->kind == AZCE_KIND_GENERIC ? cb::PrimaryFactory : cb::StandardFallbackFactory;
    if (result == AZS_OK && !scope->poisoned && scope->selection.ready() && row.factory_kind == family) {
        scope->row = row; scope->adapter_done = true;
    } else scope->poisoned = true;
}
bool cache_row_binding_current(const azsdk_cache_query::Evidence &row) noexcept {
    for (const auto &binding : source_state().cache_bindings)
        if (binding.birth_epoch == row.cache.birth_epoch && binding.source_epoch == row.cache.source_epoch &&
            binding.generation == row.cache.layout_generation && cache_binding_current(binding, binding.generation) &&
            !binding.cache.owner_before(row.cache.cache) && !row.cache.cache.owner_before(binding.cache) &&
            !binding.collector.owner_before(row.cache.collector) && !row.cache.collector.owner_before(binding.collector)) return true;
    return false;
}
void cache_forward_query(azce_call *call, cb::Forward original) {
    CacheQueryScope scope(*call);
    original.call(original.context);
    try {
    if (!scope.ready || scope.poisoned || !scope.adapter_done || !scope.selection.ready() ||
        scope.library != cache_library_scope || !cache_stamp_is(scope.row.layout_generation)) return;
    std::uint32_t value = 0;
    if (!azsdk_cache_row::copy_owned_tag23(reinterpret_cast<const void *>(call->args[8]), value) ||
        value != scope.row.row_value) return;
    auto &s = source_state();
    std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !scope.selection.ready() || !cache_source_is(scope.source, scope.pins) ||
        !cache_stamp_is(scope.row.layout_generation)) return;
    auto &row = scope.library->row;
    row.cache = scope.row; row.sequence = scope.library->message.sequence;
    row.client = scope.client; row.selector = scope.selector; row.start = scope.start; row.count = scope.count;
    scope.library->row_selection = scope.selection.revision();
    scope.library->row_valid = cache_row_binding_current(row);
    scope.published = scope.library->row_valid;
    } catch (...) { if (scope.library) scope.library->row_valid = false; }
}
void cache_forward_library_impl(unsigned build, void *server, const Build &profile, cb::Forward original) {
    if (!original.call) return;
    CacheLibraryScope scope(build, server, profile);
    original.call(original.context); // no SDK lock or observer catch across native work
}
void cache_forward_library_route(unsigned build, void *server, const Build &profile, cb::Forward original) {
    const bool admitted = azsdk_cache_capture_ready && azsdk_cache_capture_ready() == 1;
    // Always push a masking frame, including while a partial installation is
    // forwarding. An older reentrant source scope cannot authorize this call.
    cache_forward_library_impl(admitted ? build : 0, server, profile, original);
}
void cache_forward_entry_impl(azce_call *call, cb::Forward original) {
    if (!call || !original.call) return;
    if (call->kind == AZCE_KIND_QUERY) { cache_forward_query(call, original); return; }
    if (call->kind == AZCE_KIND_GENERIC || call->kind == AZCE_KIND_TRACKINFO) {
        cache_forward_query_adapter(call, original); return;
    }
    if (call->kind == AZCE_KIND_SELECTION) {
        CacheSelectionScope scope(*call);
        original.call(original.context);
        return;
    }
    auto selected = cache_selection_scope;
    if (call->kind != AZCE_KIND_CREATE || !selected || !selected->ready ||
        selected->library != cache_library_scope || !cache_library_scope->ready ||
        call->args[0] != reinterpret_cast<std::uintptr_t>(selected->pins.collector.get()) ||
        !canonical(call->args[1]) || !canonical(call->args[8])) {
        original.call(original.context);
        return;
    }
    azsdk_cache_pin::Pins pins(selected->source, &selected->pins.collector,
                              reinterpret_cast<const void *>(call->args[0]),
                              reinterpret_cast<const void *>(call->args[1]));
    const auto borrow = pins.borrow(reinterpret_cast<const std::shared_ptr<music_library::ListCache> *>(call->args[8]));
    if (!pins.ready() || borrow.receiver_creator != selected->creator) {
        original.call(original.context);
        return;
    }
    /* Only local owner holders survive the original. Its genuine X8 result is
     * read by the existing birth helper after successful native construction. */
    (void) cache_birth_impl(borrow, selected->library->profile, original, nullptr);
}
} // namespace

namespace azsdk_cache_query {
bool current_owned(Evidence &out) noexcept {
    try {
    auto scope = cache_library_scope;
    if (cache_query_scope || !scope || !scope->ready || !scope->row_valid || scope->message.kind != 0x3000 ||
        !cache_worker_is(scope->source, scope->profile)) return false;
    azsdk_cache_selection::Invocation selection(source_state().cache_selection);
    if (!selection.ready() || selection.revision() != scope->row_selection ||
        !cache_stamp_is(scope->row.cache.layout_generation)) return false;
    auto &s = source_state();
    std::unique_lock<std::mutex> lock(s.registry_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !cache_source_is(scope->source, scope->pins) ||
        !cache_row_binding_current(scope->row) || !selection.ready() ||
        !cache_stamp_is(scope->row.cache.layout_generation)) return false;
    out = scope->row;
    return true;
    } catch (...) { return false; }
}
}

namespace azsdk_cache_birth {
void forward_library_owned(unsigned build, void *server, Forward original) {
    const Build unsupported {0, 0};
    cache_forward_library_route(build, server, build >= 1 && build <= 2 ? builds[build] : unsupported, original);
}
void observe_entry_owned(azce_call *call, Forward original) {
    cache_forward_entry_impl(call, original);
}
void observe_factory_owned(const FactoryBorrow &borrow, Forward original) {
    if (!original.call) return;
    auto frame = cache_birth_scope;
    cache_poison_scopes(frame);
    const bool ready = frame && frame->eligible && !frame->poisoned &&
        !frame->factory_active && !frame->completed && borrow.receiver_creator == frame->creator &&
        borrow.borrowed_pool == frame->pool && borrow.native_result &&
        (borrow.kind == PrimaryFactory || borrow.kind == StandardFallbackFactory) &&
        /* The exact standard factory is supported only for Creator mode0.
         * Read it before forwarding, only after the actual owned Collector
         * scope matched this Creator. An arbitrary factory entry cannot
         * confer permission to follow its receiver. */
        (borrow.kind != StandardFallbackFactory ||
         load<unsigned char>(reinterpret_cast<std::uintptr_t>(borrow.receiver_creator) + 0x44) == 0) &&
        cache_stamp(frame->generation);
    if (!ready) {
        if (frame) frame->poisoned = true;
        original.call(original.context);
        return;
    }
    frame->factory_kind = borrow.kind;
    frame->factory_active = true;
    struct FactoryGuard {
        CacheBirthScope *frame;
        bool returned = false;
        explicit FactoryGuard(CacheBirthScope *value) : frame(value) {}
        ~FactoryGuard() { frame->factory_active = false; if (!returned) frame->poisoned = true; }
    } guard {frame};
    original.call(original.context); // original raw pointer result and exception remain native
    guard.returned = true;
    if (frame->poisoned || !cache_stamp_is(frame->generation) || !*borrow.native_result ||
        (borrow.kind == StandardFallbackFactory && (!frame->setup_seen || frame->setters != 16 ||
                                                  *borrow.native_result != frame->raw_result))) {
        frame->poisoned = true;
        return;
    }
    frame->raw_result = *borrow.native_result;
    frame->completed = true;
}
void observe_construction_write_owned(const WriteBorrow &borrow, Forward original) {
    if (!original.call) return;
    auto frame = cache_birth_scope;
    cache_poison_scopes(frame);
    const bool setup = borrow.kind == SetupWrite;
    const bool ready = frame && frame->eligible && !frame->poisoned && frame->factory_active &&
        frame->factory_kind == StandardFallbackFactory && !frame->write_active &&
        borrow.receiver_cache && borrow.native_status && cache_stamp_is(frame->generation) &&
        ((setup && !frame->setup_seen && borrow.argument == 16 && borrow.borrowed_pool == frame->pool) ||
         (borrow.kind == LayoutWrite && frame->setup_seen && frame->setters < 16 &&
          borrow.argument == frame->setters && borrow.receiver_cache == frame->raw_result));
    if (frame) { if (!ready) frame->poisoned = true; frame->write_active = true; }
    struct WriteGuard {
        CacheBirthScope *frame;
        bool returned = false;
        explicit WriteGuard(CacheBirthScope *value) : frame(value) {}
        ~WriteGuard() { if (frame) { frame->write_active = false; if (!returned) frame->poisoned = true; } }
    } guard {frame};
    const auto before = ready ? frame->generation : 0;
    { CacheMutation mutation; original.call(original.context); }
    guard.returned = true;
    if (!ready) return;
    std::uint64_t after = 0;
    if (frame->poisoned || !cache_stamp(after) || before == UINT64_MAX || after != before + 1 ||
        *borrow.native_status != 0) {
        frame->poisoned = true;
        return;
    }
    frame->generation = after;
    if (setup) { frame->setup_seen = true; frame->raw_result = borrow.receiver_cache; }
    else ++frame->setters;
}
int observe_birth_owned(const BirthBorrow &borrow, Forward original, Observation *out) {
    Build unsupported {0, 0};
    const Build &profile = borrow.source.build >= 1 && borrow.source.build <= 2 ?
                           builds[borrow.source.build] : unsupported;
    return cache_birth_impl(borrow, profile, original, out);
}
int observe_query_owned(const QueryBorrow &borrow, Forward original, CopyRowScalar copy, Observation *out) {
    Build unsupported {0, 0};
    const Build &profile = borrow.source.build >= 1 && borrow.source.build <= 2 ?
                           builds[borrow.source.build] : unsupported;
    return cache_query_impl(borrow, profile, original, copy, out);
}
void retire_forward(Forward original) {
    if (!original.call) return;
    cache_poison_scopes();
    CacheMutation mutation;
    original.call(original.context);
}
void selection_forward(Forward original) {
    if (!original.call) return;
    azsdk_cache_selection::Mutation mutation(source_state().cache_selection,
                                              azsdk_cache_selection::SelectionChange);
    original.call(original.context);
}
} // namespace azsdk_cache_birth

extern "C" int azsdk_sources_configure(int enable, int build_id, unsigned policy) noexcept {
    try {
        std::lock_guard<std::mutex> lock(source_state().registry_mutex);
        source_state().enabled.store(0, std::memory_order_release);
        for (auto &b : source_state().bindings) b = {};
        retire_unused();
        source_state().capture_policy.store(0, std::memory_order_relaxed);
        if (!enable || build_id < 1 || build_id > 2 || (policy & ~kKnownPolicy)) return AZS_UNSUPPORTED;
        int prior = source_state().bound_build.load(std::memory_order_relaxed);
        if (prior && prior != build_id) return AZS_UNSUPPORTED;
        std::call_once(source_state().image_once, prepare_image);
        if (!source_state().main_image.count) return AZS_UNSUPPORTED;
        source_state().bound_build.store(build_id, std::memory_order_relaxed);
        if (policy & AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER)
            std::call_once(source_state().sql_once, prepare_sqlcipher);
        source_state().capture_policy.store(policy, std::memory_order_relaxed);
        source_state().enabled.store(1, std::memory_order_release);
        return AZS_OK;
    } catch (...) { return AZS_UNSUPPORTED; }
}
extern "C" int azsdk_sources_observe_owned_message(void *server, void *out,
                                                   std::size_t size) noexcept {
    if (!output_ok(out, size, true)) return AZS_INVALID;
    if (!source_state().enabled.load(std::memory_order_acquire)) return AZS_UNSUPPORTED;
    int build = source_state().bound_build.load(std::memory_order_relaxed);
    if (build < 1 || build > 2) return AZS_UNSUPPORTED;
    try {
        return observe(reinterpret_cast<std::uintptr_t>(server), builds[build], source_state().sql_profile,
#if defined(__linux__)
                       metadata_ops,
#endif
                       out, size);
    } catch (...) { return AZS_GUARD; }
}
extern "C" int azsdk_sources_read(std::uint64_t epoch, void *out, std::size_t size) noexcept {
    if (!epoch || !output_ok(out, size)) return AZS_INVALID;
    try {
        std::unique_lock<std::mutex> lock(source_state().registry_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return AZS_BUSY;
        for (const auto &e : source_state().entries)
            if (e.sample.owner_epoch == epoch) {
                auto s = scalar_sample(e);
                copy_output(out, size, s);
                return (s.flags & AZS_CURRENT) ? AZS_OK : AZS_STALE;
            }
        return AZS_UNKNOWN;
    } catch (...) { return AZS_BUSY; }
}
extern "C" int azsdk_sources_candidate(std::uint16_t media, void *out, std::size_t size) noexcept {
    if (!media || !output_ok(out, size)) return AZS_INVALID;
    try {
        std::unique_lock<std::mutex> lock(source_state().registry_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return AZS_BUSY;
        if (bit(source_state().ambiguous_media, media)) {
            azsdk_source_sample s = {};
            s.struct_size = sizeof s;
            s.native_media_id = media;
            s.flags = AZS_HISTORY_INCOMPLETE | AZS_MEDIA_AMBIGUOUS;
            copy_output(out, size, s);
            return AZS_AMBIGUOUS;
        }
        for (const auto &e : source_state().entries)
            if (e.sample.native_media_id == media) {
                auto s = scalar_sample(e);
                if (!(s.flags & AZS_CURRENT)) continue;
                copy_output(out, size, s);
                return AZS_OK;
            }
        return AZS_UNKNOWN;
    } catch (...) { return AZS_BUSY; }
}
extern "C" unsigned azsdk_sources_sweep() noexcept {
    try {
        std::lock_guard<std::mutex> lock(source_state().registry_mutex);
        unsigned reclaimed = 0;
        for (auto &e : source_state().entries)
            if (e.sample.owner_epoch && !owner_bound(e)) {
                auto epoch = e.sample.owner_epoch;
                for (auto &b : source_state().bindings)
                    if (b.epoch == epoch) b = {};
                e = Entry(); /* Weak control blocks may be deallocated on this worker only. */
                ++reclaimed;
            }
        return reclaimed;
    } catch (...) { return 0; }
}
extern "C" int azsdk_sources_physical_ready() noexcept {
    return source_state().enabled.load(std::memory_order_acquire) && source_state().physical_prepared.load(std::memory_order_acquire);
}
extern "C" int azsdk_sources_install_connections(
    azsdk_source_connection_installer installer, void *context) noexcept {
    auto &state = source_state();
    const int build = state.bound_build.load(std::memory_order_acquire);
    if (!installer || build < 1 || build > 2) return 0;
    /* install() prepares/reuses the sealed exact-module proof before either
     * redirect. Never hold registry_mutex across installer/native callbacks.
     * This canonical State has process lifetime; source configure/disable does
     * not reset it or turn off an already reachable close observer. */
    return azsdk_connection_hooks::install(state.connection_epochs, {installer, context});
}

#if defined(AZSDK_CACHE_BIRTH_TESTING)
/* The fixture describes a genuine open/close event on its still-live aligned
 * connection object. Exercise the same embedded bookkeeping as native hooks;
 * no test map, direct generation seed, or readiness override is involved. */
extern "C" void azsdk_cache_test_connection_event(void *connection, int open) {
    auto &epochs = source_state().connection_epochs;
    if (open) {
        void *output = nullptr;
        epochs.forward_open(&output, [&]() { output = connection; return 0; });
    } else {
        epochs.forward_close(connection, []() { return 0; });
    }
}
/* The single-TU fixture declares its TEST CONTRACT and synthetic native classes
 * before including this production implementation. Only this adapter knows
 * those fixture types. It delegates every ownership/generation decision to the
 * real helpers and SourceState above; it contains no mirrored registry/model.
 * This is not compiled into ordinary builds, an installer, or a public SDK API.
 */
namespace {
cb::SourceBorrow cache_test_source(const cache_birth_test::Scope &scope) {
    cb::SourceBorrow source {scope.owned_server, scope.build, scope.source_epoch, nullptr, nullptr};
    if (scope.list_server) {
        source.mount = &scope.list_server->mount;
        if (*source.mount)
            source.database = &static_cast<music_library::SqliteMountPoint *>(source.mount->get())->database;
    }
    return source;
}
Build cache_test_profile(const cache_birth_test::Scope &scope) {
    if (!scope.owned_server || scope.build < 1 || scope.build > 2) return {0, 0};
    auto table = load<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(scope.owned_server));
    return {builds[scope.build].dispatcher, load<std::uintptr_t>(table + 5 * 8) - source_state().main_image.base};
}
} // namespace
extern "C" void azsdk_cache_test_forward_library(unsigned build, void *server,
                                                  void (*original)(void *), void *context) {
    cache_birth_test::Scope scope {build, 0, static_cast<music_library::MusicLibraryServer *>(server), nullptr, 0};
    cache_forward_library_impl(build, server, cache_test_profile(scope), {original, context});
}
extern "C" void azsdk_cache_test_forward_guarded_library(unsigned build, void *server,
                                                          void (*original)(void *), void *context) {
    cache_birth_test::Scope scope {build, 0, static_cast<music_library::MusicLibraryServer *>(server), nullptr, 0};
    cache_forward_library_route(build, server, cache_test_profile(scope), {original, context});
}
namespace {
void cache_test_publish(const cb::Observation &observed, cache_birth_test::Witness *out) noexcept {
    if (!out) return;
    out->birth_epoch = observed.birth_epoch;
    out->layout_generation = observed.layout_generation;
    out->source_epoch = observed.source_epoch;
    out->mount = observed.mount;
    out->database = observed.database;
    out->collector = observed.collector;
    out->cache = observed.cache;
    out->row_value = observed.row_value;
}
void cache_test_forward_birth(void *context) {
    static_cast<const cache_birth_test::BirthCall *>(context)->forward();
}
void cache_test_forward_query(void *context) {
    static_cast<const cache_birth_test::QueryCall *>(context)->forward();
}
bool cache_test_copy_row(void *context, std::uint32_t &value) {
    const auto &call = *static_cast<const cache_birth_test::QueryCall *>(context);
    const auto &rows = *static_cast<const NativeRows *>(call.result_storage);
    if (!rows.begin || rows.end != rows.begin + 1 || rows.capacity != rows.end || !rows.begin[0]) return false;
    value = rows.begin[0]->value; // borrowed read of actual original output; no owning carrier copy
    if (call.copy_boundary) call.copy_boundary(call.copy_context); // fixture exercises generic copier reentrancy
    return true;
}
void cache_test_reset(const cache_birth_test::Limits &limits) {
    auto &s = source_state();
    std::lock_guard<std::mutex> lock(s.registry_mutex);
    if (s.cache_active.load(std::memory_order_acquire)) {
        s.cache_saturated.store(true, std::memory_order_release);
        return; // never reset an in-flight depth that an outer guard must release
    }
    for (auto &binding : s.cache_bindings) binding = CacheBinding();
    s.cache_limit = limits.slots <= kSlots ? limits.slots : kSlots;
    s.next_cache_birth = limits.next_birth;
    s.cache_generation.store(limits.generation, std::memory_order_release);
    s.cache_active.store(0, std::memory_order_release);
    s.cache_depth_limit.store(limits.depth <= kSlots ? limits.depth : kSlots, std::memory_order_release);
    s.cache_saturated.store(!limits.generation || !limits.depth || !limits.slots,
                            std::memory_order_release);
    /* Source epochs, source receipts, owner bindings and ambiguity history are
     * untouched. These explicit cache counter seeds exist only in this test TU. */
}
int cache_test_birth(const cache_birth_test::Scope &scope, const cache_birth_test::BirthCall &call,
                      cache_birth_test::Witness *out) {
    cb::BirthBorrow borrow {cache_test_source(scope),
        scope.list_server ? &scope.list_server->collector : nullptr,
        call.creator && *call.creator ? &(*call.creator)->database : nullptr,
        call.collector, call.creator && *call.creator ? call.creator->get() : nullptr,
        call.collector ? call.collector->pool.get() : nullptr,
        static_cast<const std::shared_ptr<music_library::ListCache> *>(call.result_storage)};
    cb::Observation observed;
    int result = cache_birth_impl(borrow, cache_test_profile(scope),
        {cache_test_forward_birth, const_cast<cache_birth_test::BirthCall *>(&call)}, &observed);
    if (result == AZS_OK) cache_test_publish(observed, out);
    return result;
}
int cache_test_query(const cache_birth_test::Scope &scope, const cache_birth_test::QueryCall &call,
                      cache_birth_test::Witness *out) {
    const std::shared_ptr<music_library::ListCache> *selected = nullptr;
    if (scope.list_server && scope.client_bank < scope.list_server->clients.size() &&
        !scope.list_server->clients[scope.client_bank].empty())
        selected = &scope.list_server->clients[scope.client_bank][0];
    cb::QueryBorrow borrow {cache_test_source(scope),
        scope.list_server ? &scope.list_server->collector : nullptr, selected,
        call.adapter ? call.adapter->cache : nullptr};
    cb::Observation observed;
    int result = cache_query_impl(borrow, cache_test_profile(scope),
        {cache_test_forward_query, const_cast<cache_birth_test::QueryCall *>(&call)},
        {cache_test_copy_row, const_cast<cache_birth_test::QueryCall *>(&call)}, &observed);
    if (result == AZS_OK) cache_test_publish(observed, out);
    return result;
}
void cache_test_mutation(cache_birth_test::Mutation, void (*original)(void *), void *context) {
    cb::retire_forward({original, context});
}
cache_birth_test::Counters cache_test_counters() {
    auto &s = source_state();
    std::lock_guard<std::mutex> lock(s.registry_mutex);
    cache_birth_test::Counters result;
    for (const auto &binding : s.cache_bindings) if (binding.birth_epoch) ++result.retained;
    result.next_birth = s.next_cache_birth;
    result.generation = s.cache_generation.load(std::memory_order_acquire);
    result.active = s.cache_active.load(std::memory_order_acquire);
    result.saturated = s.cache_saturated.load(std::memory_order_acquire);
    return result;
}
} // namespace
extern "C" __attribute__((visibility("hidden")))
const cache_birth_test::Api *azsdk_cache_birth_test_api_v1() {
    static const cache_birth_test::Api api {1, cache_test_reset, cache_test_birth, cache_test_query,
                                           cache_test_mutation, cache_test_counters};
    return &api;
}
#endif

namespace {
struct ReplyCodeProfile {
    unsigned build = 0;
    std::uintptr_t begin = 0, return_pc = 0;
    std::size_t length = 0;
    char sha256[65] = {};
};
#ifdef AZSDK_CACHE_BIRTH_TESTING
thread_local ReplyCodeProfile reply_test_code;
#endif
bool reply_capture_enabled() noexcept {
    return source_state().enabled.load(std::memory_order_acquire) &&
           azsdk_cache_capture_ready && azsdk_cache_capture_ready();
}
bool reply_code_profile(ReplyCodeProfile &out) noexcept {
    if (!reply_capture_enabled()) return false;
    const unsigned build = unsigned(source_state().bound_build.load(std::memory_order_acquire));
    ReplyCodeProfile code;
#ifdef AZSDK_CACHE_BIRTH_TESTING
    if (reply_test_code.build) code = reply_test_code;
    else
#endif
    {
        azsdk_cache_reply_code generated;
        if (!azsdk_cache_reply_code_profile(int(build), &generated)) return false;
        const auto base = source_state().main_image.base;
        if (generated.begin_rva > UINTPTR_MAX - base || generated.return_rva > UINTPTR_MAX - base) return false;
        code.build = build;
        code.begin = base + generated.begin_rva;
        code.return_pc = base + generated.return_rva;
        code.length = generated.length;
        std::memcpy(code.sha256, generated.sha256, sizeof code.sha256);
    }
    if (code.build != build || !code.begin || (code.begin & 3u) || (code.return_pc & 3u) ||
        code.length < 4 || code.length > 65536 || code.length > UINTPTR_MAX - code.begin ||
        code.return_pc < code.begin + 4 || code.return_pc > code.begin + code.length ||
        code.sha256[64] || !source_state().main_image.contains(code.begin, code.length, kRead | kExecute)) return false;
    char digest[65];
    azsha256_hex(reinterpret_cast<const void *>(code.begin), code.length, digest);
    if (std::memcmp(digest, code.sha256, sizeof digest)) return false;
    out = code;
    return true;
}

/* This is a GNU weak-owner ABI view of the genuine tag2 ListenerReference
 * member, not a recovered weak<Reception> nominal type. Same-type copying of
 * this view preserves its alias/control and weak count without a converting
 * weak constructor's possible early promotion. The pointee is never accessed
 * through this erased type or retained as a raw pointer. */
struct ReplyWeakAbiPointee;
using ReplyWeakAbi = std::weak_ptr<ReplyWeakAbiPointee>;
static_assert(sizeof(ReplyWeakAbi) == 16 && alignof(ReplyWeakAbi) == 8,
              "reviewed native tag2 GNU weak owner ABI");

struct ReplyCallbackScope;
thread_local ReplyCallbackScope *reply_callback = nullptr;
struct ReplyCallbackScope {
    ReplyCallbackScope *parent;
    const void *invocation = nullptr;
    azsdk_cache_reply::CallbackReceipt receipt;
    bool ready = false;
    ReplyCallbackScope() noexcept : parent(reply_callback) { reply_callback = this; }
    ~ReplyCallbackScope() { reply_callback = parent; }
    ReplyCallbackScope(const ReplyCallbackScope &) = delete;
    ReplyCallbackScope &operator=(const ReplyCallbackScope &) = delete;
};
}

namespace azsdk_cache_reply {
void forward_ticket_owned(azce_call *call, azsdk_cache_birth::Forward original) {
    if (!original.call) return;
    ReplyWeakAbi empty;
    const ReplyWeakAbi *receiver = &empty;
    Request request;
    std::uintptr_t return_pc = 0;
    ReplyCodeProfile code;
    const auto ticket = call ? std::uintptr_t(call->args[0]) : 0;
    /* The reviewed native process entry is invoked with a live owning-queue
     * Ticket. The metadata checks below constrain that borrow; they acquire no
     * liveness. Every native field is copied BEFORE forwarding and is never
     * reread after processing can destroy/mutate its packet or Ticket. */
    if (call && !call->reserved && !call->caller_reserved && reply_code_profile(code) &&
        checked_vtable(ticket, "N13music_library21LoadInfoRequestTicketE", 14) &&
        load<unsigned char>(ticket + 0x80) == 2) {
        const auto packet = load<std::uintptr_t>(ticket + 0x28);
        if (canonical(packet)) {
            std::memcpy(request.request_id, reinterpret_cast<const void *>(ticket + 8), sizeof request.request_id);
            request.sequence = load<std::uint32_t>(packet + 8);
            request.reply_kind = load<std::uint16_t>(packet);
            receiver = reinterpret_cast<const ReplyWeakAbi *>(ticket + 0x88);
            return_pc = code.return_pc;
        }
    }
    // Ineligible/nested entries still mask an outer scope while forwarding.
    Scope selected(*receiver, request, return_pc);
    receiver = nullptr;
    selected.forward([&] { original.call(original.context); });
}

void forward_reception_owned(azce_call *call, azsdk_cache_birth::Forward original) {
    if (!original.call) return;
    ReplyCallbackScope callback; // foreign/reentrant callbacks mask older receipts
    ReplyCodeProfile code;
    const auto receiver = call ? std::uintptr_t(call->args[0]) : 0;
    if (call && !call->reserved && !call->caller_reserved && reply_code_profile(code) &&
        call->caller_link == code.return_pc) {
        Promotion native;
        native.control = call->caller_x21;
        native.caller_link = call->caller_link;
        if (receive_current_owned(native, reinterpret_cast<const void *>(receiver), callback.receipt.selected) &&
            checked_vtable(receiver, "N17trackinfo_stocker22LoadInfoRequestHandler9ReceptionE", 3)) {
            /* Native slot13's checked code keeps its actual X21 strong lease
             * until after this callback returns. The helper's temporary lock
             * is already released. These two copied fields confer no Stocker
             * ownership and are not a player load ID/source association. */
            callback.receipt.pack_id = load<std::uint32_t>(receiver + 8);
            callback.receipt.borrowed_stocker = load<std::uintptr_t>(receiver + 0x10);
            callback.invocation = current_invocation();
            callback.ready = true;
        }
    }
    original.call(original.context);
}

bool current_callback_owned(CallbackReceipt &out) noexcept {
    const auto callback = reply_callback;
    if (!callback || !callback->ready || !reply_capture_enabled() ||
        callback->invocation != current_invocation()) return false;
    out = callback->receipt;
    return true;
}
}

#ifdef AZSDK_CACHE_BIRTH_TESTING
extern "C" __attribute__((visibility("hidden"))) void azsdk_cache_test_reply_profile(
    unsigned build, std::uintptr_t begin, std::uintptr_t return_pc, std::size_t length, const char *sha256) noexcept {
    reply_test_code = {};
    if (!build || !sha256) return;
    reply_test_code.build = build;
    reply_test_code.begin = begin;
    reply_test_code.return_pc = return_pc;
    reply_test_code.length = length;
    std::memcpy(reply_test_code.sha256, sha256, sizeof reply_test_code.sha256);
}
#endif
