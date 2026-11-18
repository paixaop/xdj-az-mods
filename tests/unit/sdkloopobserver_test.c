/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic GNU native ABI and lifetime exercised in C. */
#include "azloopobserver.h"
#include "aztimestamp.h"
#include "loop_native_fixture.h"
#include <stdatomic.h>
#include <math.h>
#include <pthread.h>
static bool timestamp_equal(int order) {
    assert(order != AZ_TIME_INVALID);
    return order == AZ_TIME_EQUAL;
}
#define SAME_TIME(a, b, units, rate) timestamp_equal(compare_timestamps((a), (b), (units), (rate), 1))
typedef struct {
    _Alignas(uintptr_t) unsigned char bytes[0x100];
} Notification;
_Static_assert(sizeof(Notification) == 8 + 0xf8, "complete native loop notification");
_Static_assert(sizeof(struct azsdk_loop_sample) == 96, "private diagnostic scalar boundary");
static void set32(Notification *n, unsigned offset, uint32_t value) {
    assert(offset + 4 <= sizeof *n);
    for (unsigned i = 0; i < 4; ++i) n->bytes[offset + i] = ((unsigned char) (value >> (i * 8)));
}
static void set64(Notification *n, unsigned offset, uint64_t value) {
    assert(offset + 8 <= sizeof *n);
    for (unsigned i = 0; i < 8; ++i) n->bytes[offset + i] = ((unsigned char) (value >> (i * 8)));
}
static uint64_t get64(Notification n, unsigned offset) {
    uint64_t value;
    memcpy(&value, n.bytes + offset, sizeof value);
    return value;
}
static uint32_t get32(Notification n, unsigned offset) {
    uint32_t value;
    memcpy(&value, n.bytes + offset, sizeof value);
    return value;
}
static Notification notification(bool engaged, unsigned char mode) {
    Notification n = {0};
    n.bytes[0] = mode;
    if (engaged) {
        n.bytes[0x38] = n.bytes[0x3c] = n.bytes[0x3e] = 1;
        set32(&(n), 0x40, 1);
        set64(&(n), 0x48, 0); /* Time zero is valid; only INT64_MAX is invalid. */
    } else {
        set64(&(n), 0x48, UINT64_C(0x7fffffffffffffff));
    }
    return n;
}
static uintptr_t short_rtti(void) { return (uintptr_t) short_table.slots; }
static Notification bounds_notification(uint32_t rate, uint64_t in, uint64_t out) {
    Notification n = notification(true, 0);
    set64(&(n), 8, (uintptr_t) cue_table.slots);
    {
        const unsigned values[] = {0x20u, 0x78u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned point = values[index];
            set64(&(n), point, (uintptr_t) point_table.slots);
            set64(&(n), point + 0x10, (uintptr_t) position_table.slots);
            set64(&(n), point + 0x18, UINT64_C(0x0001010200000001));
            set32(&(n), point + 0x20, 4);
        /* Deliberately opaque and unaligned: the observer may compare, never follow it. */
            set64(&(n), point + 0x38, UINT64_C(0x10001));
            set64(&(n), point + 0x40, UINT64_C(0x10003));
            set32(&(n), point + 0x48, rate);
        }
    }
    set64(&(n), 0x48, in);
    set64(&(n), 0xa0, out);
    double scale = 1000000.0 / ((double) (rate));
    set64(&(n), 0x70, ((uint64_t) (round(scale * ((double) (in))))));
    set64(&(n), 0xc8, ((uint64_t) (round(scale * ((double) (out))))));
    return n;
}
static void unchanged(int deck) {
    struct azsdk_loop_sample value, before;
    memset(&value, 0xa5, sizeof value);
    before = value;
    assert(!azsdk_loop_read(deck, &value));
    assert(!memcmp(&value, &before, sizeof value));
}
static struct azsdk_loop_sample read(int deck) {
    struct Guarded {
        uint64_t before;
        struct azsdk_loop_sample value;
        uint64_t after;
    } out = {0};
    out.before = UINT64_C(0x1122334455667788);
    out.after = UINT64_C(0x8877665544332211);
    assert(azsdk_loop_read(deck, &out.value));
    assert(out.before == UINT64_C(0x1122334455667788) && out.after == UINT64_C(0x8877665544332211));
    assert(out.value.unit_generation && out.value.revision && out.value.observed_ns &&
           out.value.reserved >= 0 && out.value.reserved <= 13 && !out.value.reserved2 && !out.value.check_reserved);
    assert(out.value.bounds_valid == 0 || out.value.bounds_valid == 1);
    assert((out.value.bounds_valid && !out.value.reserved) || (!out.value.bounds_valid && out.value.reserved));
    if (!out.value.bounds_valid) assert(!out.value.in_s && !out.value.out_s);
    if (out.value.reserved > 0 && out.value.reserved < 12)
        assert(!out.value.check_in_samples && !out.value.check_out_samples &&
               !out.value.check_in_cached_us && !out.value.check_out_cached_us && !out.value.check_rate);
    else assert(out.value.check_rate);
    return out.value;
}
/* Reproduce the native dispatch ABI, rather than imposing the fixture's own type representation on
 * the separately reconstructed listener. Native code also calls these two vtable slots. */
static void callback2(Lease listener, Notification n) {
    uintptr_t *table;
    memcpy(&table, listener.object, sizeof table);
    typedef void (*Callback)(void *, const Notification *);
    ((Callback) (table[2]))(listener.object, &n);
}
static void emit_record(Lease listener, Notification n, unsigned player) {
    callback2(listener, n);
    uintptr_t *table;
    memcpy(&table, listener.object, sizeof table);
    typedef void (*Callback)(void *, const Notification *, unsigned);
    ((Callback) (table[3]))(listener.object, &n, player);
}
static void emit(Lease listener, bool engaged, unsigned player) {
    emit_record(listener, notification(engaged, 0), player);
}
static void expect_engaged(Lease listener, Notification n, int expected) {
    struct azsdk_loop_sample before = read(0);
    emit_record(listener, n, 0);
    struct azsdk_loop_sample after = read(0);
    assert(after.engaged == expected && after.unit_generation == before.unit_generation &&
           after.revision > before.revision && after.observed_ns >= before.observed_ns);
}
static void predicate_branches(Lease listener) {
    Notification active = notification(true, 0);
    expect_engaged(listener, active, 1); /* A current loop at time zero is engaged. */
    for (unsigned mode = 0; mode <= 1; ++mode) {
        expect_engaged(listener, notification(true, mode), 1);
        expect_engaged(listener, notification(false, mode), 0);
    }
    const unsigned byte_conditions[] = {0x38, 0x3c, 0x3e};
    {
        for (unsigned index = 0; index < sizeof byte_conditions / sizeof *byte_conditions; ++index) {
            unsigned offset = byte_conditions[index];
            Notification invalid = active;
            invalid.bytes[offset] = 0;
            expect_engaged(listener, invalid, 0);
        }
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
    set32(&(altered), 0x40, 0);
    expect_engaged(listener, altered, 0);
    set32(&(altered), 0x40, UINT32_C(0x80000000));
    expect_engaged(listener, altered, 1);
    altered = active;
    set64(&(altered), 0x48, UINT64_C(0x7fffffffffffffff));
    expect_engaged(listener, altered, 0);
    set64(&(altered), 0x48, UINT64_MAX);
    expect_engaged(listener, altered, 1); /* Exact native policy permits any other bit value. */
    /* A retained property is separate native state. EXIT clears the working copy even
     * while retained points remain valid; those points must never be substituted. */
    Notification retained = active;
    Notification exited = active;
    memset(exited.bytes + 0x38, 0, 8);
    set32(&(exited), 0x40, 0);
    set64(&(exited), 0x48, UINT64_C(0x7fffffffffffffff));
    assert(retained.bytes[0x38] && retained.bytes[0x3c] && retained.bytes[0x3e]);
    expect_engaged(listener, exited, 0);
    expect_engaged(listener, retained, 1); /* RELOOP restores the working property. */
    /* Header and unused payload bytes are not the property predicate. The full record
     * also ensures an erroneous property-base interpretation cannot pass this fixture. */
    altered = notification(false, 1);
    altered.bytes[0x30] = altered.bytes[0x34] = altered.bytes[0x36] = 1;
    expect_engaged(listener, altered, 0);
}
static void expect_bounds(unsigned deck, Lease listener, Notification n,
                          int engaged, bool valid, double in, double out) {
    struct azsdk_loop_sample peers[4] = {0};
    bool available[4] = {0};
    for (unsigned other = 0; other < 4; ++other)
        if (other != deck) available[other] = azsdk_loop_read(other, &peers[other]);
    struct azsdk_loop_sample before = read(deck);
    emit_record(listener, n, deck);
    struct azsdk_loop_sample after = read(deck);
    assert(after.engaged == engaged && after.bounds_valid == valid);
    assert(after.unit_generation == before.unit_generation && after.revision > before.revision);
    if (valid) assert(SAME_TIME(after.in_s, in, 1, after.check_rate) && SAME_TIME(after.out_s, out, 1, after.check_rate));
    else assert(!after.in_s && !after.out_s);
    if (valid || after.reserved >= 12) {
        assert(SAME_TIME(after.check_in_samples, get64(n, 0x48), after.check_rate, after.check_rate) && SAME_TIME(after.check_out_samples, get64(n, 0xa0), after.check_rate, after.check_rate));
        assert(SAME_TIME(after.check_in_cached_us, get64(n, 0x70), 1000000, after.check_rate) && SAME_TIME(after.check_out_cached_us, get64(n, 0xc8), 1000000, after.check_rate));
        assert(after.check_rate == get32(n, 0x68));
    }
    for (unsigned other = 0; other < 4; ++other)
        if (other != deck) {
            struct azsdk_loop_sample peer = {0};
            assert((bool) (azsdk_loop_read(other, &peer)) == available[other]);
            if (available[other]) assert(!memcmp(&peers[other], &peer, sizeof peer));
        }
}
static void bounds_branches(unsigned deck, Lease listener) {
    Notification valid = bounds_notification(44100, 9217, 681239);
    expect_bounds(deck, listener, valid, 1, true, 9217.0 / 44100, 681239.0 / 44100);
    /* Actual2.00 saved-loop recall: equal SourceInfo/rate, separate source owners. */
    Notification restored = bounds_notification(44100, 120785, 145031);
    set64(&(restored), 0x58, UINT64_C(0xffffa1c0bd80));
    set64(&(restored), 0xb0, UINT64_C(0xffffa1c0be80));
    set64(&(restored), 0x70, 2738889);
    set64(&(restored), 0xc8, 3288707);
    expect_bounds(deck, listener, restored, 1, true, 120785.0 / 44100, 145031.0 / 44100);
    {
        const unsigned values[] = {8u, 0x20u, 0x78u, 0x30u, 0x88u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned offset = values[index];
            Notification short_name = valid;
            set64(&(short_name), offset, short_rtti());
            expect_bounds(deck, listener, short_name, 1, false, 0, 0);
        }
    }
    /* Existing captured2.00 scalars; the fixture never reads that target. */
    uint64_t cached_in, cached_out;
    memcpy(&cached_in, valid.bytes + 0x70, 8);
    memcpy(&cached_out, valid.bytes + 0xc8, 8);
    assert(SAME_TIME(cached_in, 209002, 1000000, get32(valid, 0x68)) && SAME_TIME(cached_out, 15447596, 1000000, get32(valid, 0x68)));
    {
        const uint32_t values[] = {48000u, 96000u, 4000u, 768000u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            uint32_t rate = values[index];
            Notification n = bounds_notification(rate, 0, rate);
            expect_bounds(deck, listener, n, 1, true, 0, 1);
        }
    }
    expect_bounds(deck, listener, bounds_notification(48000, 0, 1), 1, true, 0, 1.0 / 48000);
    expect_bounds(deck, listener, bounds_notification(48000, 0, 14400000), 1, true, 0, 300);
    Notification half = bounds_notification(128, 1, 3); /* 7812.5us ->7813, not floor/even. */
    expect_bounds(deck, listener, half, 1, true, 1.0 / 128, 3.0 / 128);
    set64(&(half), 0x70, 7812);
    expect_bounds(deck, listener, half, 1, true, 1.0 / 128, 3.0 / 128);
    struct azsdk_loop_sample mismatch = read(deck);
    assert(mismatch.reserved == 0 && mismatch.check_rate == 128 && SAME_TIME(mismatch.check_in_samples, 1, mismatch.check_rate, mismatch.check_rate) &&
           SAME_TIME(mismatch.check_out_samples, 3, mismatch.check_rate, mismatch.check_rate) && SAME_TIME(mismatch.check_in_cached_us, 7812, 1000000, mismatch.check_rate) && SAME_TIME(mismatch.check_out_cached_us, 23438, 1000000, mismatch.check_rate));
    /* Physical EP1472.00 Deck2, a previous test: stable across three RELOOP receipts.
     * IN's cached1852000us differs from round(81673 * (1e6 / 44100))=1851995us.
     * The unrounded difference is only0.2 source sample. Keep source coordinates
     * sample-derived; a rounded cached timestamp must not make them unavailable. */
    Notification captured = bounds_notification(44100, 81673, 247048);
    set64(&(captured), 0x70, 1852000);
    set64(&(captured), 0xc8, 5601995);
    expect_bounds(deck, listener, captured, 1, true, 81673.0 / 44100, 247048.0 / 44100);
    mismatch = read(deck);
    assert(mismatch.reserved == 0 && mismatch.check_rate == 44100 &&
           SAME_TIME(mismatch.check_in_samples, 81673, mismatch.check_rate, mismatch.check_rate) && SAME_TIME(mismatch.check_out_samples, 247048, mismatch.check_rate, mismatch.check_rate) &&
           SAME_TIME(mismatch.check_in_cached_us, 1852000, 1000000, mismatch.check_rate) && SAME_TIME(mismatch.check_out_cached_us, 5601995, 1000000, mismatch.check_rate));
    /* Allow one sample's quantization plus half a cached microsecond, on either
     * side of either endpoint. The threshold follows rate, not a fixed epsilon. */
    struct Case {
        uint32_t rate;
        uint64_t accepted, rejected;
    };
    {
        const struct Case values[] = {(struct Case) {44100, 23, 24}, (struct Case) {48000, 21, 22}, (struct Case) {96000, 10, 11},
                                      (struct Case) {192000, 5, 6}, (struct Case) {768000, 1, 2}, (struct Case) {2000000, 1, 2},
                                      (struct Case) {2147483646, 0, 1}};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            struct Case c = values[index];
            {
                const unsigned values[] = {0x70u, 0xc8u};
                for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
                    unsigned offset = values[index];
                    Notification n = bounds_notification(c.rate, c.rate, 2ull * c.rate);
                    uint64_t exact = get64(n, offset);
                    {
                        const int values[] = {-1, 1};
                        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
                            int direction = values[index];
                            set64(&(n), offset, direction > 0 ? exact + c.accepted : exact - c.accepted);
                            expect_bounds(deck, listener, n, 1, true, 1, 2);
                            set64(&(n), offset, direction > 0 ? exact + c.rejected : exact - c.rejected);
                            expect_bounds(deck, listener, n, 1, false, 0, 0);
                            assert(read(deck).reserved == 12);
                            set64(&(n), offset, exact);
                        }
                    }
                }
            }
        }
    }
    /* Larger clocks must not hide a multi-sample discrepancy through double rounding. */
    Notification large = bounds_notification(1000000, UINT64_C(9007199254740988), UINT64_C(9007199254740992));
    set64(&(large), 0x70, UINT64_C(9007199254740990));
    expect_bounds(deck, listener, large, 1, false, 0, 0);
    assert(read(deck).reserved == 12);
    /* A subsequent valid record must recover; rejection cannot latch stale bounds. */
    expect_bounds(deck, listener, bounds_notification(44100, 81673, 247048), 1, true,
                  81673.0 / 44100, 247048.0 / 44100);
    {
        const unsigned values[] = {8u, 0x20u, 0x78u, 0x30u, 0x88u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned offset = values[index];
            Notification n = valid;
            set64(&(n), offset, 0);
            expect_bounds(deck, listener, n, 1, false, 0, 0);
            set64(&(n), offset, (uintptr_t) server_table.slots);
            expect_bounds(deck, listener, n, 1, false, 0, 0);
        }
    }
    /* Both native source-position predicates, including OUT independently of engagement. */
    {
        const unsigned values[] = {0x38u, 0x3cu, 0x3eu, 0x90u, 0x94u, 0x96u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned offset = values[index];
            Notification n = valid;
            n.bytes[offset] = 0;
            expect_bounds(deck, listener, n, offset < 0x90 ? 0 : 1, false, 0, 0);
        }
    }
    Notification n = valid;
    n.bytes[0x94] = 0xe0;
    expect_bounds(deck, listener, n, 1, false, 0, 0);
    {
        const unsigned values[] = {0x40u, 0x98u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned offset = values[index];
            n = valid;
            set32(&(n), offset, 0);
            expect_bounds(deck, listener, n, offset == 0x40 ? 0 : 1, false, 0, 0);
        }
    }
    n = valid;
    set32(&(n), 0x98, 5); /* Matching validity, different content/source tuple. */
    expect_bounds(deck, listener, n, 1, false, 0, 0);
    n = valid;
    n.bytes[0x91] ^= 1;
    expect_bounds(deck, listener, n, 1, false, 0, 0);
    {
        const unsigned values[] = {0x68u, 0xc0u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned offset = values[index];
            {
                const uint32_t values[] = {0u, UINT32_C(0x7fffffff), UINT32_MAX, 48000u};
                for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
                    uint32_t rate = values[index];
                    n = valid;
                    set32(&(n), offset, rate);
                    expect_bounds(deck, listener, n, 1, false, 0, 0);
                }
            }
        }
    }
    {
        const uint32_t values[] = {0u, UINT32_C(0x7fffffff), UINT32_MAX};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            uint32_t rate = values[index];
            n = valid;
            set32(&(n), 0x68, rate);
            set32(&(n), 0xc0, rate);
            expect_bounds(deck, listener, n, 1, false, 0, 0);
        }
    }
    {
        const unsigned values[] = {0x48u, 0xa0u, 0x70u, 0xc8u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned offset = values[index];
            {
                const uint64_t values[] = {UINT64_MAX, UINT64_C(0x7fffffffffffffff), UINT64_C(9007199254740993)};
                for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
                    uint64_t value = values[index];
                    n = valid;
                    set64(&(n), offset, value);
                    expect_bounds(deck, listener, n, offset == 0x48 && value == UINT64_C(0x7fffffffffffffff) ? 0 : 1, false, 0, 0);
                }
            }
        }
    }
    {
        const uint64_t values[] = {UINT64_C(9217), UINT64_C(1)};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            uint64_t out = values[index];
            n = valid;
            set64(&(n), 0xa0, out);
            expect_bounds(deck, listener, n, 1, false, 0, 0);
        }
    }
    {
        const unsigned values[] = {0x70u, 0xc8u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned offset = values[index];
            n = valid;
            set64(&(n), offset, 0);
            expect_bounds(deck, listener, n, 1, false, 0, 0);
        }
    }
    {
        const unsigned values[] = {0x58u, 0xb0u};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            unsigned offset = values[index];
            n = valid;
            set64(&(n), offset, 0);
            expect_bounds(deck, listener, n, 1, false, 0, 0);
            set64(&(n), offset, 0x10005);
            expect_bounds(deck, listener, n, 1, true, 9217.0 / 44100, 681239.0 / 44100);
        }
    }
    /* The bool supplied to each native setter is not a native position-valid flag. */
    n = valid;
    n.bytes[0x28] = n.bytes[0x80] = 0;
    expect_bounds(deck, listener, n, 1, true, 9217.0 / 44100, 681239.0 / 44100);
    n.bytes[0] = 1;
    expect_bounds(deck, listener, n, 1, true, 9217.0 / 44100, 681239.0 / 44100);
    Notification retained = valid;
    Notification exited = valid;
    memset(exited.bytes + 0x38, 0, 12);
    set64(&(exited), 0x48, UINT64_C(0x7fffffffffffffff));
    expect_bounds(deck, listener, exited, 0, false, 0, 0);
    expect_bounds(deck, listener, retained, 1, true, 9217.0 / 44100, 681239.0 / 44100);
}
/* Run the entire bounds/error/rounding matrix through each independently owned
 * listener, with all four decks registered concurrently, for both supported build
 * configurations. Captured numbers are ordinary inputs on every deck slot. */
static void all_deck_bounds() {
    {
        const int values[] = {1, 2};
        for (unsigned index = 0; index < sizeof values / sizeof *values; ++index) {
            int build = values[index];
            struct Server servers[4] __attribute__((cleanup(servers_clear))) = {0};
            for (unsigned i = 0; i < 4; ++i) server_init(&servers[i]);
            struct Publisher publishers[4];
            for (unsigned i = 0; i < 4; ++i) publisher_init(&publishers[i]);
            Unit units[4];
            Lease listeners[4] __attribute__((cleanup(leases_drop))) = {0};
            azsdk_loop_configure(1, build);
            for (unsigned deck = 0; deck < 4; ++deck) {
                publishers[deck].server = &servers[deck];
                units[deck] = unit_create(&publishers[deck]);
                assert(azsdk_loop_register(deck, units[deck].bytes));
                listeners[deck] = get_lease(&servers[deck]);
                assert(listeners[deck].object);
                uint32_t rate = 48000 + deck * 16000;
                Notification seed = bounds_notification(rate, (deck + 1ull) * rate, (deck + 2ull) * rate);
                emit_record(listeners[deck], seed, deck);
                assert(SAME_TIME(read(deck).in_s, deck + 1, 1, rate) && SAME_TIME(read(deck).out_s, deck + 2, 1, rate));
            }
            assert(azsdk_loop_capabilities() == 15);
            for (unsigned deck = 0; deck < 4; ++deck) {
                bounds_branches(deck, listeners[deck]);
                struct azsdk_loop_sample before[4];
                for (unsigned other = 0; other < 4; ++other) before[other] = read(other);
                for (unsigned wrong = 0; wrong < 4; ++wrong)
                    if (wrong != deck) {
                        emit_record(listeners[deck], bounds_notification(44100, 9217, 681239), wrong);
                        for (unsigned other = 0; other < 4; ++other) {
                            struct azsdk_loop_sample after = read(other);
                            assert(!memcmp(&before[other], &after, sizeof after));
                        }
                    }
                Lease retired __attribute__((cleanup(fixture_drop))) = fixture_copy(listeners[deck]);
                fixture_drop(&listeners[deck]);
                azsdk_loop_retire(deck);
                unchanged(deck);
                assert(azsdk_loop_capabilities() == (15u & ~(1u << deck)));
                assert(azsdk_loop_register(deck, units[deck].bytes));
                listeners[deck] = get_lease(&servers[deck]);
                emit_record(listeners[deck], bounds_notification(44100, 9217, 681239), deck);
                struct azsdk_loop_sample replacement = read(deck);
                assert(replacement.unit_generation > before[deck].unit_generation);
                emit_record(retired, notification(false, 0), deck);
                struct azsdk_loop_sample after = read(deck);
                assert(!memcmp(&replacement, &after, sizeof after));
                for (unsigned other = 0; other < 4; ++other)
                    if (other != deck) {
                        after = read(other);
                        assert(!memcmp(&before[other], &after, sizeof after));
                    }
            }
            azsdk_loop_configure(0, build);
            for (unsigned deck = 0; deck < 4; ++deck) {
                fixture_drop(&listeners[deck]);
                unchanged(deck);
                for (unsigned i = 0; i < servers[deck].count; ++i) assert(fixture_expired(servers[deck].listeners[i]));
            }
        }
    }
    puts("sdk loop observer: all bounds cases passed on decks1-4, both build configurations, with cross-deck isolation");
}
static struct Server *early_server;
static struct Publisher *early_publisher;
static Unit *early_unit;
/* Configure, register and publish from an early ELF constructor, then verify
 * at main that startup preserves the publication and listener ownership. GNU sanitizer runtime initialization precedes priority 101. */
__attribute__((constructor(101))) static void early_configuration() {
    early_server = calloc(1, sizeof *early_server);
    assert(early_server);
    server_init(early_server);
    early_publisher = calloc(1, sizeof *early_publisher);
    assert(early_publisher);
    publisher_init(early_publisher);
    early_publisher->server = early_server;
    early_unit = malloc(sizeof *early_unit);
    assert(early_unit);
    *early_unit = unit_create(early_publisher);
    azsdk_loop_configure(1, 2);
    assert(azsdk_loop_register(2, early_unit->bytes));
    Lease listener __attribute__((cleanup(fixture_drop))) = get_lease(early_server);
    assert(listener.object);
    emit(listener, 1, 2);
}
struct worker_group {
    Lease second;
    uint64_t generation;
    _Atomic(bool) go, retiring;
};
struct worker_context {
    struct worker_group *group;
    unsigned index;
};
static void *writer_worker(void *opaque) {
    struct worker_context *context = opaque;
    while (!atomic_load_explicit(&context->group->go, memory_order_acquire)) {}
    Notification active = context->index ? bounds_notification(96000, 0, 96000) : bounds_notification(44100, 9217, 681239);
    Notification mismatch = bounds_notification(48000, 100, 200);
    set64(&(mismatch), 0x70, 7);
    for (unsigned i = 0; i < 10000; ++i) {
        if (i % 3 == 0) emit_record(context->group->second, mismatch, 0);
        else if ((i + context->index) & 1u) emit_record(context->group->second, active, 0);
        else emit(context->group->second, false, 0);
    }

    return NULL;
}
static void *reader_worker(void *opaque) {
    struct worker_context *context = opaque;
    while (!atomic_load_explicit(&context->group->go, memory_order_acquire)) {}
    uint64_t last_revision = 0;
    for (unsigned i = 0; i < 20000; ++i) {
        struct azsdk_loop_sample value;
        if (azsdk_loop_read(0, &value)) {
            assert(value.engaged == 0 || value.engaged == 1);
            assert(!value.check_reserved);
            if (value.bounds_valid) {
                assert(value.engaged && !value.reserved);
                assert((SAME_TIME(value.in_s, 0, 1, value.check_rate) && SAME_TIME(value.out_s, 1, 1, value.check_rate)) ||
                       (SAME_TIME(value.in_s, 9217.0 / 44100, 1, value.check_rate) && SAME_TIME(value.out_s, 681239.0 / 44100, 1, value.check_rate)));
                if (value.check_rate == 96000)
                    assert(SAME_TIME(value.check_in_samples, 0, value.check_rate, value.check_rate) && SAME_TIME(value.check_out_samples, 96000, value.check_rate, value.check_rate) &&
                           SAME_TIME(value.check_in_cached_us, 0, 1000000, value.check_rate) && SAME_TIME(value.check_out_cached_us, 1000000, 1000000, value.check_rate));
                else assert(value.check_rate == 44100 && SAME_TIME(value.check_in_samples, 9217, value.check_rate, value.check_rate) &&
                            SAME_TIME(value.check_out_samples, 681239, value.check_rate, value.check_rate) && SAME_TIME(value.check_in_cached_us, 209002, 1000000, value.check_rate) &&
                            SAME_TIME(value.check_out_cached_us, 15447596, 1000000, value.check_rate));
            } else {
                assert(!value.in_s && !value.out_s);
                if (value.reserved == 12)
                    assert(value.engaged && value.check_rate == 48000 && SAME_TIME(value.check_in_samples, 100, value.check_rate, value.check_rate) &&
                           SAME_TIME(value.check_out_samples, 200, value.check_rate, value.check_rate) && SAME_TIME(value.check_in_cached_us, 7, 1000000, value.check_rate) && SAME_TIME(value.check_out_cached_us, 4167, 1000000, value.check_rate));
                else assert(value.reserved == 1 && !value.engaged && !value.check_rate &&
                            !value.check_in_samples && !value.check_out_samples &&
                            !value.check_in_cached_us && !value.check_out_cached_us);
            }
            assert(value.unit_generation == context->group->generation && value.observed_ns && value.revision);
            assert(value.revision >= last_revision);
            last_revision = value.revision;
        }
    }

    return NULL;
}
static void *retire_worker(void *opaque) {
    struct worker_group *context = opaque;
    Notification active = bounds_notification(44100, 9217, 681239);
    atomic_store_explicit(&context->retiring, true, memory_order_release);
    for (unsigned i = 0; i < 10000; ++i) emit_record(context->second, active, 0);
    return NULL;
}
int main(void) {
    assert(early_unit && early_publisher && early_server && azsdk_loop_capabilities() == 4);
    assert(read(2).engaged);
    Lease early __attribute__((cleanup(fixture_drop))) = get_lease(early_server);
    assert(early.object && early.control->strong == 2);
    azsdk_loop_retire(2);
    fixture_drop(&early);
    assert(last_expired(early_server));
    free(early_unit);
    free(early_publisher);
    server_clear(early_server);
    free(early_server);
    struct Server server __attribute__((cleanup(server_clear)));
    server_init(&server);
    struct Publisher publisher;
    publisher_init(&publisher);
    publisher.server = &server;
    assert(((unsigned char *) (&publisher.server)) - ((unsigned char *) (&publisher)) == 0x1f28);
    Unit unit = unit_create(&publisher);
    azsdk_loop_configure(0, 1);
    assert(!azsdk_loop_register(0, unit.bytes) && !azsdk_loop_capabilities());
    azsdk_loop_configure(1, 0);
    assert(!azsdk_loop_register(0, unit.bytes));
    azsdk_loop_configure(1, 3);
    assert(!azsdk_loop_register(0, unit.bytes));
    azsdk_loop_configure(1, 1);
    assert(!azsdk_loop_register(-1, unit.bytes) && !azsdk_loop_register(4, unit.bytes));
    assert(!azsdk_loop_register(0, NULL) && !azsdk_loop_register(0, ((void *) (1))));
    unchanged(-1);
    unchanged(4);
    unchanged(0);
    assert(!azsdk_loop_read(0, NULL));
    /* A valid but wrongly typed owned publisher must be rejected before its server is read. */
    Unit wrong = unit_create(&publisher);
    void *other = &server;
    memcpy(wrong.bytes + 0x1a0, &other, sizeof other);
    assert(!azsdk_loop_register(0, wrong.bytes));
    struct {
        const void *const *vtable;
    } short_object_storage = {short_table.slots};
    void *short_object = &short_object_storage;
    memcpy(wrong.bytes + 0x1a0, &short_object, sizeof short_object);
    assert(!azsdk_loop_register(0, wrong.bytes));
    __auto_type original_server = publisher.server;
    publisher.server = ((struct Server *) (&short_object_storage));
    assert(!azsdk_loop_register(0, unit.bytes));
    publisher.server = original_server;
    server.fail = true;
    assert(!azsdk_loop_register(0, unit.bytes) && !azsdk_loop_capabilities());
    server.fail = false;
    server.fail_after = true;
    assert(!azsdk_loop_register(0, unit.bytes) && !azsdk_loop_capabilities());
    assert(server.count == 1 && last_expired(&server));
    server_clear(&server);
    server.fail_after = false;
    assert(azsdk_loop_register(0, unit.bytes) && azsdk_loop_capabilities() == 1);
    assert(!azsdk_loop_register(0, unit.bytes) && server.count == 1);
    unchanged(0); /* Submission does not promise an initial notification. */
    Lease first __attribute__((cleanup(fixture_drop))) = get_lease(&server);
    assert(first.object && first.control->strong == 2);
    Notification n = notification(true, 0);
    callback2(first, n);
    unchanged(0); /* Only callback3 carries native player identity. */
    emit(first, 1, 1);
    unchanged(0); /* Wrong player must never be accepted. */
    emit(first, 1, 0);
    struct azsdk_loop_sample on = read(0);
    assert(on.engaged == 1);
    emit(first, 0, 0);
    struct azsdk_loop_sample off = read(0);
    assert(!off.engaged && off.revision > on.revision && off.unit_generation == on.unit_generation &&
           off.observed_ns >= on.observed_ns);
    predicate_branches(first);
    bounds_branches(0, first);
    azsdk_loop_retire(0);
    assert(!azsdk_loop_capabilities());
    unchanged(0);
    assert(first.control->strong == 1); /* The in-flight strong lock survives core retirement. */
    emit(first, 1, 0);
    unchanged(0);
    assert(azsdk_loop_register(0, unit.bytes));
    Lease second __attribute__((cleanup(fixture_drop))) = get_lease(&server);
    assert(second.object && second.object != first.object);
    emit(second, 0, 0);
    struct azsdk_loop_sample replacement = read(0);
    assert(replacement.unit_generation > off.unit_generation && !replacement.engaged);
    emit(first, 1, 0);
    struct azsdk_loop_sample after_old = read(0);
    assert(!after_old.engaged && after_old.unit_generation == replacement.unit_generation &&
           after_old.revision == replacement.revision);
    fixture_drop(&first);
    assert(fixture_expired(server.listeners[0]));
    /* Multiple callbacks/readers use only the scalar publication path; a final sequential
     * notification verifies recovery from any deliberately dropped contended write. */
    struct worker_group group = {.second = second, .generation = replacement.unit_generation};
    struct worker_context contexts[4];
    pthread_t threads[4];
    for (unsigned t = 0; t < 4; ++t) {
        contexts[t] = (struct worker_context) {&group, t % 2};
        assert(!pthread_create(&threads[t], NULL, t < 2 ? writer_worker : reader_worker, &contexts[t]));
    }
    atomic_store_explicit(&group.go, true, memory_order_release);
    for (unsigned t = 0; t < 4; ++t) assert(!pthread_join(threads[t], NULL));
    emit(second, 1, 0);
    assert(read(0).engaged);
    pthread_t during_configuration;
    assert(!pthread_create(&during_configuration, NULL, retire_worker, &group));
    while (!atomic_load_explicit(&group.retiring, memory_order_acquire)) {}
    azsdk_loop_configure(1, 2); /* Configuration changes invalidate every former generation. */
    assert(!pthread_join(during_configuration, NULL));
    unchanged(0);
    emit(second, 0, 0);
    unchanged(0);
    assert(azsdk_loop_register(3, unit.bytes) && azsdk_loop_capabilities() == 8);
    Lease third __attribute__((cleanup(fixture_drop))) = get_lease(&server);
    emit(third, 1, 3);
    assert(read(3).engaged);
    azsdk_loop_retire(-1);
    azsdk_loop_retire(4);
    azsdk_loop_configure(0, 2);
    unchanged(3);
    emit(third, 0, 3);
    unchanged(3);
    fixture_drop(&second);
    fixture_drop(&third);
    for (unsigned i = 0; i < server.count; ++i) assert(fixture_expired(server.listeners[i]));
    server_clear(&server);
    all_deck_bounds();
    puts("sdk loop observer: guarded GNU weak ABI, identity, generations, lifetime and concurrent publication passed");
}
