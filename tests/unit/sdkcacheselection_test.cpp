/* SPDX-License-Identifier: GPL-3.0-only */
/* Meaningful missing-resolver runtime RED. Native-shaped real GNU owner
 * fixtures below are distinct from the invented TEST adapter contract.
 * No source registry, native entry mapping, hook installation or model of the
 * missing resolver is supplied by the unavailable path. */
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
    "FAIL cache-selection: %s:%d: %s (%s)\n", __FILE__, __LINE__, why, #x); std::abort(); } } while (0)

#if !defined(__linux__) || !defined(__GLIBCXX__)
int main() { std::puts("SKIP: requires Linux and genuine GNU shared ownership"); return 77; }
#else
namespace selection_native_fixture {
struct Life { bool alive = true; };
struct Pool {
    static unsigned alive;
    std::shared_ptr<Life> life {new Life};
    Pool() { ++alive; }
    ~Pool() { life->alive = false; --alive; }
};
unsigned Pool::alive;
class Database {
public:
    virtual ~Database() {}
    std::uint16_t media = 0x301;
    unsigned char pad[0x38 - 10] = {};
    void *connection;
    explicit Database(void *value) : connection(value) {}
};
class Mount {
public:
    virtual ~Mount() {}
    std::uint16_t media = 0x301;
    unsigned char pad[0x30 - 10] = {};
};
class SqliteMount : public Mount {
public:
    std::shared_ptr<Database> database;
    explicit SqliteMount(void *value) : database(new Database(value)) {}
};
class Cache {
public:
    static unsigned alive;
    static bool arena_next, arena_live;
    static void *operator new(std::size_t);
    static void operator delete(void *) noexcept;
    const unsigned value;
    std::weak_ptr<Life> pool_life;
    Cache(unsigned tag, Pool &pool) : value(tag), pool_life(pool.life) { ++alive; }
    ~Cache() {
        auto life = pool_life.lock();
        REQUIRE(life && life->alive, "cache destruction precedes actual owning Collector pool destruction");
        --alive;
    }
};
unsigned Cache::alive;
bool Cache::arena_next, Cache::arena_live;
alignas(Cache) unsigned char cache_arena[sizeof(Cache)];
void *Cache::operator new(std::size_t bytes) {
    if (arena_next) {
        REQUIRE(!arena_live, "raw cache address cannot be reused while real scoped pin still owns it");
        arena_next = false; arena_live = true; return cache_arena;
    }
    return ::operator new(bytes);
}
void Cache::operator delete(void *p) noexcept {
    if (p == cache_arena) arena_live = false; else ::operator delete(p);
}
using CacheOwner = std::shared_ptr<Cache>;
class Collector {
public:
    virtual ~Collector() { caches.clear(); }
    unsigned char pad[0x28 - 8] = {};
    std::vector<CacheOwner> caches;
    std::unique_ptr<Pool> pool {new Pool};
    std::uint64_t tail = 0;
};
class ListServer {
public:
    static bool arena_next, arena_live;
    static void *operator new(std::size_t);
    static void operator delete(void *) noexcept;
    virtual ~ListServer() { clients.clear(); collector.reset(); mount.reset(); }
    unsigned char pad[0x28 - 8] = {};
    std::shared_ptr<Mount> mount;
    std::uint64_t gap = 0;
    std::shared_ptr<Collector> collector {new Collector};
    std::vector<std::vector<CacheOwner>> clients {4, std::vector<CacheOwner>(9)};
};
bool ListServer::arena_next, ListServer::arena_live;
alignas(ListServer) unsigned char server_arena[sizeof(ListServer)];
void *ListServer::operator new(std::size_t bytes) {
    if (arena_next) {
        REQUIRE(!arena_live, "only an actually destroyed native root can reuse its fixture address");
        arena_next = false; arena_live = true; return server_arena;
    }
    return ::operator new(bytes);
}
void ListServer::operator delete(void *p) noexcept {
    if (p == server_arena) arena_live = false; else ::operator delete(p);
}
} // namespace selection_native_fixture

/* Invented private TEST contract. Storage belongs to the fixture caller; a
 * future adapter may construct the actual header's fence there, and must
 * delegate every resolution/taint/validation decision to that real helper.
 * The storage size is not a native ABI claim or a replacement registry.
 */
namespace cache_selection_test {
using namespace selection_native_fixture;
enum Event { SelectionChange = 1, ScopeLoss = 2 };
struct StateStorage { alignas(std::max_align_t) unsigned char bytes[256]; };
struct Limits { unsigned depth = 4; std::uint64_t revision = 1, next_invocation = 0; };
struct Position { std::uint32_t outer, selector; };
struct Scope {
    unsigned build;
    void *root, *context;
    bool (*already_owned)(void *);
    bool (*supported_sqlite_mount)(void *, const void *);
};
struct EventSink {
    void *context;
    void (*around)(void *, unsigned, void (*)(void *), void *);
};
struct Call {
    void *context;
    void (*original)(void *, EventSink);
    bool (*copy_scalar)(void *, EventSink, std::uint32_t &);
};
struct Witness {
    std::uint64_t invocation = 0;
    std::weak_ptr<Mount> mount;
    std::weak_ptr<Database> database;
    std::weak_ptr<Collector> collector;
    std::weak_ptr<Cache> cache;
    std::uint32_t value = 0;
};
struct Counters { unsigned active = 0; bool saturated = false; };
struct Api {
    void (*construct)(StateStorage &, const Limits &);
    void (*destroy)(StateStorage &);
    int (*observe)(StateStorage &, const Scope &, Position, const Call &, Witness *);
    Counters (*counters)(const StateStorage &);
};
} // namespace cache_selection_test
extern "C" const cache_selection_test::Api *azsdk_cache_selection_test_api_v1() __attribute__((weak));
#if defined(AZSDK_HAVE_CACHE_SELECTION)
#include "../../src/common/azcacheselection.h"
/* Test-only adapter: all pin, bounds, receipt and fencing decisions delegate to
 * the real generic header. This constructs its actual caller-owned State in
 * fixture storage; it is not a mirrored native/source registry or service. */
namespace selection_actual_adapter {
namespace cs = azsdk_cache_selection;
using namespace selection_native_fixture;
using namespace cache_selection_test;
using ActualState = cs::State;
static_assert(sizeof(cs::State) <= sizeof(StateStorage) && alignof(cs::State) <= alignof(StateStorage),
              "fixture storage holds the actual embedded selection fence");
using ActualPins = cs::Pins<Mount, Database, Collector, Cache>;
static_assert(!std::is_copy_assignable<ActualPins>::value && !std::is_move_assignable<ActualPins>::value,
              "memberwise assignment cannot release Collector before an old cache pin");
cs::State &state(StateStorage &storage) { return *reinterpret_cast<cs::State *>(storage.bytes); }
const cs::State &state(const StateStorage &storage) { return *reinterpret_cast<const cs::State *>(storage.bytes); }
void construct(StateStorage &storage, const Limits &limits) {
    new (storage.bytes) cs::State(cs::State::TestingSeed {limits.depth, limits.revision, limits.next_invocation});
}
void destroy(StateStorage &storage) { state(storage).~ActualState(); }
void around(void *p, unsigned kind, void (*original)(void *), void *context) {
    cs::Mutation mutation(*static_cast<cs::State *>(p), kind);
    original(context); // genuine mutation and native exception, never swallowed
}
int observe(StateStorage &storage, const Scope &source, Position position, const Call &call, Witness *out) {
    auto &fence = state(storage);
    cs::Invocation invocation(fence);
    cs::Borrow borrow {source.build, source.root, source.context, source.already_owned, source.supported_sqlite_mount};
    cs::Position selected_position {position.outer, position.selector};
    auto selected = cs::resolve_owned<Mount, Database, Collector, Cache>(invocation, borrow, selected_position);
    EventSink sink {&fence, around};
    call.original(call.context, sink); // exactly once; outside observer catches
    if (selected.status != cs::Ok) return selected.status;
    if (!cs::validate_fresh(invocation, borrow, selected_position, selected)) return cs::Changed;
    try {
        std::uint32_t value = 0;
        if (!call.copy_scalar || !call.copy_scalar(call.context, sink, value)) return cs::Guard;
        if (!cs::validate_fresh(invocation, borrow, selected_position, selected)) return cs::Changed;
        if (out) {
            Witness result;
            result.invocation = selected.invocation; result.value = value;
            result.mount = selected.pins.mount(); result.database = selected.pins.database();
            result.collector = selected.pins.collector(); result.cache = selected.pins.cache();
            *out = result;
        }
        return cs::Ok;
    } catch (...) { return cs::Guard; }
}
Counters counters(const StateStorage &storage) {
    Counters result;
    result.active = state(storage).active(); result.saturated = state(storage).saturated(); return result;
}
} // namespace selection_actual_adapter
extern "C" const cache_selection_test::Api *azsdk_cache_selection_test_api_v1() {
    using namespace selection_actual_adapter;
    static const cache_selection_test::Api actual {construct, destroy, observe, counters};
    return &actual;
}
#endif

namespace {
using namespace selection_native_fixture;
using namespace cache_selection_test;
void absent_construct(StateStorage &, const Limits &) {}
void absent_destroy(StateStorage &) {}
int absent_observe(StateStorage &, const Scope &, Position, const Call &call, Witness *) {
    call.original(call.context, {nullptr, nullptr}); // genuine native work, no owner engine
    return -1;
}
Counters absent_counters(const StateStorage &) { return {}; }
const Api absent {absent_construct, absent_destroy, absent_observe, absent_counters};
const Api &api() {
    const Api *(*getter)() = azsdk_cache_selection_test_api_v1;
    return getter ? *getter() : absent;
}
struct State {
    StateStorage storage;
    explicit State(Limits limits = {}) { api().construct(storage, limits); }
    ~State() { api().destroy(storage); }
};
unsigned build_id;
template<class T> std::size_t offset(const void *base, const T *member) {
    return reinterpret_cast<const unsigned char *>(member) - static_cast<const unsigned char *>(base);
}
template<class T> bool same(const std::weak_ptr<T> &weak, const std::shared_ptr<T> &owner) {
    return !weak.owner_before(owner) && !owner.owner_before(weak) && weak.lock().get() == owner.get();
}
template<class T> bool same_weak(const std::weak_ptr<T> &a, const std::weak_ptr<T> &b) {
    return !a.owner_before(b) && !b.owner_before(a);
}
void action_thunk(void *p) { (*static_cast<std::function<void()> *>(p))(); }
void event(EventSink sink, Event kind, std::function<void()> action) {
    if (sink.around) sink.around(sink.context, kind, action_thunk, &action);
    else action();
}
struct Context {
    State &state;
    std::shared_ptr<ListServer> root;
    Position position;
    unsigned tag, marker = 1, caller_depth = 0;
    bool mount_supported = true;
    std::thread::id worker = std::this_thread::get_id();
    alignas(8) std::uint64_t connection = 0;
    Context(State &s, unsigned outer, unsigned selector, unsigned value) : state(s), position{outer, selector}, tag(value) {
        rebuild();
    }
    void rebuild() {
        root.reset(new ListServer);
        root->mount.reset(new SqliteMount(&connection));
        add(position, tag);
        REQUIRE(offset(root.get(), &root->mount) == 0x28 && offset(root.get(), &root->collector) == 0x40 &&
                offset(root.get(), &root->clients) == 0x50, "native ListServer real owner/vector prefix");
        auto &mount = static_cast<SqliteMount &>(*root->mount);
        REQUIRE(offset(&mount, &mount.database) == 0x30 && offset(mount.database.get(), &mount.database->connection) == 0x38,
                "supported native Mount/DB prefix follows caller's actual nominal proof");
    }
    void add(Position p, unsigned value) {
        CacheOwner owner(new Cache(value, *root->collector->pool));
        root->collector->caches.push_back(owner); root->clients.at(p.outer).at(p.selector - 1) = owner;
    }
    CacheOwner &selected() { return root->clients.at(position.outer).at(position.selector - 1); }
    std::shared_ptr<Database> &database() { return static_cast<SqliteMount &>(*root->mount).database; }
    static bool owned(void *p) {
        auto &c = *static_cast<Context *>(p);
        // Complete-object ownership/serialization is supplied by this actual
        // caller. These readiness checks do not acquire it or solve a race.
        return c.root && c.caller_depth && c.marker == 1 && c.worker == std::this_thread::get_id();
    }
    static bool sqlite(void *p, const void *mount) {
        auto &c = *static_cast<Context *>(p);
        return c.mount_supported && dynamic_cast<const SqliteMount *>(static_cast<const Mount *>(mount));
    }
    Scope scope() { return {build_id, root.get(), this, owned, sqlite}; }
};
struct World {
    State state;
    Context a {state, 0, 1, 101}, b {state, 3, 9, 202}, c {state, 1, 4, 303}, d {state, 2, 7, 404};
    // Independent actual owner contexts. Native banks/selectors are not decks.
};
struct NativeException {};
struct GuardedValue {
    std::uint64_t before = UINT64_C(0x123456789abcdef0);
    std::uint32_t value = 0;
    std::uint64_t after = UINT64_C(0xfedcba9876543210);
    void check() const { REQUIRE(before == UINT64_C(0x123456789abcdef0) && after == UINT64_C(0xfedcba9876543210),
                                 "actual native output guards remain intact"); }
};
unsigned native_query(ListServer *root, Position position) {
    if (!root || position.outer >= root->clients.size() || !position.selector || position.selector > 9) return 0;
    const auto &inner = root->clients[position.outer];
    if (position.selector > inner.size() || !inner[position.selector - 1]) return 0;
    return inner[position.selector - 1]->value;
}
struct NativeCall {
    Context &context;
    ListServer *const original_root;
    Position position;
    GuardedValue output;
    unsigned forwards = 0, copies = 0;
    std::function<void(EventSink)> inside_original, inside_copy;
    NativeCall(Context &c, Position p) : context(c), original_root(c.root.get()), position(p) {}
    static void forward(void *p, EventSink sink) {
        auto &call = *static_cast<NativeCall *>(p); ++call.forwards;
        unsigned value = native_query(call.original_root, call.position);
        // Native root is last read here. Synchronous scope-loss tests revoke
        // before releasing it; no concurrent free-after-check is permitted.
        if (call.inside_original) call.inside_original(sink);
        call.output.value = value;
    }
    static bool copy(void *p, EventSink sink, std::uint32_t &value) {
        auto &call = *static_cast<NativeCall *>(p); ++call.copies;
        REQUIRE(call.forwards == 1, "scalar copied only after genuine original forwarding");
        value = call.output.value;
        if (call.inside_copy) call.inside_copy(sink);
        return true;
    }
    int observe(Witness &out) {
        struct Caller {
            Context &context;
            explicit Caller(Context &c) : context(c) { ++context.caller_depth; }
            ~Caller() { --context.caller_depth; }
        } owned(context);
        auto scope = context.scope();
        Call call {this, forward, copy};
        try {
            int status = api().observe(context.state.storage, scope, position, call, &out);
            output.check(); REQUIRE(forwards == 1, "resolver forwards real native query exactly once");
            return status;
        } catch (...) { output.check(); REQUIRE(forwards == 1, "native exception forwards once"); throw; }
    }
};
Witness sentinel(Context &context) {
    Witness result;
    result.invocation = UINT64_C(0x1020304050607080); result.value = 0xcafe;
    result.mount = context.root->mount; result.database = context.database();
    result.collector = context.root->collector; result.cache = context.selected();
    return result;
}
bool unchanged(const Witness &a, const Witness &b) {
    return a.invocation == b.invocation && a.value == b.value && same_weak(a.mount, b.mount) &&
           same_weak(a.database, b.database) && same_weak(a.collector, b.collector) && same_weak(a.cache, b.cache);
}
Witness observed(Context &context) {
    NativeCall call(context, context.position); Witness witness;
    const auto mount_count = context.root->mount.use_count(), database_count = context.database().use_count();
    const auto collector_count = context.root->collector.use_count(), cache_count = context.selected().use_count();
    int status = call.observe(witness);
    std::printf("NATIVE_SELECTION_FORWARD_OK build=%u shared16=%zu vector24=%zu forwards=%u value=%u\n",
                build_id, sizeof(CacheOwner), sizeof(std::vector<CacheOwner>), call.forwards, call.output.value);
    std::fflush(stdout);
    REQUIRE(status == 0 && witness.invocation && same(witness.mount, context.root->mount) &&
            same(witness.database, context.database()) && same(witness.collector, context.root->collector) &&
            same(witness.cache, context.selected()), "missing fresh selected-owner pins and real ownership result");
    REQUIRE(call.copies == 1 && witness.value == context.tag && witness.value == call.output.value,
            "actual native selected-cache scalar copied once inside owned callback");
    REQUIRE(context.root->mount.use_count() == mount_count && context.database().use_count() == database_count &&
            context.root->collector.use_count() == collector_count && context.selected().use_count() == cache_count,
            "all strong source/cache/Collector pins end with native callback");
    REQUIRE(api().counters(context.state.storage).active == 0, "caller-owned invocation state leaves no active frame");
    return witness;
}
void refused(NativeCall &call, Witness before) {
    Witness output = before;
    auto active = api().counters(call.context.state.storage).active;
    REQUIRE(call.observe(output) != 0 && unchanged(output, before), "refusal leaves every existing weak/scalar output unchanged");
    REQUIRE(api().counters(call.context.state.storage).active == active, "refusal restores prior actual invocation depth");
}
void first_observation() {
    World w;
    for (auto context : {&w.a, &w.b, &w.c, &w.d}) observed(*context);
}
void positions_and_guards() {
    World w;
    for (unsigned selector = 1; selector <= 9; ++selector) {
        w.a.position.selector = selector; w.a.add(w.a.position, w.a.tag); observed(w.a);
    }
    auto before = sentinel(w.a);
    for (auto position : {Position{4, 1}, Position{UINT32_MAX, 1}, Position{0, 0}, Position{0, 10}, Position{0, 257}}) {
        NativeCall call(w.a, position); refused(call, before);
    }
    w.a.root->clients[0].resize(1); NativeCall short_inner(w.a, {0, 9}); refused(short_inner, before);
    w.a.position.selector = 1;
    auto owner = w.a.selected(); w.a.selected().reset(); NativeCall empty(w.a, w.a.position); refused(empty, before);
    w.a.selected() = owner;
    for (unsigned kind = 0; kind < 3; ++kind) {
        if (!kind) w.a.marker = 0;
        if (kind == 1) w.a.worker = std::thread::id();
        if (kind == 2) w.a.mount_supported = false;
        NativeCall call(w.a, w.a.position); refused(call, before);
        w.a.marker = 1; w.a.worker = std::this_thread::get_id(); w.a.mount_supported = true;
    }
    auto supported = w.a.root->mount;
    w.a.root->mount.reset(new Mount); // real live unsupported base, with no DB member
    NativeCall wrong_nominal(w.a, w.a.position); refused(wrong_nominal, before);
    w.a.root->mount = supported;
}
void reallocation_and_actual_owners() {
    World w; auto before = sentinel(w.a);
    for (unsigned stage = 0; stage != 2; ++stage) {
        auto selected = w.a.selected();
        NativeCall call(w.a, w.a.position);
        auto change = [&](EventSink) {
            // Actual root remains owned/serialized. Deliberately no taint here:
            // fresh validation must inspect CURRENT owner storage, not a saved
            // vector-element address. No stale element is dereferenced by test.
            auto old_outer = reinterpret_cast<std::uintptr_t>(w.a.root->clients.data());
            auto old_inner = reinterpret_cast<std::uintptr_t>(w.a.root->clients[0].data());
            w.a.root->clients.reserve(w.a.root->clients.capacity() + 7);
            w.a.root->clients[0].reserve(w.a.root->clients[0].capacity() + 11);
            REQUIRE(reinterpret_cast<std::uintptr_t>(w.a.root->clients.data()) != old_outer &&
                    reinterpret_cast<std::uintptr_t>(w.a.root->clients[0].data()) != old_inner,
                    "real outer and inner vector storage reallocated");
            w.a.selected() = w.b.selected();
        };
        if (!stage) call.inside_original = change; else call.inside_copy = change;
        refused(call, before); w.a.selected() = selected; observed(w.a);
    }
    for (unsigned stage = 0; stage != 2; ++stage) for (unsigned kind = 0; kind != 4; ++kind) {
        auto mount = w.a.root->mount; auto database = w.a.database();
        auto collector = w.a.root->collector; auto selected = w.a.selected();
        NativeCall call(w.a, w.a.position);
        auto change = [&](EventSink) {
            if (kind == 0) w.a.root->mount = w.b.root->mount;
            if (kind == 1) w.a.database() = w.b.database();
            if (kind == 2) w.a.root->collector = w.b.root->collector;
            if (kind == 3) w.a.selected() = w.b.selected();
        };
        if (!stage) call.inside_original = change; else call.inside_copy = change;
        refused(call, before);
        w.a.root->mount = mount; w.a.database() = database;
        w.a.root->collector = collector; w.a.selected() = selected;
        observed(w.a);
    }
}
void positive_taint_and_nesting() {
    World w; auto before = sentinel(w.a); auto selected = w.a.selected();
    for (unsigned stage = 0; stage != 2; ++stage) {
        NativeCall call(w.a, w.a.position);
        auto change_back = [&](EventSink sink) {
            event(sink, SelectionChange, [&] { w.a.selected() = w.b.selected(); w.a.selected() = selected; });
        };
        if (!stage) call.inside_original = change_back; else call.inside_copy = change_back;
        refused(call, before); observed(w.a);
    }
    NativeCall outer(w.a, w.a.position);
    outer.inside_original = [&](EventSink sink) {
        event(sink, SelectionChange, [&] {
            NativeCall nested(w.a, w.a.position); refused(nested, before);
        });
    };
    refused(outer, before); observed(w.a);
    NativeCall throwing(w.a, w.a.position);
    throwing.inside_original = [&](EventSink sink) { event(sink, SelectionChange, [] { throw NativeException(); }); };
    Witness output = before;
    try { throwing.observe(output); REQUIRE(false, "native exception must propagate"); }
    catch (const NativeException &) {}
    REQUIRE(unchanged(output, before) && api().counters(w.state.storage).active == 0,
            "native exception restores embedded fence state and leaves output unchanged");
    observed(w.a);
}
void lifecycle_pins_and_reuse() {
    World w; auto before = sentinel(w.a);
    NativeCall call(w.a, w.a.position);
    call.inside_original = [&](EventSink sink) {
        event(sink, ScopeLoss, [&] {
            // Serialized revocation occurs before release and after original's
            // final root read. This is not a token closing a concurrent-free race.
            w.a.marker = 0; w.a.root.reset();
            REQUIRE(!before.cache.expired() && !before.collector.expired() && !before.mount.expired() && !before.database.expired(),
                    "actual scoped pins retain cache AND Collector pool plus Mount/DB during native callback");
        });
    };
    refused(call, before);
    REQUIRE(before.cache.expired() && before.collector.expired() && before.mount.expired() && before.database.expired() && Pool::alive == 3,
            "only weak owners survive lost callback; cache drops before Collector pool");
    w.a.marker = 1; w.a.rebuild(); observed(w.a);
    // New real cache control at an actually recycled object address.
    w.a.selected().reset(); w.a.root->collector->caches.clear(); Cache::arena_next = true;
    w.a.add(w.a.position, w.a.tag); auto old = observed(w.a); auto address = w.a.selected().get();
    w.a.selected().reset(); w.a.root->collector->caches.clear();
    REQUIRE(old.cache.expired() && !Cache::arena_live, "old actual cache destroyed before address reuse");
    Cache::arena_next = true; w.a.add(w.a.position, w.a.tag); auto fresh = observed(w.a);
    REQUIRE(w.a.selected().get() == address && !same_weak(old.cache, fresh.cache),
            "same object address with new real GNU control is a different selection owner");
}
void exact_alias_owner_and_root_aba() {
    World w; auto before = sentinel(w.a);
    struct AliasOwner { std::shared_ptr<Collector> collector; CacheOwner cache; };
    auto retained = std::make_shared<AliasOwner>(); retained->cache = w.a.selected(); retained->collector = w.a.root->collector;
    CacheOwner alias(retained, retained->cache.get());
    NativeCall changed_owner(w.a, w.a.position);
    changed_owner.inside_original = [&](EventSink) { w.a.selected() = alias; };
    refused(changed_owner, before); w.a.selected() = retained->cache;
    // Every member owner can be restored at the same root address; the positive
    // lifecycle invocation fence must still reject the former invocation.
    w.a.root.reset(); ListServer::arena_next = true; w.a.rebuild(); before = sentinel(w.a);
    auto address = w.a.root.get();
    NativeCall root_reused(w.a, w.a.position);
    root_reused.inside_original = [&](EventSink sink) {
        event(sink, ScopeLoss, [&] {
            auto mount = w.a.root->mount; auto collector = w.a.root->collector;
            auto selected = w.a.selected();
            w.a.root.reset(); REQUIRE(!ListServer::arena_live, "old complete root actually destroyed");
            ListServer::arena_next = true; w.a.rebuild();
            w.a.selected().reset(); w.a.root->collector->caches.clear();
            w.a.root->mount = mount; w.a.root->collector = collector; w.a.selected() = selected;
            REQUIRE(w.a.root.get() == address, "new complete object at exact former root address");
        });
    };
    refused(root_reused, before); observed(w.a);
}
} // namespace

int main(int argc, char **argv) {
    static_assert(sizeof(std::shared_ptr<Mount>) == 16 && sizeof(std::weak_ptr<Mount>) == 16 &&
                  sizeof(std::vector<CacheOwner>) == 24, "exact GNU owner and vector carrier widths");
    REQUIRE(argc == 2 && (std::strcmp(argv[1], "1") == 0 || std::strcmp(argv[1], "2") == 0), "separate process per supplied build profile");
    build_id = unsigned(argv[1][0] - '0');
    first_observation();
    for (auto test : {positions_and_guards, reallocation_and_actual_owners, positive_taint_and_nesting,
                      lifecycle_pins_and_reuse, exact_alias_owner_and_root_aba}) {
        test(); REQUIRE(!Cache::alive && !Pool::alive && !Cache::arena_live && !ListServer::arena_live,
                        "native fixture lifetimes end; no resolver strong owner escapes callback");
    }
    std::printf("PASS cache-selection build=%u: real GNU owners, fresh vector storage, positive invocation fences; no native hook ABI claim\n", build_id);
    return 0;
}
#endif
