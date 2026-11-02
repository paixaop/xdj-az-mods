/* SPDX-License-Identifier: GPL-3.0-only */
/* Runtime RED contract, not a replacement observer or a native mapping.
 * Native ABI declarations below are separate from cache_birth_test::Api, an
 * invented TEST-ONLY adapter contract awaiting review and production RED.
 * Existing production source/weak-owner helpers are included unchanged.
 * No EP147, filesystem metadata, physical source or public load qualification.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#define REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr, \
    "FAIL cache-birth: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); \
    std::abort(); } } while (0)

#if !defined(__linux__)
int main() { std::puts("SKIP: requires Linux ELF and GNU shared ownership"); return 77; }
#else
#include "../../src/common/azcachebirth.h"
namespace cache_factory_fixture {
namespace cb = azsdk_cache_birth;
std::function<void()> tail, inside_final_write;
// Optional synthetic native-entry boundary for the real dispatcher integration
// fixture. Default tests keep their existing direct private-helper path. This
// replaces no allocation, write, source registry or ownership implementation.
using FactoryEntry = void (*)(const cb::FactoryBorrow &, cb::Forward);
FactoryEntry factory_entry = nullptr;
template<class F> void invoke(void *p) { (*static_cast<F *>(p))(); }
template<class F> cb::Forward forward(F &f) { return {invoke<F>, &f}; }
template<class F>
int write(const void *cache, unsigned kind, unsigned argument, const void *pool, F operation) {
    int result = -1;
    auto original = [&] { result = operation(); };
    cb::WriteBorrow borrow {cache, kind, argument, pool, &result};
    cb::observe_construction_write_owned(borrow, forward(original));
    return result;
}
}
extern "C" void azsdk_cache_test_connection_event(void *, int) __attribute__((weak));
static void native_connection_event(void *connection, int open) {
    auto event = azsdk_cache_test_connection_event;
    if (event) event(connection, open);
}
namespace music_library {
class SqliteDataBase {
public:
    virtual ~SqliteDataBase() {}
    std::uint16_t media;
    unsigned char pad[0x38 - 10] = {};
    void *connection;
    std::uint32_t fixture_layout; // fixture-only tail, never a native field claim
    SqliteDataBase(unsigned m, void *c, unsigned layout)
        : media(m), connection(c), fixture_layout(layout) {}
};
class MountPoint {
public:
    virtual ~MountPoint() {}
    std::uint16_t media;
    unsigned char pad[0x30 - 10] = {};
    explicit MountPoint(unsigned m) : media(m) {}
};
class SqliteMountPoint : public MountPoint {
public:
    std::shared_ptr<SqliteDataBase> database;
    SqliteMountPoint(unsigned m, void *c, unsigned layout)
        : MountPoint(m), database(new SqliteDataBase(m, c, layout)) {}
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
    virtual bool needsToExecuteAsync() { return worker != std::this_thread::get_id(); }
    virtual void reserved6() {}
    virtual void reserved7() {}
    virtual void reserved8() {}
    virtual void reserved9() {}
    virtual void onMessage() {}
    unsigned char pad[0x1d0 - 8] = {};
    std::thread::id worker = std::this_thread::get_id();
};
struct FixturePool {
    static unsigned alive;
    FixturePool() { ++alive; }
    ~FixturePool() { --alive; }
};
unsigned FixturePool::alive;
struct FixtureCondition { unsigned content; unsigned char mode; };
struct FixtureLayout { unsigned tag; FixturePool *borrowed_pool; };
class ListCache {
public:
    virtual ~ListCache() { --alive; }
    virtual int setup(unsigned count, FixturePool *pool) {
        item_count = count;
        layout.reset(new FixtureLayout {0, pool});
        return 0;
    }
    std::uint32_t item_count = 0, pad0 = 0;
    std::unique_ptr<FixtureCondition> condition;
    std::shared_ptr<SqliteDataBase> temporary_database;
    std::uint32_t native_counter = 0, pad1 = 0;
    std::unique_ptr<FixtureLayout> layout;
    static unsigned alive;
    static bool reuse_next;
    static void *operator new(std::size_t n);
    static void operator delete(void *p) noexcept;
    explicit ListCache(unsigned content, FixturePool *pool = nullptr, unsigned tag = 0)
        : condition(new FixtureCondition {content, 0}), layout(new FixtureLayout {tag, pool}) {
        ++alive; item_count = 16;
    }
};
unsigned ListCache::alive;
bool ListCache::reuse_next;
alignas(ListCache) unsigned char cache_arena[sizeof(ListCache)];
void *ListCache::operator new(std::size_t n) {
    if (reuse_next) { reuse_next = false; return cache_arena; }
    return ::operator new(n);
}
void ListCache::operator delete(void *p) noexcept { if (p != cache_arena) ::operator delete(p); }
using CacheOwner = std::shared_ptr<ListCache>;
class ListCacheCreator {
public:
    virtual ~ListCacheCreator() {}
    virtual ListCache *make(FixturePool *pool) {
        namespace ff = cache_factory_fixture;
        namespace cb = azsdk_cache_birth;
        const void *raw = nullptr;
        auto original = [&] {
            if (inside_make) inside_make();
            std::unique_ptr<ListCache> value(new ListCache(content, pool, database->fixture_layout));
            if (factory_kind == cb::StandardFallbackFactory) {
                unsigned count = fault == 1 ? 15 : 16;
                ff::write(value.get(), cb::SetupWrite, count, pool, [&] {
                    value->setup(count, pool); return fault == 2 ? 7 : 0;
                });
                unsigned setters = fault == 4 ? 15 : fault == 5 ? 17 : 16;
                for (unsigned index = 0; index < setters; ++index) {
                    unsigned native_index = fault == 3 && index == 9 ? 8 : index;
                    ff::write(value.get(), cb::LayoutWrite, native_index, nullptr, [&] {
                        value->layout->tag = database->fixture_layout;
                        if (index == 15 && ff::inside_final_write) ff::inside_final_write();
                        return fault == 6 && index == 7 ? 5 : 0;
                    });
                }
            }
            if (ff::tail) ff::tail();
            raw = value.release();
        };
        cb::FactoryBorrow borrow {this, factory_kind, pool, &raw};
        if (ff::factory_entry) ff::factory_entry(borrow, ff::forward(original));
        else cb::observe_factory_owned(borrow, ff::forward(original));
        return const_cast<ListCache *>(static_cast<const ListCache *>(raw));
    }
    unsigned char pad[0x30 - 8] = {};
    std::shared_ptr<SqliteDataBase> database;
    std::uint32_t content = 11;
    unsigned char mode = 0, qualifier = 0;
    std::function<void()> inside_make; // fixture-only tail
    unsigned factory_kind = azsdk_cache_birth::PrimaryFactory, fault = 0; // fixture-only
    unsigned forwarded_calls = 0; // counts this invocation independently of nested creators
    std::uintptr_t result_destination = 0; // fixture-only per-creator entry capture
};
using CreatorOwner = std::unique_ptr<ListCacheCreator>;
class ListCacheCollector {
public:
    virtual ~ListCacheCollector() { caches.clear(); --alive; }
    unsigned char pad[0x28 - 8] = {};
    std::vector<CacheOwner> caches;
    std::unique_ptr<FixturePool> pool;
    std::uint32_t native_counter = 0, pad1 = 0;
    static unsigned alive;
    static bool reuse_next;
    static void *operator new(std::size_t n);
    static void operator delete(void *p) noexcept;
    ListCacheCollector() : pool(new FixturePool) { ++alive; }
};
unsigned ListCacheCollector::alive;
bool ListCacheCollector::reuse_next;
alignas(ListCacheCollector) unsigned char collector_arena[sizeof(ListCacheCollector)];
void *ListCacheCollector::operator new(std::size_t n) {
    if (reuse_next) { reuse_next = false; return collector_arena; }
    return ::operator new(n);
}
void ListCacheCollector::operator delete(void *p) noexcept {
    if (p != collector_arena) ::operator delete(p);
}
class ListServer {
public:
    virtual ~ListServer() {}
    unsigned char pad[0x28 - 8] = {};
    std::shared_ptr<MountPoint> mount;
    std::uint64_t schema_pad = 0;
    std::shared_ptr<ListCacheCollector> collector;
    std::vector<std::vector<CacheOwner>> clients;
    ListServer() : collector(new ListCacheCollector), clients(4, std::vector<CacheOwner>(9)) {}
};
class FixtureAdapter {
public:
    virtual ~FixtureAdapter() {}
    ListCache *cache;
    std::uintptr_t result_destination = 0; // nested calls may overwrite the global recorder
    explicit FixtureAdapter(ListCache *c) : cache(c) {}
};
} // namespace music_library

/* Native-shaped row carrier. Its caller exclusively owns it; observers must
 * neither copy/move it nor retain its raw rows. AArch64 sret forwarding below
 * supplies the final native caller destination directly. */
struct NativeRow {
    std::uint32_t value;
    unsigned char pad[0x98 - 4] = {};
    static unsigned alive, destroyed;
    explicit NativeRow(unsigned v) : value(v) { ++alive; }
    ~NativeRow() { --alive; ++destroyed; }
};
unsigned NativeRow::alive, NativeRow::destroyed;
struct NativeRows {
    NativeRow **begin = nullptr, **end = nullptr, **capacity = nullptr;
    static unsigned moves;
    explicit NativeRows(unsigned value) {
        begin = new NativeRow *[1];
        try { begin[0] = new NativeRow(value); } catch (...) { delete[] begin; throw; }
        end = capacity = begin + 1;
    }
    NativeRows(const NativeRows &) = delete;
    NativeRows &operator=(const NativeRows &) = delete;
    NativeRows(NativeRows &&other) noexcept
        : begin(other.begin), end(other.end), capacity(other.capacity) {
        ++moves; other.begin = other.end = other.capacity = nullptr;
    }
    ~NativeRows() { for (auto p = begin; p != end; ++p) delete *p; delete[] begin; }
};
unsigned NativeRows::moves;
using namespace music_library;
using NativeCreate = CacheOwner (*)(ListCacheCollector *, CreatorOwner &);
using NativeQuery = NativeRows (*)(FixtureAdapter *, unsigned, unsigned,
                                   const std::shared_ptr<MountPoint> &);
extern "C" {
std::uintptr_t cb_native_owner_destination = 0, cb_native_rows_destination = 0;
CacheOwner cb_native_create_impl(ListCacheCollector *, CreatorOwner &);
NativeRows cb_native_query_impl(FixtureAdapter *, unsigned, unsigned,
                               const std::shared_ptr<MountPoint> &);
}
#if defined(__aarch64__)
extern "C" CacheOwner cb_native_create(ListCacheCollector *, CreatorOwner &);
extern "C" NativeRows cb_native_query(FixtureAdapter *, unsigned, unsigned,
                                     const std::shared_ptr<MountPoint> &);
extern "C" void cb_forward_owner(void *, NativeCreate, ListCacheCollector *, CreatorOwner *);
extern "C" void cb_forward_rows(void *, NativeQuery, FixtureAdapter *, unsigned, unsigned,
                                const std::shared_ptr<MountPoint> *);
asm(".text\n"
    ".global cb_native_create\n.type cb_native_create, %function\ncb_native_create:\n.cfi_startproc\n"
    "adrp x9, cb_native_owner_destination\n"
    "add x9, x9, :lo12:cb_native_owner_destination\nstr x8, [x9]\nb cb_native_create_impl\n.cfi_endproc\n"
    ".size cb_native_create, .-cb_native_create\n"
    ".global cb_native_query\n.type cb_native_query, %function\ncb_native_query:\n.cfi_startproc\n"
    "adrp x9, cb_native_rows_destination\n"
    "add x9, x9, :lo12:cb_native_rows_destination\nstr x8, [x9]\nb cb_native_query_impl\n.cfi_endproc\n"
    ".size cb_native_query, .-cb_native_query\n"
    ".global cb_forward_owner\n.type cb_forward_owner, %function\ncb_forward_owner:\n.cfi_startproc\n"
    "stp x29, x30, [sp, #-16]!\n.cfi_def_cfa_offset 16\n.cfi_offset 29, -16\n.cfi_offset 30, -8\n"
    "mov x29, sp\n.cfi_def_cfa_register 29\nmov x8, x0\nmov x16, x1\n"
    "mov x0, x2\nmov x1, x3\nblr x16\nldp x29, x30, [sp], #16\n"
    ".cfi_def_cfa sp, 0\n.cfi_restore 29\n.cfi_restore 30\nret\n.cfi_endproc\n"
    ".size cb_forward_owner, .-cb_forward_owner\n"
    ".global cb_forward_rows\n.type cb_forward_rows, %function\ncb_forward_rows:\n.cfi_startproc\n"
    "stp x29, x30, [sp, #-16]!\n.cfi_def_cfa_offset 16\n.cfi_offset 29, -16\n.cfi_offset 30, -8\n"
    "mov x29, sp\n.cfi_def_cfa_register 29\nmov x8, x0\nmov x16, x1\n"
    "mov x0, x2\nmov w1, w3\nmov w2, w4\nmov x3, x5\n"
    "blr x16\nldp x29, x30, [sp], #16\n"
    ".cfi_def_cfa sp, 0\n.cfi_restore 29\n.cfi_restore 30\nret\n.cfi_endproc\n"
    ".size cb_forward_rows, .-cb_forward_rows\n");
#else
static CacheOwner cb_native_create(ListCacheCollector *c, CreatorOwner &v) { return cb_native_create_impl(c, v); }
static NativeRows cb_native_query(FixtureAdapter *a, unsigned s, unsigned n,
                                  const std::shared_ptr<MountPoint> &m) { return cb_native_query_impl(a, s, n, m); }
static void cb_forward_owner(void *out, NativeCreate fn, ListCacheCollector *c, CreatorOwner *v) {
    new (out) CacheOwner(fn(c, *v));
}
static void cb_forward_rows(void *out, NativeQuery fn, FixtureAdapter *a, unsigned s, unsigned n,
                            const std::shared_ptr<MountPoint> *m) { new (out) NativeRows(fn(a, s, n, *m)); }
#endif

/* Deliberately invented SDK TEST CONTRACT. These are not native prototypes,
 * mappings, public SDK interfaces, or an alternate source-owner registry.
 * A future reviewed test adapter must call the actual production private
 * helpers, using the real source registry below; it may not implement a model.
 * Reset/seeding are fixture-only and may never become public production seams.
 */
namespace cache_birth_test {
struct Scope {
    unsigned build, client_bank;
    MusicLibraryServer *owned_server;
    ListServer *list_server;
    std::uint64_t source_epoch;
};
struct Witness {
    std::uint64_t birth_epoch = 0, layout_generation = 0, source_epoch = 0;
    std::weak_ptr<MountPoint> mount;
    std::weak_ptr<SqliteDataBase> database;
    std::weak_ptr<ListCacheCollector> collector;
    std::weak_ptr<ListCache> cache;
    unsigned row_value = 0;
};
struct BirthCall {
    void *result_storage;
    NativeCreate original;
    ListCacheCollector *collector;
    CreatorOwner *creator;
    void forward() const { cb_forward_owner(result_storage, original, collector, creator); }
};
struct QueryCall {
    void *result_storage;
    NativeQuery original;
    FixtureAdapter *adapter;
    unsigned start, count;
    const std::shared_ptr<MountPoint> *mount;
    // Fixture-only action at the already-generic production scalar copier.
    // This is not a native ABI argument or a new production reset/adapter seam.
    void (*copy_boundary)(void *);
    void *copy_context;
    QueryCall(void *storage, NativeQuery fn, FixtureAdapter *receiver, unsigned first, unsigned length,
              const std::shared_ptr<MountPoint> *source,
              void (*boundary)(void *) = nullptr, void *context = nullptr)
        : result_storage(storage), original(fn), adapter(receiver), start(first), count(length), mount(source),
          copy_boundary(boundary), copy_context(context) {}
    void forward() const { cb_forward_rows(result_storage, original, adapter, start, count, mount); }
};
enum Mutation { setup, setter, records_grow, clear, dispose };
struct Limits { unsigned slots = 4, depth = 4; std::uint64_t next_birth = 0, generation = 1; };
struct Counters { unsigned retained = 0, active = 0; std::uint64_t next_birth = 0, generation = 0; bool saturated = false; };
struct Api {
    unsigned version;
    void (*reset)(const Limits &);
    int (*birth)(const Scope &, const BirthCall &, Witness *);
    int (*query)(const Scope &, const QueryCall &, Witness *);
    void (*mutation)(Mutation, void (*)(void *), void *);
    Counters (*counters)();
};
} // namespace cache_birth_test
extern "C" const cache_birth_test::Api *azsdk_cache_birth_test_api_v1() __attribute__((weak));
#ifndef SOURCES_INC
#define SOURCES_INC "../../src/azmod/sdk_native_sources.cpp"
#endif
#if defined(AZSDK_CACHE_RECLAIM_TESTING)
std::function<void(unsigned)> cache_reclaim_test_boundary;
#endif
#include SOURCES_INC

namespace {
using namespace cache_birth_test;
/* The absent-hook path contains NO registry, owner matching, epochs or fake
 * observation. Native work is still performed once into its genuine result. */
void absent_reset(const Limits &) {}
int absent_birth(const Scope &, const BirthCall &call, Witness *) { call.forward(); return AZS_UNSUPPORTED; }
int absent_query(const Scope &, const QueryCall &call, Witness *) { call.forward(); return AZS_UNSUPPORTED; }
void absent_mutation(Mutation, void (*original)(void *), void *p) { original(p); }
Counters absent_counters() { return {}; }
const Api absent {1, absent_reset, absent_birth, absent_query, absent_mutation, absent_counters};
const Api &api() {
    const Api *(*getter)() = azsdk_cache_birth_test_api_v1;
    return getter ? *getter() : absent;
}
unsigned build_id, creates, queries;
std::function<void()> after_owner, inside_query;
template<class T> struct Guarded {
    std::uint64_t before = UINT64_C(0x1032547698badcfe);
    alignas(T) unsigned char bytes[sizeof(T)];
    std::uint64_t after = UINT64_C(0xfedcba9876543210);
    T &value() { return *reinterpret_cast<T *>(bytes); }
    void check() const {
        REQUIRE(before == UINT64_C(0x1032547698badcfe) && after == UINT64_C(0xfedcba9876543210), "original sret output guards");
    }
};
template<class T> std::size_t offset(const void *p, const T *field) {
    return reinterpret_cast<const unsigned char *>(field) - static_cast<const unsigned char *>(p);
}
struct Handler { unsigned char pad[0x30] = {}; InformationUpdater *updater; };
struct Dispatcher { unsigned char pad[0x20] = {}; Handler *handler; };
struct Context {
    MusicLibraryServer server;
    InformationUpdater updater;
    Handler handler;
    Dispatcher dispatcher;
    ListServer list;
    unsigned bank;
    alignas(8) std::uint64_t connection = 0;
    alignas(8) std::uint64_t alternate_connection = 0;
    azsdk_source_sample source = {};
    Context(unsigned client_bank, unsigned tag) : bank(client_bank) {
        handler.updater = &updater; dispatcher.handler = &handler;
        auto d = &dispatcher; std::memcpy(reinterpret_cast<char *>(&server) + builds[build_id].dispatcher, &d, sizeof d);
        auto worker = &server.worker; std::memcpy(reinterpret_cast<char *>(&server) + 0x28, &worker, sizeof worker);
        reinterpret_cast<unsigned char *>(&server)[0xd2] = 1;
        // Same native media in four contexts deliberately cannot establish source identity.
        updater.mount.reset(new SqliteMountPoint(0x301, &connection, tag));
        list.mount = updater.mount; native_connection_event(&connection,1); capture();
        REQUIRE(offset(&list, &list.mount) == 0x28 && offset(&list, &list.collector) == 0x40 &&
                offset(&list, &list.clients) == 0x50, "native ListServer fixture prefix");
        REQUIRE(offset(list.collector.get(), &list.collector->caches) == 0x28 &&
                offset(list.collector.get(), &list.collector->pool) == 0x40, "native Collector fixture prefix");
    }
    ~Context() { native_connection_event(&connection,0); }
    void capture() {
        auto table = load<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(&server));
        Build profile {builds[build_id].dispatcher, load<std::uintptr_t>(table + 5 * 8) - source_state().main_image.base};
        SqlProfile sql;
        REQUIRE(observe(reinterpret_cast<std::uintptr_t>(&server), profile, sql, metadata_ops,
                        &source, sizeof source) == AZS_OK, "existing real production source capture");
    }
    Scope scope() { return {build_id, bank, &server, &list, source.owner_epoch}; }
    SqliteMountPoint &mount() { return *static_cast<SqliteMountPoint *>(list.mount.get()); }
    CacheOwner &selected() { return list.clients[bank][0]; }
    void same_source(Context &other) { updater.mount = other.updater.mount; list.mount = updater.mount; capture(); }
};
struct World {
    Context a {0, 101}, b {3, 202}, c {1, 303}, d {2, 404};
    // Four independent client/source-owner contexts. Bank numbers are not deck IDs.
};
struct Born { CacheOwner cache; Witness witness; int status; };
Born birth(Context &context, unsigned content = 11, std::function<void()> action = {},
           unsigned factory_kind = azsdk_cache_birth::PrimaryFactory, unsigned fault = 0,
           unsigned mode = 0) {
    CreatorOwner creator(new ListCacheCreator);
    creator->database = context.mount().database; creator->content = content; creator->inside_make = action;
    creator->factory_kind = factory_kind; creator->fault = fault; creator->mode = mode;
    REQUIRE(offset(creator.get(), &creator->database) == 0x30, "native Creator real DB owner prefix");
    REQUIRE(offset(creator.get(), &creator->mode) == 0x44, "native standard Creator mode prefix");
    Guarded<CacheOwner> result; Witness witness;
    auto scope = context.scope(); BirthCall call {result.bytes, cb_native_create, context.list.collector.get(), &creator};
    unsigned prior = creator->forwarded_calls; auto database_count = creator->database.use_count();
    auto mount_count = context.list.mount.use_count(), collector_count = context.list.collector.use_count();
    int status;
    try { status = api().birth(scope, call, &witness); }
    catch (...) { result.check(); throw; }
    result.check(); REQUIRE(creator->forwarded_calls == prior + 1, "original creation forwarded exactly once");
#if defined(__aarch64__)
    REQUIRE(creator->result_destination == reinterpret_cast<std::uintptr_t>(result.bytes), "original shared_ptr X8 destination unchanged");
#endif
    REQUIRE(creator->database.use_count() == database_count, "observer must not retain a strong Creator DB owner");
    REQUIRE(context.list.mount.use_count() == mount_count && context.list.collector.use_count() == collector_count,
            "birth observer retains only genuine weak source/Collector witnesses");
    CacheOwner value = std::move(result.value()); result.value().~shared_ptr();
    REQUIRE(value && value.use_count() == 2, "exact native returned/Collector owner counts");
    context.selected() = value;
    return {value, witness, status};
}
struct Queried { Witness witness; int status; unsigned value; };
Queried query(Context &context) {
    FixtureAdapter adapter(context.selected().get()); Guarded<NativeRows> output;
    auto scope = context.scope(); QueryCall call {output.bytes, cb_native_query, &adapter, 0, 1, &context.list.mount};
    auto cache_count = context.selected().use_count(), collector_count = context.list.collector.use_count();
    auto mount_count = context.list.mount.use_count(), database_count = context.mount().database.use_count();
    unsigned prior = queries, destroyed = NativeRow::destroyed, moves = NativeRows::moves;
    Witness witness; int status;
    try { status = api().query(scope, call, &witness); }
    catch (...) {
        output.check(); REQUIRE(!NativeRow::alive, "native query exception leaves no transferred row"); throw;
    }
    output.check(); REQUIRE(queries == prior + 1, "original row query forwarded exactly once");
#if defined(__aarch64__)
    REQUIRE(adapter.result_destination == reinterpret_cast<std::uintptr_t>(output.bytes), "original 24-byte X8 destination unchanged");
    REQUIRE(NativeRows::moves == moves, "observer must not move or own native row container");
#else
    (void)moves;
#endif
    REQUIRE(NativeRow::alive == 1 && output.value().end == output.value().begin + 1, "row remains owned by native result caller");
    unsigned value = output.value().begin[0]->value;
    output.value().~NativeRows();
    REQUIRE(!NativeRow::alive && NativeRow::destroyed == destroyed + 1, "native row caller alone releases row");
    REQUIRE(context.selected().use_count() == cache_count && context.list.collector.use_count() == collector_count,
            "query pins end with native callback");
    REQUIRE(context.list.mount.use_count() == mount_count && context.mount().database.use_count() == database_count,
            "query source owner pins end with native callback");
    return {witness, status, value};
}
void observed(Context &c, const Born &b, const Queried &q) {
    // On unchanged production this is the first expected runtime failure.
    REQUIRE(b.status == AZS_OK && q.status == AZS_OK && b.witness.birth_epoch && q.witness.birth_epoch,
            "missing observed cache birth and real ownership result");
    REQUIRE(b.witness.birth_epoch == q.witness.birth_epoch && q.witness.source_epoch == c.source.owner_epoch,
            "birth/source epoch follows exact actual cache owner");
    REQUIRE(same_owner(q.witness.cache, b.cache) && same_owner(q.witness.collector, c.list.collector) &&
            same_owner(q.witness.mount, c.list.mount) && same_owner(q.witness.database, c.mount().database),
            "witness contains genuine source/cache/Collector weak owners");
    REQUIRE(q.witness.row_value == q.value && q.value == c.mount().database->fixture_layout,
            "copy actual row scalar while native row is alive");
}
void unavailable(const Queried &q) { REQUIRE(q.status != AZS_OK && !q.witness.birth_epoch, "no stale/partial owner publication"); }
void refused_unchanged(Context &context, const Born &known, const Scope &scope, const char *reason,
                       std::function<void()> copy_boundary = {}, std::function<void()> restore = {}) {
    FixtureAdapter adapter(context.selected().get()); Guarded<NativeRows> output;
    void (*boundary)(void *) = copy_boundary ? +[](void *p) { (*static_cast<std::function<void()> *>(p))(); } : nullptr;
    QueryCall call {output.bytes, cb_native_query, &adapter, 0, 1, &context.list.mount, boundary, &copy_boundary};
    Witness witness = known.witness;
    witness.birth_epoch = UINT64_C(0x13579bdf);
    witness.layout_generation = UINT64_C(0x2468ace0);
    witness.source_epoch = UINT64_C(0x1020304050607080);
    witness.row_value = 0xcafe;
    unsigned prior = queries, destroyed = NativeRow::destroyed, moves = NativeRows::moves;
    auto cache_count = context.selected().use_count(), collector_count = context.list.collector.use_count();
    auto mount_count = context.list.mount.use_count(), database_count = context.mount().database.use_count();
    int status;
    try { status = api().query(scope, call, &witness); }
    catch (...) { output.check(); throw; }
    if (restore) restore();
    output.check(); REQUIRE(queries == prior + 1, "refused observation still forwards original query exactly once");
#if defined(__aarch64__)
    REQUIRE(cb_native_rows_destination == reinterpret_cast<std::uintptr_t>(output.bytes) && NativeRows::moves == moves,
            "refusal preserves original native 24-byte X8 destination without a container move");
#else
    (void)moves;
#endif
    REQUIRE(status != AZS_OK && witness.birth_epoch == UINT64_C(0x13579bdf) &&
            witness.layout_generation == UINT64_C(0x2468ace0) &&
            witness.source_epoch == UINT64_C(0x1020304050607080) && witness.row_value == 0xcafe &&
            same_owner(witness.cache, known.cache) && same_owner(witness.collector, context.list.collector) &&
            same_owner(witness.mount, context.list.mount) && same_owner(witness.database, context.mount().database), reason);
    REQUIRE(NativeRow::alive == 1 && output.value().end == output.value().begin + 1,
            "refusal leaves the genuine returned row with its native caller");
    output.value().~NativeRows();
    REQUIRE(!NativeRow::alive && NativeRow::destroyed == destroyed + 1 &&
            context.selected().use_count() == cache_count && context.list.collector.use_count() == collector_count,
            "refusal releases scoped pins and never retains a native row");
    REQUIRE(context.list.mount.use_count() == mount_count && context.mount().database.use_count() == database_count,
            "refusal releases scoped source owner pins");
}
void thunk(void *p) { (*static_cast<std::function<void()> *>(p))(); }
void mutate(Mutation kind, std::function<void()> action = [] {}) { api().mutation(kind, thunk, &action); }
void reset(Limits limits = {}) {
    // Real production reclamation, never a parallel registry or source reset.
    // Live Worlds retain their real native owners and bindings; epochs/history
    // are deliberately not reset, including during the exhaustion test.
    azsdk_sources_sweep(); after_owner = inside_query = {};
    cache_factory_fixture::tail = cache_factory_fixture::inside_final_write = {}; api().reset(limits);
}

void first_birth() {
    reset(); World w; auto b = birth(w.a); auto q = query(w.a);
    std::printf("NATIVE_FORWARD_OK build=%u owner16=%zu rows24=%zu creates=%u queries=%u\n",
                build_id, sizeof(CacheOwner), sizeof(NativeRows), creates, queries); std::fflush(stdout);
    observed(w.a, b, q);
    for (auto c : {&w.b, &w.c, &w.d}) { auto other = birth(*c); observed(*c, other, query(*c)); }
}
CacheOwner native_hit(Context &context, unsigned content) {
    // The actual native helper supplies the current DB to a fresh owning
    // Creator BEFORE lookup. A hit returns a strong copy of an existing
    // native cache; its Creator is discarded and birth is never invoked.
    CreatorOwner unused(new ListCacheCreator);
    unused->database = context.mount().database; unused->content = content;
    unsigned prior = creates;
    for (const auto &cache : context.list.collector->caches)
        if (cache->condition->content == content && cache->condition->mode == unused->mode) {
            CacheOwner hit = cache;
            context.selected() = hit;
            REQUIRE(creates == prior, "native condition hit skips creation");
            return hit;
        }
    REQUIRE(false, "fixture must hit the actual retained condition/cache owner"); return {};
}
void sql_connection_lifetime() {
    reset(); World w;
    // The absent old bridge cannot invent a successful native open receipt.
    // A real recorded close on the new bridge also retires before observation.
    native_connection_event(&w.a.connection,0);
    auto unknown = birth(w.a);
    REQUIRE(unknown.status != AZS_OK && !unknown.witness.birth_epoch,
            "unobserved/retired SQLite connection cannot qualify cache birth");
    unavailable(query(w.a));
    native_connection_event(&w.a.connection,1); w.a.capture();
    auto known = birth(w.a); observed(w.a,known,query(w.a));
    const auto epoch = w.a.source.owner_epoch;
    native_connection_event(&w.a.connection,0);
    azsdk_source_sample retired = {};
    REQUIRE(azsdk_sources_read(epoch,&retired,sizeof retired)==AZS_STALE && !(retired.flags&AZS_CURRENT),
            "observed close invalidates source despite unchanged real weak owners and raw connection");
    unavailable(query(w.a));
    native_connection_event(&w.a.connection,1); w.a.capture();
    REQUIRE(w.a.source.owner_epoch != epoch,"fresh observed allocation at same raw address gets new source epoch");
    auto fresh = birth(w.a); observed(w.a,fresh,query(w.a));
    REQUIRE(fresh.witness.source_epoch != known.witness.source_epoch,"cache source cannot adopt a reused SQLite address");
    // Retirement inside original and scalar copy must both reject publication.
    inside_query=[&]{native_connection_event(&w.a.connection,0);};
    unavailable(query(w.a));inside_query={};
    native_connection_event(&w.a.connection,1);w.a.capture();
    auto copied=birth(w.a);observed(w.a,copied,query(w.a));
    refused_unchanged(w.a,copied,w.a.scope(),"copier-close invalidates actual connection generation before output",[&]{native_connection_event(&w.a.connection,0);});
}
void sql_connection_isolation() {
    reset(); World w;
    auto known = birth(w.b); observed(w.b,known,query(w.b));
    const auto epoch = w.b.source.owner_epoch;
    auto retained = [&]() -> bool {
        for (const auto &entry : source_state().entries)
            if (entry.sample.owner_epoch == epoch) return !entry.retired;
        return false;
    };
    auto capture_b = [&]() {
        auto table=load<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(&w.b.server));
        Build profile {builds[build_id].dispatcher,load<std::uintptr_t>(table+5*8)-source_state().main_image.base};
        SqlProfile sql; azsdk_source_sample output={}; output.owner_epoch=0xcafe;
        int status=observe(reinterpret_cast<std::uintptr_t>(&w.b.server),profile,sql,metadata_ops,&output,sizeof output);
        REQUIRE(status!=AZS_OK&&output.owner_epoch==0xcafe,"unrelated active operation refuses current publication without partial output");
    };
    source_state().connection_epochs.forward_close(&w.a.connection,[&]() {
        capture_b();
        REQUIRE(retained(),"unrelated active connection cannot permanently retire another source");
        return 0;
    });
    azsdk_source_sample current={};
    REQUIRE(azsdk_sources_read(epoch,&current,sizeof current)==AZS_OK&&(current.flags&AZS_CURRENT),
            "unchanged source receipt recovers after unrelated lifecycle operation");
    observed(w.b,known,query(w.b));
    source_state().connection_epochs.forward_close(&w.c.connection,[&]() {
        (void)azsdk_sources_sweep();
        REQUIRE(retained(),"sweep cannot reclaim source hidden only by unrelated active connection");
        return 0;
    });
    observed(w.b,known,query(w.b));
    const auto before=w.b.source.owner_epoch;
    w.b.capture();
    REQUIRE(w.b.source.owner_epoch==before,"unrelated operation cannot mint a duplicate source key");
}

#if defined(AZSDK_CACHE_RECLAIM_TESTING)
void sql_cache_reclamation_isolation() {
    Limits limits; limits.slots=1; reset(limits); World w;
    auto retained=birth(w.b); observed(w.b,retained,query(w.b));
    std::atomic<bool> active {false}, finish {false};
    std::thread closer;
    cache_reclaim_test_boundary=[&](unsigned boundary) {
        if (!boundary) {
            closer=std::thread([&] {
                source_state().connection_epochs.forward_close(&w.a.connection,[&] {
                    active.store(true,std::memory_order_release);
                    while(!finish.load(std::memory_order_acquire)) std::this_thread::yield();
                    return 0;
                });
            });
            while(!active.load(std::memory_order_acquire)) std::this_thread::yield();
        } else {
            finish.store(true,std::memory_order_release); closer.join();
        }
    };
    auto competing=birth(w.c);
    cache_reclaim_test_boundary={};
    REQUIRE(competing.status!=AZS_OK&&!competing.witness.birth_epoch,
            "receipt-only refusal cannot reclaim another live cache binding");
    observed(w.b,retained,query(w.b));
}
#endif

void hit_and_sources() {
    reset(); World w; auto b = birth(w.a); observed(w.a, b, query(w.a));
    w.c.same_source(w.a); REQUIRE(w.c.source.owner_epoch == w.a.source.owner_epoch, "same real source owner shares existing epoch");
    // A hit borrows the existing owner; its newly supplied Creator DB is unused.
    w.a.list.mount = w.b.list.mount;
    auto epoch_before_hit = api().counters().next_birth;
    auto hit = native_hit(w.a, 11);
    REQUIRE(same_owner(b.witness.cache, hit) && api().counters().next_birth == epoch_before_hit,
            "condition hit retains the actual owner and never allocates a new birth epoch");
    Scope frame {build_id, w.a.bank, &w.b.server, &w.a.list, w.b.source.owner_epoch};
    FixtureAdapter adapter(b.cache.get()); Guarded<NativeRows> rows; Witness witness;
    QueryCall call {rows.bytes, cb_native_query, &adapter, 0, 1, &w.a.list.mount};
    int status = api().query(frame, call, &witness); rows.check(); rows.value().~NativeRows();
    REQUIRE(status != AZS_OK && !witness.birth_epoch, "cache hit cannot relabel birth with another live DB/source");
    w.a.list.mount = w.a.updater.mount; observed(w.a, b, query(w.a));
    // The same source in another client does not allow substituting its cache/Collector.
    auto c = birth(w.c); observed(w.c, c, query(w.c));
    REQUIRE(!same_owner(c.witness.cache, b.cache) && !same_owner(c.witness.collector, w.a.list.collector),
            "client/source ownership contexts remain independent");
}
void native_exceptions() {
    reset(); World w; bool caught = false;
    try { birth(w.a, 11, [] { throw std::runtime_error("native creator exact exception"); }); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "native creator exact exception") == 0; }
    REQUIRE(caught && api().counters().retained == 0 && !api().counters().active, "creator exception propagates with no partial birth");
    after_owner = [] { throw std::runtime_error("native after owner"); }; caught = false;
    try { birth(w.a); } catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "native after owner") == 0; }
    after_owner = {}; REQUIRE(caught && w.a.list.collector->caches.empty(), "native temporary owner cleanup on unwind");
    auto b = birth(w.a); observed(w.a, b, query(w.a));
    inside_query = [] { throw std::runtime_error("native row query exact exception"); }; caught = false;
    try { query(w.a); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "native row query exact exception") == 0; }
    inside_query = {};
    REQUIRE(caught && !api().counters().active && !NativeRow::alive, "row query exception propagates without stranded scope");
    auto next = birth(w.b); observed(w.b, next, query(w.b));
}
void reentrant_birth() {
    reset(); World w; Born nested;
    auto outer = birth(w.a, 11, [&] { nested = birth(w.b); observed(w.b, nested, query(w.b)); });
    REQUIRE(outer.status != AZS_OK && !outer.witness.birth_epoch,
            "nested construction permanently poisons its surrounding factory");
    unavailable(query(w.a));
    observed(w.b, nested, query(w.b));
    auto fresh = birth(w.c); observed(w.c, fresh, query(w.c));
    REQUIRE(fresh.witness.source_epoch != nested.witness.source_epoch && fresh.witness.birth_epoch != nested.witness.birth_epoch,
            "nested refusal restores the prior frame before the next independent construction");
}
/* Real Collector.create has native cleanup/disposal calls after Creator work.
 * A layout mutation there must not be relabeled as the newly completed birth. */
void factory_cleanup_mutation() {
    reset(); World w;
    for (auto context : {&w.a, &w.b, &w.c, &w.d}) {
        after_owner = [] { mutate(setter); };
        auto value = birth(*context);
        after_owner = {};
        REQUIRE(value.status != AZS_OK && !value.witness.birth_epoch,
                "post-factory mutation must not acquire a new birth stamp");
        unavailable(query(*context));
    }
}
void factory_write_receipts() {
    namespace ff = cache_factory_fixture;
    namespace cb = azsdk_cache_birth;
    reset(); World w;
    for (auto context : {&w.a, &w.b, &w.c, &w.d}) {
        auto primary = birth(*context); observed(*context, primary, query(*context));
        auto fallback = birth(*context, 81, {}, cb::StandardFallbackFactory);
        observed(*context, fallback, query(*context));
        REQUIRE(fallback.witness.layout_generation > primary.witness.layout_generation,
                "complete fallback writes retain their final successful generation");
        context->selected() = primary.cache;
        unavailable(query(*context)); // a layout mutation retires earlier births
        context->selected() = fallback.cache;
        observed(*context, fallback, query(*context));
        for (unsigned fault = 1; fault <= 6; ++fault) {
            auto bad = birth(*context, 81 + fault, {}, cb::StandardFallbackFactory, fault);
            REQUIRE(bad.status != AZS_OK && !bad.witness.birth_epoch,
                    "wrong setup/status or missing/repeated/extra/failing setter sequence declines");
            unavailable(query(*context));
        }
        ff::tail = [] { mutate(setter); };
        auto after_final = birth(*context, 91, {}, cb::StandardFallbackFactory);
        ff::tail = {};
        REQUIRE(after_final.status != AZS_OK && !after_final.witness.birth_epoch,
                "factory cleanup mutation cannot replace final-setter stamp");
        ff::inside_final_write = [] { mutate(setter); };
        auto nested = birth(*context, 92, {}, cb::StandardFallbackFactory);
        ff::inside_final_write = {};
        REQUIRE(nested.status != AZS_OK && !nested.witness.birth_epoch,
                "nested mutation inside final setter cannot be adopted as its own fence");
        auto unsupported = birth(*context, 93, {}, 99);
        REQUIRE(unsupported.status != AZS_OK && !unsupported.witness.birth_epoch,
                "unreviewed factory mode still forwards but cannot publish");
        auto wrong_mode = birth(*context, 94, {}, cb::StandardFallbackFactory, 0, 1);
        REQUIRE(wrong_mode.status != AZS_OK && !wrong_mode.witness.birth_epoch,
                "unsupported standard Creator mode must forward without a birth claim");
    }
}
void mutation_guards() {
    reset(); World w;
    for (auto kind : {setup, setter, records_grow, clear, dispose}) {
        auto b = birth(w.a); observed(w.a, b, query(w.a)); auto before = api().counters().generation;
        mutate(kind, [&] {
            unavailable(query(w.a));
            mutate(setter, [&] { REQUIRE(api().counters().active >= 2, "nested mutation guard remains active"); unavailable(query(w.a)); });
        });
        REQUIRE(!api().counters().active && api().counters().generation > before, "retirement precedes original and unwinds depth");
        unavailable(query(w.a));
    }
    auto b = birth(w.b); observed(w.b, b, query(w.b)); bool caught = false;
    try { mutate(setup, [] { throw std::runtime_error("native mutation"); }); }
    catch (const std::runtime_error &) { caught = true; }
    REQUIRE(caught && !api().counters().active, "mutation exception preserves native exception and clears active guard");
    unavailable(query(w.b));
    auto renewed = birth(w.c); observed(w.c, renewed, query(w.c));
    inside_query = [] { mutate(setter); }; unavailable(query(w.c)); inside_query = {};
}
void bounded_nonwrap() {
    Limits limits; limits.slots = 2; reset(limits); World w;
    auto a = birth(w.a); observed(w.a, a, query(w.a)); auto b = birth(w.b); observed(w.b, b, query(w.b));
    auto c = birth(w.c); REQUIRE(c.status != AZS_OK && !c.witness.birth_epoch, "bounded birth table fails unavailable");
    unavailable(query(w.c)); REQUIRE(api().counters().retained <= 2, "no hidden unbounded owner registry");
    limits.next_birth = UINT64_MAX - 1; reset(limits);
    auto last = birth(w.d); observed(w.d, last, query(w.d));
    REQUIRE(last.witness.birth_epoch == UINT64_MAX, "final nonzero birth epoch");
    auto exhausted = birth(w.c, 22); REQUIRE(exhausted.status != AZS_OK && !exhausted.witness.birth_epoch,
                                           "birth epoch exhaustion never wraps or reuses");
    REQUIRE(api().counters().next_birth == UINT64_MAX, "exhausted birth counter remains saturated");
    limits.next_birth = 0; limits.generation = UINT64_MAX; reset(limits);
    auto g = birth(w.a, 23); observed(w.a, g, query(w.a)); mutate(setter); unavailable(query(w.a));
    REQUIRE(api().counters().generation == UINT64_MAX && api().counters().saturated, "layout generation never wraps");
    limits.generation = 1; limits.depth = 1; reset(limits);
    mutate(setup, [] { mutate(setter); });
    REQUIRE(!api().counters().active && api().counters().saturated, "bounded nesting poisons without depth wrap");
}
void weak_lifetimes_and_reuse() {
    reset(); World w; ListCache::reuse_next = true;
    auto old = birth(w.a); observed(w.a, old, query(w.a)); auto address = old.cache.get();
    auto old_weak = old.witness.cache;
    w.a.selected().reset(); w.a.list.collector->caches.clear(); old.cache.reset();
    REQUIRE(old_weak.expired(), "birth table retains no strong cache/row owner");
    ListCache::reuse_next = true; auto fresh = birth(w.a, 22); observed(w.a, fresh, query(w.a));
    REQUIRE(fresh.cache.get() == address && !same_owner(old_weak, fresh.cache) &&
            fresh.witness.birth_epoch != old.witness.birth_epoch, "same cache address is a different real owner");
    fresh.cache.reset(); w.a.selected().reset(); w.a.list.collector->caches.clear();
    w.a.list.collector.reset(); ListCacheCollector::reuse_next = true;
    w.a.list.collector.reset(new ListCacheCollector); auto first = birth(w.a, 31); observed(w.a, first, query(w.a));
    auto collector_address = w.a.list.collector.get(); auto weak_collector = first.witness.collector;
    first.cache.reset(); w.a.selected().reset(); w.a.list.collector.reset();
    REQUIRE(weak_collector.expired(), "birth table cannot prolong Collector/pool lifetime");
    ListCacheCollector::reuse_next = true; w.a.list.collector.reset(new ListCacheCollector);
    auto next = birth(w.a, 32); observed(w.a, next, query(w.a));
    REQUIRE(w.a.list.collector.get() == collector_address && !same_owner(weak_collector, w.a.list.collector),
            "Collector address reuse cannot select an old pool owner");
}
void actual_query_pins_and_output() {
    reset(); World w; auto b = birth(w.a); observed(w.a, b, query(w.a));
    auto cache = b.witness.cache; auto collector = b.witness.collector;
    FixtureAdapter adapter(b.cache.get()); Guarded<NativeRows> output;
    auto scope = w.a.scope(); QueryCall call {output.bytes, cb_native_query, &adapter, 0, 1, &w.a.list.mount};
    // These weak witnesses are output sentinels, not fabricated ownership words.
    Witness sentinel = b.witness; sentinel.row_value = 0xf00d;
    auto birth_epoch = sentinel.birth_epoch, source_epoch = sentinel.source_epoch;
    inside_query = [&] {
        mutate(clear, [&] {
            w.a.list.collector->caches.clear(); w.a.selected().reset(); b.cache.reset();
            w.a.list.collector.reset();
            REQUIRE(!cache.expired() && !collector.expired() && FixturePool::alive == 4,
                    "actual query frame pins cache AND its owning Collector/pool");
        });
    };
    int status = api().query(scope, call, &sentinel); inside_query = {}; output.check();
    REQUIRE(status != AZS_OK && sentinel.birth_epoch == birth_epoch && sentinel.source_epoch == source_epoch &&
            sentinel.row_value == 0xf00d && !sentinel.cache.owner_before(cache) && !cache.owner_before(sentinel.cache),
            "retired query leaves existing nontrivial output unchanged");
    REQUIRE(cache.expired() && collector.expired() && FixturePool::alive == 3,
            "scoped query pins release before returning to native caller");
    REQUIRE(NativeRow::alive == 1, "native caller still exclusively owns returned row after owner retirement");
    output.value().~NativeRows(); REQUIRE(!NativeRow::alive, "no row retained by observer");
}
void existing_source_retirement() {
    reset(); World w; auto b = birth(w.a); observed(w.a, b, query(w.a));
    auto before = w.a.source.owner_epoch;
    // Existing production owner helper retires the server binding. The real
    // native ListServer still owns the old MountPoint/DB; media does not help.
    {
        std::lock_guard<std::mutex> lock(source_state().registry_mutex);
        retire_server(reinterpret_cast<std::uintptr_t>(&w.a.server));
    }
    azsdk_source_sample old = {};
    REQUIRE(azsdk_sources_read(before, &old, sizeof old) == AZS_STALE, "real source helper marks the exact epoch stale");
    unavailable(query(w.a));
    w.a.capture(); auto fresh = birth(w.a, 51); observed(w.a, fresh, query(w.a));
    REQUIRE(fresh.witness.source_epoch != before, "retired source epoch cannot be revived by a cache hit");
}
void receipt_revision_and_cold_cache() {
    reset(); World w; auto known = birth(w.a); observed(w.a, known, query(w.a));
    auto epoch = w.a.source.owner_epoch, receipt = w.a.source.revision;
    w.a.capture();
    REQUIRE(w.a.source.owner_epoch == epoch && w.a.source.revision > receipt,
            "same actual source owners keep their epoch across a new receipt revision");
    observed(w.a, known, query(w.a));
    auto next_birth = api().counters().next_birth;
    CreatorOwner creator(new ListCacheCreator);
    creator->database = w.a.mount().database;
    creator->content = known.cache->condition->content;
    Guarded<CacheOwner> output;
    BirthCall native_only {output.bytes, cb_native_create, w.a.list.collector.get(), &creator};
    unsigned prior = creates; native_only.forward(); output.check();
    REQUIRE(creates == prior + 1, "cold native cache is genuinely created outside observer birth capture");
#if defined(__aarch64__)
    REQUIRE(cb_native_owner_destination == reinterpret_cast<std::uintptr_t>(output.bytes), "cold creation keeps original owner X8 destination");
#endif
    CacheOwner cold = std::move(output.value()); output.value().~shared_ptr();
    REQUIRE(cold && cold.use_count() == 2 && cold.get() != known.cache.get() &&
            cold->condition->content == known.cache->condition->content &&
            cold->condition->mode == known.cache->condition->mode,
            "cold cache shares content/media/source but has a distinct real native owner");
    w.a.selected() = cold;
    // No fabricated pointer and no delayed pool access: both caches remain
    // genuinely retained by this live native Collector throughout these calls.
    refused_unchanged(w.a, known, w.a.scope(), "cold same-content/media cache must decline unchanged output");
    refused_unchanged(w.a, known, w.a.scope(), "repeated cold query cannot create or renew a birth witness");
    REQUIRE(api().counters().next_birth == next_birth, "query cannot allocate a missing cache birth epoch");
}
void live_context_guard_refusals() {
    reset(); World w;
    auto wrong_epoch = birth(w.a, 61); observed(w.a, wrong_epoch, query(w.a));
    auto scope = w.a.scope(); scope.source_epoch = w.b.source.owner_epoch;
    REQUIRE(scope.source_epoch != w.a.source.owner_epoch, "wrong epoch belongs to another real live source");
    refused_unchanged(w.a, wrong_epoch, scope, "wrong source_epoch with real a-context must decline unchanged output");
    auto no_marker = birth(w.a, 62); observed(w.a, no_marker, query(w.a));
    reinterpret_cast<unsigned char *>(&w.a.server)[0xd2] = 0;
    refused_unchanged(w.a, no_marker, w.a.scope(), "missing invocation marker on live receiver must decline observation");
    reinterpret_cast<unsigned char *>(&w.a.server)[0xd2] = 1;
    auto bad_marker = birth(w.a, 63); observed(w.a, bad_marker, query(w.a));
    reinterpret_cast<unsigned char *>(&w.a.server)[0xd2] = 2;
    refused_unchanged(w.a, bad_marker, w.a.scope(), "unsupported invocation marker on live receiver must decline observation");
    reinterpret_cast<unsigned char *>(&w.a.server)[0xd2] = 1;
    auto wrong_worker = birth(w.a, 64); observed(w.a, wrong_worker, query(w.a));
    auto worker = w.a.server.worker; w.a.server.worker = std::thread::id();
    refused_unchanged(w.a, wrong_worker, w.a.scope(), "worker-affinity refusal on live receiver still forwards original");
    w.a.server.worker = worker;
    // The World, its shared owners and all receivers remain alive. These guards
    // narrow a real invocation; they never acquire liveness for raw pointers.
}
void live_source_tuple_changes() {
    reset(); World w; auto known = birth(w.a); observed(w.a, known, query(w.a));
    const auto epoch = w.a.source.owner_epoch, revision = w.a.source.revision;
    auto database = w.a.mount().database;
    const auto connection = database->connection;
    const auto database_media = database->media, mount_media = w.a.mount().media;
    REQUIRE(connection != &w.a.alternate_connection &&
            !(reinterpret_cast<std::uintptr_t>(&w.a.alternate_connection) & 7u),
            "replacement connection is another aligned live fixture object");
    // No capture() between these mutations and the query: the old real source
    // entry and both real shared owners remain live throughout. Test before
    // forwarding, inside the native original, and inside the borrowed copier.
    for (unsigned stage = 0; stage != 3; ++stage) {
        unsigned changed = 0;
        std::function<void()> change = [&] { ++changed; database->connection = &w.a.alternate_connection; };
        std::function<void()> restore = [&] { database->connection = connection; };
        if (!stage) change();
        if (stage == 1) inside_query = change;
        refused_unchanged(w.a, known, w.a.scope(),
                          "same live DB owner with a changed connection cannot qualify the old source epoch",
                          stage == 2 ? change : std::function<void()>(), restore);
        inside_query = {};
        REQUIRE(changed == 1, "connection changed at the intended real invocation boundary");
        observed(w.a, known, query(w.a));
        for (unsigned mask = 1; mask != 4; ++mask) {
            changed = 0;
            change = [&] {
                ++changed;
                if (mask & 1) database->media = 0x401;
                if (mask & 2) w.a.mount().media = 0x401;
            };
            restore = [&] { database->media = database_media; w.a.mount().media = mount_media; };
            if (!stage) change();
            if (stage == 1) inside_query = change;
            refused_unchanged(w.a, known, w.a.scope(),
                              "changed DB/Mount media tuple cannot qualify a same-owner old source epoch",
                              stage == 2 ? change : std::function<void()>(), restore);
            inside_query = {};
            REQUIRE(changed == 1, "media changed at the intended real invocation boundary");
            observed(w.a, known, query(w.a));
        }
    }
    REQUIRE(w.a.source.owner_epoch == epoch && w.a.source.revision == revision,
            "tuple refusal does not rely on another capture or a fabricated source entry");
}
void copy_boundary_borrow_changes() {
    reset(); World w; auto known = birth(w.a); auto other = birth(w.b);
    observed(w.a, known, query(w.a)); observed(w.b, other, query(w.b));
    const auto next_birth = api().counters().next_birth;
    // The fixture changes shared_ptr values in stable live holder storage.
    // It does not reallocate a vector or claim that pinning its pointee keeps
    // a vector element in place. All replacements are genuine live owners.
    auto collector = w.a.list.collector;
    auto selected = w.a.selected();
    auto mount = w.a.list.mount;
    auto database = w.a.mount().database;
    for (unsigned kind = 0; kind != 4; ++kind) {
        unsigned changed = 0;
        std::function<void()> change = [&] {
            ++changed;
            switch (kind) {
            case 0: w.a.list.collector = w.b.list.collector; break;
            case 1: w.a.selected() = other.cache; break;
            case 2: w.a.list.mount = w.b.list.mount; break;
            case 3: w.a.mount().database = w.b.mount().database; break;
            }
        };
        std::function<void()> restore = [&] {
            w.a.list.collector = collector; w.a.selected() = selected;
            w.a.list.mount = mount; w.a.mount().database = database;
        };
        refused_unchanged(w.a, known, w.a.scope(),
                          "copier reentrancy changing a real cache/Collector/Mount/DB borrow must decline unchanged",
                          change, restore);
        REQUIRE(changed == 1, "borrow changes after the genuine native row scalar was read");
        observed(w.a, known, query(w.a));
    }
    REQUIRE(api().counters().next_birth == next_birth, "borrow-change refusal cannot renew a cache birth");
}
void source_disable_reenable() {
    reset(); World w; auto old = birth(w.a); observed(w.a, old, query(w.a));
    auto epoch = w.a.source.owner_epoch;
    REQUIRE(azsdk_sources_configure(0, int(build_id), 0) == AZS_UNSUPPORTED, "real source disable retires bindings");
    refused_unchanged(w.a, old, w.a.scope(), "disabled source must decline unchanged while native query forwards");
    REQUIRE(azsdk_sources_configure(1, int(build_id), 0) == AZS_OK, "real source provider reenables without epoch reset");
    w.a.capture();
    REQUIRE(w.a.source.owner_epoch != epoch, "reenabled identical real owners receive a fresh non-reused source epoch");
    azsdk_source_sample retired = {};
    REQUIRE(azsdk_sources_read(epoch, &retired, sizeof retired) == AZS_STALE,
            "disable/reenable leaves the old source epoch retired");
    auto before_hit = api().counters().next_birth;
    auto hit = native_hit(w.a, old.cache->condition->content);
    REQUIRE(same_owner(old.witness.cache, hit), "reenabled native condition lookup still returns the old retained cache owner");
    refused_unchanged(w.a, old, w.a.scope(), "reenabled source cannot renew an old native cache birth");
    REQUIRE(api().counters().next_birth == before_hit, "reenable/query cannot manufacture a new birth for an old hit");
    auto fresh = birth(w.a, 71); observed(w.a, fresh, query(w.a));
    REQUIRE(fresh.witness.source_epoch == w.a.source.owner_epoch && fresh.witness.source_epoch != epoch,
            "only an actual new birth can bind the new live source epoch");
}
} // namespace

extern "C" CacheOwner cb_native_create_impl(ListCacheCollector *collector, CreatorOwner &creator) {
    ++creates; ++creator->forwarded_calls;
    creator->result_destination = cb_native_owner_destination;
    CacheOwner result(creator->make(collector->pool.get()));
    if (after_owner) after_owner();
    result->native_counter = ++collector->native_counter;
    collector->caches.push_back(result); return result;
}
extern "C" NativeRows cb_native_query_impl(FixtureAdapter *adapter, unsigned start, unsigned count,
                                            const std::shared_ptr<MountPoint> &mount) {
    ++queries; REQUIRE(start == 0 && count == 1 && mount && adapter->cache, "genuine native query arguments");
    adapter->result_destination = cb_native_rows_destination;
    REQUIRE(FixturePool::alive && adapter->cache->layout->borrowed_pool, "native owned query keeps actual pool alive");
    if (inside_query) inside_query();
    return NativeRows(adapter->cache->layout->tag);
}
int main(int argc, char **argv) {
    static_assert(sizeof(CacheOwner) == 16 && !std::is_trivially_copyable<CacheOwner>::value,
                  "real 16-byte nontrivial GNU owner return");
    static_assert(sizeof(NativeRows) == 24 && sizeof(NativeRow) == 0x98, "native result carrier/row fixture widths");
    REQUIRE(argc == 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0), "separate process per build profile");
    build_id = unsigned(argv[1][0] - '0');
    REQUIRE(azsdk_sources_configure(1, int(build_id), 0) == AZS_OK, "existing source registry starts without physical policy");
    REQUIRE(api().version == 1, "reviewed test-contract version");
    first_birth();
#if defined(AZSDK_CACHE_RECLAIM_TESTING)
    sql_cache_reclamation_isolation();
    std::puts("PASS cache-reclamation: unrelated lifecycle activity never evicts a still-owned binding");
    return 0;
#endif
    for (auto test : {sql_connection_lifetime, sql_connection_isolation, hit_and_sources, native_exceptions, reentrant_birth, factory_cleanup_mutation, factory_write_receipts, mutation_guards, bounded_nonwrap,
                      weak_lifetimes_and_reuse, actual_query_pins_and_output, existing_source_retirement,
                      receipt_revision_and_cold_cache, live_context_guard_refusals, live_source_tuple_changes,
                      copy_boundary_borrow_changes, source_disable_reenable}) {
        test();
        REQUIRE(!ListCache::alive && !ListCacheCollector::alive && !FixturePool::alive && !NativeRow::alive,
                "only weak/copy records survive native callbacks");
    }
#if defined(__aarch64__)
    std::printf("PASS cache-birth build=%u: real GNU owners, exact X8 destinations, native rows, source contexts and retirement\n", build_id);
#else
    std::printf("PASS cache-birth build=%u: GNU owner semantics only; AArch64 X8 NOT tested\n", build_id);
#endif
    return 0;
}
#endif
