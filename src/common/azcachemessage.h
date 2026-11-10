/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEMESSAGE_H
#define AZCACHEMESSAGE_H
/* Private copied request context, not a protocol/source/load identity. The
 * caller already owns the full native DataStreamMessage and its arguments
 * during capture, and the complete server for the forwarded invocation.
 * Address/bounds checks only constrain that borrow; they acquire no liveness.
 * Only canonical integer variants are observed. Native conversions are never
 * called and native message bytes/transport are never modified.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
namespace azsdk_cache_message {
struct Borrow { unsigned build; const void *server, *message; std::uint32_t parameter; };
struct Envelope {
    std::uint16_t kind = 0;
    std::uint32_t sequence = 0, parameter = 0, present = 0, fields[8] = {};
    unsigned client = 0, selector = 0; // native vector positions, never deck IDs
};
namespace detail {
inline bool canonical(std::uintptr_t p) noexcept {
    return p >= 0x10000 && p <= UINT64_C(0x0000ffffffffffff) && !(p & 7u);
}
template<class T> inline T scalar(std::uintptr_t p) noexcept {
    T result; std::memcpy(&result,reinterpret_cast<const void *>(p),sizeof result); return result;
}
inline bool capture(const Borrow &borrow, Envelope &output) noexcept {
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    (void)borrow; (void)output; return false;
#else
    if ((borrow.build != 1 && borrow.build != 2) || !borrow.server) return false;
    const auto message = reinterpret_cast<std::uintptr_t>(borrow.message);
    if (!canonical(message)) return false;
    Envelope value;
    value.kind = scalar<std::uint16_t>(message);
    if (value.kind != 0x2102 && value.kind != 0x2002 && value.kind != 0x3000) return false;
    if (scalar<std::uint32_t>(message+4) != UINT32_C(0x872349ae)) return false;
    value.sequence = scalar<std::uint32_t>(message+8);
    value.parameter = borrow.parameter;
    const auto begin = scalar<std::uintptr_t>(message+0x28);
    const auto end = scalar<std::uintptr_t>(message+0x30);
    const auto capacity = scalar<std::uintptr_t>(message+0x38);
    /* 4096 is a conservative observation quota, not a native protocol maximum.
     * At most five selected argument records are copied, irrespective of count. */
    if (!canonical(begin) || !canonical(end) || !canonical(capacity) || end < begin || capacity < end ||
        (end-begin)%48 || (capacity-begin)%48 || (end-begin)/48 > 4096 || (capacity-begin)/48 > 4096) return false;
    const auto count = (end-begin)/48;
    const unsigned required = value.kind == 0x3000 ? 4 : 2;
    if (count < required) return false;
    for (unsigned i=0;i<8;++i) {
        if (i >= required && !(value.kind == 0x3000 && i == 7 && count > 7)) continue;
        const auto argument = begin + std::uintptr_t(i)*48;
        if (scalar<std::uint32_t>(argument) != 1) return false;
        value.fields[i] = scalar<std::uint32_t>(argument+8);
        value.present |= 1u << i;
    }
    value.client = (value.fields[0] >> 24) & 255u;
    value.selector = (value.fields[0] >> 16) & 255u;
    if (value.selector < 1 || value.selector > 9) return false;
    output = value; return true;
#endif
}
struct Frame;
inline Frame *&head() noexcept { static thread_local Frame *value = nullptr; return value; }
struct Frame {
    Frame *parent;
    const void *server;
    std::uintptr_t packet_key;
    Envelope value;
    unsigned depth;
    bool valid;
    explicit Frame(const Borrow &borrow) noexcept
        : parent(head()),server(borrow.server),packet_key(reinterpret_cast<std::uintptr_t>(borrow.message)),depth(parent ? (parent->depth < 32 ? parent->depth+1 : 32) : 1),
          valid((!parent || parent->depth < 32) && capture(borrow,value)) {
        /* Even an unsupported/overflow invocation hides the older frame. No
         * depth counter wraps and no native call is suppressed by this quota. */
        head() = this;
    }
    ~Frame() { head() = parent; }
    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
};
} // namespace detail
/* The original runs exactly once, outside any observer catch boundary. Only
 * scalars survive its message destruction; no message/argument pointer is
 * retained in Frame and teardown never rereads the native input. */
inline std::uintptr_t take_packet_key(const void *server) noexcept {
    auto *frame=detail::head();
    if (!frame || !frame->valid || frame->server!=server) return 0;
    auto key=frame->packet_key;
    frame->packet_key=0;
    return key;
}
template<class F> inline void forward_owned(const Borrow &borrow,F &&original) {
    detail::Frame frame(borrow);
    std::forward<F>(original)();
}
inline bool current(const void *owned_server,Envelope &out) noexcept {
    const auto frame = detail::head();
    if (!owned_server || !frame || !frame->valid || frame->server != owned_server) return false;
    out = frame->value; return true;
}
} // namespace azsdk_cache_message
#endif
