/* SPDX-License-Identifier: GPL-3.0-only */
/* The native GUI supplies two genuine strong references when installing its
 * TrackStructureData pair. The audio-side pack registry independently proves
 * deck and load membership. Only counted immutable Content leaves the writer. */
#include "../common/azstatus_native.h"
#include "../common/azstatus_owned.h"
#include "../common/aztimestamp.h"
#include "../azmod/azmod-mod.h"
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include "../common/aznative_c.h"
#include "../common/aznative_sync.h"



typedef struct Profile Profile;
struct Profile {
    uintptr_t pack, grid_holder, grid_content, phrase_holder, phrase_content;
};
static const Profile profiles[] = {
    {0x2612530, 0x2754518, 0x2798328, 0x29280a0, 0x2797d30},
    {0x294c9b0, 0x2a9cfa8, 0x2ae4df8, 0x2c99430, 0x2ae4800}};
static const Profile *profile;
#define DEFINE_READ(type, name)                                         \
    static type read_##name(uintptr_t object, unsigned offset) {        \
        type value;                                                     \
        memcpy(&value, (const void *) (object + offset), sizeof value); \
        return value;                                                   \
    }
DEFINE_READ(uintptr_t, ptr)
DEFINE_READ(uint32_t, u32)
DEFINE_READ(int64_t, i64)
DEFINE_READ(int, int)
DEFINE_READ(double, double)
#undef DEFINE_READ
static bool typed(uintptr_t object, uintptr_t vtable) {
    return object >= 0x10000 && !(object & 7) && read_ptr(object, 0) == vtable;
}
enum {
    slots = 32
};
static const uint64_t routes = AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES) |
                               AZ_STATUS_TOPIC(AZ_STATUS_PHASE) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
typedef struct Object Object;
struct Object {
    uintptr_t address;
    uint64_t token;
};
typedef struct Pair Pair;
struct Pair {
    Object pack, holder[2], content[2];
    uintptr_t owners[8];
    uint64_t binding;
    struct aznm_packet packet;
};
/* Several logical decks can share a single active pack. Each GUI owner
 * supplies an independent membership witness for the same two holders. */
static bool witnessed(const Pair *p, uintptr_t owner) {
    for (unsigned i = 0; i < 8; ++i)
        if (p->owners[i] && p->owners[i] == owner) return true;
    return false;
}
static bool remember(Pair *p, uintptr_t owner) {
    if (!owner || witnessed(p, owner)) return true;
    for (unsigned i = 0; i < 8; ++i)
        if (!p->owners[i]) {
            p->owners[i] = owner;
            return true;
        }
    return false;
}
static bool forget(Pair *p, uintptr_t owner) {
    bool remaining = false;
    for (unsigned i = 0; i < 8; ++i) {
        if (p->owners[i] == owner) p->owners[i] = 0;
        remaining |= p->owners[i] != 0;
    }
    return remaining;
}
struct Published {
    _Atomic(uint64_t) serial, owner, tokens[5], providers[2], revisions[2];
    _Atomic(int64_t) offset;
    _Atomic(uint32_t) rate;
};
typedef struct Slot Slot;
struct Slot {
    atomic_flag busy;
    _Atomic(bool) deck_only;
    Pair pair;
    struct Published published;
};
static Slot cache[slots] = {[0 ... slots - 1] = {.busy = ATOMIC_FLAG_INIT}};
static _Atomic(uint64_t) identities;
static _Atomic(bool) enabled;
/* Native registry/broadcast calls can reenter the same writer. */
static pthread_mutex_t map_mutation;
static pthread_once_t map_once = PTHREAD_ONCE_INIT;
static void map_mutex_init(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&map_mutation, &attr) || pthread_mutexattr_destroy(&attr)) abort();
}
#define MAP_MUTATION(name)                                \
    if (pthread_once(&map_once, map_mutex_init)) abort(); \
    AZ_MUTEX_LOCK(name, map_mutation)
enum {
    PROOF_ENTER,
    PROOF_TYPE,
    PROOF_PIN,
    PROOF_MEMBER,
    PROOF_CACHE,
    PROOF_EMIT,
    PROOF_DIAGS
};
static _Atomic(uint64_t) proof_counts[PROOF_DIAGS];
static unsigned bucket(uintptr_t pack) { return (unsigned) ((pack >> 6) % slots); }
static uint64_t identity() {
    uint64_t v = atomic_fetch_add_explicit(&(identities), 1, memory_order_relaxed) + 1;
    return v && v <= AZSP_SERIAL_MAX ? AZSP_MAP | v : 0;
}
struct Lock {
    Slot *slot;
    bool held;
};
static struct Lock slot_lock(Slot *slot) {
    return (struct Lock) {slot, !atomic_flag_test_and_set_explicit(&slot->busy, memory_order_acquire)};
}
static void slot_unlock(struct Lock *lock) {
    if (lock->held) atomic_flag_clear_explicit(&lock->slot->busy, memory_order_release);
}
#define SLOT_LOCK(name, slot) struct Lock name __attribute__((cleanup(slot_unlock))) = slot_lock(&(slot))
static void pin_drop(uint64_t *pin) { azn_pin_drop(*pin); }
#define PIN_DROP(name, token) uint64_t name __attribute__((cleanup(pin_drop))) = (token)
static void discard(Pair *p) {
    azn_pin_drop(p->pack.token);
    for (unsigned i = 0; i < 2; i++) {
        azn_pin_drop(p->holder[i].token);
        azn_pin_drop(p->content[i].token);
    }
    (*p) = (Pair) {0};
}
static bool pin(Object *o, uintptr_t address) {
    o->address = address;
    o->token = azn_owned_pin(address);
    return o->token != 0;
}
static void release_packet(const struct aznm_packet *p) {
    if (p) {
        azn_pin_drop(p->grid.content);
        azn_pin_drop(p->phrases.content);
    }
}
static bool copy_packet(const Pair *p, struct aznm_packet *out) {
    (*out) = p->packet;
    out->grid.content = out->phrases.content = 0;
    uintptr_t ignored;
    uint64_t tokens[] = {p->content[0].token, p->content[1].token};
    for (unsigned i = 0; i < 2; i++) {
        if (!tokens[i]) continue;
        if (!azn_pin_hold(tokens[i], &ignored)) {
            release_packet(&(*out));
            return false;
        }
        (i ? &out->phrases : &out->grid)->content = tokens[i];
    }
    return true;
}
/* Grid origins and cells can precede source zero. These are individually
 * committed integer samples. Shift the signed origin without losing bits. */
static int committed_grid_order(int64_t first, int64_t second, uint32_t rate) {
    return compare_timestamps((uint64_t) (first) ^ UINT64_C(0x8000000000000000),
                              (uint64_t) (second) ^ UINT64_C(0x8000000000000000), rate, rate, 1);
}
static bool sample_matches(const Pair *p) {
    if (!p->pack.address) return false;
    for (unsigned i = 0; i < 2; i++) {
        uintptr_t h = p->holder[i].address;
        if (read_ptr(p->pack.address, i ? 0x120 : 0xd8) != h ||
            (h && read_ptr(h, 0x28) != p->content[i].address)) return false;
    }
    uintptr_t grid = p->holder[0].address;
    return !grid || (read_u32(grid, 0x40) == p->packet.grid.sample_rate &&
                     committed_grid_order(read_i64(grid, 0x30), p->packet.grid.offset_samples,
                                          p->packet.grid.sample_rate) == AZ_TIME_EQUAL);
}
static void publish_identity(Slot *slot, const Pair *p) {
    __auto_type out = &(slot->published);
    uint64_t serial = atomic_load_explicit(&(out->serial), memory_order_relaxed);
    /* Exhaustion permanently closes this snapshot. Never recycle a sequence
     * while a reader might still hold an earlier edition of the same slot-> */
    if (serial >= UINT64_MAX - 2) {
        atomic_store_explicit(&(out->serial), UINT64_MAX, memory_order_release);
        return;
    }
    atomic_fetch_add_explicit(&(out->serial), 1, memory_order_acq_rel);
    /* Pair with the reader's acquire fence through any relaxed field it
     * observes from this edition, so its final serial read sees this cut. */
    atomic_thread_fence(memory_order_release);
    const uint64_t tokens[] = {p->pack.token, p->holder[0].token, p->holder[1].token,
                               p->content[0].token, p->content[1].token};
    for (unsigned i = 0; i < 5; ++i) atomic_store_explicit(&(out->tokens[i]), tokens[i], memory_order_relaxed);
    atomic_store_explicit(&(out->owner), p->packet.owner, memory_order_relaxed);
    for (unsigned i = 0; i < 2; ++i) {
        __auto_type part = (i ? &p->packet.phrases : &p->packet.grid);
        atomic_store_explicit(&(out->providers[i]), part->provider, memory_order_relaxed);
        atomic_store_explicit(&(out->revisions[i]), part->revision, memory_order_relaxed);
    }
    atomic_store_explicit(&(out->offset), p->packet.grid.offset_samples, memory_order_relaxed);
    atomic_store_explicit(&(out->rate), p->packet.grid.sample_rate, memory_order_relaxed);
    atomic_store_explicit(&(out->serial), serial + 2, memory_order_release);
}
static bool current_identity(unsigned d, const struct azn_pack_view *view, const struct aznm_packet *stamp) {
    __auto_type in = &(cache[bucket(view->object)].published);
    uint64_t serial = atomic_load_explicit(&(in->serial), memory_order_acquire);
    if (!serial || (serial & 1) || atomic_load_explicit(&(in->owner), memory_order_relaxed) != stamp->owner) return false;
    for (unsigned i = 0; i < 2; ++i) {
        __auto_type part = (i ? &stamp->phrases : &stamp->grid);
        if (atomic_load_explicit(&(in->providers[i]), memory_order_relaxed) != part->provider ||
            atomic_load_explicit(&(in->revisions[i]), memory_order_relaxed) != part->revision) return false;
    }
    Pair held __attribute__((cleanup(discard))) = {0};
    Object *objects[] = {&held.pack, &held.holder[0], &held.holder[1], &held.content[0], &held.content[1]};
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t token = atomic_load_explicit(&(in->tokens[i]), memory_order_relaxed);
        if (!token) continue;
        if (!azn_pin_hold(token, &objects[i]->address)) return false;
        objects[i]->token = token;
    }
    held.packet.grid.offset_samples = atomic_load_explicit(&(in->offset), memory_order_relaxed);
    held.packet.grid.sample_rate = atomic_load_explicit(&(in->rate), memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (serial != atomic_load_explicit(&(in->serial), memory_order_relaxed) || held.pack.address != view->object) return false;
    bool valid = sample_matches(&(held)) && azn_pack_current(d, &(*view));
    atomic_thread_fence(memory_order_acquire);
    return valid && serial == atomic_load_explicit(&(in->serial), memory_order_relaxed);
}
static void emit(unsigned deck, const struct azn_pack_view *view, const Pair *p) {
    atomic_fetch_add_explicit(&(proof_counts[PROOF_EMIT]), 1, memory_order_relaxed);
    if (!(azn_interest(deck) & routes) || !azn_pack_current(deck, &(*view))) return;
    struct azstatus_native_receipt receipt = {0};
    receipt.kind = AZN_MAPS;
    receipt.deck = deck;
    receipt.unit = view->unit;
    receipt.epoch = view->fence;
    receipt.load_generation = view->load;
    if (!copy_packet(p, &(receipt.map))) {
        azn_map_gap(deck);
        return;
    }
    if (!azn_offer_map_cut(&receipt)) release_packet(&receipt.map);
}
static void release_pair(Slot *slot) {
    publish_identity(slot, &((Pair) {0}));
    discard(&(slot->pair));
    atomic_store_explicit(&(slot->deck_only), false, memory_order_release);
}
static void reap_deck_only_pairs() {
    for (unsigned slot_index = 0; slot_index < slots; ++slot_index) {
        Slot *slot = &(cache[slot_index]);
        if (!atomic_load_explicit(&(slot->deck_only), memory_order_acquire)) continue;
        SLOT_LOCK(lock, (*slot));
        if (lock.held && atomic_load_explicit(&(slot->deck_only), memory_order_relaxed) &&
            !azn_pack_retains(slot->pair.pack.address)) release_pair(slot);
    }
}
static void seed(unsigned deck, uint64_t unit_unused __attribute__((unused)), uint64_t fence_unused __attribute__((unused)), uint64_t load) {
    if (!atomic_load_explicit(&(enabled), memory_order_acquire) || !load || deck >= 4) return;
    struct azn_pack_view view = {0};
    if (!azn_pack_acquire(deck, &view)) return;
    PIN_DROP(drop, view.token);
    Slot *slot = &(cache[bucket(view.object)]);
    SLOT_LOCK(lock, (*slot));
    if (!lock.held) {
        azn_map_gap(deck);
        return;
    }
    Pair *p = &(slot->pair);
    if (view.load == load && p->pack.address == view.object && sample_matches(p)) emit(deck, &(view), p);
}
static void pack_changed(unsigned deck, uint64_t unit, uint64_t fence, uint64_t load) {
    reap_deck_only_pairs();
    seed(deck, unit, fence, load);
}
static void request_seed(unsigned d) {
    if (d >= 4) return;
    struct azn_pack_view view = {0};
    if (!azn_pack_acquire(d, &view)) return;
    seed(d, view.unit, view.fence, view.load);
    azn_pin_drop(view.token);
}
static void affected_gap(uintptr_t pack) {
    for (unsigned d = 0; d < 4; d++) {
        struct azn_pack_view v = {0};
        if (azn_pack_acquire(d, &v)) {
            if (v.object == pack) azn_map_gap(d);
            azn_pin_drop(v.token);
        }
    }
}
static int update_pair(uintptr_t owner, uintptr_t pack, uintptr_t gh, uintptr_t gc, uintptr_t ph, uintptr_t pc, uint64_t existing) {
    /* Every nonzero argument is borrowed from the observed native call. */
    atomic_fetch_add_explicit(&(proof_counts[PROOF_ENTER]), 1, memory_order_relaxed);
    if (!atomic_load_explicit(&(enabled), memory_order_acquire) || (!owner && !existing) || !typed(pack, profile->pack) ||
        (!gh && !ph) || (gc && !gh) || (pc && !ph) ||
        (gh && !typed(gh, profile->grid_holder)) || (gc && !typed(gc, profile->grid_content)) ||
        (ph && !typed(ph, profile->phrase_holder)) || (pc && !typed(pc, profile->phrase_content))) {
        atomic_fetch_add_explicit(&(proof_counts[PROOF_TYPE]), 1, memory_order_relaxed);
        return 0;
    }
    Pair next = {0};
    if (!pin(&(next.pack), pack) || (gh && !pin(&(next.holder[0]), gh)) || (gc && !pin(&(next.content[0]), gc)) ||
        (ph && !pin(&(next.holder[1]), ph)) || (pc && !pin(&(next.content[1]), pc))) {
        atomic_fetch_add_explicit(&(proof_counts[PROOF_PIN]), 1, memory_order_relaxed);
        discard(&(next));
        return 0;
    }
    next.packet.grid.offset_samples = gh ? read_i64(gh, 0x30) : 0;
    next.packet.grid.sample_rate = gh ? read_u32(gh, 0x40) : 0;
    if (!sample_matches(&(next))) {
        atomic_fetch_add_explicit(&(proof_counts[PROOF_MEMBER]), 1, memory_order_relaxed);
        discard(&(next));
        return 0;
    }
    Slot *slot = &(cache[bucket(pack)]);
    {
        SLOT_LOCK(lock, (*slot));
        if (!lock.held) {
            affected_gap(pack);
            discard(&(next));
            return 0;
        }
        Pair *old = &(slot->pair);
        bool same = old->pack.address == pack;
        if ((existing && (!same || old->binding != existing || (owner && !witnessed(old, owner)) || old->holder[0].address != gh || old->holder[1].address != ph)) ||
            !sample_matches(&(next))) {
            discard(&(next));
            return 0;
        }
        bool same_pair = same && old->holder[0].address == gh && old->holder[1].address == ph;
        if (same_pair) memcpy(next.owners, old->owners, sizeof next.owners);
        if (!remember(&(next), owner)) {
            affected_gap(pack);
            discard(&(next));
            return 0;
        }
        next.binding = same_pair ? old->binding : identity();
        if (!next.binding) {
            discard(&(next));
            return 0;
        }
        if (gc && pc) {
            next.packet.owner = same_pair && old->packet.owner ? old->packet.owner : identity();
            if (!next.packet.owner) {
                discard(&(next));
                return 0;
            }
        }
        for (unsigned i = 0; i < 2; i++) {
            struct aznm_component *n = (i ? &next.packet.phrases : &next.packet.grid);
            const struct aznm_component *o = (i ? &old->packet.phrases : &old->packet.grid);
            if (!next.holder[i].address && (!same || !o->provider)) continue;
            bool same_holder = same && o->provider && old->holder[i].address == next.holder[i].address;
            n->provider = same_holder || (!next.holder[i].address && o->provider) ? o->provider : identity();
            bool changed = !same_holder || old->content[i].address != next.content[i].address ||
                           (!i && (n->sample_rate != o->sample_rate ||
                                   committed_grid_order(n->offset_samples, o->offset_samples, n->sample_rate) != AZ_TIME_EQUAL));
            if (!n->provider || (changed && o->revision == UINT64_MAX)) {
                discard(&(next));
                return 0;
            }
            n->revision = same_holder ? o->revision + changed : 1;
            n->availability = next.content[i].address ? AZ_AVAIL_READY : next.holder[i].address ? AZ_AVAIL_LOADING
                                                                                                : AZ_AVAIL_STALE;
            if (changed) next.packet.changed |= i ? AZNM_PHRASES : AZNM_GRID;
        }
        if (!same || next.packet.owner != old->packet.owner || next.packet.changed) next.packet.changed |= AZNM_PAIR;
        if (!next.packet.changed) {
            memcpy(old->owners, next.owners, sizeof old->owners);
            if (owner) atomic_store_explicit(&(slot->deck_only), false, memory_order_release);
            discard(&(next));
            return 1;
        }
        publish_identity(slot, &(next));
        discard(old);
        (*old) = next;
        if (owner) atomic_store_explicit(&(slot->deck_only), false, memory_order_release);
        atomic_fetch_add_explicit(&(proof_counts[PROOF_CACHE]), 1, memory_order_relaxed);
        for (unsigned d = 0; d < 4; d++) {
            struct azn_pack_view v = {0};
            if (azn_pack_acquire(d, &v)) {
                if (v.object == pack && sample_matches(old)) emit(d, &(v), old);
                azn_pin_drop(v.token);
            }
        }
    }
    return 1;
}
static int prove_pair(uintptr_t owner, uintptr_t pack, uintptr_t gh, uintptr_t gc, uintptr_t ph, uintptr_t pc) {
    return update_pair(owner, pack, gh, gc, ph, pc, 0);
}
static int refresh_pair(uintptr_t owner, uintptr_t gh, uintptr_t gc, uintptr_t ph, uintptr_t pc) {
    if (!owner || (!gh && !ph)) return 0;
    for (unsigned slot_index = 0; slot_index < slots; ++slot_index) {
        Slot *slot = &(cache[slot_index]);
        uintptr_t pack = 0;
        uint64_t token = 0, binding = 0;
        {
            SLOT_LOCK(lock, (*slot));
            if (!lock.held || !witnessed(&(slot->pair), owner)) continue;
            if (slot->pair.holder[0].address != gh || slot->pair.holder[1].address != ph) return 0;
            token = slot->pair.pack.token;
            binding = slot->pair.binding;
            if (!azn_pin_hold(token, &pack)) return 0;
        }
        int rc = update_pair(owner, pack, gh, gc, ph, pc, binding);
        azn_pin_drop(token);
        return rc;
    }
    return 0;
}
static uint64_t begin_pair(uintptr_t owner, uintptr_t pack) {
    unsigned mask = 0;
    for (unsigned d = 0; d < 4; d++) {
        struct azn_pack_view view = {0};
        if (!azn_pack_acquire(d, &view)) continue;
        bool match = pack && view.object == pack;
        if (!pack && owner) {
            Slot *s = &(cache[bucket(view.object)]);
            SLOT_LOCK(lock, (*s));
            match = lock.held && witnessed(&(s->pair), owner) && s->pair.pack.address == view.object;
        }
        if (match && azn_pack_current(d, &view)) mask |= 1u << d;
        azn_pin_drop(view.token);
    }
    return azn_map_begin_mask(mask);
}
static void retire_pair(uintptr_t owner) {
    if (!owner) return;
    for (unsigned slot_index = 0; slot_index < slots; ++slot_index) {
        Slot *slot = &(cache[slot_index]);
        SLOT_LOCK(lock, (*slot));
        if (!lock.held || !witnessed(&(slot->pair), owner)) continue;
        Pair *p = &(slot->pair);
        if (forget(p, owner)) continue;
        /* Browse can release the last GUI alias without changing any of the
         * deck's native holders. Retain the witnessed pair under the deck's
         * independent pack ownership, and reap it on pack withdrawal. */
        atomic_store_explicit(&(slot->deck_only), true, memory_order_release);
        if (!azn_pack_retains(p->pack.address)) release_pair(slot);
    }
}
/* Native grid Cell is {signed sample,double bpm}. The stock accessor
 * (1.30/2.00) adds the holder's signed origin before converting to time.
 * PhraseMeterWidget uses one-based section endpoints, indexes the same
 * 16-byte cells after subtracting one, and divides by holder+0x40. */
static int read_rows_impl(const struct aznm_component *component, unsigned kind, void *rows, uint32_t capacity, uint32_t *out, int64_t *ticks) {
    if (!component || !out || !component->content || !profile ||
        (kind != AZNM_GRID && kind != AZNM_PHRASES)) return AZ_INVALID;
    uintptr_t content;
    if (!azn_pin_hold(component->content, &content)) return AZ_STALE;
    PIN_DROP(drop, component->content);
    bool grid = kind == AZNM_GRID;
    if (!typed(content, grid ? profile->grid_content : profile->phrase_content)) return AZ_STALE;
    const int n = read_int(content, grid ? 0x30 : 0x48);
    if (n < 0 || (unsigned) (n) > (grid ? 32768u : 1024u)) return AZ_LIMIT;
    *out = (unsigned) (n);
    if (!rows && !ticks) return AZ_OK;
    if (capacity < *out) return AZ_LIMIT;
    uintptr_t base = read_ptr(content, grid ? 0x28 : 0x40);
    if (n && base < 0x10000) return AZ_STALE;
    if (grid) {
        if (!component->sample_rate || component->sample_rate >= INT32_MAX) return AZ_STALE;
        __auto_type dst = ((struct az_beat *) (rows));
        int64_t previous = 0;
        for (int i = 0; i < n; i++) {
            uintptr_t cell = base + 16u * i;
            int64_t sample;
            double bpm = read_double(cell, 8);
            if (__builtin_add_overflow(read_i64(cell, 0), component->offset_samples, &sample) ||
                (i && committed_grid_order(sample, previous, component->sample_rate) != AZ_TIME_AFTER) ||
                committed_grid_order(sample, INT64_C(9007199254740992), component->sample_rate) == AZ_TIME_AFTER ||
                committed_grid_order(sample, -INT64_C(9007199254740992), component->sample_rate) == AZ_TIME_BEFORE ||
                !isfinite(bpm) || bpm <= 0) return AZ_STALE;
            if (dst) dst[i] = (struct az_beat) {(double) sample / component->sample_rate, bpm, 0, 0};
            if (ticks) ticks[i] = sample;
            previous = sample;
        }
        /* Both firmware builders copy a zero-based cell index into this
         * auxiliary table only when their first encoded BE16 is exactly 1.
         * Prefix and trailing cells do not identify a complete bar. */
        uintptr_t marks = read_ptr(content, 0x38);
        int m = read_int(content, 0x40);
        if (dst && marks >= 0x10000 && m > 0 && m <= n) {
            int previous_mark = -1;
            bool valid = true;
            for (int j = 0; j < m; j++) {
                int at = read_int(marks, 4u * (unsigned) (j));
                if (at <= previous_mark || at >= n) {
                    valid = false;
                    break;
                }
                previous_mark = at;
            }
            if (valid) {
                int first = read_int(marks, 0);
                for (int j = 1; j < m; j++) {
                    int end = read_int(marks, 4u * (unsigned) (j));
                    for (int at = first; at < end; at++) dst[at].beat_in_bar = (unsigned) (at - first + 1);
                    first = end;
                }
                dst[first].beat_in_bar = 1;
            }
        }
    } else {
        __auto_type dst = ((struct az_phrase *) (rows));
        /* TrackPhrase::fromAtom maps PSSI moods 1/2/3 to native enum
         * values 0/1/2 (1.30 table 0x2797e98, 2.00 table 0x2ae4968).
         * The public raw_mood field uses the PSSI domain, like file readers. */
        uint32_t native_mood = read_u32(content, 0x3c);
        uint32_t mood = native_mood < 3 ? native_mood + 1 : 0;
        uint32_t prior = 0;
        for (int i = 0; i < n; i++) {
            uintptr_t row = base + 20u * i;
            uint32_t start = read_u32(row, 4), end = read_u32(row, 8);
            if (!start || end <= start || (i && start < prior)) return AZ_STALE;
            dst[i] = (struct az_phrase) {start - 1, end - 1, read_u32(row, 0), mood, 0, 0};
            prior = end;
        }
    }
    return AZ_OK;
}
static int read_rows(const struct aznm_component *component, unsigned kind, void *rows, uint32_t capacity, uint32_t *out) {
    return read_rows_impl(component, kind, rows, capacity, out, NULL);
}
static int read_samples(const struct aznm_component *component, int64_t *ticks, uint32_t capacity, uint32_t *out, uint32_t *rate) {
    if (!rate) return AZ_INVALID;
    *rate = 0;
    if (!component || !component->sample_rate || component->sample_rate >= INT32_MAX) return AZ_STALE;
    int result = read_rows_impl(component, AZNM_GRID, NULL, capacity, out, ticks);
    if (result == AZ_OK) *rate = component->sample_rate;
    return result;
}
#include "sdk_status_map_writers.inc"
#include "sdk_status_map_analysis.inc"

int azn_maps_current(unsigned d, uint64_t load, const struct aznm_packet *stamp) {
    if (!stamp || d >= 4 || !stamp->owner) return 0;
    struct azn_pack_view v = {0};
    if (!azn_pack_acquire(d, &v)) return 0;
    bool valid = v.load == load && current_identity(d, &(v), &(*stamp));
    azn_pin_drop(v.token);
    return valid;
}
unsigned azn_maps_install(int build, uintptr_t bias, azn_setting_patch_fn patch) {
    if (!patch || build < 1 || build > 2) return 0;
    configure_analysis(build, bias);
    const uintptr_t sites[][4] = {{0xd295b0, 0xd29b20, 0x176e748, 0x176fbe0}, {0xdb34d0, 0xdb3a40, 0x18c9d30, 0x18cb1e8}};
    const uint32_t words[][4] = {{0xc8dffc03, 0xf9400802, 0xa9b17bfd, 0xa9b67bfd}, {0xc8dffc03, 0xf9400802, 0xa9b17bfd, 0xa9b67bfd}};
    void *hooks[] = {((void *) (grid_assign)), ((void *) (grid_retime)),
                     ((void *) (phrase_registry)), ((void *) (phrase_broadcast))};
    for (unsigned i = 0; i < 4; i++) {
        uintptr_t target = bias + sites[build - 1][i];
        if (!(map_writer_installed & (1u << i)) && read_u32(target, 0) == words[build - 1][i] &&
            patch(target, hooks[i], &map_writer_original[i], "status-map-writer") == 1) map_writer_installed |= 1u << i;
    }
    return map_writer_installed;
}
int azn_maps_configure(int build) {
    if (build != 1 && build != 2) return 0;
    profile = &profiles[build - 1];
    atomic_store_explicit(&(enabled), true, memory_order_release);
    if (azn_pack_observe(pack_changed)) return 1;
    atomic_store_explicit(&(enabled), false, memory_order_release);
    return 0;
}
const struct azn_maps_api *azn_maps_service(void) {
    static const struct azn_maps_api api = {sizeof api, 1, prove_pair, retire_pair, read_rows, release_packet, request_seed, refresh_pair, begin_pair, azn_map_end_mask, read_samples,
                                            analysis_capture, analysis_current, analysis_publish};
    return atomic_load_explicit(&(enabled), memory_order_acquire) ? &api : NULL;
}
