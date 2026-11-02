/* SPDX-License-Identifier: GPL-3.0-only */
/* Real GNU shared/weak ownership, synthetic native-shaped classes. No EP147 execution.
 * Including the production implementation permits explicit fixture Build/SqlProfile data;
 * production entry points cannot accept these profiles or bypass their native guards. */
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#if !defined(__linux__)
int main() {
    std::puts("SKIP: native source fixture requires Linux ELF/GNU ABI");
    return 77;
}
#else
#include <fcntl.h>
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace music_library {
class SqliteDataBase {
   public:
    virtual ~SqliteDataBase() { ++destroyed; }
    std::uint16_t media;
    unsigned char padding[0x38 - 10] = {};
    void *connection;
    static std::atomic<unsigned> destroyed;
    SqliteDataBase(std::uint16_t m, void *c) : media(m), connection(c) {}
};
std::atomic<unsigned> SqliteDataBase::destroyed {0};
class MountPoint {
   public:
    virtual ~MountPoint() { ++destroyed; }
    std::uint16_t media;
    unsigned char padding[0x30 - 10] = {};
    static std::atomic<unsigned> destroyed;
    explicit MountPoint(std::uint16_t m) : media(m) {}
};
std::atomic<unsigned> MountPoint::destroyed {0};
class SqliteMountPoint : public MountPoint {
   public:
    std::shared_ptr<SqliteDataBase> database;
    SqliteMountPoint(std::uint16_t m, void *c) : MountPoint(m), database(std::make_shared<SqliteDataBase>(m, c)) {}
};
class NoDBMountPoint : public MountPoint {
   public:
    explicit NoDBMountPoint(std::uint16_t m) : MountPoint(m) {}
};
class InformationUpdater {
   public:
    virtual ~InformationUpdater() {}
    std::shared_ptr<MountPoint> mount;
};
class MusicLibraryServer {
   public:
    virtual ~MusicLibraryServer() {}
    virtual void reserved2() {}
    virtual void reserved3() {}
    virtual void reserved4() {}
    virtual bool needsToExecuteAsync() {
        ++affinity_calls;
        if (throws) throw std::runtime_error("synthetic worker method failure");
        return std::this_thread::get_id() != worker;
    }
    virtual void reserved6() {}
    virtual void reserved7() {}
    virtual void reserved8() {}
    virtual void reserved9() {}
    virtual void onMessage() {}
    unsigned char padding[0x1d0 - 8] = {};
    std::thread::id worker = std::this_thread::get_id();
    std::atomic<unsigned> affinity_calls {0};
    bool throws = false;
};
} // namespace music_library
#if defined(AZS_SAMPLE_BOUNDARY)
/* The suite instruments only its copied production source at scalar_sample entry.
 * Native weak owners may expire independently of registry_mutex. */
static void (*source_sample_boundary)();
#endif
#ifndef SOURCES_INC
#define SOURCES_INC "../../src/azmod/sdk_native_sources.cpp"
#endif
#include SOURCES_INC

namespace {
using namespace music_library;
struct Handler {
    unsigned char padding[0x30] = {};
    InformationUpdater *updater;
};
struct Dispatcher {
    unsigned char padding[0x20] = {};
    Handler *handler;
};
unsigned test_build;
std::atomic<unsigned> sql_calls {0}, stat_calls {0}, open_calls {0}, read_calls {0}, close_calls {0};
std::atomic<unsigned> size_calls {0}, native_close_calls {0}, native_read_calls {0};
int forbidden_native_fd = -1, metadata_mode = 0, size_mode = 0;
struct FileStorage {
    const NativeIO *methods;
    NativeVFS *vfs;
    std::uint64_t padding = 0;
    std::int32_t fd;
    unsigned char tail[120 - 28] = {};
};
static_assert(offsetof(FileStorage, fd) == 0x18 && sizeof(FileStorage) == 120, "Unix file fixture");
struct Connection {
    FileStorage *file = nullptr;
    NativeVFS *vfs = nullptr;
    int failed_opcode = 0;
};
NativeIO fixture_io[3];
NativeVFS fixture_vfs;
int fake_close(NativeFile *) {
    ++native_close_calls;
    return 0;
}
int fake_read(NativeFile *, void *, int, std::int64_t) {
    ++native_read_calls;
    return 0;
}
int fake_control(NativeFile *, int, void *) { return 0; }
int fake_open(NativeVFS *, const char *, NativeFile *, int, int *) { return 0; }
int fake_size(NativeFile *file, std::int64_t *out) {
    ++size_calls;
    auto &f = *reinterpret_cast<FileStorage *>(file);
    struct stat value;
    if (::fstat(f.fd, &value)) return 1;
    *out = value.st_size;
    if (size_mode == 1) ++*out;
    if (size_mode == 2) ++f.fd;
    if (size_mode == 3) f.methods = &fixture_io[1];
    if (size_mode == 4) f.vfs = nullptr;
    return size_mode == 5 ? 1 : 0;
}
int fake_sql_control(sqlite3 *db, const char *schema, int opcode, void *out) {
    ++sql_calls;
    assert(!std::strcmp(schema, "main"));
    auto &c = *reinterpret_cast<Connection *>(db);
    if (c.failed_opcode == opcode) return 1;
    if (opcode == 7) *static_cast<NativeFile **>(out) = reinterpret_cast<NativeFile *>(c.file);
    else if (opcode == 27) *static_cast<NativeVFS **>(out) = c.vfs;
    else assert(false);
    return 0;
}
int counted_stat(int fd, struct stat *out) {
    unsigned call = ++stat_calls;
    assert(fd == forbidden_native_fd);
    if (metadata_mode == 1 || (metadata_mode == 2 && call == 2)) return -1;
    int result = ::fstat(fd, out);
    if (!result && metadata_mode == 3 && call == 2) ++out->st_ino;
    if (!result && metadata_mode == 4) out->st_mode = S_IFIFO;
    return result;
}
int counted_open(const char *path, int flags) {
    ++open_calls;
    assert(!std::strncmp(path, "/proc/self/fdinfo/", 18));
    assert((flags & (O_RDONLY | O_CLOEXEC | O_NONBLOCK)) == (O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    return metadata_mode == 5 ? -1 : ::open(path, flags);
}
ssize_t counted_read(int fd, void *out, std::size_t size) {
    ++read_calls;
    assert(fd != forbidden_native_fd && size <= 4096);
    if (metadata_mode == 6) return -1;
    if (metadata_mode == 7) {
        std::memset(out, 'x', size);
        return static_cast<ssize_t>(size);
    }
    if (metadata_mode == 8) {
        *static_cast<char *>(out) = 'x';
        return 1;
    }
    if (metadata_mode == 9) size = size < 32 ? size : 32;
    return ::read(fd, out, size);
}
int counted_close(int fd) {
    ++close_calls;
    assert(fd != forbidden_native_fd); /* Must never close/dup-close SQLite's own FD. */
    int result = ::close(fd);
    return metadata_mode == 10 ? -1 : result;
}
const MetadataOps counted_ops {counted_stat, counted_open, counted_read, counted_close};
void clear_counts() {
    sql_calls = stat_calls = open_calls = read_calls = close_calls = 0;
    size_calls = native_close_calls = native_read_calls = 0;
    metadata_mode = size_mode = 0;
}
void no_physical_calls() {
    assert(!sql_calls && !stat_calls && !open_calls && !read_calls && !close_calls && !size_calls);
    assert(!native_close_calls && !native_read_calls);
}
void no_filesystem_calls() {
    assert(!stat_calls && !open_calls && !read_calls && !close_calls && !size_calls);
    assert(!native_close_calls && !native_read_calls);
}
void configure(unsigned policy = 0) {
    assert(azsdk_sources_configure(0, static_cast<int>(test_build), 0) == AZS_UNSUPPORTED);
    azsdk_sources_sweep();
    assert(azsdk_sources_configure(1, static_cast<int>(test_build), policy) == AZS_OK);
}
template <class T>
void put(void *object, std::size_t offset, const T &value) {
    std::memcpy(static_cast<unsigned char *>(object) + offset, &value, sizeof value);
}
struct Fixture {
    MusicLibraryServer server;
    InformationUpdater updater;
    Handler handler;
    Dispatcher dispatcher;
    Connection connection;
    explicit Fixture(std::uint16_t media) {
        handler.updater = &updater;
        dispatcher.handler = &handler;
        auto p = &dispatcher;
        put(&server, builds[test_build].dispatcher, p);
        auto thread = &server.worker;
        put(&server, 0x28, thread);
        marker(1);
        replace(media);
        assert(reinterpret_cast<unsigned char *>(&updater.mount) - reinterpret_cast<unsigned char *>(&updater) == 8);
        auto &m = mount();
        assert(reinterpret_cast<unsigned char *>(&m.database) - reinterpret_cast<unsigned char *>(&m) == 0x30);
        assert(reinterpret_cast<unsigned char *>(&m.database->connection) - reinterpret_cast<unsigned char *>(m.database.get()) == 0x38);
    }
    void marker(unsigned char value) { put(&server, 0xd2, value); }
    void replace(std::uint16_t media) { updater.mount = std::make_shared<SqliteMountPoint>(media, &connection); }
    SqliteMountPoint &mount() { return *static_cast<SqliteMountPoint *>(updater.mount.get()); }
    Build profile() const {
        auto table = load<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(&server));
        return {builds[test_build].dispatcher, load<std::uintptr_t>(table + 5 * 8) - source_state().main_image.base};
    }
};
SqlProfile empty_sql;
int capture(Fixture &f, azsdk_source_sample *out, const SqlProfile &p = empty_sql) {
    try {
        return observe(reinterpret_cast<std::uintptr_t>(&f.server), f.profile(), p, counted_ops,
                       out, out ? sizeof *out : 0);
    } catch (...) { return AZS_GUARD; } /* Same exception boundary as the C entry point. */
}
azsdk_source_sample captured(Fixture &f, const SqlProfile &p = empty_sql) {
    azsdk_source_sample out = {};
    assert(capture(f, &out, p) == AZS_OK);
    assert(out.struct_size == sizeof out && out.owner_epoch && out.revision && out.observed_ns);
    assert((out.flags & (AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE)) ==
           (AZS_OWNER_OBSERVED | AZS_CURRENT | AZS_HISTORY_INCOMPLETE));
    assert(!out.reserved && out.native_media_id == f.mount().media);
    return out;
}
template <class F>
void unchanged(F action, int result) {
    azsdk_source_sample out, before;
    std::memset(&out, 0xa5, sizeof out);
    before = out;
    assert(action(&out) == result);
    assert(!std::memcmp(&out, &before, sizeof out));
}
void api_and_guards() {
    assert(azsdk_sources_configure(0, 0, 0) == AZS_UNSUPPORTED);
    unchanged([](azsdk_source_sample *o) { return azsdk_sources_observe_owned_message(nullptr, o, sizeof *o); }, AZS_UNSUPPORTED);
    assert(azsdk_sources_configure(1, 0, 0) == AZS_UNSUPPORTED);
    assert(azsdk_sources_configure(1, static_cast<int>(test_build), 2) == AZS_UNSUPPORTED);
    configure(AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER);
    Fixture f(0x101);
    clear_counts();
    /* The real entry point refuses this synthetic method RVA even with correct RTTI. */
    unchanged([&](azsdk_source_sample *o) { return azsdk_sources_observe_owned_message(&f.server, o, sizeof *o); }, AZS_GUARD);
    assert(!f.server.affinity_calls);
    no_physical_calls();
    assert(azsdk_sources_observe_owned_message(&f.server, nullptr, 1) == AZS_INVALID);
    unsigned char prefix[7] = {};
    assert(azsdk_sources_observe_owned_message(&f.server, prefix, sizeof prefix) == AZS_INVALID);
    f.marker(0);
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    assert(!f.server.affinity_calls);
    f.marker(2);
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    assert(!f.server.affinity_calls);
    f.marker(1);
    std::uintptr_t no_worker = 0;
    put(&f.server, 0x28, no_worker);
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    assert(!f.server.affinity_calls);
    auto worker = &f.server.worker;
    put(&f.server, 0x28, worker);
    int result = 0;
    std::thread foreign([&] { result = capture(f, nullptr); });
    foreign.join();
    assert(result == AZS_GUARD && f.server.affinity_calls == 1);
    no_physical_calls();
    f.server.throws = true;
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.server.throws = false;
    no_physical_calls();
    auto server = reinterpret_cast<std::uintptr_t>(&f.server);
    auto table = load<std::uintptr_t>(server);
    std::uintptr_t heap_table[11] = {};
    auto outside_image = reinterpret_cast<std::uintptr_t>(heap_table);
    put(&f.server, 0, outside_image);
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    put(&f.server, 0, table);
    auto dispatcher = &f.dispatcher;
    std::uintptr_t null_pointer = 0;
    put(&f.server, builds[test_build].dispatcher, null_pointer);
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    put(&f.server, builds[test_build].dispatcher, dispatcher);
    f.dispatcher.handler = nullptr;
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.dispatcher.handler = &f.handler;
    f.handler.updater = nullptr;
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.handler.updater = &f.updater;
    auto good_updater = f.handler.updater;
    f.handler.updater = reinterpret_cast<InformationUpdater *>(&f.server);
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.handler.updater = good_updater;
    f.updater.mount.reset();
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.updater.mount = std::make_shared<NoDBMountPoint>(0x101);
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.replace(0x101);
    auto good_mount = f.updater.mount;
    f.updater.mount = std::shared_ptr<MountPoint>(std::shared_ptr<MountPoint>(), good_mount.get());
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.updater.mount = good_mount;
    auto good_database = f.mount().database;
    f.mount().database.reset();
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.mount().database = std::shared_ptr<SqliteDataBase>(good_database, reinterpret_cast<SqliteDataBase *>(&f.server));
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.mount().database = good_database;
    f.mount().database->media = 0;
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.mount().database->media = 0x102;
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.mount().database->media = 0x101;
    f.mount().database->connection = nullptr;
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.mount().database->connection = &f.connection;
    auto db = f.mount().database;
    f.mount().database = std::shared_ptr<SqliteDataBase>(std::shared_ptr<SqliteDataBase>(), db.get());
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_GUARD);
    f.mount().database = db;
    no_physical_calls();
    auto out = captured(f); /* Profile unavailable: no native/filesystem calls. */
    assert(out.physical_status == AZS_PHYSICAL_PROFILE && !(out.flags & AZS_PHYSICAL));
    no_physical_calls();
    assert(azsdk_sources_configure(1, test_build == 1 ? 2 : 1, 0) == AZS_UNSUPPORTED);
    azsdk_source_sample old;
    assert(azsdk_sources_read(out.owner_epoch, &old, sizeof old) == AZS_STALE);
    assert(!(old.flags & (AZS_CURRENT | AZS_PHYSICAL)));
}
void weak_lifetime_and_outputs() {
    configure();
    Fixture f(0x110);
    auto m_count = f.updater.mount.use_count(), d_count = f.mount().database.use_count();
    auto first = captured(f);
    assert(f.updater.mount.use_count() == m_count && f.mount().database.use_count() == d_count);
    auto second = captured(f);
    assert(second.owner_epoch == first.owner_epoch && second.revision > first.revision);
    azsdk_source_sample out;
    assert(azsdk_sources_candidate(0x110, &out, sizeof out) == AZS_OK && out.owner_epoch == first.owner_epoch);
    unchanged([](azsdk_source_sample *o) { return azsdk_sources_read(0, o, sizeof *o); }, AZS_INVALID);
    unchanged([](azsdk_source_sample *o) { return azsdk_sources_read(UINT64_MAX, o, sizeof *o); }, AZS_UNKNOWN);
    unchanged([](azsdk_source_sample *o) { return azsdk_sources_candidate(0, o, sizeof *o); }, AZS_INVALID);
    assert(azsdk_sources_read(first.owner_epoch, nullptr, sizeof out) == AZS_INVALID);
    unsigned char prefix[17], expected[17];
    std::memset(prefix, 0xa5, sizeof prefix);
    std::memcpy(expected, prefix, sizeof prefix);
    assert(azsdk_sources_read(first.owner_epoch, prefix + 1, 8) == AZS_OK);
    assert(prefix[0] == expected[0] && !std::memcmp(prefix + 9, expected + 9, 8));
    std::uint32_t size;
    std::memcpy(&size, prefix + 1, 4);
    assert(size == sizeof out); /* Unaligned, exactly eight-byte earlier prefix is valid. */
    struct Extended {
        azsdk_source_sample sample;
        unsigned char tail[16];
    } extended;
    std::memset(&extended, 0xa5, sizeof extended);
    assert(azsdk_sources_read(first.owner_epoch, &extended, sizeof extended) == AZS_OK);
    for (auto byte : extended.tail) assert(byte == 0xa5);
    unsigned destroyed_m = MountPoint::destroyed, destroyed_d = SqliteDataBase::destroyed;
    f.updater.mount.reset();
    assert(MountPoint::destroyed == destroyed_m + 1 && SqliteDataBase::destroyed == destroyed_d + 1);
    assert(azsdk_sources_read(first.owner_epoch, &out, sizeof out) == AZS_STALE);
    assert(!(out.flags & AZS_CURRENT));
    assert(azsdk_sources_candidate(0x110, &out, sizeof out) == AZS_UNKNOWN);
    assert(azsdk_sources_sweep() == 1);
    unchanged([&](azsdk_source_sample *o) { return azsdk_sources_read(first.owner_epoch, o, sizeof *o); }, AZS_UNKNOWN);
    f.replace(0x110);
    auto next = captured(f);
    assert(next.owner_epoch > first.owner_epoch && (next.flags & AZS_MEDIA_AMBIGUOUS));
    assert(azsdk_sources_candidate(0x110, &out, sizeof out) == AZS_AMBIGUOUS && !out.owner_epoch);
    f.updater.mount.reset();
    azsdk_sources_sweep();
    assert(azsdk_sources_candidate(0x110, &out, sizeof out) == AZS_AMBIGUOUS && !out.owner_epoch);
}
#if defined(AZS_SAMPLE_BOUNDARY)
static Fixture *expiring_fixture;
static unsigned sample_boundary_calls;
void expire_source_before_sample() {
    ++sample_boundary_calls;
    expiring_fixture->updater.mount.reset();
}
void candidate_expiry_during_sample() {
    configure();
    Fixture f(0x111);
    auto first = captured(f);
    expiring_fixture = &f;
    sample_boundary_calls = 0;
    source_sample_boundary = expire_source_before_sample;
    /* Expire after candidate selection but before the copied snapshot's lifetime
     * check. An OK result must describe that exact copied snapshot. */
    unchanged([](azsdk_source_sample *out) {
        return azsdk_sources_candidate(0x111, out, sizeof *out);
    },
              AZS_UNKNOWN);
    source_sample_boundary = nullptr;
    expiring_fixture = nullptr;
    assert(sample_boundary_calls == 1);
    azsdk_source_sample stale;
    assert(azsdk_sources_read(first.owner_epoch, &stale, sizeof stale) == AZS_STALE);
    assert(!(stale.flags & (AZS_CURRENT | AZS_PHYSICAL)));
    assert(!stale.device && !stale.inode && !stale.mount_id && !stale.file_size && !stale.physical_observed_ns);
}
#endif
void owner_address_reuse() {
    configure();
    Fixture f(0x120);
    std::unique_ptr<SqliteMountPoint> raw(new SqliteMountPoint(0x120, &f.connection));
    auto first_owner = std::shared_ptr<MountPoint>(raw.get(), [](MountPoint *) {});
    f.updater.mount = first_owner;
    auto a = captured(f);
    auto second_owner = std::shared_ptr<MountPoint>(raw.get(), [](MountPoint *) {});
    f.updater.mount = second_owner; /* SAME live raw address, DIFFERENT actual owner. */
    auto b = captured(f);
    assert(a.owner_epoch != b.owner_epoch && (b.flags & AZS_MEDIA_AMBIGUOUS));
    assert(first_owner.use_count() == 1 && second_owner.use_count() == 2);
    azsdk_source_sample out;
    assert(azsdk_sources_read(a.owner_epoch, &out, sizeof out) == AZS_STALE);
    std::unique_ptr<SqliteDataBase> db(new SqliteDataBase(0x120, &f.connection));
    auto db_first = std::shared_ptr<SqliteDataBase>(db.get(), [](SqliteDataBase *) {});
    f.mount().database = db_first;
    auto c = captured(f);
    f.mount().database = std::shared_ptr<SqliteDataBase>(db.get(), [](SqliteDataBase *) {});
    auto d = captured(f);
    assert(c.owner_epoch != d.owner_epoch && db_first.use_count() == 1);
    Connection other;
    f.mount().database->connection = &other;
    auto e = captured(f);
    assert(e.owner_epoch != d.owner_epoch);
    f.updater.mount.reset();
    azsdk_sources_sweep();
    assert(azsdk_sources_candidate(0x120, &out, sizeof out) == AZS_AMBIGUOUS);
    /* Aliasing shared pointers may have one owner but point at two actual objects. */
    struct Pair {
        SqliteMountPoint a, b;
        explicit Pair(void *p) : a(0x121, p), b(0x121, p) {}
    };
    auto pair = std::make_shared<Pair>(&f.connection);
    f.updater.mount = std::shared_ptr<MountPoint>(pair, &pair->a);
    a = captured(f);
    f.updater.mount = std::shared_ptr<MountPoint>(pair, &pair->b);
    b = captured(f);
    assert(a.owner_epoch != b.owner_epoch);
}
void binding_and_quota() {
    configure();
    Fixture a(0x130), b(0x130);
    b.updater.mount = a.updater.mount;
    auto first = captured(a), second = captured(b);
    assert(first.owner_epoch == second.owner_epoch);
    a.updater.mount.reset();
    assert(capture(a, nullptr) == AZS_GUARD);
    azsdk_source_sample out;
    assert(azsdk_sources_read(first.owner_epoch, &out, sizeof out) == AZS_OK);
    b.updater.mount.reset();
    assert(capture(b, nullptr) == AZS_GUARD);
    assert(azsdk_sources_read(first.owner_epoch, &out, sizeof out) == AZS_STALE);
    configure();
    Fixture f(0x140);
    for (unsigned i = 0; i < kSlots; ++i) {
        f.replace(static_cast<std::uint16_t>(0x140 + i));
        captured(f);
    }
    f.replace(0x180);
    source_state().capture_policy.store(AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER);
    clear_counts();
    unchanged([&](azsdk_source_sample *o) { return capture(f, o); }, AZS_LIMIT);
    no_physical_calls();
    assert(azsdk_sources_sweep() == kSlots);
    auto after_limit = captured(f);
    assert(after_limit.flags & AZS_MEDIA_AMBIGUOUS);
    configure();
    std::vector<std::unique_ptr<Fixture> > servers;
    servers.emplace_back(new Fixture(0x190));
    first = captured(*servers[0]);
    for (unsigned i = 1; i < kSlots + 1; ++i) {
        servers.emplace_back(new Fixture(0x190));
        servers.back()->updater.mount = servers[0]->updater.mount;
        if (i < kSlots) assert(captured(*servers.back()).owner_epoch == first.owner_epoch);
    }
    unchanged([&](azsdk_source_sample *o) { return capture(*servers.back(), o); }, AZS_LIMIT);
    assert(azsdk_sources_candidate(0x190, &out, sizeof out) == AZS_AMBIGUOUS && !out.owner_epoch);
    configure();
    Fixture last(0x191);
    auto saved_epoch = source_state().next_epoch;
    source_state().next_epoch = UINT64_MAX;
    unchanged([&](azsdk_source_sample *o) { return capture(last, o); }, AZS_LIMIT);
    source_state().next_epoch = saved_epoch; /* Isolated exhaustion fixture; production never resets it. */
    auto saved_revision = source_state().next_revision;
    source_state().next_revision = UINT64_MAX;
    unchanged([&](azsdk_source_sample *o) { return capture(last, o); }, AZS_LIMIT);
    source_state().next_revision = saved_revision;
}
SqlProfile physical_profile() {
    for (unsigned i = 0; i < 3; ++i) {
        fixture_io[i] = {};
        fixture_io[i].version = i == 2 ? 1 : 3;
        fixture_io[i].close = fake_close;
        fixture_io[i].read = fake_read;
        fixture_io[i].size = fake_size;
        fixture_io[i].control = fake_control;
    }
    fixture_vfs = {};
    fixture_vfs.version = 3;
    fixture_vfs.os_size = 120;
    fixture_vfs.max_path = 512;
    fixture_vfs.name = "unix";
    fixture_vfs.open = fake_open;
    SqlProfile p;
    p.image = source_state().main_image;
    p.file_control = fake_sql_control;
    for (unsigned i = 0; i < 3; ++i) p.io_rvas[i] = reinterpret_cast<std::uintptr_t>(&fixture_io[i]) - p.image.base;
    p.open_rva = reinterpret_cast<std::uintptr_t>(fake_open) - p.image.base;
    p.read_rva = reinterpret_cast<std::uintptr_t>(fake_read) - p.image.base;
    p.size_rva = reinterpret_cast<std::uintptr_t>(fake_size) - p.image.base;
    p.control_rva = reinterpret_cast<std::uintptr_t>(fake_control) - p.image.base;
    p.ready = true;
    return p;
}
void mount_id_parser() {
    std::uint64_t out = 99;
    auto parse = [&](const char *s) { return parse_mount_id(s, std::strlen(s), out); };
    assert(parse("pos:\t0\nflags:\t0100000\nmnt_id:\t1234\nino:\t7\n") && out == 1234);
    assert(parse("mnt_id: 18446744073709551615 \n") && out == UINT64_MAX);
    assert(!parse("mnt_id: 18446744073709551616\n"));
    assert(!parse("mnt_id: 0\n") && !parse("mnt_id: -1\n") && !parse("mnt_id: +1\n"));
    assert(!parse("mnt_id: 7x\n") && !parse("mnt_id:\n") && !parse("mnt_id: 7"));
    assert(!parse("mnt_id: 7\nmnt_id: 8\n") && !parse("other: 7\n") && !parse("xmnt_id: 8\n"));
}
void vfs_name_boundaries() {
    /* The image check is a module-span bound, not a string-object bound. Put each
     * valid C string immediately before an inaccessible page while the synthetic
     * image span includes both pages, so plain builds catch widened comparisons. */
    long page_size = ::sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    auto bytes = static_cast<std::size_t>(page_size);
    auto memory = static_cast<char *>(::mmap(nullptr, 2 * bytes, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    assert(!::mprotect(memory + bytes, bytes, PROT_NONE));
    pid_t child = ::fork();
    assert(child >= 0);
    if (!child) {
        SqlProfile p;
        auto begin = reinterpret_cast<std::uintptr_t>(memory);
        p.image.ranges[0] = {begin, begin + 2 * bytes, kRead};
        p.image.count = 1;
        const char *const accepted[] = {"unix", "unix-excl", "unix-none", "unix-dotfile"};
        const char *const rejected[] = {"", "u", "uni", "unix-", "unix-e", "unix-exc", "unix-dotfil",
                                        "other", "unix-exclX", "unix-none-", "unix-dotfile-"};
        for (auto name : accepted) {
            auto size = std::strlen(name) + 1;
            char *at = memory + bytes - size;
            std::memcpy(at, name, size);
            assert(supported_name(p, at));
        }
        for (auto name : rejected) {
            auto size = std::strlen(name) + 1;
            char *at = memory + bytes - size;
            std::memcpy(at, name, size);
            assert(!supported_name(p, at));
        }
        /* Bounds/overflow must be rejected before the first invalid byte read. */
        assert(!supported_name(p, nullptr));
        assert(!supported_name(p, reinterpret_cast<const char *>(UINTPTR_MAX)));
        p.image.ranges[0].end = begin + bytes;
        char *unterminated = memory + bytes - 4;
        std::memcpy(unterminated, "unix", 4);
        assert(!supported_name(p, unterminated));
        p.image.ranges[0].flags = 0;
        assert(!supported_name(p, unterminated));
        _exit(0);
    }
    int vfs_name_child_status = 0;
    assert(::waitpid(child, &vfs_name_child_status, 0) == child);
    assert(WIFEXITED(vfs_name_child_status) && WEXITSTATUS(vfs_name_child_status) == 0);
    assert(!::munmap(memory, 2 * bytes));
}
void rtti_name_boundaries() {
    long page_size = ::sysconf(_SC_PAGESIZE);
    assert(page_size > 0);
    auto bytes = static_cast<std::size_t>(page_size);
    auto memory = static_cast<char *>(::mmap(nullptr, 2 * bytes, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    assert(!::mprotect(memory + bytes, bytes, PROT_NONE));
    pid_t child = ::fork();
    assert(child >= 0);
    if (!child) {
        /* Native-shaped metadata tests rejection before any virtual invocation.
         * A short unrelated RTTI C string is a valid input, including GNU '*'. */
        alignas(8) std::uintptr_t type[2] = {};
        alignas(8) std::uintptr_t table[3] = {0, reinterpret_cast<std::uintptr_t>(type), 0};
        alignas(8) std::uintptr_t object = reinterpret_cast<std::uintptr_t>(table + 2);
        auto begin = reinterpret_cast<std::uintptr_t>(memory);
        Image image;
        image.ranges[0] = {begin, begin + 2 * bytes, kRead};
        image.ranges[1] = {reinterpret_cast<std::uintptr_t>(type), reinterpret_cast<std::uintptr_t>(type + 2), kRead};
        image.ranges[2] = {reinterpret_cast<std::uintptr_t>(table), reinterpret_cast<std::uintptr_t>(table + 3), kRead};
        image.count = 3;
        source_state().main_image = image; /* Private child; parent profile is unchanged. */
        const char *expected = "N13music_library18MusicLibraryServerE";
        const char *const rejected[] = {"", "*", "N1aE", "*N1aE", "N13music_library18MusicLibraryServerEX"};
        for (auto name : rejected) {
            auto size = std::strlen(name) + 1;
            char *at = memory + bytes - size;
            std::memcpy(at, name, size);
            type[1] = reinterpret_cast<std::uintptr_t>(at);
            assert(!checked_vtable(reinterpret_cast<std::uintptr_t>(&object), expected, 1));
        }
        for (auto name : {expected, "*N13music_library18MusicLibraryServerE"}) {
            auto size = std::strlen(name) + 1;
            char *at = memory + bytes - size;
            std::memcpy(at, name, size);
            type[1] = reinterpret_cast<std::uintptr_t>(at);
            assert(checked_vtable(reinterpret_cast<std::uintptr_t>(&object), expected, 1) == object);
        }
        type[1] = UINTPTR_MAX;
        assert(!checked_vtable(reinterpret_cast<std::uintptr_t>(&object), expected, 1));
        _exit(0);
    }
    int rtti_name_child_status = 0;
    assert(::waitpid(child, &rtti_name_child_status, 0) == child);
    assert(WIFEXITED(rtti_name_child_status) && WEXITSTATUS(rtti_name_child_status) == 0);
    assert(!::munmap(memory, 2 * bytes));
}
void physical_capture() {
    configure(AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER);
    Fixture f(0x210);
    char path[] = "/tmp/az-source-fixture.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unsigned char bytes[4096] = {};
    assert(::write(fd, bytes, sizeof bytes) == static_cast<ssize_t>(sizeof bytes));
    forbidden_native_fd = fd;
    struct flock record_lock = {};
    record_lock.l_type = F_WRLCK;
    record_lock.l_whence = SEEK_SET;
    assert(!::fcntl(fd, F_SETLK, &record_lock));
    FileStorage file {};
    auto profile = physical_profile();
    file.methods = &fixture_io[0];
    file.vfs = &fixture_vfs;
    file.fd = fd;
    f.connection.file = &file;
    f.connection.vfs = &fixture_vfs;
    clear_counts();
    auto good = captured(f, profile);
    assert(good.physical_status == AZS_PHYSICAL_OK && (good.flags & AZS_PHYSICAL));
    assert(sql_calls == 2 && stat_calls == 2 && open_calls == 1 && close_calls == 1 && size_calls == 1);
    assert(read_calls > 0 && read_calls <= 8 && !native_read_calls && !native_close_calls);
    struct stat original;
    assert(!::fstat(fd, &original));
    assert(good.device == static_cast<std::uint64_t>(original.st_dev) && good.inode == original.st_ino &&
           good.file_size == sizeof bytes && good.mount_id && good.physical_observed_ns);
    char moved[128];
    std::snprintf(moved, sizeof moved, "%s.old", path);
    assert(!::rename(path, moved));
    int replacement = ::open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(replacement >= 0);
    struct stat replaced;
    assert(!::fstat(replacement, &replaced) && original.st_ino != replaced.st_ino);
    auto still_original = captured(f, profile);
    assert(still_original.owner_epoch == good.owner_epoch && still_original.inode == good.inode);
    assert(!::close(replacement) && !::unlink(path) && !::unlink(moved));
    clear_counts();
    source_state().capture_policy.store(0);
    auto disabled = captured(f, profile);
    assert(disabled.physical_status == AZS_PHYSICAL_DISABLED && !(disabled.flags & AZS_PHYSICAL));
    no_physical_calls();
    source_state().capture_policy.store(AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER);
    profile.ready = false; /* Even with callable API pointers, no native/filesystem calls. */
    auto missing = captured(f, profile);
    assert(missing.physical_status == AZS_PHYSICAL_PROFILE);
    no_physical_calls();
    profile.ready = true;
    f.marker(0);
    unchanged([&](azsdk_source_sample *o) { return capture(f, o, profile); }, AZS_GUARD);
    no_physical_calls();
    f.marker(1);
    int result = 0;
    std::thread foreign([&] { result = capture(f, nullptr, profile); });
    foreign.join();
    assert(result == AZS_GUARD);
    no_physical_calls();
    for (int opcode : {7, 27}) {
        clear_counts();
        f.connection.failed_opcode = opcode;
        auto rejected = captured(f, profile);
        assert(rejected.physical_status == AZS_PHYSICAL_API && !(rejected.flags & AZS_PHYSICAL));
        no_filesystem_calls();
    }
    f.connection.failed_opcode = 0;
    for (unsigned which = 0; which < 6; ++which) {
        clear_counts();
        auto p = profile;
        switch (which) {
        case 0: fixture_vfs.name = "other"; break;
        case 1: fixture_vfs.version = 2; break;
        case 2: fixture_vfs.os_size = 119; break;
        case 3: fixture_vfs.max_path = 513; break;
        case 4: ++p.open_rva; break;
        case 5: f.connection.vfs = reinterpret_cast<NativeVFS *>(reinterpret_cast<unsigned char *>(&fixture_vfs) + 1); break;
        }
        assert(captured(f, p).physical_status == AZS_PHYSICAL_VFS);
        no_filesystem_calls();
        profile = physical_profile();
        f.connection.vfs = &fixture_vfs;
    }
    for (unsigned which = 0; which < 6; ++which) {
        clear_counts();
        auto p = profile;
        switch (which) {
        case 0: fixture_io[0].version = 1; break;
        case 1: ++p.io_rvas[0]; break;
        case 2: ++p.read_rva; break;
        case 3: ++p.size_rva; break;
        case 4: ++p.control_rva; break;
        case 5: file.methods = reinterpret_cast<NativeIO *>(reinterpret_cast<unsigned char *>(&fixture_io[0]) + 1); break;
        }
        assert(captured(f, p).physical_status == AZS_PHYSICAL_METHODS);
        no_filesystem_calls();
        profile = physical_profile();
        file.methods = &fixture_io[0];
    }
    for (int which : {0, 1}) {
        clear_counts();
        if (which) file.fd = -1;
        else file.vfs = nullptr;
        assert(captured(f, profile).physical_status == AZS_PHYSICAL_DESCRIPTOR);
        no_filesystem_calls();
        file.fd = fd;
        file.vfs = &fixture_vfs;
    }
    for (int mode = 1; mode <= 10; ++mode) {
        clear_counts();
        metadata_mode = mode;
        auto rejected = captured(f, profile);
        assert(rejected.physical_status == (mode == 3 ? AZS_PHYSICAL_CHANGED : mode == 9 ? AZS_PHYSICAL_OK
                                                                                         : AZS_PHYSICAL_METADATA));
        assert(read_calls <= 8 && close_calls == open_calls - (mode == 5 ? 1u : 0u));
        if (mode == 8) assert(read_calls == 8);
        assert(!native_read_calls && !native_close_calls);
    }
    for (int mode = 1; mode <= 5; ++mode) {
        clear_counts();
        size_mode = mode;
        auto rejected = captured(f, profile);
        assert(rejected.physical_status == (mode == 5 ? AZS_PHYSICAL_METADATA : AZS_PHYSICAL_CHANGED));
        assert(!(rejected.flags & AZS_PHYSICAL) && !rejected.inode && !rejected.mount_id);
        file.fd = fd;
        file.methods = &fixture_io[0];
        file.vfs = &fixture_vfs;
    }
    clear_counts();
    for (unsigned i = 0; i < 3; ++i) {
        file.methods = &fixture_io[i];
        assert(captured(f, profile).physical_status == AZS_PHYSICAL_OK);
    }
    auto last = captured(f, profile);
    f.updater.mount.reset();
    azsdk_source_sample stale;
    assert(azsdk_sources_read(last.owner_epoch, &stale, sizeof stale) == AZS_STALE);
    assert(!(stale.flags & AZS_PHYSICAL) && !stale.device && !stale.inode && !stale.mount_id && !stale.file_size);
    assert(!native_read_calls && !native_close_calls);
    /* POSIX record locks disappear if ANY extra descriptor for this inode is closed
     * by the same process. Merely checking that the original FD still works misses
     * the dangerous dup-and-close pattern. A child must still conflict with our lock. */
    char fd_path[64];
    std::snprintf(fd_path, sizeof fd_path, "/proc/self/fd/%d", fd);
    pid_t child = ::fork();
    assert(child >= 0);
    if (!child) {
        int probe = ::open(fd_path, O_RDWR | O_CLOEXEC);
        if (probe < 0) _exit(2);
        int result = ::fcntl(probe, F_SETLK, &record_lock);
        int error = errno;
        ::close(probe);
        _exit(result < 0 && (error == EACCES || error == EAGAIN) ? 0 : 1);
    }
    int child_status;
    assert(::waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && !WEXITSTATUS(child_status));
    /* The test owns this scratch FD; production never owns or closes SQLite's FD. */
    forbidden_native_fd = -1;
    assert(!::close(fd));
}
void concurrency_and_busy() {
    configure();
    Fixture fixture(0x310);
    auto initial = captured(fixture);
    std::atomic<bool> holding {false}, release {false};
    std::thread holder([&] {
        std::lock_guard<std::mutex> lock(source_state().registry_mutex);
        holding.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!holding.load(std::memory_order_acquire)) std::this_thread::yield();
    clear_counts();
    unchanged([&](azsdk_source_sample *o) { return azsdk_sources_read(initial.owner_epoch, o, sizeof *o); }, AZS_BUSY);
    unchanged([&](azsdk_source_sample *o) { return azsdk_sources_candidate(0x310, o, sizeof *o); }, AZS_BUSY);
    unchanged([&](azsdk_source_sample *o) { return capture(fixture, o); }, AZS_BUSY);
    no_physical_calls();
    release.store(true, std::memory_order_release);
    holder.join();
    std::atomic<bool> start {false}, done {false};
    std::atomic<std::uint64_t> epoch {0};
    std::thread producer([&] {
        Fixture f(0x320); /* The producer is its actual fixture/native worker. */
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < 20000; ++i) {
            azsdk_source_sample out;
            int result = capture(f, &out);
            assert(result == AZS_OK || result == AZS_BUSY);
            if (result == AZS_OK) epoch.store(out.owner_epoch, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    });
    std::vector<std::thread> readers;
    for (unsigned r = 0; r < 4; ++r) readers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        std::uint64_t revision = 0;
        do {
            auto id = epoch.load(std::memory_order_acquire);
            if (!id) continue;
            azsdk_source_sample out;
            int result = azsdk_sources_read(id, &out, sizeof out);
            assert(result == AZS_OK || result == AZS_STALE || result == AZS_BUSY);
            if (result == AZS_BUSY) continue;
            assert(out.owner_epoch == id && out.native_media_id == 0x320 && out.revision >= revision);
            assert(out.struct_size == sizeof out && out.observed_ns && (out.flags & AZS_HISTORY_INCOMPLETE));
            revision = out.revision;
        } while (!done.load(std::memory_order_acquire));
    });
    start.store(true, std::memory_order_release);
    producer.join();
    for (auto &reader : readers) reader.join();
    assert(epoch.load());
    azsdk_source_sample final;
    assert(azsdk_sources_read(epoch.load(), &final, sizeof final) == AZS_STALE);
    no_physical_calls();
    assert(azsdk_sources_configure(0, test_build, 0) == AZS_UNSUPPORTED);
    azsdk_sources_sweep();
}
void native_sqlcipher(const char *path) {
    /* Explicit optional research run: public SQLite calls on fresh, unkeyed scratch DBs
     * only. The real SQLCipher profile and Unix file adapter are used unchanged. The C++
     * worker/ownership inputs remain the synthetic model, not an EP147 hook acceptance. */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "native library load failed: %s\n", dlerror());
        std::abort();
    }
    assert(azsdk_sources_configure(1, test_build, AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER) == AZS_OK);
    assert(azsdk_sources_physical_ready());
    using Open = int (*)(const char *, void **, int, const char *);
    using Close = int (*)(void *);
    using Exec = int (*)(void *, const char *, int (*)(void *, int, char **, char **), void *, char **);
    auto open_db = reinterpret_cast<Open>(dlsym(handle, "sqlite3_open_v2"));
    auto close_db = reinterpret_cast<Close>(dlsym(handle, "sqlite3_close"));
    auto exec = reinterpret_cast<Exec>(dlsym(handle, "sqlite3_exec"));
    for (auto entry : {reinterpret_cast<std::uintptr_t>(open_db), reinterpret_cast<std::uintptr_t>(close_db),
                       reinterpret_cast<std::uintptr_t>(exec)})
        assert(entry && source_state().sql_profile.image.contains(entry, 4, kRead | kExecute));
    char original_path[] = "/tmp/az-source-native.XXXXXX";
    int placeholder = mkstemp(original_path);
    assert(placeholder >= 0 && !::close(placeholder));
    void *connection = nullptr, *replacement = nullptr;
    assert(!open_db(original_path, &connection, 6, nullptr) && connection);
    assert(!exec(connection, "CREATE TABLE source_fixture(id INTEGER); INSERT INTO source_fixture VALUES(1);",
                 nullptr, nullptr, nullptr));
    Fixture fixture(0x410);
    fixture.mount().database->connection = connection;
    auto native_sample = [&] {
        azsdk_source_sample sample = {};
        assert(observe(reinterpret_cast<std::uintptr_t>(&fixture.server), fixture.profile(), source_state().sql_profile,
                       metadata_ops, &sample, sizeof sample) == AZS_OK);
        assert(sample.physical_status == AZS_PHYSICAL_OK && (sample.flags & AZS_PHYSICAL));
        return sample;
    };
    auto before = native_sample();
    struct stat original_stat;
    assert(!::stat(original_path, &original_stat));
    assert(before.device == static_cast<std::uint64_t>(original_stat.st_dev) &&
           before.inode == original_stat.st_ino && before.file_size == static_cast<std::uint64_t>(original_stat.st_size));
    char moved[128];
    std::snprintf(moved, sizeof moved, "%s.old", original_path);
    assert(!::rename(original_path, moved));
    assert(!open_db(original_path, &replacement, 6, nullptr) && replacement);
    assert(!exec(replacement, "CREATE TABLE source_fixture(id INTEGER); INSERT INTO source_fixture VALUES(2);",
                 nullptr, nullptr, nullptr));
    struct stat replacement_stat;
    assert(!::stat(original_path, &replacement_stat) && replacement_stat.st_ino != original_stat.st_ino);
    auto after = native_sample();
    assert(after.owner_epoch == before.owner_epoch && after.inode == before.inode && after.device == before.device &&
           after.mount_id == before.mount_id && after.file_size == before.file_size);
    fixture.updater.mount.reset();
    azsdk_source_sample stale;
    assert(azsdk_sources_read(before.owner_epoch, &stale, sizeof stale) == AZS_STALE && !(stale.flags & AZS_PHYSICAL));
    assert(!close_db(connection) && !close_db(replacement));
    assert(!::unlink(original_path) && !::unlink(moved));
    assert(azsdk_sources_configure(0, test_build, 0) == AZS_UNSUPPORTED);
    azsdk_sources_sweep();
    dlclose(handle); /* The successful production profile holds its own DSO reference. */
    std::printf("native SQLCipher profile/owned-file rename fixture passed (build %u)\n", test_build);
}
#if defined(AZS_CONSTRUCTOR_REGRESSION)
std::uint64_t early_epoch = 0;
/* The C azmod_init constructor may precede default-priority C++ global initialization.
 * Publish a real weak-owner record at priority101, then verify it survived until main. */
__attribute__((constructor(101))) void early_configure_and_capture() {
    test_build = 1;
    assert(azsdk_sources_configure(1, 1, 0) == AZS_OK);
    Fixture f(0x510);
    SqlProfile local_profile;
    early_epoch = captured(f, local_profile).owner_epoch;
}
#endif
} // namespace
int main(int argc, char **argv) {
#if defined(AZS_CONSTRUCTOR_REGRESSION)
    azsdk_source_sample early;
    assert(early_epoch && azsdk_sources_read(early_epoch, &early, sizeof early) == AZS_STALE);
#endif
    if (argc == 4 && !std::strcmp(argv[1], "--native-sqlcipher")) {
        test_build = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
        assert(test_build == 1 || test_build == 2);
        native_sqlcipher(argv[2]);
        return 0;
    }
    test_build = argc == 2 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 1;
    assert(test_build == 1 || test_build == 2);
    api_and_guards();
    weak_lifetime_and_outputs();
#if defined(AZS_SAMPLE_BOUNDARY)
    candidate_expiry_during_sample();
#endif
    owner_address_reuse();
    binding_and_quota();
    mount_id_parser();
    vfs_name_boundaries();
    rtti_name_boundaries();
    physical_capture();
    concurrency_and_busy();
    std::printf("native source ownership/physical fixtures passed (build %u)\n", test_build);
    return 0;
}
#endif
