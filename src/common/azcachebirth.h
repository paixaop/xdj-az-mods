/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEBIRTH_H
#define AZCACHEBIRTH_H
#include <cstdint>
#include <memory>

/* Private C++ borrow/forward contract. This is neither a native prototype
 * nor an installed/public SDK interface. */
namespace music_library {
class MountPoint;
class SqliteDataBase;
class ListCache;
class ListCacheCollector;
}
struct azce_call;
namespace azsdk_cache_birth {
struct SourceBorrow {
    void *server;
    unsigned build;
    std::uint64_t source_epoch;
    const std::shared_ptr<music_library::MountPoint> *mount;
    const std::shared_ptr<music_library::SqliteDataBase> *database;
};
struct Forward {
    void (*call)(void *);
    void *context;
};
/* Actual owned MusicLibraryServer callback and reviewed register bridge.
 * These keep local native owner pins; neither installs a hook nor turns a
 * copied message, pointer or TLS marker into ownership. */
__attribute__((visibility("hidden"))) void forward_library_owned(unsigned, void *, Forward);
__attribute__((visibility("hidden"))) void observe_entry_owned(azce_call *, Forward);
struct BirthBorrow {
    SourceBorrow source;
    const std::shared_ptr<music_library::ListCacheCollector> *collector;
    const std::shared_ptr<music_library::SqliteDataBase> *creator_database;
    const void *receiver_collector;
    const void *receiver_creator;
    const void *borrowed_pool;
    /* The original constructs this REAL native shared_ptr in its caller's
     * result storage. It is read only after Forward returns normally. */
    const std::shared_ptr<music_library::ListCache> *native_result;
};
/* A wrapper may identify only an exact supported native factory. Primary
 * construction permits no relevant mutation. Standard fallback mode0 requires
 * setup16 followed by successful setters0..15 on its one newly owned raw result.
 * The raw result slot is live only in this same Collector invocation. */
enum FactoryKind { PrimaryFactory = 1, StandardFallbackFactory = 2 };
struct FactoryBorrow {
    const void *receiver_creator;
    unsigned kind;
    const void *borrowed_pool;
    const void *const *native_result;
};
enum WriteKind { SetupWrite = 1, LayoutWrite = 2 };
struct WriteBorrow {
    const void *receiver_cache;
    unsigned kind, argument; // setup count or incoming setter index
    const void *borrowed_pool; // setup only
    const int *native_status; // W0 copied by the original's actual wrapper
};
/* Neither function guesses native signatures or installs an entry. Wrappers
 * must preserve native results and provide only live, exact supported calls.
 * Unexpected or nested construction is forwarded but cannot publish a birth. */
__attribute__((visibility("hidden"))) void observe_factory_owned(const FactoryBorrow &, Forward);
__attribute__((visibility("hidden"))) void observe_construction_write_owned(const WriteBorrow &, Forward);
struct QueryBorrow {
    SourceBorrow source;
    const std::shared_ptr<music_library::ListCacheCollector> *collector;
    const std::shared_ptr<music_library::ListCache> *selected_cache;
    const void *receiver_cache;
};
struct CopyRowScalar {
    /* Called only after successful original forwarding, inside its owning
     * callback. This copies a scalar; it must not copy/move/own a native row
     * container, retain rows, or follow any saved pointer on a later call. */
    bool (*copy)(void *, std::uint32_t &);
    void *context;
};
struct Observation {
    std::uint64_t birth_epoch = 0, layout_generation = 0, source_epoch = 0;
    std::weak_ptr<music_library::MountPoint> mount;
    std::weak_ptr<music_library::SqliteDataBase> database;
    std::weak_ptr<music_library::ListCacheCollector> collector;
    std::weak_ptr<music_library::ListCache> cache;
    std::uint32_t row_value = 0;
    unsigned factory_kind = 0; // reviewed creation family, never a native type/deck ID
};
/* These functions preserve the original's destination, return ownership and
 * exceptions by invoking the supplied closure once. They never create/free a
 * native result object. Failure leaves Observation unchanged. Borrow fields
 * must remain valid for this actual invocation; guards do not acquire liveness.
 * In particular, a shared_ptr pin keeps its pointee alive, not the storage of
 * a borrowed shared_ptr in a mutable vector. Each holder reread requires live
 * storage, or a proved preceding mutation fence that declines before that
 * reread. A native wrapper must prove this or use stable owned snapshots with
 * fresh validation in its actual owned scope; this helper does not resolve
 * native vector elements or establish their storage lifetime.
 * Birth is ONLY for a proved fresh Collector creation, never a cache hit.
 * Query pins the actual selected cache AND its actual owning Collector only
 * until the callback returns. Only weak owners persist in SourceState.
 * Production entry points enforce the configured exact-build worker profile.
 */
__attribute__((visibility("hidden"))) int observe_birth_owned(
    const BirthBorrow &, Forward, Observation *);
__attribute__((visibility("hidden"))) int observe_query_owned(
    const QueryBorrow &, Forward, CopyRowScalar, Observation *);
/* Conservative retirement before native setup/setter/storage/lifetime work.
 * No native receiver is read here. The active guard spans normal/exceptional
 * forwarding; saturation permanently disables cache acceptance until restart.
 */
__attribute__((visibility("hidden"))) void retire_forward(Forward);
/* Fence selected-owner storage before native assignment/clear. This prevents
 * a query from rereading released vector holders, without relabeling or
 * destroying the independent birth/layout witness of a still-owned cache. */
__attribute__((visibility("hidden"))) void selection_forward(Forward);
} // namespace azsdk_cache_birth

#endif
