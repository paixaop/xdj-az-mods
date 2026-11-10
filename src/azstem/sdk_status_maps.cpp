/* SPDX-License-Identifier: GPL-3.0-only */
/* The native GUI supplies two genuine strong references when installing its
 * TrackStructureData pair. The audio-side pack registry independently proves
 * deck and load membership. Only counted immutable Content leaves the writer. */
#include "../common/azstatus_native.h"
#include "../common/azstatus_owned.h"
#include "../common/aztimestamp.h"
#include "../azmod/azmod-mod.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
struct Profile {
    uintptr_t pack, grid_holder, grid_content, phrase_holder, phrase_content;
};
constexpr Profile profiles[] = {
    {0x2612530, 0x2754518, 0x2798328, 0x29280a0, 0x2797d30},
    {0x294c9b0, 0x2a9cfa8, 0x2ae4df8, 0x2c99430, 0x2ae4800}};
const Profile *profile;
template <class T>
T read_value(uintptr_t object, unsigned offset = 0) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(object + offset), sizeof value);
    return value;
}
bool typed(uintptr_t object, uintptr_t vtable) {
    return object >= 0x10000 && !(object & 7) && read_value<uintptr_t>(object) == vtable;
}
constexpr unsigned slots = 32;
constexpr uint64_t routes = AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) |
                            AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
struct Object {
    uintptr_t address = 0;
    uint64_t token = 0;
};
struct Pair {
    Object pack, holder[2], content[2];
    uintptr_t owners[8] = {};
    uint64_t binding = 0;
    aznm_packet packet {};
};
/* Several logical decks can share a single active pack. Each GUI owner
 * supplies an independent membership witness for the same two holders. */
bool witnessed(const Pair &p, uintptr_t owner) {
    for (auto id : p.owners)
        if (id && id == owner) return true;
    return false;
}
bool remember(Pair &p, uintptr_t owner) {
    if (!owner || witnessed(p, owner)) return true;
    for (auto &id : p.owners)
        if (!id) {
            id = owner;
            return true;
        }
    return false;
}
bool forget(Pair &p, uintptr_t owner) {
    bool remaining = false;
    for (auto &id : p.owners) {
        if (id == owner) id = 0;
        remaining |= id != 0;
    }
    return remaining;
}
struct Slot {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::atomic<bool> deck_only {false};
    Pair pair;
    /* Head validation must not reserve the native writer's cache gate. These
     * atomic identities refer only to counted pins, never borrowed pointers.
     * A reader holds each exact incarnation before examining native members. */
    struct Published {
        std::atomic<uint64_t> serial {0}, owner {0}, tokens[5] {}, providers[2] {}, revisions[2] {};
        std::atomic<int64_t> offset {0};
        std::atomic<uint32_t> rate {0};
    } published;
};
Slot cache[slots];
std::atomic<uint64_t> identities {0};
std::atomic<bool> enabled {false};
enum {
    PROOF_ENTER,
    PROOF_TYPE,
    PROOF_PIN,
    PROOF_MEMBER,
    PROOF_CACHE,
    PROOF_EMIT,
    PROOF_DIAGS
};
std::atomic<uint64_t> proof_counts[PROOF_DIAGS];
unsigned bucket(uintptr_t pack) { return unsigned((pack >> 6) % slots); }
uint64_t identity() {
    uint64_t v = identities.fetch_add(1, std::memory_order_relaxed) + 1;
    return v && v <= AZSP_SERIAL_MAX ? AZSP_MAP | v : 0;
}
struct Lock {
    Slot &slot;
    bool held;
    explicit Lock(Slot &s) : slot(s), held(!s.busy.test_and_set(std::memory_order_acquire)) {}
    ~Lock() {
        if (held) slot.busy.clear(std::memory_order_release);
    }
};
void discard(Pair &p) {
    azn_pin_drop(p.pack.token);
    for (unsigned i = 0; i < 2; i++) {
        azn_pin_drop(p.holder[i].token);
        azn_pin_drop(p.content[i].token);
    }
    p = Pair {};
}
bool pin(Object &o, uintptr_t address) {
    o.address = address;
    o.token = azn_owned_pin(address);
    return o.token != 0;
}
void release_packet(const aznm_packet *p) {
    if (p) {
        azn_pin_drop(p->grid.content);
        azn_pin_drop(p->phrases.content);
    }
}
bool copy_packet(const Pair &p, aznm_packet &out) {
    out = p.packet;
    out.grid.content = out.phrases.content = 0;
    uintptr_t ignored;
    uint64_t tokens[] = {p.content[0].token, p.content[1].token};
    for (unsigned i = 0; i < 2; i++) {
        if (!tokens[i]) continue;
        if (!azn_pin_hold(tokens[i], &ignored)) {
            release_packet(&out);
            return false;
        }
        (i ? out.phrases : out.grid).content = tokens[i];
    }
    return true;
}
/* Grid origins and cells can precede source zero. These are individually
 * committed integer samples. Shift the signed origin without losing bits. */
int committed_grid_order(int64_t first, int64_t second, uint32_t rate) {
    return compare_timestamps(uint64_t(first) ^ UINT64_C(0x8000000000000000),
                              uint64_t(second) ^ UINT64_C(0x8000000000000000), rate, rate, 1);
}
bool sample_matches(const Pair &p) {
    if (!p.pack.address) return false;
    for (unsigned i = 0; i < 2; i++) {
        uintptr_t h = p.holder[i].address;
        if (read_value<uintptr_t>(p.pack.address, i ? 0x120 : 0xd8) != h ||
            (h && read_value<uintptr_t>(h, 0x28) != p.content[i].address)) return false;
    }
    uintptr_t grid = p.holder[0].address;
    return !grid || (read_value<uint32_t>(grid, 0x40) == p.packet.grid.sample_rate &&
                     committed_grid_order(read_value<int64_t>(grid, 0x30), p.packet.grid.offset_samples,
                                          p.packet.grid.sample_rate) == AZ_TIME_EQUAL);
}
void publish_identity(Slot &slot, const Pair &p) {
    auto &out = slot.published;
    uint64_t serial = out.serial.load(std::memory_order_relaxed);
    /* Exhaustion permanently closes this snapshot. Never recycle a sequence
     * while a reader might still hold an earlier edition of the same slot. */
    if (serial >= UINT64_MAX - 2) {
        out.serial.store(UINT64_MAX, std::memory_order_release);
        return;
    }
    out.serial.fetch_add(1, std::memory_order_acq_rel);
    /* Pair with the reader's acquire fence through any relaxed field it
     * observes from this edition, so its final serial read sees this cut. */
    std::atomic_thread_fence(std::memory_order_release);
    const uint64_t tokens[] = {p.pack.token, p.holder[0].token, p.holder[1].token,
                               p.content[0].token, p.content[1].token};
    for (unsigned i = 0; i < 5; ++i) out.tokens[i].store(tokens[i], std::memory_order_relaxed);
    out.owner.store(p.packet.owner, std::memory_order_relaxed);
    for (unsigned i = 0; i < 2; ++i) {
        const auto &part = i ? p.packet.phrases : p.packet.grid;
        out.providers[i].store(part.provider, std::memory_order_relaxed);
        out.revisions[i].store(part.revision, std::memory_order_relaxed);
    }
    out.offset.store(p.packet.grid.offset_samples, std::memory_order_relaxed);
    out.rate.store(p.packet.grid.sample_rate, std::memory_order_relaxed);
    out.serial.store(serial + 2, std::memory_order_release);
}
bool current_identity(unsigned d, const azn_pack_view &view, const aznm_packet &stamp) {
    const auto &in = cache[bucket(view.object)].published;
    uint64_t serial = in.serial.load(std::memory_order_acquire);
    if (!serial || (serial & 1) || in.owner.load(std::memory_order_relaxed) != stamp.owner) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const auto &part = i ? stamp.phrases : stamp.grid;
        if (in.providers[i].load(std::memory_order_relaxed) != part.provider ||
            in.revisions[i].load(std::memory_order_relaxed) != part.revision) return false;
    }
    Pair held;
    struct Drop { Pair &pair; ~Drop() { discard(pair); } } drop {held};
    Object *objects[] = {&held.pack, &held.holder[0], &held.holder[1], &held.content[0], &held.content[1]};
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t token = in.tokens[i].load(std::memory_order_relaxed);
        if (!token) continue;
        if (!azn_pin_hold(token, &objects[i]->address)) return false;
        objects[i]->token = token;
    }
    held.packet.grid.offset_samples = in.offset.load(std::memory_order_relaxed);
    held.packet.grid.sample_rate = in.rate.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (serial != in.serial.load(std::memory_order_relaxed) || held.pack.address != view.object) return false;
    bool valid = sample_matches(held) && azn_pack_current(d, &view);
    std::atomic_thread_fence(std::memory_order_acquire);
    return valid && serial == in.serial.load(std::memory_order_relaxed);
}
void emit(unsigned deck, const azn_pack_view &view, const Pair &p) {
    proof_counts[PROOF_EMIT].fetch_add(1, std::memory_order_relaxed);
    if (!(azn_interest(deck) & routes) || !azn_pack_current(deck, &view)) return;
    azstatus_native_receipt receipt {};
    receipt.kind = AZN_MAPS;
    receipt.deck = deck;
    receipt.unit = view.unit;
    receipt.epoch = view.fence;
    receipt.load_generation = view.load;
    if (!copy_packet(p, receipt.map)) {
        azn_map_gap(deck);
        return;
    }
    if (!azn_offer_map_cut(&receipt)) release_packet(&receipt.map);
}
void release_pair(Slot &slot) {
    publish_identity(slot, Pair {});
    discard(slot.pair);
    slot.deck_only.store(false, std::memory_order_release);
}
void reap_deck_only_pairs() {
    for (auto &slot : cache) {
        if (!slot.deck_only.load(std::memory_order_acquire)) continue;
        Lock lock(slot);
        if (lock.held && slot.deck_only.load(std::memory_order_relaxed) &&
            !azn_pack_retains(slot.pair.pack.address)) release_pair(slot);
    }
}
void seed(unsigned deck, uint64_t, uint64_t, uint64_t load) {
    if (!enabled.load(std::memory_order_acquire) || !load || deck >= 4) return;
    azn_pack_view view {};
    if (!azn_pack_acquire(deck, &view)) return;
    struct Drop {
        uint64_t t;
        ~Drop() { azn_pin_drop(t); }
    } drop {view.token};
    Slot &slot = cache[bucket(view.object)];
    Lock lock(slot);
    if (!lock.held) {
        azn_map_gap(deck);
        return;
    }
    Pair &p = slot.pair;
    if (view.load == load && p.pack.address == view.object && sample_matches(p)) emit(deck, view, p);
}
void pack_changed(unsigned deck, uint64_t unit, uint64_t fence, uint64_t load) {
    reap_deck_only_pairs();
    seed(deck, unit, fence, load);
}
void request_seed(unsigned d) {
    if (d >= 4) return;
    azn_pack_view view {};
    if (!azn_pack_acquire(d, &view)) return;
    seed(d, view.unit, view.fence, view.load);
    azn_pin_drop(view.token);
}
void affected_gap(uintptr_t pack) {
    for (unsigned d = 0; d < 4; d++) {
        azn_pack_view v {};
        if (azn_pack_acquire(d, &v)) {
            if (v.object == pack) azn_map_gap(d);
            azn_pin_drop(v.token);
        }
    }
}
int update_pair(uintptr_t owner, uintptr_t pack, uintptr_t gh, uintptr_t gc, uintptr_t ph, uintptr_t pc, uint64_t existing) {
    /* Every nonzero argument is borrowed from the observed native call. */
    proof_counts[PROOF_ENTER].fetch_add(1, std::memory_order_relaxed);
    if (!enabled.load(std::memory_order_acquire) || (!owner && !existing) || !typed(pack, profile->pack) ||
        (!gh && !ph) || (gc && !gh) || (pc && !ph) ||
        (gh && !typed(gh, profile->grid_holder)) || (gc && !typed(gc, profile->grid_content)) ||
        (ph && !typed(ph, profile->phrase_holder)) || (pc && !typed(pc, profile->phrase_content))) {
        proof_counts[PROOF_TYPE].fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    Pair next;
    if (!pin(next.pack, pack) || (gh && !pin(next.holder[0], gh)) || (gc && !pin(next.content[0], gc)) ||
        (ph && !pin(next.holder[1], ph)) || (pc && !pin(next.content[1], pc))) {
        proof_counts[PROOF_PIN].fetch_add(1, std::memory_order_relaxed);
        discard(next);
        return 0;
    }
    next.packet.grid.offset_samples = gh ? read_value<int64_t>(gh, 0x30) : 0;
    next.packet.grid.sample_rate = gh ? read_value<uint32_t>(gh, 0x40) : 0;
    if (!sample_matches(next)) {
        proof_counts[PROOF_MEMBER].fetch_add(1, std::memory_order_relaxed);
        discard(next);
        return 0;
    }
    Slot &slot = cache[bucket(pack)];
    {
        Lock lock(slot);
        if (!lock.held) {
            affected_gap(pack);
            discard(next);
            return 0;
        }
        Pair &old = slot.pair;
        bool same = old.pack.address == pack;
        if ((existing && (!same || old.binding != existing || (owner && !witnessed(old, owner)) || old.holder[0].address != gh || old.holder[1].address != ph)) ||
            !sample_matches(next)) {
            discard(next);
            return 0;
        }
        bool same_pair = same && old.holder[0].address == gh && old.holder[1].address == ph;
        if (same_pair) std::memcpy(next.owners, old.owners, sizeof next.owners);
        if (!remember(next, owner)) {
            affected_gap(pack);
            discard(next);
            return 0;
        }
        next.binding = same_pair ? old.binding : identity();
        if (!next.binding) {
            discard(next);
            return 0;
        }
        if (gc && pc) {
            next.packet.owner = same_pair && old.packet.owner ? old.packet.owner : identity();
            if (!next.packet.owner) {
                discard(next);
                return 0;
            }
        }
        for (unsigned i = 0; i < 2; i++) {
            aznm_component &n = i ? next.packet.phrases : next.packet.grid;
            const aznm_component &o = i ? old.packet.phrases : old.packet.grid;
            if (!next.holder[i].address && (!same || !o.provider)) continue;
            bool same_holder = same && o.provider && old.holder[i].address == next.holder[i].address;
            n.provider = same_holder || (!next.holder[i].address && o.provider) ? o.provider : identity();
            bool changed = !same_holder || old.content[i].address != next.content[i].address ||
                           (!i && (n.sample_rate != o.sample_rate ||
                                   committed_grid_order(n.offset_samples, o.offset_samples, n.sample_rate) != AZ_TIME_EQUAL));
            if (!n.provider || (changed && o.revision == UINT64_MAX)) {
                discard(next);
                return 0;
            }
            n.revision = same_holder ? o.revision + changed : 1;
            n.availability = next.content[i].address ? AZ_AVAIL_READY : next.holder[i].address ? AZ_AVAIL_LOADING
                                                                                               : AZ_AVAIL_STALE;
            if (changed) next.packet.changed |= i ? AZNM_PHRASES : AZNM_GRID;
        }
        if (!same || next.packet.owner != old.packet.owner || next.packet.changed) next.packet.changed |= AZNM_PAIR;
        if (!next.packet.changed) {
            std::memcpy(old.owners, next.owners, sizeof old.owners);
            if (owner) slot.deck_only.store(false, std::memory_order_release);
            discard(next);
            return 1;
        }
        publish_identity(slot, next);
        discard(old);
        old = next;
        if (owner) slot.deck_only.store(false, std::memory_order_release);
        proof_counts[PROOF_CACHE].fetch_add(1, std::memory_order_relaxed);
        for (unsigned d = 0; d < 4; d++) {
            azn_pack_view v {};
            if (azn_pack_acquire(d, &v)) {
                if (v.object == pack && sample_matches(old)) emit(d, v, old);
                azn_pin_drop(v.token);
            }
        }
    }
    return 1;
}
int prove_pair(uintptr_t owner, uintptr_t pack, uintptr_t gh, uintptr_t gc, uintptr_t ph, uintptr_t pc) {
    return update_pair(owner, pack, gh, gc, ph, pc, 0);
}
int refresh_pair(uintptr_t owner, uintptr_t gh, uintptr_t gc, uintptr_t ph, uintptr_t pc) {
    if (!owner || (!gh && !ph)) return 0;
    for (auto &slot : cache) {
        uintptr_t pack = 0;
        uint64_t token = 0, binding = 0;
        {
            Lock lock(slot);
            if (!lock.held || !witnessed(slot.pair, owner)) continue;
            if (slot.pair.holder[0].address != gh || slot.pair.holder[1].address != ph) return 0;
            token = slot.pair.pack.token;
            binding = slot.pair.binding;
            if (!azn_pin_hold(token, &pack)) return 0;
        }
        int rc = update_pair(owner, pack, gh, gc, ph, pc, binding);
        azn_pin_drop(token);
        return rc;
    }
    return 0;
}
uint64_t begin_pair(uintptr_t owner, uintptr_t pack) {
    unsigned mask = 0;
    for (unsigned d = 0; d < 4; d++) {
        azn_pack_view view {};
        if (!azn_pack_acquire(d, &view)) continue;
        bool match = pack && view.object == pack;
        if (!pack && owner) {
            Slot &s = cache[bucket(view.object)];
            Lock lock(s);
            match = lock.held && witnessed(s.pair, owner) && s.pair.pack.address == view.object;
        }
        if (match && azn_pack_current(d, &view)) mask |= 1u << d;
        azn_pin_drop(view.token);
    }
    return azn_map_begin_mask(mask);
}
void retire_pair(uintptr_t owner) {
    if (!owner) return;
    for (auto &slot : cache) {
        Lock lock(slot);
        if (!lock.held || !witnessed(slot.pair, owner)) continue;
        Pair &p = slot.pair;
        if (forget(p, owner)) continue;
        /* Browse can release the last GUI alias without changing any of the
         * deck's native holders. Retain the witnessed pair under the deck's
         * independent pack ownership, and reap it on pack withdrawal. */
        slot.deck_only.store(true, std::memory_order_release);
        if (!azn_pack_retains(p.pack.address)) release_pair(slot);
    }
}
/* Native grid Cell is {signed sample,double bpm}. The stock accessor
 * (1.30/2.00) adds the holder's signed origin before converting to time.
 * PhraseMeterWidget uses one-based section endpoints, indexes the same
 * 16-byte cells after subtracting one, and divides by holder+0x40. */
int read_rows_impl(const aznm_component *component, unsigned kind, void *rows, uint32_t capacity, uint32_t *out, int64_t *ticks) {
    if (!component || !out || !component->content || !profile ||
        (kind != AZNM_GRID && kind != AZNM_PHRASES)) return AZ_INVALID;
    uintptr_t content;
    if (!azn_pin_hold(component->content, &content)) return AZ_STALE;
    struct Drop {
        uint64_t t;
        ~Drop() { azn_pin_drop(t); }
    } drop {component->content};
    bool grid = kind == AZNM_GRID;
    if (!typed(content, grid ? profile->grid_content : profile->phrase_content)) return AZ_STALE;
    const int n = read_value<int>(content, grid ? 0x30 : 0x48);
    if (n < 0 || unsigned(n) > (grid ? 32768u : 1024u)) return AZ_LIMIT;
    *out = unsigned(n);
    if (!rows && !ticks) return AZ_OK;
    if (capacity < *out) return AZ_LIMIT;
    uintptr_t base = read_value<uintptr_t>(content, grid ? 0x28 : 0x40);
    if (n && base < 0x10000) return AZ_STALE;
    if (grid) {
        if (!component->sample_rate || component->sample_rate >= INT32_MAX) return AZ_STALE;
        auto *dst = static_cast<az_beat *>(rows);
        int64_t previous = 0;
        for (int i = 0; i < n; i++) {
            uintptr_t cell = base + 16u * i;
            int64_t sample;
            double bpm = read_value<double>(cell, 8);
            if (__builtin_add_overflow(read_value<int64_t>(cell), component->offset_samples, &sample) ||
                (i && committed_grid_order(sample, previous, component->sample_rate) != AZ_TIME_AFTER) ||
                committed_grid_order(sample, INT64_C(9007199254740992), component->sample_rate) == AZ_TIME_AFTER ||
                committed_grid_order(sample, -INT64_C(9007199254740992), component->sample_rate) == AZ_TIME_BEFORE ||
                !std::isfinite(bpm) || bpm <= 0) return AZ_STALE;
            if (dst) dst[i] = {double(sample) / component->sample_rate, bpm, 0, 0};
            if (ticks) ticks[i] = sample;
            previous = sample;
        }
        /* Both firmware builders copy a zero-based cell index into this
         * auxiliary table only when their first encoded BE16 is exactly 1.
         * Prefix and trailing cells do not identify a complete bar. */
        uintptr_t marks = read_value<uintptr_t>(content, 0x38);
        int m = read_value<int>(content, 0x40);
        if (dst && marks >= 0x10000 && m > 0 && m <= n) {
            int previous_mark = -1;
            bool valid = true;
            for (int j = 0; j < m; j++) {
                int at = read_value<int>(marks, 4u * unsigned(j));
                if (at <= previous_mark || at >= n) {
                    valid = false;
                    break;
                }
                previous_mark = at;
            }
            if (valid) {
                int first = read_value<int>(marks);
                for (int j = 1; j < m; j++) {
                    int end = read_value<int>(marks, 4u * unsigned(j));
                    for (int at = first; at < end; at++) dst[at].beat_in_bar = unsigned(at - first + 1);
                    first = end;
                }
                dst[first].beat_in_bar = 1;
            }
        }
    } else {
        auto *dst = static_cast<az_phrase *>(rows);
        uint32_t mood = read_value<uint32_t>(content, 0x3c);
        uint32_t prior = 0;
        for (int i = 0; i < n; i++) {
            uintptr_t row = base + 20u * i;
            uint32_t start = read_value<uint32_t>(row, 4), end = read_value<uint32_t>(row, 8);
            if (!start || end <= start || (i && start < prior)) return AZ_STALE;
            dst[i] = {start - 1, end - 1, read_value<uint32_t>(row), mood, 0, 0};
            prior = end;
        }
    }
    return AZ_OK;
}
int read_rows(const aznm_component *component, unsigned kind, void *rows, uint32_t capacity, uint32_t *out) {
    return read_rows_impl(component, kind, rows, capacity, out, nullptr);
}
int read_samples(const aznm_component *component, int64_t *ticks, uint32_t capacity, uint32_t *out, uint32_t *rate) {
    if (!rate) return AZ_INVALID;
    *rate = 0;
    if (!component || !component->sample_rate || component->sample_rate >= INT32_MAX) return AZ_STALE;
    int result = read_rows_impl(component, AZNM_GRID, nullptr, capacity, out, ticks);
    if (result == AZ_OK) *rate = component->sample_rate;
    return result;
}
#include "sdk_status_map_writers.inc"
} // namespace
extern "C" int azn_maps_current(unsigned d, uint64_t load, const aznm_packet *stamp) {
    if (!stamp || d >= 4 || !stamp->owner) return 0;
    azn_pack_view v {};
    if (!azn_pack_acquire(d, &v)) return 0;
    bool valid = v.load == load && current_identity(d, v, *stamp);
    azn_pin_drop(v.token);
    return valid;
}
extern "C" unsigned azn_maps_install(int build, uintptr_t bias, azn_setting_patch_fn patch) {
    if (!patch || build < 1 || build > 2) return 0;
    constexpr uintptr_t sites[][4] = {{0xd295b0, 0xd29b20, 0x176e748, 0x176fbe0}, {0xdb34d0, 0xdb3a40, 0x18c9d30, 0x18cb1e8}};
    constexpr uint32_t words[][4] = {{0xc8dffc03, 0xf9400802, 0xa9b17bfd, 0xa9b67bfd}, {0xc8dffc03, 0xf9400802, 0xa9b17bfd, 0xa9b67bfd}};
    void *hooks[] = {reinterpret_cast<void *>(grid_assign), reinterpret_cast<void *>(grid_retime),
                     reinterpret_cast<void *>(phrase_registry), reinterpret_cast<void *>(phrase_broadcast)};
    for (unsigned i = 0; i < 4; i++) {
        uintptr_t target = bias + sites[build - 1][i];
        if (!(map_writer_installed & (1u << i)) && read_value<uint32_t>(target) == words[build - 1][i] &&
            patch(target, hooks[i], &map_writer_original[i], "status-map-writer") == 1) map_writer_installed |= 1u << i;
    }
    return map_writer_installed;
}
extern "C" int azn_maps_configure(int build) {
    if (build != 1 && build != 2) return 0;
    profile = &profiles[build - 1];
    enabled.store(true, std::memory_order_release);
    if (azn_pack_observe(pack_changed)) return 1;
    enabled.store(false, std::memory_order_release);
    return 0;
}
extern "C" const azn_maps_api *azn_maps_service(void) {
    static const azn_maps_api api = {sizeof api, 1, prove_pair, retire_pair, read_rows, release_packet, request_seed, refresh_pair, begin_pair, azn_map_end_mask, read_samples};
    return enabled.load(std::memory_order_acquire) ? &api : nullptr;
}
