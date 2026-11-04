/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic GNU ELF/weak_ptr model; no firmware or native device is executed. */
#include "azliveobserver.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#if !defined(__linux__)
int main() {
    std::puts("SKIP: live observer fixture requires GNU Linux ELF main-image guards");
    return 77;
}
#else
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            std::fprintf(stderr, "FAIL live: %s:%d: %s\n", __FILE__, __LINE__, #c); \
            std::abort();                                                           \
        }                                                                           \
    } while (0)
static std::atomic<int> allocation_failure {-1};
void *operator new(std::size_t bytes) {
    int count = allocation_failure.load();
    while (count >= 0) {
        if (!count) throw std::bad_alloc();
        if (allocation_failure.compare_exchange_weak(count, count - 1)) break;
    }
    void *p = std::malloc(bytes ? bytes : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
/* Every replaceable C++11 allocation form shares the same failure budget. GNU7 forces
 * the whole static sanitizer archive (including strong new/delete definitions), so this
 * allocator fixture uses isolated shared sanitizer runtimes for its sanitizer passes. */
void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept {
    try {
        return ::operator new(bytes);
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t bytes, const std::nothrow_t &) noexcept {
    try {
        return ::operator new[](bytes);
    } catch (...) {
        return nullptr;
    }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { ::operator delete(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { ::operator delete[](p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static void allocator_contract() {
    for (unsigned array = 0; array < 2; ++array) {
        allocation_failure.store(0);
        void *failed = array ? ::operator new[](17, std::nothrow) : ::operator new(17, std::nothrow);
        CHECK(!failed && allocation_failure.load() == 0);
        allocation_failure.store(1);
        void *first = array ? ::operator new[](17, std::nothrow) : ::operator new(17, std::nothrow);
        CHECK(first && allocation_failure.load() == 0);
        std::memset(first, 0xa5, 17);
        void *next = array ? ::operator new(17, std::nothrow) : ::operator new[](17, std::nothrow);
        CHECK(!next && allocation_failure.load() == 0);
        if (array) ::operator delete[](first, std::nothrow);
        else ::operator delete(first, std::nothrow);
        allocation_failure.store(-1);
        void *zero = array ? ::operator new[](0, std::nothrow) : ::operator new(0, std::nothrow);
        CHECK(zero);
        if (array) ::operator delete[](zero, std::nothrow);
        else ::operator delete(zero, std::nothrow);
    }
}
/* Deliberately only three bytes: any larger play read must fail ASan. */
struct Play {
    unsigned char bytes[3];
};
class ListenerMirror {
   public:
    virtual ~ListenerMirror() {}
    virtual void callback2(const Play &) = 0;
    virtual void callback3(const Play &, unsigned) = 0;
};
namespace dj_player {
class StateNotificationServer {
   public:
    virtual ~StateNotificationServer() {}
    virtual void sync(std::weak_ptr<ListenerMirror> value) { subscribe(3, value); }
    virtual void reserved3(std::weak_ptr<ListenerMirror>) {
        std::fputs("FAIL live: native Sync registration slot 3\n", stderr);
        std::fflush(stderr);
        std::abort();
    }
    virtual void reserved4() {}
    virtual void tempo_slider_lock(std::weak_ptr<ListenerMirror> value) { subscribe(4, value); }
    virtual void reserved6() {}
    virtual void reserved7() {}
    virtual void reserved8() {}
    virtual void reserved9() {}
    virtual void reserved10() {}
    virtual void reserved11() {}
    virtual void reserved12() {}
    virtual void reserved13() {}
    /* The wrong-slot control must reach a valid GNU registration ABI and an exact
     * diagnostic, rather than calling an unrelated void() method with weak_ptr args. */
    virtual void reserved14(std::weak_ptr<ListenerMirror>) {
        std::fputs("FAIL live: native registration slot 14\n", stderr);
        std::fflush(stderr);
        std::abort();
    }
    virtual void play(std::weak_ptr<ListenerMirror> value) { subscribe(0, value); }
    virtual void reserved16() {}
    virtual void reserved17() {}
    virtual void reserved18() {}
    virtual void quantize(std::weak_ptr<ListenerMirror> value) { subscribe(1, value); }
    virtual void reserved20() {}
    virtual void key_lock(std::weak_ptr<ListenerMirror> value) { subscribe(2, value); }
    virtual void reserved22() {}
    virtual void reserved23() {}
    virtual void reserved24() {}
    virtual void reserved25() {}
    std::vector<std::weak_ptr<ListenerMirror> > listeners[5];
    int fail_stream = -1;
    bool fail_after = false;
    void subscribe(unsigned stream, const std::weak_ptr<ListenerMirror> &value) {
        if (fail_stream == static_cast<int>(stream) && !fail_after) throw std::runtime_error("before weak capture");
        listeners[stream].push_back(value);
        if (fail_stream == static_cast<int>(stream) && fail_after) throw std::runtime_error("after weak capture");
    }
};
class UnitStateServer {
   public:
    virtual ~UnitStateServer() {}
    unsigned char padding[0x1f20] = {};
    StateNotificationServer *server = nullptr;
};
} // namespace dj_player
struct Unit {
    alignas(std::uintptr_t) unsigned char bytes[0x1a8] = {};
    explicit Unit(dj_player::UnitStateServer *p) { std::memcpy(bytes + 0x1a0, &p, sizeof p); }
};
struct Fixture {
    dj_player::StateNotificationServer server;
    dj_player::UnitStateServer publisher;
    Unit unit;
    Fixture() : unit(&publisher) {
        publisher.server = &server;
        CHECK(reinterpret_cast<const unsigned char *>(&publisher.server) - reinterpret_cast<const unsigned char *>(&publisher) == 0x1f28);
    }
    std::shared_ptr<ListenerMirror> lease(unsigned stream) {
        CHECK(server.listeners[stream].size());
        auto p = server.listeners[stream].back().lock();
        CHECK(p);
        return p;
    }
};
static std::uintptr_t vtable(const void *p) {
    std::uintptr_t t;
    std::memcpy(&t, p, sizeof t);
    return t;
}
static void dispatch(const std::shared_ptr<ListenerMirror> &p, const void *bytes, unsigned deck, bool full = true) {
    const std::uintptr_t *t = reinterpret_cast<const std::uintptr_t *>(vtable(p.get()));
    using Callback2 = void (*)(void *, const void *);
    using Callback3 = void (*)(void *, const void *, unsigned);
    reinterpret_cast<Callback2>(t[2])(p.get(), bytes);
    if (full) reinterpret_cast<Callback3>(t[3])(p.get(), bytes, deck);
}
static void emit_play(const std::shared_ptr<ListenerMirror> &p, unsigned a, unsigned b, unsigned c, unsigned d = 0) {
    /* Heap allocation makes byte four an ASan redzone. */
    std::unique_ptr<Play> n(new Play {{static_cast<unsigned char>(a), static_cast<unsigned char>(b), static_cast<unsigned char>(c)}});
    dispatch(p, n.get(), d);
}
static void emit_scalar(const std::shared_ptr<ListenerMirror> &p, std::uint32_t value, unsigned d = 0) {
    std::unique_ptr<std::uint32_t> n(new std::uint32_t(value));
    dispatch(p, n.get(), d);
}
static void unchanged(int d) {
    azsdk_live_sample out, before;
    std::memset(&out, 0xa5, sizeof out);
    before = out;
    CHECK(!azsdk_live_read(d, &out));
    CHECK(!std::memcmp(&out, &before, sizeof out));
}
static void sync_unchanged(int d) {
    azsdk_sync_sample out, before;
    std::memset(&out, 0xa5, sizeof out);
    before = out;
    CHECK(!azsdk_sync_read(d, &out));
    CHECK(!std::memcmp(&out, &before, sizeof out));
}
static azsdk_sync_sample sync_read(int d = 0) {
    struct Guard {
        std::uint64_t pre;
        azsdk_sync_sample value;
        std::uint64_t post;
    } out = {};
    out.pre = UINT64_C(0x123456789abcdef0);
    out.post = UINT64_C(0x0fedcba987654321);
    CHECK(azsdk_sync_read(d, &out.value));
    CHECK(out.pre == UINT64_C(0x123456789abcdef0) && out.post == UINT64_C(0x0fedcba987654321));
    CHECK(out.value.unit_generation && out.value.revision && out.value.observed_ns && out.value.valid <= 1);
    CHECK(!out.value.valid || out.value.raw_mode == 0 || out.value.raw_mode == 2);
    return out.value;
}
static azsdk_live_sample read(int d = 0) {
    struct Guard {
        std::uint64_t pre;
        azsdk_live_sample value;
        std::uint64_t post;
    } out = {};
    out.pre = UINT64_C(0x123456789abcdef0);
    out.post = UINT64_C(0x0fedcba987654321);
    CHECK(azsdk_live_read(d, &out.value));
    CHECK(out.pre == UINT64_C(0x123456789abcdef0) && out.post == UINT64_C(0x0fedcba987654321));
    CHECK(out.value.unit_generation && out.value.observed_fields && !out.value.reserved);
    CHECK(!(out.value.valid_fields & ~out.value.observed_fields));
    for (unsigned s = 0; s < 3; ++s) {
        bool observed = out.value.observed_fields & (1u << s);
        CHECK((out.value.revisions[s] != 0) == observed);
        CHECK((out.value.observed_ns[s] != 0) == observed);
    }
    return out.value;
}
static void validation() {
    Fixture f;
    unchanged(-1);
    unchanged(4);
    unchanged(0);
    CHECK(!azsdk_live_read(0, nullptr));
    sync_unchanged(-1);
    sync_unchanged(4);
    sync_unchanged(0);
    CHECK(!azsdk_sync_read(0, nullptr));
    CHECK(!azsdk_live_register(-1, &f.unit) && !azsdk_live_register(4, &f.unit));
    for (int build : {-1, 0, 3}) {
        azsdk_live_configure(1, build);
        CHECK(!azsdk_live_capabilities() && !azsdk_live_register(0, &f.unit));
        sync_unchanged(0);
    }
    azsdk_live_configure(0, 2);
    CHECK(!azsdk_live_register(0, &f.unit));
    azsdk_live_configure(1, 2);
    CHECK(!azsdk_live_register(0, nullptr));
    Unit empty(nullptr);
    CHECK(!azsdk_live_register(0, &empty));
    struct Wrong {
        virtual ~Wrong() {}
    } wrong;
    Unit wrong_publisher(reinterpret_cast<dj_player::UnitStateServer *>(&wrong));
    CHECK(!azsdk_live_register(0, &wrong_publisher));
    f.publisher.server = reinterpret_cast<dj_player::StateNotificationServer *>(&wrong);
    CHECK(!azsdk_live_register(0, &f.unit));
    f.publisher.server = nullptr;
    CHECK(!azsdk_live_register(0, &f.unit));
    f.publisher.server = &f.server;
    /* The copied table is in this ELF, but one purported method is ordinary RW data. */
    alignas(std::uintptr_t) static std::uintptr_t bad_table[28];
    const auto original = vtable(&f.server);
    std::memcpy(bad_table, reinterpret_cast<const void *>(original - 16), sizeof bad_table);
    bad_table[2 + 19] = reinterpret_cast<std::uintptr_t>(bad_table);
    std::uintptr_t forged = reinterpret_cast<std::uintptr_t>(bad_table + 2);
    reinterpret_cast<std::uintptr_t *>(&f.server)[0] = forged;
    CHECK(!azsdk_live_register(0, &f.unit));
    reinterpret_cast<std::uintptr_t *>(&f.server)[0] = original;
    CHECK(azsdk_live_register(0, &f.unit));
    CHECK(azsdk_live_capabilities() == 1 && !azsdk_live_register(0, &f.unit));
    for (unsigned s = 0; s < 4; ++s) CHECK(f.server.listeners[s].size() == 1);
    unchanged(0); // Registration does not manufacture an initial receipt.
    auto p = f.lease(0);
    Play n = {{1, 1, 1}};
    dispatch(p, &n, 0, false);
    unchanged(0);
    dispatch(p, &n, 1);
    unchanged(0);
    azsdk_live_retire(-1);
    azsdk_live_retire(4);
    CHECK(azsdk_live_capabilities() == 1);
    azsdk_live_retire(0);
    CHECK(!azsdk_live_capabilities());
    dispatch(p, &n, 0);
    unchanged(0);
    p.reset();
    for (unsigned s = 0; s < 4; ++s) CHECK(f.server.listeners[s][0].expired());
}
static void components_and_lifetime() {
    Fixture f;
    azsdk_live_configure(1, 1);
    CHECK(azsdk_live_register(0, &f.unit));
    auto p = f.lease(0), q = f.lease(1), k = f.lease(2);
    emit_play(p, 0, 0, 0);
    auto first = read();
    CHECK(first.observed_fields == AZSDK_LIVE_PLAY && first.valid_fields == AZSDK_LIVE_PLAY);
    CHECK(!first.mode_is_2 && !first.play_pause && !first.play_aux && first.quantize == -1 && first.key_lock == -1);
    for (unsigned a = 0; a < 2; ++a)
        for (unsigned b = 0; b < 2; ++b)
            for (unsigned c = 0; c < 2; ++c) {
                emit_play(p, a, b, c);
                auto out = read();
                CHECK(out.mode_is_2 == static_cast<int>(a) && out.play_pause == static_cast<int>(b) && out.play_aux == static_cast<int>(c));
            }
    auto before = read();
    emit_scalar(q, 0);
    auto quantize = read();
    CHECK(quantize.observed_fields == 3 && quantize.valid_fields == 3 && quantize.quantize == 0);
    CHECK(quantize.revisions[0] == before.revisions[0] && quantize.observed_ns[0] == before.observed_ns[0]);
    emit_scalar(k, 1);
    auto key = read();
    CHECK(key.observed_fields == 7 && key.valid_fields == 7 && key.key_lock == 1);
    CHECK(key.revisions[0] == before.revisions[0] && key.revisions[1] == quantize.revisions[1]);
    for (unsigned i = 0; i < 3; ++i) {
        unsigned a = i == 0 ? 2 : 1, b = i == 1 ? 255 : 1, c = i == 2 ? 2 : 1;
        auto old = read();
        emit_play(p, a, b, c);
        auto out = read();
        CHECK(out.observed_fields == 7 && out.valid_fields == 6);
        CHECK(out.mode_is_2 == -1 && out.play_pause == -1 && out.play_aux == -1);
        CHECK(out.quantize == 0 && out.key_lock == 1 && out.revisions[0] > old.revisions[0]);
        CHECK(out.observed_ns[0] >= old.observed_ns[0] && out.revisions[1] == old.revisions[1]);
    }
    for (std::uint32_t invalid : {2u, 256u, UINT32_MAX}) {
        emit_scalar(q, invalid);
        emit_scalar(k, invalid);
        auto out = read();
        CHECK(out.observed_fields == 7 && !out.valid_fields && out.quantize == -1 && out.key_lock == -1);
        emit_scalar(q, 0);
        emit_scalar(k, 1);
    }
    emit_play(p, 1, 0, 1);
    emit_scalar(q, 1);
    emit_scalar(k, 0);
    auto old = read();
    CHECK(old.valid_fields == 7 && old.quantize == 1 && !old.key_lock);
    azsdk_live_retire(0);
    emit_play(p, 1, 1, 1);
    emit_scalar(q, 1);
    emit_scalar(k, 1);
    unchanged(0);
    Fixture next;
    CHECK(azsdk_live_register(0, &next.unit));
    unchanged(0);
    emit_play(p, 1, 1, 1);
    emit_scalar(q, 1);
    emit_scalar(k, 1);
    unchanged(0);
    emit_scalar(next.lease(1), 0);
    auto current = read();
    CHECK(current.unit_generation != old.unit_generation && current.observed_fields == 2 && current.quantize == 0);
    auto next_play = next.lease(0), held = next.lease(1), next_key = next.lease(2);
    emit_play(next_play, 1, 0, 0);
    emit_scalar(next_key, 0);
    auto coherent = read();
    emit_play(p, 1, 1, 1);
    emit_scalar(q, 1);
    emit_scalar(k, 1);
    auto preserved = read();
    CHECK(preserved.valid_fields == coherent.valid_fields && preserved.unit_generation == coherent.unit_generation);
    for (unsigned s = 0; s < 3; ++s) CHECK(preserved.revisions[s] == coherent.revisions[s]);
    azsdk_live_configure(1, 2);
    emit_scalar(held, 1);
    unchanged(0);
}
static void failure_rollback() {
    for (int stream = 0; stream < 5; ++stream)
        for (int after = 0; after < 2; ++after) {
            Fixture f;
            azsdk_live_configure(1, 2);
            f.server.fail_stream = stream;
            f.server.fail_after = after;
            CHECK(!azsdk_live_register(0, &f.unit));
            CHECK(!azsdk_live_capabilities());
            unchanged(0);
            sync_unchanged(0);
            for (unsigned s = 0; s < 5; ++s)
                for (const auto &weak : f.server.listeners[s]) CHECK(weak.expired());
            f.server.fail_stream = -1;
            CHECK(azsdk_live_register(0, &f.unit));
            emit_scalar(f.lease(1), 1);
            CHECK(read().quantize == 1);
            emit_scalar(f.lease(3), 2);
            CHECK(sync_read().valid && sync_read().raw_mode == 2);
            azsdk_live_retire(0);
        }
    for (int allocation = 0; allocation < 5; ++allocation) {
        Fixture f;
        azsdk_live_configure(1, 2);
        allocation_failure.store(allocation);
        CHECK(!azsdk_live_register(0, &f.unit));
        allocation_failure.store(-1);
        CHECK(!azsdk_live_capabilities());
        unchanged(0);
        sync_unchanged(0);
        for (unsigned s = 0; s < 5; ++s) CHECK(f.server.listeners[s].empty());
        CHECK(azsdk_live_register(0, &f.unit));
        azsdk_live_retire(0);
    }
}
static void every_deck() {
    Fixture f[4];
    azsdk_live_configure(1, 2);
    for (unsigned d = 0; d < 4; ++d) CHECK(azsdk_live_register(d, &f[d].unit));
    CHECK(azsdk_live_capabilities() == 15);
    for (unsigned d = 0; d < 4; ++d) {
        for (unsigned wrong = 0; wrong < 4; ++wrong)
            if (wrong != d) {
                emit_play(f[d].lease(0), 1, 1, 1, wrong);
                emit_scalar(f[d].lease(1), 1, wrong);
                emit_scalar(f[d].lease(2), 1, wrong);
            }
        unchanged(d);
        emit_play(f[d].lease(0), d & 1, (d >> 1) & 1, 1, d);
        emit_scalar(f[d].lease(1), d & 1, d);
        emit_scalar(f[d].lease(2), (d >> 1) & 1, d);
        auto out = read(d);
        CHECK(out.valid_fields == 7 && out.mode_is_2 == static_cast<int>(d & 1) && out.play_pause == static_cast<int>((d >> 1) & 1));
        CHECK(out.quantize == static_cast<int>(d & 1) && out.key_lock == static_cast<int>((d >> 1) & 1));
    }
    azsdk_live_configure(0, 2);
    for (unsigned d = 0; d < 4; ++d) unchanged(d);
}
static void sync_components_and_lifetime() {
    for (int build : {1, 2}) {
        Fixture f[4];
        std::shared_ptr<ListenerMirror> held[4];
        std::uint64_t old_generation[4] = {};
        azsdk_live_configure(1, build);
        for (unsigned d = 0; d < 4; ++d) {
            CHECK(azsdk_live_register(d, &f[d].unit));
            held[d] = f[d].lease(3);
            sync_unchanged(d);
            unchanged(d);
            std::uint32_t on = 2;
            dispatch(held[d], &on, d, false);
            sync_unchanged(d); // Slot 2 lacks routing; no initial replay is invented.
            for (unsigned wrong = 0; wrong < 4; ++wrong)
                if (wrong != d) emit_scalar(held[d], 2, wrong);
            sync_unchanged(d);
            for (std::uint32_t mode : {0u, 2u, 1u, 3u, 256u, UINT32_MAX, UINT32_C(0x80000000), 0u}) {
                emit_scalar(held[d], mode, d);
                auto receipt = sync_read(d);
                std::int32_t expected;
                std::memcpy(&expected, &mode, sizeof expected);
                CHECK(receipt.raw_mode == expected);
                CHECK(receipt.valid == (mode == 0 || mode == 2));
                old_generation[d] = receipt.unit_generation;
                unchanged(d); // Fourth stream cannot overwrite legacy arrays or fields.
            }
            emit_play(f[d].lease(0), 1, 0, 1, d);
            emit_scalar(f[d].lease(1), 0, d);
            emit_scalar(f[d].lease(2), 1, d);
            auto legacy = read(d);
            auto before = sync_read(d);
            emit_scalar(held[d], 2, d);
            auto after = sync_read(d);
            CHECK(after.revision > before.revision && after.observed_ns >= before.observed_ns);
            CHECK(after.unit_generation == legacy.unit_generation);
            auto same_legacy = read(d);
            CHECK(!std::memcmp(&legacy, &same_legacy, sizeof legacy));
            emit_play(f[d].lease(0), 0, 1, 0, d);
            emit_scalar(f[d].lease(1), 1, d);
            emit_scalar(f[d].lease(2), 0, d);
            auto same_sync = sync_read(d);
            CHECK(!std::memcmp(&after, &same_sync, sizeof after));
            emit_scalar(held[d], 1, d);
            auto invalid = sync_read(d);
            CHECK(!invalid.valid && invalid.raw_mode == 1 && invalid.revision > after.revision);
            CHECK(invalid.observed_ns >= after.observed_ns); // Invalid receipt clears prior valid state.
            azsdk_live_retire(d);
            emit_scalar(held[d], 2, d);
            sync_unchanged(d);
            unchanged(d);
        }
        CHECK(!azsdk_live_capabilities());
        Fixture replacement[4];
        for (unsigned d = 0; d < 4; ++d) {
            CHECK(azsdk_live_register(d, &replacement[d].unit));
            emit_scalar(held[d], 2, d); // Strong leases from retired units cannot republish.
            sync_unchanged(d);
            emit_scalar(replacement[d].lease(3), 0, d);
            auto current = sync_read(d);
            CHECK(current.valid && current.raw_mode == 0 && current.unit_generation != old_generation[d]);
        }
        std::shared_ptr<ListenerMirror> replacement_held[4];
        for (unsigned d = 0; d < 4; ++d) replacement_held[d] = replacement[d].lease(3);
        azsdk_live_configure(0, build);
        for (unsigned d = 0; d < 4; ++d) {
            emit_scalar(replacement_held[d], 2, d);
            sync_unchanged(d);
        }
    }
}
static void concurrent() {
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        Fixture f;
        azsdk_live_configure(1, 2);
        CHECK(azsdk_live_register(0, &f.unit));
        auto p = f.lease(0), q = f.lease(1), k = f.lease(2);
        emit_play(p, 0, 0, 0);
        auto generation = read().unit_generation;
        auto sync = f.lease(3);
        emit_scalar(sync, 0);
        std::atomic<bool> stop {false};
        std::thread writer([&] {
            std::uint32_t zero = 0, one = 1;
            Play a = {{0, 0, 0}}, b = {{1, 1, 1}};
            while (!stop.load()) {
                dispatch(p, &a, 0);
                dispatch(p, &b, 0);
                dispatch(q, &zero, 0);
                dispatch(q, &one, 0);
                dispatch(k, &one, 0);
                dispatch(k, &zero, 0);
                std::uint32_t sync_on = 2;
                dispatch(sync, &sync_on, 0);
                dispatch(sync, &one, 0);
                dispatch(sync, &zero, 0);
            }
        });
        std::thread other([&] {
            Play n = {{1, 1, 1}};
            while (!stop.load()) dispatch(p, &n, 0);
        });
        for (unsigned n = 0; n < 4000; ++n) {
            azsdk_live_sample out;
            if (azsdk_live_read(0, &out)) {
                CHECK(out.unit_generation == generation);
                if (out.valid_fields & 1) CHECK(out.mode_is_2 == out.play_pause && out.play_pause == out.play_aux);
                CHECK(!(out.valid_fields & ~7u));
            }
            azsdk_sync_sample sync_out;
            if (azsdk_sync_read(0, &sync_out)) {
                CHECK(sync_out.unit_generation == generation && sync_out.revision && sync_out.observed_ns);
                CHECK(sync_out.valid == (sync_out.raw_mode == 0 || sync_out.raw_mode == 2));
            }
        }
        if (cycle & 1) azsdk_live_retire(0);
        else azsdk_live_configure(1, 1);
        for (unsigned n = 0; n < 100; ++n) {
            unchanged(0);
            sync_unchanged(0);
        }
        stop.store(true);
        writer.join();
        other.join();
        CHECK(!azsdk_live_capabilities());
    }
}
static Fixture *early_fixture;
/* A native ELF constructor can register before this translation unit's default C++
 * initializer. Constant initialization must preserve that publication and ownership. */
__attribute__((constructor(101))) static void early_configuration() {
    early_fixture = new Fixture;
    azsdk_live_configure(1, 2);
    CHECK(azsdk_live_register(2, &early_fixture->unit));
    CHECK(early_fixture->server.listeners[3].size() == 1);
    CHECK(early_fixture->server.listeners[4].size() == 1);
    emit_scalar(early_fixture->lease(1), 1, 2);
    emit_scalar(early_fixture->lease(3), 2, 2);
    emit_scalar(early_fixture->lease(4), 1, 2);
}
static void tempo_lock_registration() {
    for (int build = 1; build <= 2; ++build) {
        Fixture units[4];
        azsdk_live_configure(1, build);
        for (int deck = 0; deck < 4; ++deck) {
            CHECK(azsdk_live_register(deck, &units[deck].unit));
            CHECK(units[deck].server.listeners[4].size() == 1);
            CHECK(!units[deck].server.listeners[4][0].expired());
            azsdk_tempo_lock_sample sample, before;
            std::memset(&sample, 0xa5, sizeof sample);
            before = sample;
            CHECK(!azsdk_tempo_lock_read(deck, &sample) && !std::memcmp(&sample, &before, sizeof sample));
            auto held = units[deck].lease(4);
            emit_scalar(held, 1, (deck + 1) % 4);
            CHECK(!azsdk_tempo_lock_read(deck, &sample));
            std::uint64_t revision = 0;
            for (std::uint32_t value : {0u, 1u, 2u, 0x100u, 0xffffffffu, 0u}) {
                emit_scalar(held, value, deck);
                CHECK(azsdk_tempo_lock_read(deck, &sample));
                CHECK(sample.valid == (value <= 1) && sample.revision > revision && sample.observed_ns);
                CHECK(static_cast<std::uint32_t>(sample.raw_locked) == value);
                revision = sample.revision;
            }
            before = sample;
            emit_scalar(units[deck].lease(1), 1, deck);
            CHECK(azsdk_tempo_lock_read(deck, &sample) && !std::memcmp(&sample, &before, sizeof sample));
            azsdk_live_retire(deck);
            emit_scalar(held, 1, deck);
            CHECK(!azsdk_tempo_lock_read(deck, &sample));
        }
        for (int deck = 0; deck < 4; ++deck) azsdk_live_retire(deck);
    }
}
int main(int argc, char **argv) {
    static_assert(sizeof(azsdk_live_sample) == 88, "legacy private sample ABI unchanged");
    static_assert(sizeof(azsdk_sync_sample) == 32, "separate copied Sync sample ABI");
    allocator_contract();
    CHECK(early_fixture && azsdk_live_capabilities() == 4 && read(2).quantize == 1);
    CHECK(sync_read(2).valid && sync_read(2).raw_mode == 2);
    azsdk_tempo_lock_sample early_tempo;
    CHECK(azsdk_tempo_lock_read(2, &early_tempo) && early_tempo.valid && early_tempo.raw_locked == 1);
    for (unsigned s = 0; s < 5; ++s) CHECK(!early_fixture->server.listeners[s][0].expired());
    azsdk_live_retire(2);
    for (unsigned s = 0; s < 5; ++s) CHECK(early_fixture->server.listeners[s][0].expired());
    delete early_fixture;
    tempo_lock_registration();
    if (argc == 2 && !std::strcmp(argv[1], "--sync-payload-short")) {
        Fixture short_payload;
        azsdk_live_configure(1, 2);
        CHECK(azsdk_live_register(0, &short_payload.unit));
        std::unique_ptr<Play> bytes(new Play {{2, 0, 0}});
        dispatch(short_payload.lease(3), bytes.get(), 0);
        std::fputs("FAIL live: three-byte Sync payload lacked ASan diagnosis\n", stderr);
        return 1;
    }
    CHECK(argc == 1);
    validation();
    components_and_lifetime();
    failure_rollback();
    every_deck();
    sync_components_and_lifetime();
    concurrent();
    azsdk_live_configure(0, 2);
    std::puts("PASS live: GNU observer ABI, bounded payloads, per-stream validity, lifecycle, rollback, concurrency");
}
#endif
