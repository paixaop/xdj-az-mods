/* SPDX-License-Identifier: GPL-3.0-only */
/* Private boundary. No native command backend is certified by this interface. */
#ifndef AZNATIVECOMMANDS_H
#define AZNATIVECOMMANDS_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define AZSDK_NATIVE_NOTHROW noexcept
#else
#define AZSDK_NATIVE_NOTHROW
#endif

struct scmd_adapter;
/* Fills a complete, fail-closed adapter. Its callbacks return AZ_UNSUPPORTED;
 * they never dereference a native object or claim completion. NULL is allowed. */
__attribute__((visibility("hidden"))) void azsdk_native_commands_adapter(struct scmd_adapter *out) AZSDK_NATIVE_NOTHROW;
/* Operation bits, not a promise that a native queue exists. Currently always 0. */
__attribute__((visibility("hidden"))) uint64_t azsdk_native_commands_capabilities(void) AZSDK_NATIVE_NOTHROW;

#ifdef __cplusplus
}

#include <atomic>
#include <mutex>

namespace azsdk_native {
/* A core-owned admission gate for a future verified native owner callback.
 *
 * publish's caller owns the complete object and MUST route its destruction
 * through retire(generation). Only AZ_OK from retire permits native destruction.
 * The gate itself outlives every Lease. An epoch check alone cannot pin an object.
 *
 * enter is bounded and does not lock or allocate. One admitted caller at a time
 * keeps the owner check and any following inline operation inside one lease.
 * publish/retire may lock; retire waits for a different thread's existing lease
 * and MUST NOT run in a playback/draw callback. It never cancels native work.
 *
 * This mechanism does not prove native thread affinity, source/load identity,
 * native queue lifetime, argument encodings, or completion. None is inferred
 * from successful admission. The disabled C adapter does not use this gate. */
class __attribute__((visibility("hidden"))) OwnerGate {
   public:
    typedef bool (*OwnerThreadCheck)(void *owned_object);
    class Lease {
       public:
        Lease() noexcept;
        ~Lease() noexcept;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        /* NULL/0 on the wrong thread or after end. Never retain owner() beyond end. */
        void *owner() const noexcept;
        uint64_t generation() const noexcept;
        /* Wrong-thread end leaves the lease held. Only its admitting thread ends it. */
        int end() noexcept;
       private:
        friend class OwnerGate;
        OwnerGate *gate_;
        void *owner_;
        uint64_t thread_;
        uint64_t generation_;
    };

    OwnerGate() noexcept;
    ~OwnerGate() noexcept;
    OwnerGate(const OwnerGate &) = delete;
    OwnerGate &operator=(const OwnerGate &) = delete;
    /* Output remains unchanged on failure. Generations are nonzero, never reused. */
    int publish(void *owned_object, OwnerThreadCheck check, uint64_t *generation) noexcept;
    int enter(Lease &out) noexcept;
    /* A stale generation cannot retire a replacement. Reentrant retirement closes
     * admission and returns AZ_BUSY; end the lease and retry before destroying. */
    int retire(uint64_t generation) noexcept;
    uint64_t generation() const noexcept;
    bool held_by_current_thread() const noexcept;

   private:
    enum {
        Live = 1,
        Reader = 2,
        Retiring = 4
    };
    std::atomic<unsigned> state_;
    std::atomic<uint64_t> generation_;
    std::atomic<uint64_t> holder_;
    static std::atomic<uint64_t> next_thread_token_;
    std::mutex control_;
    uint64_t last_generation_;
    void *owner_;
    OwnerThreadCheck check_;
    static uint64_t thread_cookie() noexcept;
#ifdef AZSDK_NATIVE_COMMANDS_TESTING
    friend struct OwnerGateTestAccess;
#endif
};
} // namespace azsdk_native
#endif
#undef AZSDK_NATIVE_NOTHROW
#endif
