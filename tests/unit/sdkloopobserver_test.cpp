/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic GNU ABI/lifetime model; it never loads or invokes EP147. */
#include "azloopobserver.h"
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#if !defined(__linux__)
int main() {
    std::puts("SKIP: loop observer fixture requires Linux ELF main-image guards");
    return 77;
}
#else
namespace dj_player {
class CueProperty {
   public:
    virtual ~CueProperty() {}
};
class PointProperty {
   public:
    virtual ~PointProperty() {}
};
} // namespace dj_player
namespace player_basis {
class PositionWithSourceInfo {
   public:
    virtual void position() {}
};
} // namespace player_basis
struct alignas(std::uintptr_t) Notification {
    unsigned char bytes[0x100];
};
static_assert(sizeof(Notification) == 8 + 0xf8, "complete native loop notification");
static_assert(sizeof(azsdk_loop_sample) == 96, "private diagnostic scalar boundary");
static void scalar32(Notification &n, unsigned offset, std::uint32_t value) {
    assert(offset + 4 <= sizeof n);
    for (unsigned i = 0; i < 4; ++i) n.bytes[offset + i] = static_cast<unsigned char>(value >> (i * 8));
}
static void scalar64(Notification &n, unsigned offset, std::uint64_t value) {
    assert(offset + 8 <= sizeof n);
    for (unsigned i = 0; i < 8; ++i) n.bytes[offset + i] = static_cast<unsigned char>(value >> (i * 8));
}
static std::uint64_t scalar64(const Notification &n, unsigned offset) {
    std::uint64_t value;
    std::memcpy(&value, n.bytes + offset, sizeof value);
    return value;
}
static std::uint32_t scalar32(const Notification &n, unsigned offset) {
    std::uint32_t value;
    std::memcpy(&value, n.bytes + offset, sizeof value);
    return value;
}
static Notification notification(bool engaged, unsigned char mode = 0) {
    Notification n = {};
    n.bytes[0] = mode;
    if (engaged) {
        n.bytes[0x38] = n.bytes[0x3c] = n.bytes[0x3e] = 1;
        scalar32(n, 0x40, 1);
        scalar64(n, 0x48, 0); /* Time zero is valid; only INT64_MAX is invalid. */
    } else {
        scalar64(n, 0x48, UINT64_C(0x7fffffffffffffff));
    }
    return n;
}
template <class T>
static std::uintptr_t fixture_vtable() {
    static const T object;
    std::uintptr_t value;
    std::memcpy(&value, &object, sizeof value);
    return value;
}
/* Keep this type function-local: GNU COMDAT RTTI names for external classes are
 * not poisoned as individual globals by GCC7 ASan. This short local name is. */
static std::uintptr_t short_rtti() {
    class A {
       public:
        virtual ~A() {}
    };
    return fixture_vtable<A>();
}
static Notification bounds_notification(std::uint32_t rate = 44100, std::uint64_t in = 9217,
                                        std::uint64_t out = 681239) {
    Notification n = notification(true);
    scalar64(n, 8, fixture_vtable<dj_player::CueProperty>());
    for (unsigned point : {0x20u, 0x78u}) {
        scalar64(n, point, fixture_vtable<dj_player::PointProperty>());
        scalar64(n, point + 0x10, fixture_vtable<player_basis::PositionWithSourceInfo>());
        scalar64(n, point + 0x18, UINT64_C(0x0001010200000001));
        scalar32(n, point + 0x20, 4);
        /* Deliberately opaque and unaligned: the observer may compare, never follow it. */
        scalar64(n, point + 0x38, UINT64_C(0x10001));
        scalar64(n, point + 0x40, UINT64_C(0x10003));
        scalar32(n, point + 0x48, rate);
    }
    scalar64(n, 0x48, in);
    scalar64(n, 0xa0, out);
    double scale = 1000000.0 / static_cast<double>(rate);
    scalar64(n, 0x70, static_cast<std::uint64_t>(std::round(scale * static_cast<double>(in))));
    scalar64(n, 0xc8, static_cast<std::uint64_t>(std::round(scale * static_cast<double>(out))));
    return n;
}
class ListenerMirror {
   public:
    virtual ~ListenerMirror() {}
    virtual void callback2(const Notification &) = 0;
    virtual void callback3(const Notification &, unsigned player) = 0;
};
/* Names and vtable slots intentionally model the reconstructed native ABI. The registration
 * function's weak_ptr type differs at source level from production: native weak_ptr storage
 * and the virtual-call ABI must interoperate, including ownership and exception unwinding. */
namespace dj_player {
class StateNotificationServer {
   public:
    virtual ~StateNotificationServer() {}
    virtual void reserved2() {}
    virtual void reserved3() {}
    virtual void reserved4() {}
    virtual void reserved5() {}
    virtual void reserved6() {}
    virtual void reserved7() {}
    virtual void reserved8() {}
    virtual void reserved9() {}
    virtual void reserved10() {}
    virtual void addObserver(std::weak_ptr<ListenerMirror> value) {
        if (fail) throw std::runtime_error("synthetic registration failure");
        listeners.push_back(value);
        if (fail_after) throw std::runtime_error("synthetic failure after weak capture");
    }
    virtual void reserved12() {}
    virtual void reserved13() {}
    virtual void reserved14() {}
    virtual void reserved15() {}
    virtual void reserved16() {}
    virtual void reserved17() {}
    virtual void reserved18() {}
    virtual void reserved19() {}
    virtual void reserved20() {}
    virtual void reserved21() {}
    virtual void reserved22() {}
    virtual void reserved23() {}
    virtual void reserved24() {}
    virtual void reserved25() {}
    std::vector<std::weak_ptr<ListenerMirror> > listeners;
    bool fail = false;
    bool fail_after = false;
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
    explicit Unit(dj_player::UnitStateServer *publisher) { std::memcpy(bytes + 0x1a0, &publisher, sizeof publisher); }
};
static void unchanged(int deck) {
    azsdk_loop_sample value, before;
    std::memset(&value, 0xa5, sizeof value);
    before = value;
    assert(!azsdk_loop_read(deck, &value));
    assert(!std::memcmp(&value, &before, sizeof value));
}
static azsdk_loop_sample read(int deck) {
    struct Guarded {
        std::uint64_t before;
        azsdk_loop_sample value;
        std::uint64_t after;
    } out = {};
    out.before = UINT64_C(0x1122334455667788);
    out.after = UINT64_C(0x8877665544332211);
    assert(azsdk_loop_read(deck, &out.value));
    assert(out.before == UINT64_C(0x1122334455667788) && out.after == UINT64_C(0x8877665544332211));
    assert(out.value.unit_generation && out.value.revision && out.value.observed_ns &&
           out.value.reserved >= 0 && out.value.reserved <= 13 && !out.value.reserved2 && !out.value.check_reserved);
    assert(out.value.bounds_valid == 0 || out.value.bounds_valid == 1);
    assert((out.value.bounds_valid && !out.value.reserved) || (!out.value.bounds_valid && out.value.reserved));
    if (!out.value.bounds_valid) assert(out.value.in_s == 0 && out.value.out_s == 0);
    if (out.value.reserved > 0 && out.value.reserved < 12)
        assert(!out.value.check_in_samples && !out.value.check_out_samples &&
               !out.value.check_in_cached_us && !out.value.check_out_cached_us && !out.value.check_rate);
    else assert(out.value.check_rate);
    return out.value;
}
/* Reproduce the native dispatch ABI, rather than imposing the fixture mirror's C++ RTTI on
 * the separately reconstructed listener. Native code also calls these two vtable slots. */
static void callback2(const std::shared_ptr<ListenerMirror> &listener, const Notification &n) {
    std::uintptr_t *table;
    std::memcpy(&table, listener.get(), sizeof table);
    using Callback = void (*)(void *, const Notification &);
    reinterpret_cast<Callback>(table[2])(listener.get(), n);
}
static void emit_record(const std::shared_ptr<ListenerMirror> &listener, const Notification &n, unsigned player) {
    callback2(listener, n);
    std::uintptr_t *table;
    std::memcpy(&table, listener.get(), sizeof table);
    using Callback = void (*)(void *, const Notification &, unsigned);
    reinterpret_cast<Callback>(table[3])(listener.get(), n, player);
}
static void emit(const std::shared_ptr<ListenerMirror> &listener, bool engaged, unsigned player) {
    emit_record(listener, notification(engaged), player);
}
static void expect_engaged(const std::shared_ptr<ListenerMirror> &listener, const Notification &n, int expected) {
    azsdk_loop_sample before = read(0);
    emit_record(listener, n, 0);
    azsdk_loop_sample after = read(0);
    assert(after.engaged == expected && after.unit_generation == before.unit_generation &&
           after.revision > before.revision && after.observed_ns >= before.observed_ns);
}
static void predicate_branches(const std::shared_ptr<ListenerMirror> &listener) {
    Notification active = notification(true);
    expect_engaged(listener, active, 1); /* A current loop at time zero is engaged. */
    for (unsigned mode = 0; mode <= 1; ++mode) {
        expect_engaged(listener, notification(true, mode), 1);
        expect_engaged(listener, notification(false, mode), 0);
    }
    const unsigned byte_conditions[] = {0x38, 0x3c, 0x3e};
    for (unsigned offset : byte_conditions) {
        Notification invalid = active;
        invalid.bytes[offset] = 0;
        expect_engaged(listener, invalid, 0);
    }
    Notification altered = active;
    altered.bytes[0x3c] = 0xe0;
    expect_engaged(listener, altered, 0); /* Only the low five bits count. */
    altered.bytes[0x3c] = 0xe1;
    expect_engaged(listener, altered, 1);
    altered = active;
    altered.bytes[0x38] = altered.bytes[0x3e] = 2;
    expect_engaged(listener, altered, 1); /* Native tests nonzero, not canonical bool. */
    altered = active;
    scalar32(altered, 0x40, 0);
    expect_engaged(listener, altered, 0);
    scalar32(altered, 0x40, UINT32_C(0x80000000));
    expect_engaged(listener, altered, 1);
    altered = active;
    scalar64(altered, 0x48, UINT64_C(0x7fffffffffffffff));
    expect_engaged(listener, altered, 0);
    scalar64(altered, 0x48, UINT64_MAX);
    expect_engaged(listener, altered, 1); /* Exact native policy permits any other bit value. */
    /* A retained property is separate native state. EXIT clears the working copy even
     * while retained points remain valid; those points must never be substituted. */
    Notification retained = active;
    Notification exited = active;
    std::memset(exited.bytes + 0x38, 0, 8);
    scalar32(exited, 0x40, 0);
    scalar64(exited, 0x48, UINT64_C(0x7fffffffffffffff));
    assert(retained.bytes[0x38] && retained.bytes[0x3c] && retained.bytes[0x3e]);
    expect_engaged(listener, exited, 0);
    expect_engaged(listener, retained, 1); /* RELOOP restores the working property. */
    /* Header and unused payload bytes are not the property predicate. The full record
     * also ensures an erroneous property-base interpretation cannot pass this fixture. */
    altered = notification(false, 1);
    altered.bytes[0x30] = altered.bytes[0x34] = altered.bytes[0x36] = 1;
    expect_engaged(listener, altered, 0);
}
static void expect_bounds(unsigned deck, const std::shared_ptr<ListenerMirror> &listener, const Notification &n,
                          int engaged, bool valid, double in = 0, double out = 0) {
    azsdk_loop_sample peers[4] = {};
    bool available[4] = {};
    for (unsigned other = 0; other < 4; ++other)
        if (other != deck) available[other] = azsdk_loop_read(other, &peers[other]);
    azsdk_loop_sample before = read(deck);
    emit_record(listener, n, deck);
    azsdk_loop_sample after = read(deck);
    assert(after.engaged == engaged && after.bounds_valid == valid);
    assert(after.unit_generation == before.unit_generation && after.revision > before.revision);
    assert(after.in_s == in && after.out_s == out);
    if (valid || after.reserved >= 12) {
        assert(after.check_in_samples == scalar64(n, 0x48) && after.check_out_samples == scalar64(n, 0xa0));
        assert(after.check_in_cached_us == scalar64(n, 0x70) && after.check_out_cached_us == scalar64(n, 0xc8));
        assert(after.check_rate == scalar32(n, 0x68));
    }
    for (unsigned other = 0; other < 4; ++other) if (other != deck) {
        azsdk_loop_sample peer = {};
        assert(bool(azsdk_loop_read(other, &peer)) == available[other]);
        if (available[other]) assert(!std::memcmp(&peers[other], &peer, sizeof peer));
    }
}
static void bounds_branches(unsigned deck, const std::shared_ptr<ListenerMirror> &listener) {
    Notification valid = bounds_notification();
    expect_bounds(deck, listener, valid, 1, true, 9217.0 / 44100, 681239.0 / 44100);
    /* Actual2.00 saved-loop recall: equal SourceInfo/rate, separate source owners. */
    Notification restored = bounds_notification(44100, 120785, 145031);
    scalar64(restored, 0x58, UINT64_C(0xffffa1c0bd80));
    scalar64(restored, 0xb0, UINT64_C(0xffffa1c0be80));
    scalar64(restored, 0x70, 2738889);
    scalar64(restored, 0xc8, 3288707);
    expect_bounds(deck, listener, restored, 1, true, 120785.0 / 44100, 145031.0 / 44100);
    for (unsigned offset : {8u, 0x20u, 0x78u, 0x30u, 0x88u}) {
        Notification short_name = valid;
        scalar64(short_name, offset, short_rtti());
        expect_bounds(deck, listener, short_name, 1, false);
    }
    /* Existing captured2.00 scalars; the fixture never reads that target. */
    std::uint64_t cached_in, cached_out;
    std::memcpy(&cached_in, valid.bytes + 0x70, 8);
    std::memcpy(&cached_out, valid.bytes + 0xc8, 8);
    assert(cached_in == 209002 && cached_out == 15447596);
    for (std::uint32_t rate : {48000u, 96000u, 4000u, 768000u}) {
        Notification n = bounds_notification(rate, 0, rate);
        expect_bounds(deck, listener, n, 1, true, 0, 1);
    }
    expect_bounds(deck, listener, bounds_notification(48000, 0, 1), 1, true, 0, 1.0 / 48000);
    expect_bounds(deck, listener, bounds_notification(48000, 0, 14400000), 1, true, 0, 300);
    Notification half = bounds_notification(128, 1, 3); /* 7812.5us ->7813, not floor/even. */
    expect_bounds(deck, listener, half, 1, true, 1.0 / 128, 3.0 / 128);
    scalar64(half, 0x70, 7812);
    expect_bounds(deck, listener, half, 1, true, 1.0 / 128, 3.0 / 128);
    azsdk_loop_sample mismatch = read(deck);
    assert(mismatch.reserved == 0 && mismatch.check_rate == 128 && mismatch.check_in_samples == 1 &&
           mismatch.check_out_samples == 3 && mismatch.check_in_cached_us == 7812 && mismatch.check_out_cached_us == 23438);
    /* Physical EP1472.00 Deck2, a previous test: stable across three RELOOP receipts.
     * IN's cached1852000us differs from round(81673 * (1e6 / 44100))=1851995us.
     * The unrounded difference is only0.2 source sample. Keep source coordinates
     * sample-derived; a rounded cached timestamp must not make them unavailable. */
    Notification captured = bounds_notification(44100, 81673, 247048);
    scalar64(captured, 0x70, 1852000);
    scalar64(captured, 0xc8, 5601995);
    expect_bounds(deck, listener, captured, 1, true, 81673.0 / 44100, 247048.0 / 44100);
    mismatch = read(deck);
    assert(mismatch.reserved == 0 && mismatch.check_rate == 44100 &&
           mismatch.check_in_samples == 81673 && mismatch.check_out_samples == 247048 &&
           mismatch.check_in_cached_us == 1852000 && mismatch.check_out_cached_us == 5601995);
    /* Allow one sample's quantization plus half a cached microsecond, on either
     * side of either endpoint. The threshold follows rate, not a fixed epsilon. */
    struct Case { std::uint32_t rate; std::uint64_t accepted, rejected; };
    for (Case c : {Case{44100,23,24}, Case{48000,21,22}, Case{96000,10,11},
                   Case{192000,5,6}, Case{768000,1,2}, Case{2000000,1,2},
                   Case{2147483646,0,1}}) {
        for (unsigned offset : {0x70u, 0xc8u}) {
            Notification n = bounds_notification(c.rate, c.rate, 2ull * c.rate);
            std::uint64_t exact = scalar64(n, offset);
            for (int direction : {-1, 1}) {
                scalar64(n, offset, direction > 0 ? exact + c.accepted : exact - c.accepted);
                expect_bounds(deck, listener, n, 1, true, 1, 2);
                scalar64(n, offset, direction > 0 ? exact + c.rejected : exact - c.rejected);
                expect_bounds(deck, listener, n, 1, false);
                assert(read(deck).reserved == 12);
                scalar64(n, offset, exact);
            }
        }
    }
    /* Larger clocks must not hide a multi-sample discrepancy through double rounding. */
    Notification large = bounds_notification(1000000, UINT64_C(9007199254740988), UINT64_C(9007199254740992));
    scalar64(large, 0x70, UINT64_C(9007199254740990));
    expect_bounds(deck, listener, large, 1, false);
    assert(read(deck).reserved == 12);
    /* A subsequent valid record must recover; rejection cannot latch stale bounds. */
    expect_bounds(deck, listener, bounds_notification(44100, 81673, 247048), 1, true,
                  81673.0 / 44100, 247048.0 / 44100);
    for (unsigned offset : {8u, 0x20u, 0x78u, 0x30u, 0x88u}) {
        Notification n = valid;
        scalar64(n, offset, 0);
        expect_bounds(deck, listener, n, 1, false);
        scalar64(n, offset, fixture_vtable<dj_player::StateNotificationServer>());
        expect_bounds(deck, listener, n, 1, false);
    }
    /* Both native source-position predicates, including OUT independently of engagement. */
    for (unsigned offset : {0x38u, 0x3cu, 0x3eu, 0x90u, 0x94u, 0x96u}) {
        Notification n = valid;
        n.bytes[offset] = 0;
        expect_bounds(deck, listener, n, offset < 0x90 ? 0 : 1, false);
    }
    Notification n = valid;
    n.bytes[0x94] = 0xe0;
    expect_bounds(deck, listener, n, 1, false);
    for (unsigned offset : {0x40u, 0x98u}) {
        n = valid;
        scalar32(n, offset, 0);
        expect_bounds(deck, listener, n, offset == 0x40 ? 0 : 1, false);
    }
    n = valid;
    scalar32(n, 0x98, 5); /* Matching validity, different content/source tuple. */
    expect_bounds(deck, listener, n, 1, false);
    n = valid;
    n.bytes[0x91] ^= 1;
    expect_bounds(deck, listener, n, 1, false);
    for (unsigned offset : {0x68u, 0xc0u}) {
        for (std::uint32_t rate : {0u, UINT32_C(0x7fffffff), UINT32_MAX, 48000u}) {
            n = valid;
            scalar32(n, offset, rate);
            expect_bounds(deck, listener, n, 1, false);
        }
    }
    for (std::uint32_t rate : {0u, UINT32_C(0x7fffffff), UINT32_MAX}) {
        n = valid;
        scalar32(n, 0x68, rate);
        scalar32(n, 0xc0, rate);
        expect_bounds(deck, listener, n, 1, false);
    }
    for (unsigned offset : {0x48u, 0xa0u, 0x70u, 0xc8u}) {
        for (std::uint64_t value : {UINT64_MAX, UINT64_C(0x7fffffffffffffff), UINT64_C(9007199254740993)}) {
            n = valid;
            scalar64(n, offset, value);
            expect_bounds(deck, listener, n, offset == 0x48 && value == UINT64_C(0x7fffffffffffffff) ? 0 : 1, false);
        }
    }
    for (std::uint64_t out : {UINT64_C(9217), UINT64_C(1)}) {
        n = valid;
        scalar64(n, 0xa0, out);
        expect_bounds(deck, listener, n, 1, false);
    }
    for (unsigned offset : {0x70u, 0xc8u}) {
        n = valid;
        scalar64(n, offset, 0);
        expect_bounds(deck, listener, n, 1, false);
    }
    for (unsigned offset : {0x58u, 0xb0u}) {
        n = valid;
        scalar64(n, offset, 0);
        expect_bounds(deck, listener, n, 1, false);
        scalar64(n, offset, 0x10005);
        expect_bounds(deck, listener, n, 1, true, 9217.0 / 44100, 681239.0 / 44100);
    }
    /* The bool supplied to each native setter is not a native position-valid flag. */
    n = valid;
    n.bytes[0x28] = n.bytes[0x80] = 0;
    expect_bounds(deck, listener, n, 1, true, 9217.0 / 44100, 681239.0 / 44100);
    n.bytes[0] = 1;
    expect_bounds(deck, listener, n, 1, true, 9217.0 / 44100, 681239.0 / 44100);
    Notification retained = valid;
    Notification exited = valid;
    std::memset(exited.bytes + 0x38, 0, 12);
    scalar64(exited, 0x48, UINT64_C(0x7fffffffffffffff));
    expect_bounds(deck, listener, exited, 0, false);
    expect_bounds(deck, listener, retained, 1, true, 9217.0 / 44100, 681239.0 / 44100);
}
/* Run the entire bounds/error/rounding matrix through each independently owned
 * listener, with all four decks registered concurrently, for both supported build
 * configurations. Captured numbers are ordinary inputs on every deck slot. */
static void all_deck_bounds() {
    for (int build : {1, 2}) {
        dj_player::StateNotificationServer servers[4];
        dj_player::UnitStateServer publishers[4];
        std::unique_ptr<Unit> units[4];
        std::shared_ptr<ListenerMirror> listeners[4];
        azsdk_loop_configure(1, build);
        for (unsigned deck = 0; deck < 4; ++deck) {
            publishers[deck].server = &servers[deck];
            units[deck].reset(new Unit(&publishers[deck]));
            assert(azsdk_loop_register(deck, units[deck]->bytes));
            listeners[deck] = servers[deck].listeners.back().lock();
            assert(listeners[deck]);
            std::uint32_t rate = 48000 + deck * 16000;
            Notification seed = bounds_notification(rate, (deck + 1ull) * rate, (deck + 2ull) * rate);
            emit_record(listeners[deck], seed, deck);
            assert(read(deck).in_s == deck + 1 && read(deck).out_s == deck + 2);
        }
        assert(azsdk_loop_capabilities() == 15);
        for (unsigned deck = 0; deck < 4; ++deck) {
            bounds_branches(deck, listeners[deck]);
            azsdk_loop_sample before[4];
            for (unsigned other = 0; other < 4; ++other) before[other] = read(other);
            for (unsigned wrong = 0; wrong < 4; ++wrong) if (wrong != deck) {
                emit_record(listeners[deck], bounds_notification(), wrong);
                for (unsigned other = 0; other < 4; ++other) {
                    azsdk_loop_sample after = read(other);
                    assert(!std::memcmp(&before[other], &after, sizeof after));
                }
            }
            auto retired = listeners[deck];
            azsdk_loop_retire(deck);
            unchanged(deck);
            assert(azsdk_loop_capabilities() == (15u & ~(1u << deck)));
            assert(azsdk_loop_register(deck, units[deck]->bytes));
            listeners[deck] = servers[deck].listeners.back().lock();
            emit_record(listeners[deck], bounds_notification(), deck);
            azsdk_loop_sample replacement = read(deck);
            assert(replacement.unit_generation > before[deck].unit_generation);
            emit_record(retired, notification(false), deck);
            azsdk_loop_sample after = read(deck);
            assert(!std::memcmp(&replacement, &after, sizeof after));
            for (unsigned other = 0; other < 4; ++other) if (other != deck) {
                after = read(other);
                assert(!std::memcmp(&before[other], &after, sizeof after));
            }
        }
        azsdk_loop_configure(0, build);
        for (unsigned deck = 0; deck < 4; ++deck) {
            listeners[deck].reset();
            unchanged(deck);
            for (const auto &weak : servers[deck].listeners) assert(weak.expired());
        }
    }
    std::puts("sdk loop observer: all bounds cases passed on decks1-4, both build configurations, with cross-deck isolation");
}
static dj_player::StateNotificationServer *early_server;
static dj_player::UnitStateServer *early_publisher;
static Unit *early_unit;
/* The C library's ELF constructor can run before this C++ TU's default initializer.
 * Configure, register and publish first, then verify at main that no C++ initializer
 * overwrote that state. GNU sanitizer runtime initialization precedes priority 101. */
__attribute__((constructor(101))) static void early_configuration() {
    early_server = new dj_player::StateNotificationServer;
    early_publisher = new dj_player::UnitStateServer;
    early_publisher->server = early_server;
    early_unit = new Unit(early_publisher);
    azsdk_loop_configure(1, 2);
    assert(azsdk_loop_register(2, early_unit->bytes));
    auto listener = early_server->listeners.back().lock();
    assert(listener);
    emit(listener, 1, 2);
}
int main() {
    assert(early_unit && early_publisher && early_server && azsdk_loop_capabilities() == 4);
    assert(read(2).engaged);
    auto early = early_server->listeners.back().lock();
    assert(early && early.use_count() == 2);
    azsdk_loop_retire(2);
    early.reset();
    assert(early_server->listeners.back().expired());
    delete early_unit;
    delete early_publisher;
    delete early_server;
    dj_player::StateNotificationServer server;
    dj_player::UnitStateServer publisher;
    publisher.server = &server;
    assert(reinterpret_cast<unsigned char *>(&publisher.server) - reinterpret_cast<unsigned char *>(&publisher) == 0x1f28);
    Unit unit(&publisher);
    azsdk_loop_configure(0, 1);
    assert(!azsdk_loop_register(0, unit.bytes) && !azsdk_loop_capabilities());
    azsdk_loop_configure(1, 0);
    assert(!azsdk_loop_register(0, unit.bytes));
    azsdk_loop_configure(1, 3);
    assert(!azsdk_loop_register(0, unit.bytes));
    azsdk_loop_configure(1, 1);
    assert(!azsdk_loop_register(-1, unit.bytes) && !azsdk_loop_register(4, unit.bytes));
    assert(!azsdk_loop_register(0, nullptr) && !azsdk_loop_register(0, reinterpret_cast<void *>(1)));
    unchanged(-1);
    unchanged(4);
    unchanged(0);
    assert(!azsdk_loop_read(0, nullptr));
    /* A valid but wrongly typed owned publisher must be rejected before its server is read. */
    Unit wrong(&publisher);
    void *other = &server;
    std::memcpy(wrong.bytes + 0x1a0, &other, sizeof other);
    assert(!azsdk_loop_register(0, wrong.bytes));
    class A {
       public:
        virtual ~A() {}
    };
    A short_name;
    void *short_object = &short_name;
    std::memcpy(wrong.bytes + 0x1a0, &short_object, sizeof short_object);
    assert(!azsdk_loop_register(0, wrong.bytes));
    auto original_server = publisher.server;
    publisher.server = reinterpret_cast<dj_player::StateNotificationServer *>(&short_name);
    assert(!azsdk_loop_register(0, unit.bytes));
    publisher.server = original_server;
    server.fail = true;
    assert(!azsdk_loop_register(0, unit.bytes) && !azsdk_loop_capabilities());
    server.fail = false;
    server.fail_after = true;
    assert(!azsdk_loop_register(0, unit.bytes) && !azsdk_loop_capabilities());
    assert(server.listeners.size() == 1 && server.listeners.back().expired());
    server.listeners.clear();
    server.fail_after = false;
    assert(azsdk_loop_register(0, unit.bytes) && azsdk_loop_capabilities() == 1);
    assert(!azsdk_loop_register(0, unit.bytes) && server.listeners.size() == 1);
    unchanged(0); /* Submission does not promise an initial notification. */
    auto first = server.listeners.back().lock();
    assert(first && first.use_count() == 2);
    Notification n = notification(true);
    callback2(first, n);
    unchanged(0); /* Only callback3 carries native player identity. */
    emit(first, 1, 1);
    unchanged(0); /* Wrong player must never be accepted. */
    emit(first, 1, 0);
    azsdk_loop_sample on = read(0);
    assert(on.engaged == 1);
    emit(first, 0, 0);
    azsdk_loop_sample off = read(0);
    assert(!off.engaged && off.revision > on.revision && off.unit_generation == on.unit_generation &&
           off.observed_ns >= on.observed_ns);
    predicate_branches(first);
    bounds_branches(0, first);
    azsdk_loop_retire(0);
    assert(!azsdk_loop_capabilities());
    unchanged(0);
    assert(first.use_count() == 1); /* The in-flight strong lock survives core retirement. */
    emit(first, 1, 0);
    unchanged(0);
    assert(azsdk_loop_register(0, unit.bytes));
    auto second = server.listeners.back().lock();
    assert(second && second != first);
    emit(second, 0, 0);
    azsdk_loop_sample replacement = read(0);
    assert(replacement.unit_generation > off.unit_generation && !replacement.engaged);
    emit(first, 1, 0);
    azsdk_loop_sample after_old = read(0);
    assert(!after_old.engaged && after_old.unit_generation == replacement.unit_generation &&
           after_old.revision == replacement.revision);
    first.reset();
    assert(server.listeners.front().expired());
    /* Multiple callbacks/readers use only the scalar publication path; a final sequential
     * notification verifies recovery from any deliberately dropped contended write. */
    std::atomic<bool> go {false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 2; ++t) threads.emplace_back([&, t] {
        while (!go.load(std::memory_order_acquire)) {}
        Notification active = t ? bounds_notification(96000, 0, 96000) : bounds_notification();
        Notification mismatch = bounds_notification(48000, 100, 200);
        scalar64(mismatch, 0x70, 7);
        for (unsigned i = 0; i < 10000; ++i) {
            if (i % 3 == 0) emit_record(second, mismatch, 0);
            else if ((i + t) & 1u) emit_record(second, active, 0);
            else emit(second, false, 0);
        }
    });
    for (unsigned t = 0; t < 2; ++t) threads.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) {}
        std::uint64_t last_revision = 0;
        for (unsigned i = 0; i < 20000; ++i) {
            azsdk_loop_sample value;
            if (azsdk_loop_read(0, &value)) {
                assert(value.engaged == 0 || value.engaged == 1);
                assert(!value.check_reserved);
                if (value.bounds_valid) {
                    assert(value.engaged && !value.reserved);
                    assert((value.in_s == 0 && value.out_s == 1) ||
                           (value.in_s == 9217.0 / 44100 && value.out_s == 681239.0 / 44100));
                    if (value.check_rate == 96000)
                        assert(value.check_in_samples == 0 && value.check_out_samples == 96000 &&
                               value.check_in_cached_us == 0 && value.check_out_cached_us == 1000000);
                    else assert(value.check_rate == 44100 && value.check_in_samples == 9217 &&
                                value.check_out_samples == 681239 && value.check_in_cached_us == 209002 &&
                                value.check_out_cached_us == 15447596);
                } else {
                    assert(value.in_s == 0 && value.out_s == 0);
                    if (value.reserved == 12)
                        assert(value.engaged && value.check_rate == 48000 && value.check_in_samples == 100 &&
                               value.check_out_samples == 200 && value.check_in_cached_us == 7 && value.check_out_cached_us == 4167);
                    else assert(value.reserved == 1 && !value.engaged && !value.check_rate &&
                                !value.check_in_samples && !value.check_out_samples &&
                                !value.check_in_cached_us && !value.check_out_cached_us);
                }
                assert(value.unit_generation == replacement.unit_generation && value.observed_ns && value.revision);
                assert(value.revision >= last_revision);
                last_revision = value.revision;
            }
        }
    });
    go.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    emit(second, 1, 0);
    assert(read(0).engaged);
    std::atomic<bool> retiring {false};
    std::thread during_configuration([&] {
        Notification active = bounds_notification();
        retiring.store(true, std::memory_order_release);
        for (unsigned i = 0; i < 10000; ++i) emit_record(second, active, 0);
    });
    while (!retiring.load(std::memory_order_acquire)) {}
    azsdk_loop_configure(1, 2); /* Configuration changes invalidate every former generation. */
    during_configuration.join();
    unchanged(0);
    emit(second, 0, 0);
    unchanged(0);
    assert(azsdk_loop_register(3, unit.bytes) && azsdk_loop_capabilities() == 8);
    auto third = server.listeners.back().lock();
    emit(third, 1, 3);
    assert(read(3).engaged);
    azsdk_loop_retire(-1);
    azsdk_loop_retire(4);
    azsdk_loop_configure(0, 2);
    unchanged(3);
    emit(third, 0, 3);
    unchanged(3);
    second.reset();
    third.reset();
    for (const auto &entry : server.listeners) assert(entry.expired());
    server.listeners.clear();
    all_deck_bounds();
    std::puts("sdk loop observer: guarded GNU weak ABI, identity, generations, lifetime and concurrent publication passed");
}
#endif
