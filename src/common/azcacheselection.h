/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHESELECTION_H
#define AZCACHESELECTION_H
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#if !defined(__GLIBCXX__)
#error "Native selected-owner resolution requires GNU libstdc++ shared ownership"
#endif

/* Private GNU-owner resolver, not a native entry prototype or public SDK API.
 * The caller ALREADY owns the complete native object and serializes its member
 * access for the actual callback. Readiness/epochs do not acquire that lifetime
 * or close a concurrent free-after-check window. State is embedded/caller-owned
 * (for example in existing SourceState), never a separate registry/service.
 * Atomic bookkeeping permits conservative shared fencing; it does not make a
 * native root/vector concurrently readable or make native data races safe.
 */
namespace azsdk_cache_selection {
enum Status { Ok = 0, Unavailable = 1, Guard = 2, Changed = 3, Limit = 4 };
enum Event { SelectionChange = 1, ScopeLoss = 2 };
struct Position { std::uint32_t outer, selector; };
struct Borrow {
    unsigned build;
    const void *root;
    void *context;
    /* Must inspect caller-owned scope state, not follow an unowned root.
     * It certifies the actual supported native layout/worker and serialization.
     * The mount proof is called only with a genuine scoped Mount owner pinned.
     * Proof callbacks are read-only. Any synchronous native mutation/release
     * instead requires a positive Mutation fence before it can occur. */
    bool (*already_owned)(void *);
    bool (*supported_sqlite_mount)(void *, const void *);
};
class Invocation;
class Mutation;
class State {
    static constexpr unsigned max_depth = 32;
    const unsigned limit_;
    std::atomic<unsigned> active_ {0}, mutating_ {0};
    std::atomic<std::uint64_t> revision_ {1}, next_ {0};
    std::atomic<bool> saturated_ {false};
    friend class Invocation;
    friend class Mutation;
    bool stamp(std::uint64_t &revision) const noexcept {
        if (saturated_.load(std::memory_order_acquire) || mutating_.load(std::memory_order_acquire)) return false;
        revision = revision_.load(std::memory_order_acquire);
        return revision && !mutating_.load(std::memory_order_acquire) &&
               !saturated_.load(std::memory_order_acquire) &&
               revision_.load(std::memory_order_acquire) == revision;
    }
    bool stamp_is(std::uint64_t revision) const noexcept {
        std::uint64_t current = 0;
        return stamp(current) && current == revision;
    }
    bool enter(std::uint64_t &id, std::uint64_t &revision) noexcept {
        if (!stamp(revision)) return false;
        unsigned active = active_.load(std::memory_order_acquire);
        while (active < limit_) {
            if (active_.compare_exchange_weak(active, active + 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                auto next = next_.load(std::memory_order_acquire);
                while (next != UINT64_MAX) {
                    if (next_.compare_exchange_weak(next, next + 1, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                        id = next + 1;
                        if (stamp_is(revision)) return true;
                        active_.fetch_sub(1, std::memory_order_release);
                        return false;
                    }
                }
                saturated_.store(true, std::memory_order_release);
                active_.fetch_sub(1, std::memory_order_release);
                return false;
            }
        }
        saturated_.store(true, std::memory_order_release);
        return false;
    }
public:
    explicit State(unsigned depth = max_depth) noexcept : limit_(depth) {
        if (!depth || depth > max_depth) saturated_.store(true, std::memory_order_relaxed);
    }
    State(const State &) = delete;
    State &operator=(const State &) = delete;
    unsigned active() const noexcept { return active_.load(std::memory_order_acquire); }
    bool saturated() const noexcept { return saturated_.load(std::memory_order_acquire); }
#if defined(AZSDK_CACHE_SELECTION_TESTING)
    struct TestingSeed { unsigned depth; std::uint64_t revision, next; };
    explicit State(TestingSeed seed) noexcept : State(seed.depth) {
        revision_.store(seed.revision, std::memory_order_relaxed);
        next_.store(seed.next, std::memory_order_relaxed);
        if (!seed.revision) saturated_.store(true, std::memory_order_relaxed);
    }
#endif
};
class Invocation {
    State *state_;
    std::uint64_t id_ = 0, revision_ = 0;
    bool entered_ = false;
public:
    explicit Invocation(State &state) noexcept : state_(&state) { entered_ = state.enter(id_, revision_); }
    ~Invocation() { if (entered_) state_->active_.fetch_sub(1, std::memory_order_release); }
    Invocation(const Invocation &) = delete;
    Invocation &operator=(const Invocation &) = delete;
    bool ready() const noexcept { return entered_ && state_->stamp_is(revision_); }
    const State *state() const noexcept { return state_; }
    std::uint64_t id() const noexcept { return entered_ ? id_ : 0; }
    std::uint64_t revision() const noexcept { return revision_; }
};
/* Positive fence BEFORE selection-storage changes or synchronous scope loss.
 * Retain through actual native mutation/normal return/unwind. A changed-back
 * owner remains rejected in every invocation that preceded this fence. New
 * invocations still require independent actual ownership/serialization proof.
 */
class Mutation {
    State *state_;
    bool entered_ = false;
public:
    Mutation(State &state, unsigned event) noexcept : state_(&state) {
        if (event != SelectionChange && event != ScopeLoss) state.saturated_.store(true, std::memory_order_release);
        unsigned depth = state.mutating_.load(std::memory_order_acquire);
        while (depth < state.limit_) {
            if (state.mutating_.compare_exchange_weak(depth, depth + 1, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) { entered_ = true; break; }
        }
        if (!entered_) state.saturated_.store(true, std::memory_order_release);
        auto revision = state.revision_.load(std::memory_order_acquire);
        for (;;) {
            if (revision == UINT64_MAX) { state.saturated_.store(true, std::memory_order_release); break; }
            if (state.revision_.compare_exchange_weak(revision, revision + 1, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) break;
        }
    }
    ~Mutation() { if (entered_) state_->mutating_.fetch_sub(1, std::memory_order_release); }
    Mutation(const Mutation &) = delete;
    Mutation &operator=(const Mutation &) = delete;
};
namespace detail {
inline bool add(std::uintptr_t base, std::uintptr_t offset, std::uintptr_t &out) noexcept {
    if (offset > UINTPTR_MAX - base) return false;
    out = base + offset; return true;
}
struct VectorView { std::uintptr_t begin, end, capacity; };
static_assert(sizeof(std::uintptr_t) == 8 && sizeof(VectorView) == 24, "reviewed native 64-bit GNU vector prefix");
inline bool count(const VectorView &view, std::uintptr_t stride, std::uintptr_t &out) noexcept {
    if (!stride) return false;
    if (!view.begin) { out = 0; return !view.end && !view.capacity; }
    if ((view.begin | view.end | view.capacity) & 7u || view.end < view.begin || view.capacity < view.end ||
        (view.end - view.begin) % stride || (view.capacity - view.begin) % stride) return false;
    out = (view.end - view.begin) / stride; return true;
}
inline VectorView view(std::uintptr_t address) noexcept {
    VectorView result; std::memcpy(&result, reinterpret_cast<const void *>(address), sizeof result); return result;
}
template<class T> std::shared_ptr<T> owner(std::uintptr_t address) noexcept {
    /* This is a genuine C++ strong copy from the actual live GNU shared_ptr
     * field, never memcpy of ownership words or owner construction from self. */
    return *reinterpret_cast<const std::shared_ptr<T> *>(address);
}
template<class T> bool same(const std::shared_ptr<T> &a, const std::shared_ptr<T> &b) noexcept {
    return a && b && a.get() == b.get() && !a.owner_before(b) && !b.owner_before(a);
}
inline bool allowed(const Invocation &scope, const Borrow &borrow) noexcept {
    /* Fence readiness is deliberately first: after scope loss do not even ask
     * a native-accessing validator to inspect formerly owned root storage. */
    if (!scope.ready() || (borrow.build != 1 && borrow.build != 2) || !borrow.root ||
        (reinterpret_cast<std::uintptr_t>(borrow.root) & 7u) || !borrow.already_owned ||
        !borrow.supported_sqlite_mount) return false;
    try { return borrow.already_owned(borrow.context) && scope.ready(); }
    catch (...) { return false; }
}
} // namespace detail
template<class Mount, class Database, class Collector, class Cache>
class Pins {
    /* Reverse destruction is mandatory: cache before Collector/pool, then DB
     * and Mount. Default memberwise assignment would not preserve this order. */
    std::shared_ptr<Mount> mount_;
    std::shared_ptr<Database> database_;
    std::shared_ptr<Collector> collector_;
    std::shared_ptr<Cache> cache_;
public:
    Pins() noexcept = default;
    Pins(std::shared_ptr<Mount> mount, std::shared_ptr<Database> database,
         std::shared_ptr<Collector> collector, std::shared_ptr<Cache> cache) noexcept
        : mount_(std::move(mount)), database_(std::move(database)), collector_(std::move(collector)), cache_(std::move(cache)) {}
    Pins(Pins &&) noexcept = default;
    Pins(const Pins &) = delete;
    Pins &operator=(const Pins &) = delete;
    Pins &operator=(Pins &&) = delete;
    const std::shared_ptr<Mount> &mount() const noexcept { return mount_; }
    const std::shared_ptr<Database> &database() const noexcept { return database_; }
    const std::shared_ptr<Collector> &collector() const noexcept { return collector_; }
    const std::shared_ptr<Cache> &cache() const noexcept { return cache_; }
};
template<class Mount, class Database, class Collector, class Cache>
struct Resolution {
    int status;
    const State *state = nullptr;
    std::uint64_t invocation = 0, revision = 0;
    const void *root = nullptr;
    Position position {0, 0};
    Pins<Mount, Database, Collector, Cache> pins;
    explicit Resolution(int value) noexcept : status(value) {}
    Resolution(const Invocation &scope, const Borrow &borrow, Position selected,
               Pins<Mount, Database, Collector, Cache> owners) noexcept
        : status(Ok), state(scope.state()), invocation(scope.id()), revision(scope.revision()), root(borrow.root),
          position(selected), pins(std::move(owners)) {}
    Resolution(Resolution &&) noexcept = default;
    Resolution(const Resolution &) = delete;
    Resolution &operator=(const Resolution &) = delete;
    Resolution &operator=(Resolution &&) = delete;
};
/* No holder pointer survives this function. Actual ListServer/Mount complete
 * object lifetime and serialization must already cover each read. A valid
 * vector shape is not proof that fabricated/unowned memory may be followed.
 * Returned pins must stay inside the actual owned callback; only weak/copy
 * evidence may be retained afterwards. No source epoch/tuple registry lives here.
 */
template<class Mount, class Database, class Collector, class Cache>
Resolution<Mount, Database, Collector, Cache> resolve_owned(const Invocation &scope, const Borrow &borrow,
                                                           Position position) noexcept {
    using Result = Resolution<Mount, Database, Collector, Cache>;
    static_assert(sizeof(std::shared_ptr<Mount>) == 16 && sizeof(std::weak_ptr<Mount>) == 16,
                  "reviewed native GNU two-word shared/weak ABI");
    if (!detail::allowed(scope, borrow)) return Result(Guard);
    if (position.selector < 1 || position.selector > 9) return Result(Unavailable);
    std::uintptr_t mount_address = 0, collector_address = 0, outer_address = 0;
    const auto root = reinterpret_cast<std::uintptr_t>(borrow.root);
    if (!detail::add(root, 0x28, mount_address) || !detail::add(root, 0x40, collector_address) ||
        !detail::add(root, 0x50, outer_address)) return Result(Guard);
    std::shared_ptr<Mount> mount;
    std::shared_ptr<Database> database;
    std::shared_ptr<Collector> collector;
    std::shared_ptr<Cache> cache;
    mount = detail::owner<Mount>(mount_address);
    collector = detail::owner<Collector>(collector_address);
    if (!mount || !collector || !scope.ready()) return Result(Unavailable);
    try { if (!borrow.supported_sqlite_mount(borrow.context, mount.get())) return Result(Guard); }
    catch (...) { return Result(Guard); }
    if (!detail::allowed(scope, borrow)) return Result(Changed);
    std::uintptr_t database_address = 0;
    if (!detail::add(reinterpret_cast<std::uintptr_t>(mount.get()), 0x30, database_address)) return Result(Guard);
    database = detail::owner<Database>(database_address);
    const auto outer = detail::view(outer_address);
    std::uintptr_t outer_count = 0;
    if (!detail::count(outer, 24, outer_count) || position.outer >= outer_count) return Result(Unavailable);
    std::uintptr_t inner_address = 0;
    if (position.outer > (UINTPTR_MAX - outer.begin) / 24 ||
        !detail::add(outer.begin, std::uintptr_t(position.outer) * 24, inner_address)) return Result(Guard);
    const auto inner = detail::view(inner_address);
    std::uintptr_t inner_count = 0;
    // Both reviewed native selector tables are exactly 0,1,...,8.
    const std::uintptr_t slot = position.selector - 1;
    if (!detail::count(inner, 16, inner_count) || slot >= inner_count) return Result(Unavailable);
    std::uintptr_t cache_address = 0;
    if (slot > (UINTPTR_MAX - inner.begin) / 16 || !detail::add(inner.begin, slot * 16, cache_address)) return Result(Guard);
    cache = detail::owner<Cache>(cache_address);
    if (!database || !cache || !detail::allowed(scope, borrow)) return Result(Changed);
    return Result(scope, borrow, position,
                  Pins<Mount, Database, Collector, Cache>(std::move(mount), std::move(database),
                                                        std::move(collector), std::move(cache)));
}
template<class Mount, class Database, class Collector, class Cache>
bool validate_fresh(const Invocation &scope, const Borrow &borrow, Position position,
                    const Resolution<Mount, Database, Collector, Cache> &selected) noexcept {
    if (selected.status != Ok || selected.state != scope.state() || selected.invocation != scope.id() ||
        selected.revision != scope.revision() || selected.root != borrow.root ||
        selected.position.outer != position.outer || selected.position.selector != position.selector ||
        !detail::allowed(scope, borrow)) return false;
    auto fresh = resolve_owned<Mount, Database, Collector, Cache>(scope, borrow, position);
    return fresh.status == Ok && detail::same(selected.pins.mount(), fresh.pins.mount()) &&
           detail::same(selected.pins.database(), fresh.pins.database()) &&
           detail::same(selected.pins.collector(), fresh.pins.collector()) &&
           detail::same(selected.pins.cache(), fresh.pins.cache()) && detail::allowed(scope, borrow);
}
} // namespace azsdk_cache_selection
#endif
