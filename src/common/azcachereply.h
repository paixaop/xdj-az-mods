/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEREPLY_H
#define AZCACHEREPLY_H
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#if !defined(__GLIBCXX__)
#error "Native reply ownership requires real GNU libstdc++ weak/shared owners"
#endif

/* Private synchronous owner handoff, not a native prototype or transport join.
 * The caller already owns/serializes the actual selected Ticket while copying
 * its genuine tag2 weak receiver and complete RequestID. Neither wire fields,
 * TLS, nor a native address acquire that ownership. No raw Ticket/packet or
 * strong Reception is retained. The 16 RequestID bytes remain opaque; they are
 * not a player LoadRequestID, TrackID, process epoch, or source association.
 *
 * At the actual Reception entry the native bridge must capture ORIGINAL X21
 * and LR before calling C++. The reviewed slot13 holds the promoted strong
 * control in X21 through its callback and releases it afterward. The caller
 * must bind return_pc to the checked exact-build/code/module call site
 * (1.30 RVA10b408c, 2.00 RVA1144490); arbitrary supplied scalars do not prove a
 * native invocation. The borrowed callback receiver is live under that native
 * lease. This helper never reads a raw control block or builds an owner from
 * raw bits; it locks only its genuinely copied weak owner during verification.
 * Scope itself is a caller-owned, immovable local and must stay alive on the
 * same serialized thread through forward(); TLS only routes nested invocations.
 */
namespace azsdk_cache_reply {
struct Request {
    std::uint64_t request_id[2] = {};
    std::uint32_t sequence = 0;
    std::uint16_t reply_kind = 0;
};
struct Promotion { std::uintptr_t control = 0, caller_link = 0; };
struct Receipt {
    Request request;
    std::weak_ptr<void> reception;
};
class Scope;
inline bool receive_current_owned(const Promotion &, const void *, Receipt &) noexcept;
inline const void *current_invocation() noexcept;

class Scope {
    // Keep the genuine original weak<T> type. GNU converting weak<T> to
    // weak<void> may lock it even in the constructor; same-type copying never
    // needs to promote it. This storage contains a real constructed weak_ptr,
    // not a weak owner fabricated from copied native representation bits.
    alignas(std::weak_ptr<void>) unsigned char reception_[16];
    void (*destroy_)(void *);
    bool (*copy_)(const void *, const void *, std::uintptr_t, std::weak_ptr<void> &);
    Request request_;
    std::uintptr_t return_pc_;
    bool used_ = false, active_ = false;
    static Scope *&current() noexcept {
        static thread_local Scope *scope = nullptr;
        return scope;
    }
    template<class T> static void destroy(void *storage) noexcept {
        typedef std::weak_ptr<T> Weak;
        reinterpret_cast<Weak *>(storage)->~Weak();
    }
    template<class T>
    static bool copy(const void *storage, const void *borrowed_receiver, std::uintptr_t control,
                     std::weak_ptr<void> &out) noexcept {
        const auto &receiver = *reinterpret_cast<const std::weak_ptr<T> *>(storage);
        const std::shared_ptr<T> lease = receiver.lock();
        if (!lease || lease.get() != borrowed_receiver) return false;
        std::uintptr_t strong_words[2];
        std::memcpy(strong_words, &lease, sizeof strong_words);
        if (strong_words[1] != control) return false;
        // This conversion uses an already-owned, live shared_ptr. It neither
        // locks a weak owner early nor constructs an owner from native bits.
        out = lease;
        return true;
    }
    class ForwardScope {
        Scope &scope_;
        Scope *previous_;
    public:
        explicit ForwardScope(Scope &scope) noexcept : scope_(scope), previous_(current()) {
            // Even an ineligible/reused nested scope masks its caller. A scope
            // is single-use for observation; every original still runs once.
            scope_.active_ = !scope_.used_;
            scope_.used_ = true;
            current() = &scope_;
        }
        ~ForwardScope() { scope_.active_ = false; current() = previous_; }
        ForwardScope(const ForwardScope &) = delete;
        ForwardScope &operator=(const ForwardScope &) = delete;
    };
    friend bool receive_current_owned(const Promotion &, const void *, Receipt &) noexcept;
    friend const void *current_invocation() noexcept;
public:
    template<class T>
    Scope(const std::weak_ptr<T> &receiver, const Request &request, std::uintptr_t return_pc) noexcept
        : destroy_(&destroy<T>), copy_(&copy<T>), request_(request), return_pc_(return_pc) {
        static_assert(sizeof(std::uintptr_t) == 8 && sizeof(std::weak_ptr<T>) == 16 &&
                      sizeof(std::weak_ptr<void>) == 16 && sizeof(std::shared_ptr<void>) == 16,
                      "reviewed 64-bit GNU native alias/control owner pair");
        static_assert(alignof(std::weak_ptr<T>) <= alignof(std::weak_ptr<void>),
                      "genuine typed GNU weak owner storage alignment");
        new (reception_) std::weak_ptr<T>(receiver);
    }
    ~Scope() { destroy_(reception_); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    Scope(Scope &&) = delete;
    Scope &operator=(Scope &&) = delete;

    template<class F>
    auto forward(F &&original) -> decltype(std::forward<F>(original)()) {
        ForwardScope active(*this);
        return std::forward<F>(original)();
    }

    bool receive_owned(const Promotion &native, const void *borrowed_receiver, Receipt &out) noexcept {
        if (current() != this || !active_ || !return_pc_ ||
            native.caller_link != return_pc_ || !native.control || !borrowed_receiver) return false;
        std::uintptr_t weak_words[2];
        std::memcpy(weak_words, reception_, sizeof weak_words);
        if (weak_words[0] != reinterpret_cast<std::uintptr_t>(borrowed_receiver) ||
            weak_words[1] != native.control) return false;

        // Native already selected and owns the callback receiver. This local
        // promotion ends on return, before the wrapper forwards its original.
        Receipt staged;
        if (!copy_(reception_, borrowed_receiver, native.control, staged.reception)) return false;
        staged.request = request_;
        out.request = staged.request;
        out.reception.swap(staged.reception);
        return true;
    }
};

inline bool receive_current_owned(const Promotion &native, const void *borrowed_receiver,
                                  Receipt &out) noexcept {
    Scope *scope = Scope::current();
    return scope && scope->receive_owned(native, borrowed_receiver, out);
}
/* SDK stack context only: this neither names nor owns a native object. Used
 * to prevent a nested Ticket (even with identical scalars/receiver) from
 * borrowing an outer callback's already-copied receipt. */
inline const void *current_invocation() noexcept { return Scope::current(); }
} // namespace azsdk_cache_reply
#endif
