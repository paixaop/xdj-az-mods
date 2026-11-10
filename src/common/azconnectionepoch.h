/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCONNECTIONEPOCH_H
#define AZCONNECTIONEPOCH_H

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

/* Private, embeddable bookkeeping for actually observed connection operations.
 * This installs no hooks, accepts no native profile and owns no native object.
 * Callers must prove that each closure forwards the actual native operation
 * exactly once and that its output storage remains owned through the call.
 * A receipt identifies an observed incarnation, never a sqlite3* owner/lease.
 * Connections already open before observation stay unknown. The caller's
 * native borrow/worker and post-observation validation contracts still apply.
 */
namespace azsdk_connection_epoch {
struct Receipt {
    std::uint64_t generation;
};

/* Bounds are compile-time policy, including small finite
 * bounds for meaningful exhaustion fixtures. No reset can revive old tokens.
 * Retired slots may be reused; their monotonically allocated generations cannot.
 * Embed one State in the existing source owner; no service, queue or global is
 * created. Keep that State alive for every operation and receipt that uses its
 * generation namespace. The internal mutex is never held while native
 * forwarding executes.
 */
template <std::size_t Capacity = 32,
          std::uint64_t MaxGeneration = UINT64_MAX,
          std::uint64_t MaxSequence = UINT64_MAX, unsigned MaxActive = 32>
class State {
    static_assert(Capacity > 0 && MaxGeneration > 0 && MaxSequence > 0 && MaxActive > 0,
                  "connection epoch bounds must be positive");
    struct Entry {
        const void *connection;
        std::uint64_t generation;
    };
    struct PreserveErrno {
        int value;
        PreserveErrno() noexcept : value(errno) {}
        ~PreserveErrno() noexcept { errno = value; }
    };
    struct Operation {
        State &state;
        std::uint64_t sequence = 0;
        bool tracked = false, eligible = false, status_eligible = false, close = false;
        Operation(State &s, const void *connection, bool closing) noexcept : state(s) {
            state.enter(*this, connection, closing);
        }
        ~Operation() noexcept {
            state.complete(*this, false, -1, nullptr, nullptr);
        }
        Operation(const Operation &) = delete;
        Operation &operator=(const Operation &) = delete;
    };
    mutable std::mutex mutex_;
    mutable std::atomic<bool> saturated_;
    Entry entries_[Capacity];
    Entry status_entries_[Capacity];
    std::uint64_t next_generation_ = 0, sequence_ = 0;
    std::uint64_t next_status_generation_ = 0, last_close_ = 0;
    unsigned active_ = 0, closing_ = 0;

    void retire(const void *connection) noexcept {
        if (!connection) return;
        for (auto &entry : entries_)
            if (entry.connection == connection) entry = Entry {};
        for (auto &entry : status_entries_)
            if (entry.connection == connection) entry = Entry {};
    }
    void enter(Operation &operation, const void *connection, bool closing) noexcept {
        PreserveErrno preserve;
        if (saturated_.load(std::memory_order_acquire)) return;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing) retire(connection); // before the original can run
            if (saturated_.load(std::memory_order_acquire)) return;
            if (sequence_ == MaxSequence || active_ == MaxActive) {
                saturated_.store(true, std::memory_order_release);
                return;
            }
            operation.eligible = !closing && active_ == 0;
            operation.sequence = ++sequence_;
            operation.status_eligible = !closing && !closing_;
            operation.close = closing;
            if (closing) { ++closing_; last_close_ = operation.sequence; }
            operation.tracked = true;
            ++active_;
        } catch (...) {
            /* Optional bookkeeping must not prevent actual native forwarding.
             * If synchronization fails, no receipt can be trusted again. */
            saturated_.store(true, std::memory_order_release);
        }
    }
    void complete(Operation &operation, bool offer, int result,
                  const void *connection, Receipt *receipt) noexcept {
        PreserveErrno preserve;
        if (!operation.tracked) return;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_) {
                saturated_.store(true, std::memory_order_release);
                operation.tracked = false;
                return;
            }
            bool duplicate = false;
            if (offer && connection) {
                for (auto &entry : entries_)
                    if (entry.connection == connection) {
                        /* Two births without an observed retirement are
                         * ambiguous, including a failed nonnull output. */
                        entry = Entry {};
                        duplicate = true;
                    }
            }
            if (offer && result == 0 && connection && !duplicate &&
                operation.eligible && active_ == 1 && sequence_ == operation.sequence &&
                !saturated_.load(std::memory_order_acquire)) {
                Entry *free = nullptr;
                for (auto &entry : entries_)
                    if (!entry.generation) { free = &entry; break; }
                if (free) {
                    if (next_generation_ == MaxGeneration) {
                        saturated_.store(true, std::memory_order_release);
                    } else {
                        const auto generation = ++next_generation_;
                        *free = Entry {connection, generation};
                        if (receipt) *receipt = Receipt {generation};
                    }
                }
                /* Capacity refusal never evicts another live incarnation. */
            }
            /* The traced SQLite allocator may open different libraries on independent
             * workers. Its successful distinct connection outputs are separate native
             * objects. A close overlapping this open still makes the birth ambiguous.
             * Keep this namespace separate from the stricter legacy receipt contract. */
            if (offer && connection) {
                Entry *slot = nullptr;
                bool duplicate_status = false;
                for (auto &entry : status_entries_) {
                    if (entry.connection == connection) { entry = Entry {}; duplicate_status = true; }
                    if (!entry.connection && !slot) slot = &entry;
                }
                if (slot && !duplicate_status && !result && operation.status_eligible &&
                    !closing_ && last_close_ < operation.sequence && next_status_generation_ < MaxGeneration &&
                    !saturated_.load(std::memory_order_acquire))
                    *slot = Entry {connection, ++next_status_generation_};
            }
            if (operation.close) --closing_;
            --active_;
            operation.tracked = false;
        } catch (...) {
            saturated_.store(true, std::memory_order_release);
            operation.tracked = false;
        }
    }

public:
    State() noexcept : saturated_(false), entries_{}, status_entries_{} {}
    State(const State &) = delete;
    State &operator=(const State &) = delete;

    /* The closure owns the real native argument/return contract. In particular,
     * this helper never reads a filename, VFS, database object or key. T may be
     * an incomplete native sqlite3 type; only its caller-owned output is copied.
     * Any other lifecycle entry while original is running invalidates this
     * birth, even after that nested/concurrent operation has already returned.
     */
    template <class T, class F>
    int forward_open(T **native_output, F &&original, Receipt *receipt = nullptr) {
        Operation operation(*this, nullptr, false);
        const int result = std::forward<F>(original)();
        const int native_errno = errno;
        const void *connection = native_output ? static_cast<const void *>(*native_output) : nullptr;
        complete(operation, true, result, connection, receipt);
        errno = native_errno;
        return result;
    }

    /* Retirement is unconditional at entry. BUSY, errors, deferred zombie
     * release and exceptions cannot revive the retired generation. Completion
     * touches only the specific operation counter, never a pointer lookup that
     * might now name a different allocation. */
    template <class F>
    int forward_close(const void *connection, F &&original) {
        Operation operation(*this, connection, true);
        const int result = std::forward<F>(original)();
        const int native_errno = errno;
        complete(operation, false, result, nullptr, nullptr);
        errno = native_errno;
        return result;
    }

    /* Copied scalar observation only. This cannot acquire native liveness or
     * prevent a close immediately after the observation. No read mints a birth.
     * Unknown/active/retired/saturated observations leave the caller unchanged.
     */
    bool read(const void *connection, Receipt &receipt) const noexcept {
        PreserveErrno preserve;
        if (!connection || saturated_.load(std::memory_order_acquire)) return false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ || saturated_.load(std::memory_order_acquire)) return false;
            for (const auto &entry : entries_)
                if (entry.generation && entry.connection == connection) {
                    receipt = Receipt {entry.generation};
                    return true;
                }
        } catch (...) {
            saturated_.store(true, std::memory_order_release);
        }
        return false;
    }
    /* Close revokes its own status receipt under this same lock before forwarding.
     * Other workers may open or close an unrelated connection during this read. */
    std::uint64_t status_generation(const void *connection) const noexcept {
        PreserveErrno preserve;
        if (!connection || saturated_.load(std::memory_order_acquire)) return 0;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!saturated_.load(std::memory_order_acquire))
                for (const auto &entry : status_entries_)
                    if (entry.connection == connection) return entry.generation;
        } catch (...) { saturated_.store(true, std::memory_order_release); }
        return 0;
    }
};
} // namespace azsdk_connection_epoch

#endif
