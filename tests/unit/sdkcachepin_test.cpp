/* SPDX-License-Identifier: GPL-3.0-only */
/* GNU ownership and reviewed prefix fixtures only. No native source registry,
 * module/worker admission, installed entry, or public source/load assertion. */
#include "../../src/common/azcachebirth.h"
#include "../../src/common/azsources.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>
#if !defined(__GLIBCXX__)
#error "Pin ownership fixture requires real GNU libstdc++"
#endif
#if defined(__has_include)
#if __has_include("../../src/common/azcachepin.h")
#include "../../src/common/azcachepin.h"
#define HAVE_CACHE_PIN 1
#endif
#endif
#define CHECK(x, why) do { if (!(x)) { std::fprintf(stderr, "FAIL cache-pin: %s\n", why); std::abort(); } } while (0)
namespace fixture {
unsigned pools, collectors, caches, creators, databases, mounts;
std::vector<char> cleanup;
struct Pool { Pool() { ++pools; } ~Pool() { --pools; cleanup.push_back('P'); } };
}
namespace music_library {
class SqliteDataBase {
public:
    explicit SqliteDataBase(unsigned id) : identity(id) { ++fixture::databases; }
    ~SqliteDataBase() { --fixture::databases; }
    unsigned identity;
};
class MountPoint {
public:
    MountPoint() { ++fixture::mounts; }
    ~MountPoint() { --fixture::mounts; }
};
class ListCacheCollector {
public:
    virtual ~ListCacheCollector() { --fixture::collectors; fixture::cleanup.push_back('C'); }
    unsigned char prefix[0x40 - 8] = {};
    std::unique_ptr<fixture::Pool> pool;
    ListCacheCollector() : pool(new fixture::Pool) { ++fixture::collectors; }
};
class ListCache {
    fixture::Pool *pool_;
public:
    explicit ListCache(fixture::Pool *pool) : pool_(pool) { ++fixture::caches; }
    ~ListCache() {
        CHECK(!pool_ || fixture::pools, "native caller cleans cache while its pool still lives");
        --fixture::caches; fixture::cleanup.push_back('R');
    }
};
}
namespace {
namespace cb = azsdk_cache_birth;
using music_library::MountPoint;
using music_library::SqliteDataBase;
using music_library::ListCacheCollector;
using music_library::ListCache;
using Owner = std::shared_ptr<ListCache>;
struct Creator {
    virtual ~Creator() { --fixture::creators; }
    unsigned char prefix[0x30 - 8] = {};
    std::shared_ptr<SqliteDataBase> database;
    explicit Creator(const std::shared_ptr<SqliteDataBase> &db) : database(db) { ++fixture::creators; }
};
struct Context {
    int server;
    unsigned build;
    std::uint64_t epoch;
    std::unique_ptr<std::shared_ptr<MountPoint>> mount;
    std::unique_ptr<std::shared_ptr<SqliteDataBase>> database;
    std::unique_ptr<std::shared_ptr<ListCacheCollector>> collector;
    std::unique_ptr<std::unique_ptr<Creator>> creator;
    Context(unsigned profile, unsigned index) : server(int(index)), build(profile), epoch(100 + index),
        mount(new std::shared_ptr<MountPoint>(new MountPoint)),
        database(new std::shared_ptr<SqliteDataBase>(new SqliteDataBase(index))),
        collector(new std::shared_ptr<ListCacheCollector>(new ListCacheCollector)),
        creator(new std::unique_ptr<Creator>(new Creator(*database))) {
        CHECK(reinterpret_cast<const char *>(&(*collector)->pool) - reinterpret_cast<const char *>(collector->get()) == 0x40 &&
              reinterpret_cast<const char *>(&(*creator)->database) - reinterpret_cast<const char *>(creator->get()) == 0x30,
              "reviewed Collector pool and Creator DB fixture prefixes");
    }
    cb::SourceBorrow source() const { return {const_cast<int *>(&server), build, epoch, mount.get(), database.get()}; }
    void release_holders() { creator.reset(); database.reset(); mount.reset(); collector.reset(); }
};
template<class T> bool same(const std::shared_ptr<T> &a, const std::shared_ptr<T> &b) {
    return a.get() == b.get() && !a.owner_before(b) && !b.owner_before(a);
}
struct Result {
    std::uint64_t before = UINT64_C(0xcafed00d);
    alignas(Owner) unsigned char storage[sizeof(Owner)];
    std::uint64_t after = UINT64_C(0xabcdef01);
    bool initialized = false;
    Owner &owner() { return *reinterpret_cast<Owner *>(storage); }
    void clear() { if (initialized) { owner().~shared_ptr(); initialized = false; } }
    ~Result() { clear(); }
    void guards() { CHECK(before == UINT64_C(0xcafed00d) && after == UINT64_C(0xabcdef01), "genuine result guards unchanged"); }
};
void invoke(void *context) { (*static_cast<std::function<void()> *>(context))(); }
template<class Observe>
int forward(const cb::SourceBorrow &source, const std::shared_ptr<ListCacheCollector> *collector,
            const void *receiver, const void *creator, Result &result, cb::Forward original, Observe observe,
            bool missing_result = false) {
#if defined(HAVE_CACHE_PIN)
    return azsdk_cache_pin::forward_owned(source, collector, receiver, creator,
        missing_result ? nullptr : reinterpret_cast<const Owner *>(result.storage), original, observe);
#else
    // Missing helper still forwards real native-shaped work once, without a
    // pin registry, guessed reads, copied owners or successful observation.
    (void)source; (void)collector; (void)receiver; (void)creator; (void)result; (void)observe; (void)missing_result;
    if (!original.call) return AZS_INVALID;
    original.call(original.context); return AZS_UNSUPPORTED;
#endif
}
void native_baseline() {
    Context context(1, 0); Result result; unsigned originals = 0;
    auto original = [&] { ++originals; new (result.storage) Owner(new ListCache((*context.collector)->pool.get())); result.initialized = true; };
    original(); result.guards();
    CHECK(originals == 1 && result.owner().use_count() == 1 && fixture::caches == 1, "actual caller-owned GNU result baseline");
    result.clear(); CHECK(!fixture::caches, "only native result caller destroys returned owner");
    std::puts("NATIVE_GNU_PIN_BASELINE_OK owner16=16 creator_holder8=8"); std::fflush(stdout);
}
void pinned(unsigned build, unsigned index, bool release, bool replace_pool, bool throwing) {
    Context context(build, index); Result result; unsigned originals = 0, observed = 0;
    const auto cleanup_begin = fixture::cleanup.size();
    const auto source = context.source();
    const auto receiver = context.collector->get();
    const auto creator = context.creator->get();
    const auto pool = receiver->pool.get();
    const std::weak_ptr<MountPoint> mount = *context.mount;
    const std::weak_ptr<SqliteDataBase> database = *context.database;
    const std::weak_ptr<ListCacheCollector> collector = *context.collector;
    std::unique_ptr<fixture::Pool> moved_pool;
    std::function<void()> original = [&] {
        ++originals;
        if (throwing) { if (release) context.release_holders(); throw std::runtime_error("native pin exact exception"); }
        new (result.storage) Owner(new ListCache(pool)); result.initialized = true;
        if (replace_pool) { moved_pool = std::move(receiver->pool); receiver->pool.reset(new fixture::Pool); }
        if (release) context.release_holders();
        if (replace_pool) {
            // New, genuinely owned holders may reuse allocator addresses. The
            // adapter must keep the old control identities, never reread them.
            context.mount.reset(new std::shared_ptr<MountPoint>(new MountPoint));
            context.database.reset(new std::shared_ptr<SqliteDataBase>(new SqliteDataBase(index + 100)));
            context.collector.reset(new std::shared_ptr<ListCacheCollector>(new ListCacheCollector));
            context.creator.reset(new std::unique_ptr<Creator>(new Creator(*context.database)));
        }
    };
    bool caught = false; int status = AZS_UNSUPPORTED;
    auto observer = [&](const cb::BirthBorrow &borrow, cb::Forward work) {
        ++observed;
        CHECK(borrow.source.server == source.server && borrow.source.build == build && borrow.source.source_epoch == source.source_epoch,
              "source callback/profile/epoch scalars preserved without admission claim");
        CHECK(borrow.source.mount != source.mount && borrow.source.database != source.database &&
              borrow.collector != context.collector.get() && borrow.creator_database != &creator->database,
              "all owner holder storage is local before native forwarding");
        CHECK(same(*borrow.source.mount, mount.lock()) && same(*borrow.source.database, database.lock()) &&
              same(*borrow.creator_database, database.lock()) && same(*borrow.collector, collector.lock()) &&
              borrow.receiver_collector == receiver && borrow.receiver_creator == creator && borrow.borrowed_pool == pool &&
              borrow.native_result == reinterpret_cast<const Owner *>(result.storage), "exact real owner aliases and original result destination");
        work.call(work.context);
        result.guards();
        CHECK(!mount.expired() && !database.expired() && !collector.expired() &&
              borrow.source.mount->get() == mount.lock().get() && borrow.creator_database->get() == database.lock().get() &&
              borrow.collector->get() == receiver && borrow.borrowed_pool == pool && *borrow.native_result,
              "local snapshots survive released/replaced native holders without post-native raw reads");
        result.clear(); // caller cleanup, while actual observer callback owns its scoped pins
        return AZS_OK;
    };
    try { status = forward(source, context.collector.get(), receiver, context.creator.get(), result, {invoke, &original}, observer); }
    catch (const std::runtime_error &e) { caught = std::strcmp(e.what(), "native pin exact exception") == 0; }
    result.guards();
    CHECK(originals == 1 && observed == 1 && (throwing ? caught : status == AZS_OK),
          "missing observed stable cache/source ownership pins");
    CHECK(caught == throwing && !result.initialized, "native exception/result cleanup is untouched");
    if (release) CHECK(mount.expired() && database.expired() && collector.expired(), "adapter retains no strong owners after callback");
    if (release && !throwing) {
        std::size_t row = fixture::cleanup.size(), collect = fixture::cleanup.size();
        for (auto i = cleanup_begin; i < fixture::cleanup.size(); ++i) {
            if (fixture::cleanup[i] == 'R' && row == fixture::cleanup.size()) row = i;
            if (fixture::cleanup[i] == 'C' && collect == fixture::cleanup.size()) collect = i;
        }
        CHECK(row < collect && collect < fixture::cleanup.size(), "caller result cleanup precedes last Collector/pool pin release");
    }
}
void refused(unsigned kind) {
    Context context(1, kind + 20), other(2, kind + 40); Result result; auto source = context.source();
    const std::shared_ptr<ListCacheCollector> *collector = context.collector.get();
    const void *receiver = context.collector->get(), *creator = context.creator.get();
    switch (kind) {
    case 0: source.mount = nullptr; break;
    case 1: source.database = nullptr; break;
    case 2: collector = nullptr; break;
    case 3: receiver = other.collector->get(); break;
    case 4: creator = nullptr; break;
    case 5: context.creator->reset(); break;
    case 6: (*context.creator)->database.reset(); break;
    case 7: (*context.creator)->database = *other.database; break;
    case 8: (*context.creator)->database = std::shared_ptr<SqliteDataBase>(*context.database, other.database->get()); break;
    case 9: (*context.creator)->database = std::shared_ptr<SqliteDataBase>(context.database->get(), [](SqliteDataBase *) {}); break;
    case 10: (*context.collector)->pool.reset(); break;
    case 11: source.source_epoch = 0; break;
    case 12: source.server = nullptr; break;
    case 13: context.collector->reset(); break;
    case 14: context.mount->reset(); break;
    case 15: context.database->reset(); break;
    case 16: break; // genuine native output exists; observer result input deliberately absent
    }
    unsigned originals = 0, observed = 0;
    std::function<void()> original = [&] { ++originals; new (result.storage) Owner(new ListCache(nullptr)); result.initialized = true; };
    int status = forward(source, collector, receiver, creator, result, {invoke, &original},
        [&](const cb::BirthBorrow &, cb::Forward) { ++observed; return AZS_OK; }, kind == 16);
    result.guards();
    CHECK(status != AZS_OK && originals == 1 && !observed && result.owner().use_count() == 1,
          "null/mismatched real inputs decline observation while native result forwards once");
    result.clear();
}
void nested(unsigned build) {
    Context outer(build, 70), inner(build, 71); Result a, b; unsigned originals = 0, observed = 0;
    std::function<void()> inner_original = [&] { ++originals; new (b.storage) Owner(new ListCache((*inner.collector)->pool.get())); b.initialized = true; };
    std::function<void()> outer_original = [&] {
        ++originals;
        int status = forward(inner.source(), inner.collector.get(), inner.collector->get(), inner.creator.get(), b, {invoke, &inner_original},
            [&](const cb::BirthBorrow &borrow, cb::Forward work) {
                ++observed; CHECK(borrow.source.source_epoch == inner.epoch, "nested source is independent");
                work.call(work.context); b.clear(); return AZS_OK;
            });
        CHECK(status == AZS_OK, "nested helper forwarded actual native result");
        new (a.storage) Owner(new ListCache((*outer.collector)->pool.get())); a.initialized = true;
    };
    int status = forward(outer.source(), outer.collector.get(), outer.collector->get(), outer.creator.get(), a, {invoke, &outer_original},
        [&](const cb::BirthBorrow &borrow, cb::Forward work) {
            ++observed; auto database = *borrow.creator_database;
            work.call(work.context);
            CHECK(borrow.source.source_epoch == outer.epoch && same(*borrow.creator_database, database), "reentry never overwrites outer local holders");
            a.clear(); return AZS_OK;
        });
    CHECK(status == AZS_OK && originals == 2 && observed == 2, "independent nested forwarding calls exactly once");
}
void no_original() {
    Context context(1, 90); Result result; unsigned observed = 0;
    int status = forward(context.source(), context.collector.get(), context.collector->get(), context.creator.get(), result,
        {nullptr, nullptr}, [&](const cb::BirthBorrow &, cb::Forward) { ++observed; return AZS_OK; });
    CHECK(status == AZS_INVALID && !observed && !result.initialized, "null original cannot create observed work or touch output");
    result.guards();
}
}
int main() {
    static_assert(sizeof(Owner) == 16 && sizeof(std::unique_ptr<Creator>) == 8, "genuine GNU owner/Creator-holder widths");
#if defined(HAVE_CACHE_PIN)
    static_assert(!std::is_copy_constructible<azsdk_cache_pin::Pins>::value &&
                  !std::is_move_constructible<azsdk_cache_pin::Pins>::value, "local borrow holder addresses cannot move");
#endif
    native_baseline();
    for (unsigned build : {1u, 2u}) for (unsigned context = 0; context != 4; ++context) {
        pinned(build, context, false, false, false);
        pinned(build, context, true, false, false);
        pinned(build, context, true, true, false);
        pinned(build, context, true, false, true);
    }
    for (unsigned kind = 0; kind != 17; ++kind) refused(kind);
    nested(1); nested(2); no_original();
    CHECK(!fixture::pools && !fixture::collectors && !fixture::caches && !fixture::creators &&
          !fixture::databases && !fixture::mounts, "no pin owner/result leaks");
    std::puts("PASS cache-pin: real GNU local owners; holder replacement, raw result lifetime, exceptions and reentry; four contexts/both profiles");
}
