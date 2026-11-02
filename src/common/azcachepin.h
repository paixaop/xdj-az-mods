/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEPIN_H
#define AZCACHEPIN_H
#include "azcachebirth.h"
#include "azsources.h"
#include <cstring>
#include <utility>
#if !defined(__GLIBCXX__)
#error "Native cache pins require real GNU libstdc++ shared ownership"
#endif

/* Private pre-forward owner snapshots, not native liveness acquisition.
 * The caller already owns/serializes the complete supported source scope,
 * Collector and Creator holder through every read made by the constructor.
 * Source/Collector holder references are real GNU shared_ptrs. creator_holder
 * is the actual live one-word owning raw-pointer reference, NOT a shared_ptr.
 * Its Creator contains the real DB shared_ptr at+0x30; Collector+0x40 contains
 * its actual pool pointer. No raw native receiver/holder is read after this
 * constructor, and no Creator owner is fabricated. Native callback admission,
 * exact type/build, source freshness and mutation fences remain caller duties.
 */
namespace azsdk_cache_pin {
class Pins {
    std::shared_ptr<music_library::MountPoint> mount_;
    std::shared_ptr<music_library::SqliteDataBase> database_, creator_database_;
    std::shared_ptr<music_library::ListCacheCollector> collector_;
    azsdk_cache_birth::SourceBorrow source_;
    const void *creator_ = nullptr, *pool_ = nullptr;
    int status_ = AZS_GUARD;
public:
    Pins(const azsdk_cache_birth::SourceBorrow &source,
         const std::shared_ptr<music_library::ListCacheCollector> *actual_collector,
         const void *receiver_collector, const void *creator_holder) noexcept
        : source_ {source.server, source.build, source.source_epoch, &mount_, &database_} {
        static_assert(sizeof(std::shared_ptr<music_library::SqliteDataBase>) == 16,
                      "reviewed GNU native DB shared-owner width");
        if (!source.server || !source.source_epoch || source.build < 1 || source.build > 2 ||
            !source.mount || !source.database || !actual_collector || !creator_holder) return;
        mount_ = *source.mount;
        database_ = *source.database;
        collector_ = *actual_collector;
        if (!mount_ || !database_ || !collector_ || collector_.get() != receiver_collector) return;
        std::memcpy(&creator_, creator_holder, sizeof creator_);
        const auto creator_address = reinterpret_cast<std::uintptr_t>(creator_);
        const auto collector_address = reinterpret_cast<std::uintptr_t>(collector_.get());
        if (!creator_ || creator_address > UINTPTR_MAX - 0x30 || collector_address > UINTPTR_MAX - 0x40) return;
        creator_database_ = *reinterpret_cast<const std::shared_ptr<music_library::SqliteDataBase> *>(creator_address + 0x30);
        if (!creator_database_ || creator_database_.get() != database_.get() ||
            creator_database_.owner_before(database_) || database_.owner_before(creator_database_)) return;
        std::memcpy(&pool_, reinterpret_cast<const void *>(collector_address + 0x40), sizeof pool_);
        if (pool_) status_ = AZS_OK;
    }
    Pins(const Pins &) = delete;
    Pins &operator=(const Pins &) = delete;
    Pins(Pins &&) = delete;
    Pins &operator=(Pins &&) = delete;
    bool ready() const noexcept { return status_ == AZS_OK; }
    int status() const noexcept { return status_; }
    /* native_result is the genuine original-X8 shared_ptr destination. It is
     * not read here: native forwarding must construct it before the existing
     * birth helper reads it. Every owner-holder pointer below belongs to this
     * immovable local object. The two raw receiver aliases/pool are comparison
     * scalars only, never post-native dereference permissions.
     * The Borrow and all snapshots end with Pins; nothing may retain them.
     */
    azsdk_cache_birth::BirthBorrow borrow(
        const std::shared_ptr<music_library::ListCache> *native_result) const noexcept {
        return {source_, &collector_, &creator_database_, collector_.get(), creator_, pool_, native_result};
    }
};

/* Observe synchronously invokes the existing birth implementation with its
 * already validated Build profile and the supplied original exactly once.
 * It must neither retain Borrow nor catch/translate native exceptions. The
 * helper owns no result carrier/cache; normal native cache/pool lifetime after
 * return remains with the caller. No SDK lock spans the original here.
 */
template<class Observe>
int forward_owned(const azsdk_cache_birth::SourceBorrow &source,
                  const std::shared_ptr<music_library::ListCacheCollector> *actual_collector,
                  const void *receiver_collector, const void *creator_holder,
                  const std::shared_ptr<music_library::ListCache> *native_result,
                  azsdk_cache_birth::Forward original, Observe &&observe) {
    if (!original.call) return AZS_INVALID;
    if (!native_result) { original.call(original.context); return AZS_GUARD; }
    Pins pins(source, actual_collector, receiver_collector, creator_holder);
    if (!pins.ready()) { original.call(original.context); return pins.status(); }
    const auto borrow = pins.borrow(native_result);
    return std::forward<Observe>(observe)(borrow, original);
}
} // namespace azsdk_cache_pin
#endif
