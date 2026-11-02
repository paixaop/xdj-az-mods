/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aznativecommands.h"
#include "../azmod/sdk_command_engine.h"
#include <exception>
#include <thread>

namespace azsdk_native {
static_assert(ATOMIC_INT_LOCK_FREE == 2, "native admission must not use a hidden lock");
static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2,
              "native generation must not use a hidden lock");

std::atomic<uint64_t> OwnerGate::next_thread_token_(0);
uint64_t OwnerGate::thread_cookie() noexcept {
    static thread_local uint64_t cookie = 0;
    if (cookie) return cookie;
    uint64_t expected = next_thread_token_.load(std::memory_order_relaxed);
    if (expected == UINT64_MAX) return 0;
    /* A TLS address or pthread_t can be reused by a later thread. This serial
     * never wraps; one CAS keeps first-use admission bounded under contention. */
    if (!next_thread_token_.compare_exchange_strong(expected, expected + 1, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) return 0;
    cookie = expected + 1;
    return cookie;
}
OwnerGate::Lease::Lease() noexcept : gate_(nullptr), owner_(nullptr), thread_(0), generation_(0) {}
OwnerGate::Lease::~Lease() noexcept {
    /* Destroying a live lease on a foreign thread would otherwise lose the only
     * release handle and leave teardown waiting indefinitely. */
    if (gate_ && end() != AZ_OK) std::terminate();
}
void *OwnerGate::Lease::owner() const noexcept {
    return gate_ && thread_ == OwnerGate::thread_cookie() ? owner_ : nullptr;
}
uint64_t OwnerGate::Lease::generation() const noexcept {
    return owner() ? generation_ : 0;
}
int OwnerGate::Lease::end() noexcept {
    if (!gate_ || thread_ != OwnerGate::thread_cookie()) return AZ_INVALID;
    OwnerGate *gate = gate_;
    gate_ = nullptr;
    owner_ = nullptr;
    thread_ = 0;
    generation_ = 0;
    gate->holder_.store(0, std::memory_order_release);
    gate->state_.fetch_and(~unsigned(Reader), std::memory_order_release);
    return AZ_OK;
}
OwnerGate::OwnerGate() noexcept
    : state_(0), generation_(0), holder_(0), last_generation_(0), owner_(nullptr), check_(nullptr) {}
OwnerGate::~OwnerGate() noexcept {
    uint64_t current = generation();
    if (current && retire(current) != AZ_OK) std::terminate();
}
uint64_t OwnerGate::generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}
bool OwnerGate::held_by_current_thread() const noexcept {
    uint64_t thread = thread_cookie();
    return thread && holder_.load(std::memory_order_acquire) == thread;
}
int OwnerGate::publish(void *owned_object, OwnerThreadCheck check, uint64_t *generation) noexcept {
    if (!owned_object || !check || !generation) return AZ_INVALID;
    try {
        std::lock_guard<std::mutex> lock(control_);
        if (state_.load(std::memory_order_acquire)) return AZ_BUSY;
        if (last_generation_ == UINT64_MAX) return AZ_LIMIT;
        owner_ = owned_object;
        check_ = check;
        uint64_t current = ++last_generation_;
        generation_.store(current, std::memory_order_release);
        state_.store(Live, std::memory_order_release);
        *generation = current;
        return AZ_OK;
    } catch (...) {
        return AZ_FAILED;
    }
}
int OwnerGate::enter(Lease &out) noexcept {
    if (out.gate_) return AZ_BUSY;
    uint64_t thread = thread_cookie();
    if (!thread) return AZ_BUSY;
    unsigned expected = Live;
    if (!state_.compare_exchange_strong(expected, Live | Reader, std::memory_order_acquire,
                                        std::memory_order_relaxed))
        return expected & Reader ? AZ_BUSY : AZ_UNSUPPORTED;
    /* No native pointer may be read before this admission. Retirement cannot
     * change owner_/check_ until the reader bit is released. */
    holder_.store(thread, std::memory_order_release);
    out.gate_ = this;
    out.owner_ = owner_;
    out.thread_ = thread;
    out.generation_ = generation_.load(std::memory_order_acquire);
    int status = AZ_OK;
    try {
        if (!check_(owner_)) status = AZ_INVALID;
        else if (state_.load(std::memory_order_acquire) & Retiring) status = AZ_STALE;
    } catch (...) {
        status = AZ_FAILED;
    }
    if (status != AZ_OK) out.end();
    return status;
}
int OwnerGate::retire(uint64_t expected_generation) noexcept {
    if (!expected_generation) return AZ_INVALID;
    /* Do not wait for our own lease or block behind another retirer waiting for
     * it. Closing admission still prevents a new caller from entering. */
    uint64_t thread = thread_cookie();
    if (thread && holder_.load(std::memory_order_acquire) == thread) {
        if (generation() != expected_generation) return AZ_STALE;
        state_.fetch_or(Retiring, std::memory_order_acq_rel);
        return AZ_BUSY;
    }
    try {
        std::lock_guard<std::mutex> lock(control_);
        if (generation() != expected_generation) return AZ_STALE;
        state_.fetch_or(Retiring, std::memory_order_acq_rel);
        while (state_.load(std::memory_order_acquire) & Reader) std::this_thread::yield();
        owner_ = nullptr;
        check_ = nullptr;
        generation_.store(0, std::memory_order_release);
        state_.store(0, std::memory_order_release);
        return AZ_OK;
    } catch (...) {
        return AZ_FAILED;
    }
}
} // namespace azsdk_native

namespace {
int unavailable_arguments(void *, const az_command_request *, const scmd_target *) { return AZ_UNSUPPORTED; }
int unavailable_begin(void *, const az_command_request *, scmd_target *) { return AZ_UNSUPPORTED; }
int unavailable_dispatch(void *, const az_command_request *, uint64_t, scmd_completion *) { return AZ_UNSUPPORTED; }
void unavailable_end(void *, uint32_t) {}
} // namespace

extern "C" void azsdk_native_commands_adapter(scmd_adapter *out) noexcept {
    if (!out) return;
    const scmd_adapter adapter = {nullptr, unavailable_arguments, unavailable_begin, unavailable_dispatch, unavailable_end};
    *out = adapter;
}
extern "C" uint64_t azsdk_native_commands_capabilities(void) noexcept { return 0; }
