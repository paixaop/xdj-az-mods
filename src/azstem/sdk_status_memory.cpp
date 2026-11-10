/* SPDX-License-Identifier: GPL-3.0-only */
/* MemoryCueSet commits have their own lifetime. Active packs, recently observed
 * holders/content and queued revisions each hold counted native references.
 * Audio/panel/native writers never decode comments, destroy native objects or
 * wait for a reader. The reducer reads a pinned immutable vector and reaps it. */
#include "../common/azstatus_memory.h"
#include "../common/azstatus_owned.h"
#include "../common/azstatus_analysis_origin.h"
#include "../common/azloadobserver.h"
#include "../common/azcuepersist.h"
#include "../common/azcuetext.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#if defined(__linux__)
#include <link.h>
#endif

namespace {
struct Entry {
    uintptr_t address;
    uint32_t prologue[4];
};
struct MemProfile {
    uintptr_t pack, holder, content;
    Entry entry[5];
};
#include "sdk_status_memory_profiles.h"
MemProfile profile {};
void *originals[5];
std::atomic<bool> enabled {false};
std::atomic<bool> connected {false};
std::atomic<bool> pack_profile_ready {false};
std::atomic<azn_pack_observer> pack_observer {nullptr};
bool observing() { return enabled.load(std::memory_order_acquire) && connected.load(std::memory_order_acquire); }
constexpr unsigned pool_count = 256, cache_count = 32;
constexpr uint64_t refbits = 16, refs_mask = (1u << refbits) - 1, busy = UINT64_MAX;
struct Pin {
    /* High bits are a nonreused incarnation. A zero low count is retired.
     * Only the reducer can recycle a retired object's storage. */
    std::atomic<uint64_t> state {0};
    uintptr_t object = 0;
    uint64_t epoch = 0, load = 0, unit = 0;
    std::atomic<uintptr_t> holder {0};
    std::atomic<uint64_t> provider {0}, serial {0}, holder_pin {0}, latest_pin {0};
    bool confirmed_empty = false; // protected by publishing
    std::atomic_flag publishing = ATOMIC_FLAG_INIT;
};
Pin pool[pool_count];
std::atomic<uint64_t> sequence {0}, providers {0};
std::atomic<bool> reap_needed {false};
std::atomic<uint64_t> active[4] = {};
std::atomic<uint64_t> cached_holder[cache_count] = {}, cached_content[cache_count] = {};
struct Writing {
    std::atomic<unsigned> count {0};
    std::atomic<uint64_t> interference {0};
};
Writing writers[cache_count];
Writing holder_writers[cache_count];

template <class T>
T scalar(uintptr_t addr) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void *>(addr), sizeof value);
    return value;
}
bool ptr(uintptr_t addr) { return addr >= 0x10000 && addr <= UINT64_C(0x0000ffffffffffff) && !(addr & 7); }
uintptr_t object_word(uintptr_t addr) {
    return __atomic_load_n(reinterpret_cast<const uintptr_t *>(addr), __ATOMIC_ACQUIRE);
}
bool typed(uintptr_t obj, uintptr_t table) { return ptr(obj) && object_word(obj) == table; }
uint64_t new_id(std::atomic<uint64_t> &seq, uint64_t maximum) {
    uint64_t value = seq.load(std::memory_order_relaxed);
    for (unsigned n = 0; n < 8 && value < maximum; ++n)
        if (seq.compare_exchange_strong(value, value + 1, std::memory_order_relaxed)) return value + 1;
    return 0;
}
Pin *pin_for(uint64_t token) {
    unsigned index = unsigned(token & 511u);
    return token && index < pool_count ? &pool[index] : nullptr;
}
bool hold(uint64_t token) {
    Pin *p = pin_for(token);
    if (!p) return false;
    const uint64_t identity = token >> 9;
    uint64_t state = p->state.load(std::memory_order_acquire);
    for (unsigned n = 0; n < 8 && state >> refbits == identity && (state & refs_mask) && (state & refs_mask) < refs_mask; ++n)
        if (p->state.compare_exchange_strong(state, state + 1, std::memory_order_acq_rel)) return true;
    return false;
}
void drop(uint64_t token) {
    Pin *p = pin_for(token);
    if (!p) return;
    /* Callers own exactly one reference, so neither retirement nor reuse can race this RMW. */
    uint64_t old = p->state.fetch_sub(1, std::memory_order_acq_rel);
    if ((old & refs_mask) == 1 && !reap_needed.exchange(true, std::memory_order_acq_rel)) azn_notify();
}
struct Hold {
    uint64_t token;
    explicit Hold(uint64_t id) : token(hold(id) ? id : 0) {}
    ~Hold() {
        if (token) drop(token);
    }
    Pin *get() const { return pin_for(token); }
};
uint64_t make_pin(uintptr_t object, uint64_t epoch = 0, uint64_t load = 0, uint64_t unit = 0) {
    uint64_t id = new_id(sequence, (UINT64_C(1) << 46) - 1);
    if (!id) return 0;
    for (unsigned n = 0; n < pool_count; ++n) {
        unsigned i = unsigned(id + n) % pool_count;
        uint64_t empty = 0;
        if (!pool[i].state.compare_exchange_strong(empty, busy, std::memory_order_acq_rel)) continue;
        Pin &p = pool[i];
        if (!azn_ref_pin(object)) {
            p.state.store(0, std::memory_order_release);
            return 0;
        }
        p.object = object;
        p.epoch = epoch;
        p.load = load;
        p.unit = unit;
        p.confirmed_empty = false;
        p.holder.store(0, std::memory_order_relaxed);
        p.provider.store(0, std::memory_order_relaxed);
        p.serial.store(0, std::memory_order_relaxed);
        p.holder_pin.store(0, std::memory_order_relaxed);
        p.latest_pin.store(0, std::memory_order_relaxed);
        p.publishing.clear(std::memory_order_relaxed);
        p.state.store((id << refbits) | 1, std::memory_order_release);
        return (id << 9) | i;
    }
    return 0;
}
unsigned cache_slot(uintptr_t obj) { return unsigned((obj >> 4) ^ (obj >> 12)) % cache_count; }
uint64_t known(std::atomic<uint64_t> *cache, uintptr_t obj) {
    for (unsigned i = 0; i < cache_count; i++) {
        uint64_t token = cache[(cache_slot(obj) + i) % cache_count].load(std::memory_order_acquire);
        if (!hold(token)) continue;
        if (pin_for(token)->object == obj) return token;
        drop(token);
    }
    return 0;
}
/* The caller owns object. Keep a small bounded seed cache even before a pack
 * matches a deck. A numeric address lookup succeeds only while the cache
 * holds a genuine reference to this exact object. */
uint64_t remember(std::atomic<uint64_t> *cache, uintptr_t object, uintptr_t table) {
    if (!typed(object, table)) return 0;
    uint64_t prior = known(cache, object);
    if (prior) return prior;
    uint64_t token = make_pin(object);
    if (!token || !hold(token)) {
        if (token) drop(token);
        return 0;
    }
    for (unsigned i = 0; i < cache_count; i++) {
        uint64_t empty = 0;
        if (cache[(cache_slot(object) + i) % cache_count].compare_exchange_strong(empty, token, std::memory_order_acq_rel)) return token;
    }
    prior = cache[cache_slot(object)].exchange(token, std::memory_order_acq_rel);
    if (prior) drop(prior);
    return token;
}
bool current(unsigned deck, const Pin &bind) {
    azsdk_load_sample load {};
    return azsdk_load_read(int(deck), &load) && load.unit_generation == bind.unit && load.load_generation == bind.load &&
           load.invalidation_epoch == bind.epoch;
}
void memory_loss(unsigned deck, const Pin &bind) {
    if (!(azn_interest(deck) & AZ_STATUS_TOPIC(AZ_STATUS_CUES))) return;
    azstatus_native_receipt r {};
    r.kind = AZN_MEMORY;
    r.deck = deck;
    r.unit = bind.unit;
    r.epoch = bind.epoch;
    r.load_generation = bind.load;
    r.valid = AZ_AVAIL_STALE;
    r.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_CUES);
    r.attempt = bind.provider.load(std::memory_order_acquire);
    r.memory_serial = bind.serial.load(std::memory_order_acquire);
    azn_emit(&r);
}
/* owned_holder/content_pin are supplied by a real setter/factory, or by the
 * cache of still-pinned objects. A raw pointer read from the pack is used
 * only for comparison, never to borrow or resurrect the target. */
bool fill(unsigned deck, Pin &binding, uint64_t holder_token, uint64_t content_token, bool explicit_empty,
          bool committed, azstatus_native_receipt &out) {
    if (!current(deck, binding)) return false;
    const uintptr_t holder = object_word(binding.object + 0xe8);
    Pin *hp = pin_for(holder_token), *cp = pin_for(content_token);
    if ((hp && hp->object != holder) || (cp && (!hp || object_word(holder + 0x28) != cp->object))) return false;
    if (explicit_empty && (!hp || object_word(holder + 0x28) != 0)) return false;
    if (binding.publishing.test_and_set(std::memory_order_acquire)) return false;
    struct Guard {
        Pin &pin;
        ~Guard() { pin.publishing.clear(std::memory_order_release); }
    } guard {binding};
    if (!current(deck, binding) || object_word(binding.object + 0xe8) != holder ||
        (cp && object_word(holder + 0x28) != cp->object) || (explicit_empty && object_word(holder + 0x28))) return false;
    uintptr_t previous = binding.holder.load(std::memory_order_relaxed);
    uint64_t provider = binding.provider.load(std::memory_order_relaxed);
    if (!provider || previous != holder) {
        uint64_t n = new_id(providers, AZSP_SERIAL_MAX);
        if (!n) {
            memory_loss(deck, binding);
            return false;
        }
        provider = AZSP_MEMORY | n;
        binding.holder.store(holder, std::memory_order_relaxed);
        binding.provider.store(provider, std::memory_order_relaxed);
        binding.confirmed_empty = false;
        committed = true;
    }
    if (holder_token && hold(holder_token)) {
        uint64_t old = binding.holder_pin.exchange(holder_token, std::memory_order_acq_rel);
        if (old) drop(old);
    } else if (previous != holder) {
        uint64_t old = binding.holder_pin.exchange(0, std::memory_order_acq_rel);
        if (old) drop(old);
    }
    if (content_token && hold(content_token)) {
        uint64_t old = binding.latest_pin.exchange(content_token, std::memory_order_acq_rel);
        if (old) drop(old);
    } else if (explicit_empty || previous != holder) {
        uint64_t old = binding.latest_pin.exchange(0, std::memory_order_acq_rel);
        if (old) drop(old);
    }
    uint64_t revision = binding.serial.load(std::memory_order_relaxed);
    if (content_token || (hp && object_word(holder + 0x28))) binding.confirmed_empty = false;
    else if (explicit_empty) binding.confirmed_empty = true;
    if (committed) {
        if (revision == UINT64_MAX) {
            memory_loss(deck, binding);
            return false;
        }
        binding.serial.store(++revision, std::memory_order_relaxed);
    }
    out = {};
    out.size = sizeof out;
    out.version = 1;
    out.kind = AZN_MEMORY;
    out.deck = deck;
    out.unit = binding.unit;
    out.epoch = binding.epoch;
    out.load_generation = binding.load;
    out.attempt = provider;
    out.memory_serial = revision;
    if (content_token && hold(content_token)) {
        out.memory_pin = content_token;
        out.valid = AZ_AVAIL_READY;
    } else out.valid = hp && binding.confirmed_empty ? AZ_AVAIL_EMPTY : AZ_AVAIL_LOADING;
    return true;
}
int seed(unsigned deck, azstatus_native_receipt *out) {
    if (deck >= 4 || !out || out->size < sizeof *out || out->version != 1) return 0;
    Hold pack(active[deck].load(std::memory_order_acquire));
    Pin *binding = pack.get();
    if (!binding || !current(deck, *binding)) return 0;
    auto &replacement = holder_writers[cache_slot(binding->object)];
    if (replacement.count.load(std::memory_order_acquire)) return 0;
    uint64_t replace_cut = replacement.interference.load(std::memory_order_acquire);
    uintptr_t holder = object_word(binding->object + 0xe8);
    auto &write = writers[cache_slot(holder)];
    if (write.count.load(std::memory_order_acquire)) return 0;
    uint64_t cut = write.interference.load(std::memory_order_acquire);
    uint64_t h = holder ? known(cached_holder, holder) : 0;
    if (!h && holder) {
        Hold prior(binding->holder_pin.load(std::memory_order_acquire));
        if (prior.token && prior.get()->object == holder && hold(prior.token)) h = prior.token;
    }
    uintptr_t content = h ? object_word(holder + 0x28) : 0;
    uint64_t c = content ? known(cached_content, content) : 0;
    if (!c && content) {
        Hold prior(binding->latest_pin.load(std::memory_order_acquire));
        if (prior.token && prior.get()->object == content && hold(prior.token)) c = prior.token;
    }
    bool ok = fill(deck, *binding, h, c, false, false, *out);
    if (ok && (write.count.load(std::memory_order_acquire) || cut != write.interference.load(std::memory_order_acquire) ||
               replacement.count.load(std::memory_order_acquire) || replace_cut != replacement.interference.load(std::memory_order_acquire))) {
        if (out->memory_pin) drop(out->memory_pin);
        ok = false;
    }
    if (c) drop(c);
    if (h) drop(h);
    return ok;
}
void send(azstatus_native_receipt &r) {
    if ((!(azn_interest(r.deck) & AZ_STATUS_TOPIC(AZ_STATUS_CUES)) || !azn_offer(&r)) && r.memory_pin) drop(r.memory_pin);
}
void seed_active(unsigned deck) {
    azstatus_native_receipt r {};
    r.size = sizeof r;
    r.version = 1;
    if (seed(deck, &r)) send(r);
}
struct Writer {
    Writer *previous;
    uintptr_t holder, content;
    uint64_t holder_token, content_token;
    Writing &bucket;
    uint64_t cut;
    bool nested = false, contended = false, witnessed = false;
    static thread_local Writer *top;
    Writer(uintptr_t h, uintptr_t c) : previous(top), holder(h), content(c), holder_token(remember(cached_holder, h, profile.holder)), content_token(c ? remember(cached_content, c, profile.content) : 0), bucket(writers[cache_slot(h)]), cut(bucket.interference.load(std::memory_order_acquire)) {
        top = this;
        for (auto p = previous; p; p = p->previous)
            if (p->holder == h && p->witnessed) {
                nested = true;
                contended = p->uncertain();
                cut = p->cut;
                break;
            }
        if (!nested && bucket.count.fetch_add(1, std::memory_order_acq_rel)) {
            contended = true;
            bucket.interference.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    bool uncertain() const { return contended || cut != bucket.interference.load(std::memory_order_acquire); }
    ~Writer() {
        top = previous;
        if (!nested) {
            bool last = bucket.count.fetch_sub(1, std::memory_order_acq_rel) == 1;
            if (uncertain()) {
                for (unsigned d = 0; d < 4; ++d) {
                    Hold bind(active[d].load(std::memory_order_acquire));
                    Pin *b = bind.get();
                    if (!b || !current(d, *b)) continue;
                    uintptr_t h = object_word(b->object + 0xe8);
                    if (h == holder) memory_loss(d, *b);
                    if (last && cache_slot(h) == cache_slot(holder)) seed_active(d);
                }
            }
        }
        if (holder_token) drop(holder_token);
        if (content_token) drop(content_token);
    }
};
thread_local Writer *Writer::top = nullptr;
template <class F>
F original(unsigned i) {
    return reinterpret_cast<F>(__atomic_load_n(originals + i, __ATOMIC_ACQUIRE));
}
void on_broadcast(void *self, void *listener) {
    uintptr_t holder = reinterpret_cast<uintptr_t>(self);
    Writer *w = Writer::top;
    while (w && w->holder != holder) w = w->previous;
    if (w && !w->witnessed) {
        w->witnessed = true;
        for (unsigned d = 0; d < 4; ++d) {
            Hold bind(active[d].load(std::memory_order_acquire));
            Pin *b = bind.get();
            if (!b || !current(d, *b) || object_word(b->object + 0xe8) != holder) continue;
            auto &replacement = holder_writers[cache_slot(b->object)];
            if (replacement.count.load(std::memory_order_acquire)) {
                replacement.interference.fetch_add(1, std::memory_order_acq_rel);
                memory_loss(d, *b);
                continue;
            }
            if (w->uncertain()) continue; // last writer emits a scoped cut and an owned seed
            if (!w->holder_token || (w->content && !w->content_token) || object_word(holder + 0x28) != w->content) {
                memory_loss(d, *b);
                continue;
            }
            azstatus_native_receipt r {};
            if (fill(d, *b, w->holder_token, w->content_token, !w->content, true, r)) send(r);
            else memory_loss(d, *b);
        }
    }
    original<void (*)(void *, void *)>(2)(self, listener);
}
bool set_content(void *self, const uintptr_t *input, void *listener) {
    auto fn = original<bool (*)(void *, const uintptr_t *, void *)>(1);
    if (!observing() || !input) return fn(self, input, listener);
    Writer w(reinterpret_cast<uintptr_t>(self), *input);
    bool changed = fn(self, input, listener);
    if (changed && !w.witnessed) {
        for (unsigned d = 0; d < 4; ++d) {
            Hold bind(active[d].load(std::memory_order_acquire));
            Pin *b = bind.get();
            if (b && current(d, *b) && object_word(b->object + 0xe8) == w.holder) memory_loss(d, *b);
        }
    }
    return changed;
}
void set_holder(void *builder, const uintptr_t *where, const uintptr_t *input) {
    auto fn = original<void (*)(void *, const uintptr_t *, const uintptr_t *)>(0);
    if (!observing() || !where || !input || !typed(*where, profile.pack)) {
        fn(builder, where, input);
        return;
    }
    const uintptr_t pack = *where, target = *input;
    uint64_t h = target ? remember(cached_holder, target, profile.holder) : 0;
    // advance() can invoke another setter before returning. A post-call seed
    // alone would silently collapse A/B/A. Mark overlap, then reseed only when
    // every owner in this bucket has left its replacement call.
    struct Scope {
        Writing &gate;
        uintptr_t pack, target;
        uint64_t pin, cut;
        bool overlap;
        ~Scope() {
            bool last = gate.count.fetch_sub(1, std::memory_order_acq_rel) == 1;
            bool lost = overlap || cut != gate.interference.load(std::memory_order_acquire);
            for (unsigned d = 0; d < 4; ++d) {
                Hold bind(active[d].load(std::memory_order_acquire));
                Pin *b = bind.get();
                if (!b || !current(d, *b)) continue;
                if (b->object == pack && (lost || object_word(pack + 0xe8) != target)) memory_loss(d, *b);
                if (last && (b->object == pack || (lost && cache_slot(b->object) == cache_slot(pack)))) seed_active(d);
            }
            if (pin) drop(pin);
        }
    };
    auto &gate = holder_writers[cache_slot(pack)];
    uint64_t cut = gate.interference.load(std::memory_order_acquire);
    bool overlap = gate.count.fetch_add(1, std::memory_order_acq_rel) != 0;
    if (overlap) gate.interference.fetch_add(1, std::memory_order_acq_rel);
    Scope scope {gate, pack, target, h, cut, overlap};
    fn(builder, where, input);
}
uintptr_t create_holder(void *manager, const void *id, const uintptr_t *input) {
    /* This factory constructs a holder with the caller's existing Content ref,
     * before it is attached to any deck's pack. */
    uint64_t c = observing() && input && *input ? remember(cached_content, *input, profile.content) : 0;
    auto result = original<uintptr_t (*)(void *, const void *, const uintptr_t *)>(3)(manager, id, input);
    if (c) drop(c);
    return result;
}
/* Stock returns an intrusive holder by value using AAPCS64's x8 indirect
 * result. A nontrivial C++ return preserves that calling convention. Ownership
 * belongs to the original caller; this wrapper only pins additional leases. */
struct HolderResult {
    uintptr_t object;
    ~HolderResult() {}
};
HolderResult cache_update(const unsigned char *track, const uintptr_t *input, void *repository) {
    auto fn = original<HolderResult (*)(const unsigned char *, const uintptr_t *, void *)>(4);
    if (!observing() || !track || !input) return fn(track, input, repository);
    const uintptr_t target = *input;
    uint64_t c = target ? remember(cached_content, target, profile.content) : 0;
    struct Staged {
        alignas(Writer) unsigned char storage[4][sizeof(Writer)];
        Writer *writers[4] = {};
        unsigned count = 0;
        uint64_t token;
        explicit Staged(uint64_t t) : token(t) {}
        ~Staged() {
            while (count) writers[--count]->~Writer();
            if (token) drop(token);
        }
    } stage(c);
    for (unsigned d = 0; d < 4; ++d) {
        Hold bind(active[d].load(std::memory_order_acquire));
        Pin *p = bind.get();
        if (!p || !current(d, *p) || std::memcmp(reinterpret_cast<const void *>(p->object + 0x38), track, 12)) continue;
        uintptr_t holder = object_word(p->object + 0xe8);
        uint64_t hp = known(cached_holder, holder);
        if (!hp) continue;
        bool present = false;
        for (unsigned i = 0; i < stage.count; ++i)
            if (stage.writers[i]->holder == holder) present = true;
        if (!present) {
            stage.writers[stage.count] = new (stage.storage[stage.count]) Writer(holder, target);
            ++stage.count;
        }
        drop(hp);
    }
    HolderResult result = fn(track, input, repository);
    if (typed(result.object, profile.holder)) {
        uint64_t h = remember(cached_holder, result.object, profile.holder);
        bool captured = false;
        for (unsigned i = 0; i < stage.count; ++i)
            if (stage.writers[i]->holder == result.object && stage.writers[i]->witnessed) captured = true;
        if (!captured && h && (!target || c) && object_word(result.object + 0x28) == target) {
            for (unsigned d = 0; d < 4; ++d) {
                Hold bind(active[d].load(std::memory_order_acquire));
                Pin *p = bind.get();
                if (!p || !current(d, *p) || object_word(p->object + 0xe8) != result.object) continue;
                azstatus_native_receipt r {};
                if (fill(d, *p, h, c, !target, false, r)) {
                    if (p->serial.load(std::memory_order_acquire) > 1) r.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_CUES);
                    send(r);
                }
            }
        }
        if (h) drop(h);
    }
    return result;
}
/* Decode on the single core consumer while the immutable Content is pinned.
 * The Content constructor copies the 0x80-byte vector rows. Commit setters
 * replace the entire Content. Optional raw memory palette and RGB are distinct. */
bool coordinate(uint64_t samples, uint64_t us, uint32_t rate, double &seconds) {
    if (!rate || rate >= 0x7fffffffu) return false;
    const int sample_range = compare_timestamps(samples, UINT64_C(9007199254740992), rate, rate, 1);
    const int us_range = compare_timestamps(us, UINT64_C(9007199254740992), 1000000, rate, 1);
    if (sample_range == AZ_TIME_INVALID || us_range == AZ_TIME_INVALID ||
        sample_range > AZ_TIME_EQUAL || us_range > AZ_TIME_EQUAL) return false;
    using W = unsigned __int128;
    const int consistent = compare_timestamps(W(samples) * 1000000u, W(us) * rate, uint64_t(rate) * 1000000u, rate, 1);
    if (consistent == AZ_TIME_INVALID || consistent != AZ_TIME_EQUAL) return false;
    seconds = double(samples) / rate;
    return true;
}
int read_rows(uint64_t token, azn_memory_rows *out) {
    if (!out || out->size < offsetof(azn_memory_rows, clock_version) || out->version != 1) return 0;
    const bool clocks = out->size >= offsetof(azn_memory_rows, stored_ms);
    const bool storage_keys = out->size >= sizeof *out;
    if (clocks) out->clock_version = 0;
    Hold content(token);
    Pin *p = content.get();
    if (!p || !typed(p->object, profile.content)) return 0;
    uintptr_t start = object_word(p->object + 0x28), end = object_word(p->object + 0x30);
    out->count = 0;
    out->availability = AZ_AVAIL_STALE;
    if (end < start || (end - start) % 0x80 || (end - start) / 0x80 > AZ_NATIVE_CUE_LIMIT || (start != end && !ptr(start))) return 1;
    unsigned count = unsigned((end - start) / 0x80);
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t row = start + i * 0x80;
        auto &entry = out->rows[i];
        entry = {};
        if (scalar<unsigned char>(row) != 0) return 1;
        const auto t = azcue_read_timestamps(reinterpret_cast<const void *>(row));
        if (!coordinate(t.in_sample, t.in_us, t.sample_rate, entry.start_s)) return 1;
        const uint64_t absent = UINT64_C(0x7fffffffffffffff);
        // Both stock profiles retain zero in the OUT sample cache of ordinary
        // memory points while the OUT microseconds carry the absent marker.
        bool loop = t.out_us != absent;
        if (!loop && t.out_sample != absent) {
            const int empty_cache = compare_timestamps(t.out_sample, 0, t.sample_rate, t.sample_rate, 1);
            if (empty_cache == AZ_TIME_INVALID || empty_cache != AZ_TIME_EQUAL) return 1;
        }
        if (loop) {
            const int interval = compare_timestamps(t.out_sample, t.in_sample, t.sample_rate, t.sample_rate, 1);
            if (interval == AZ_TIME_INVALID || interval != AZ_TIME_AFTER ||
                !coordinate(t.out_sample, t.out_us, t.sample_rate, entry.end_s)) return 1;
        }
        if (clocks) {
            out->points[i][0] = {AZN_COORD, t.sample_rate, static_cast<int64_t>(t.in_sample)};
            out->points[i][1] = {loop ? AZN_COORD : AZN_ABSENT, t.sample_rate,
                                 loop ? static_cast<int64_t>(t.out_sample) : 0};
        }
        if (storage_keys) {
            out->stored_ms[i][0] = t.in_us / 1000;
            out->stored_ms[i][1] = loop ? t.out_us / 1000 : 0;
        }
        entry.size = sizeof entry;
        entry.version = AZ_DATA_VERSION;
        entry.entry_id = i + 1;
        entry.classification = AZ_CUE_MEMORY;
        entry.type = loop ? AZ_CUE_LOOP : AZ_CUE_POINT;
        entry.valid = AZ_CUE_START | AZ_CUE_COLOR | (loop ? AZ_CUE_END : 0);
        entry.saved = entry.modified = -1;
        entry.rgb = scalar<uint32_t>(row + 0x78) & 0xffffff;
        unsigned automatic = scalar<unsigned char>(row + 0x28);
        entry.auto_active = -1;
        if (automatic <= 1) {
            entry.auto_active = automatic;
            entry.valid |= AZ_CUE_AUTO_ACTIVE;
        }
        unsigned palette = scalar<unsigned char>(row + 0x7c);
        if (palette <= 8) {
            entry.valid |= AZ_CUE_COLOR_ID;
            entry.color_id = palette;
        }
        uintptr_t comment = object_word(row + 0x70);
        if (comment) {
            azcue_copy_utf8_prefix(entry.comment, reinterpret_cast<const char *>(comment));
            entry.valid |= AZ_CUE_COMMENT;
        }
    }
    out->count = count;
    out->availability = count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    if (clocks) out->clock_version = storage_keys ? 2 : 1;
    return 1;
}
uint64_t now_epoch(unsigned deck) {
    return azn_lifecycle_fence(deck);
}
#include "sdk_status_memory_recall.inc"
#include "sdk_status_file_cues.inc"
#if defined(__linux__)
int main_image(dl_phdr_info *info, size_t, void *result) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    *static_cast<uintptr_t *>(result) = info->dlpi_addr;
    return 1;
}
#endif
} // namespace

extern "C" azn_hot_link azn_memory_copied(unsigned deck, uint64_t fence, uint64_t load, uintptr_t context,
                                          uintptr_t destination, uintptr_t source, uint32_t previous_revision, uint64_t *file_row) {
    if (file_row) *file_row = 0;
    auto frame = recall_frame;
    if (!frame || deck >= 4 || !context || !frame->owned || !frame->candidates ||
        !typed(frame->facade, recall_bias + recall_profile->facade_table) || object_word(frame->facade + 0x60) != context) return {};
    const auto &edition = frame->candidates->deck[deck];
    if (!edition.entry || edition.epoch != fence || edition.load != load ||
        scalar<uint32_t>(destination + 8) != previous_revision + 1u) return {};
    const uintptr_t retained = object_word(context + 0x18);
    if (destination == retained) {
        frame->retained = source == frame->owned ? retained : 0;
        frame->retained_revision = previous_revision + 1u;
        return {};
    }
    /* The stock CueLoadFacade first copies the task property to kind zero.
     * Its loader increments the retained revision once more on committing the
     * kind. Only that same retained property can establish the working copy. */
    if (destination != context + 0x160 || !frame->retained || frame->retained != source || source != retained ||
        scalar<uint32_t>(source + 8) != frame->retained_revision + 1u) return {};
    Hold a(edition.pack_token), c(edition.content_token);
    if (!a.token || !c.token) return {};
    const auto now = selected_edition(deck, a.get()->object, c.get()->object, edition.entry);
    if (now.unit != edition.unit || now.epoch != fence || now.load != load || now.provider != edition.provider ||
        now.serial != edition.serial || now.pack_token != a.token || now.content_token != c.token) return {};
    if (file_row) *file_row = imported_memory_row(c.get()->object, edition.entry);
    return {edition.provider, edition.serial, edition.entry, AZN_MEMORY_LINK_READY};
}

extern "C" uint64_t azn_owned_pin(uintptr_t owned) { return ptr(owned) ? make_pin(owned) : 0; }
extern "C" int azn_pin_hold(uint64_t token, uintptr_t *object) {
    if (!object || !hold(token)) return 0;
    *object = pin_for(token)->object;
    return 1;
}
extern "C" void azn_pin_drop(uint64_t token) {
    if (token) drop(token);
}
extern "C" int azn_pack_current(unsigned deck, const azn_pack_view *view) {
    if (deck >= 4 || !view || !view->token || active[deck].load(std::memory_order_acquire) != view->token) return 0;
    Pin *pin = pin_for(view->token);
    return pin && pin->object == view->object && pin->epoch == view->fence && pin->load == view->load && pin->unit == view->unit &&
           current(deck, *pin) && active[deck].load(std::memory_order_acquire) == view->token;
}
extern "C" int azn_pack_acquire(unsigned deck, azn_pack_view *out) {
    if (deck >= 4 || !out) return 0;
    uint64_t token = active[deck].load(std::memory_order_acquire);
    if (!hold(token)) return 0;
    Pin *p = pin_for(token);
    const azn_pack_view view = {token, p->unit, p->epoch, p->load, p->object};
    if (!azn_pack_current(deck, &view)) {
        drop(token);
        return 0;
    }
    *out = view;
    return 1;
}
extern "C" int azn_pack_retains(uintptr_t pack) {
    if (!pack) return 0;
    for (const auto &slot : active) {
        const auto token = slot.load(std::memory_order_acquire);
        if (!token) continue;
        const Hold held(token);
        if (!held.token || held.get()->object == pack || slot.load(std::memory_order_acquire) != token)
            return 1;
    }
    return 0;
}
extern "C" int azn_pack_observe(azn_pack_observer observer) {
    if (!observer) return 0;
    azn_pack_observer expected = nullptr;
    if (!pack_observer.compare_exchange_strong(expected, observer, std::memory_order_acq_rel))
        return expected == observer;
    for (unsigned d = 0; d < 4; ++d) {
        azn_pack_view view {};
        if (azn_pack_acquire(d, &view)) {
            observer(d, view.unit, view.fence, view.load);
            drop(view.token);
        }
    }
    return 1;
}
extern "C" int azn_memory_configure(int build, azexit_install_fn install) {
#if defined(__linux__) && defined(__aarch64__)
    if (build < 1 || build > 2 || !install || enabled.load(std::memory_order_acquire)) return 0;
    uintptr_t bias = 0;
    dl_iterate_phdr(main_image, &bias);
    profile = mem_profiles[build - 1];
    profile.pack += bias;
    profile.holder += bias;
    profile.content += bias;
    pack_profile_ready.store(true, std::memory_order_release);
    void *hooks[] = {reinterpret_cast<void *>(set_holder), reinterpret_cast<void *>(set_content),
                     reinterpret_cast<void *>(on_broadcast), reinterpret_cast<void *>(create_holder), reinterpret_cast<void *>(cache_update)};
    for (unsigned i = 0; i < 5; ++i) {
        profile.entry[i].address += bias;
        if (std::memcmp(reinterpret_cast<const void *>(profile.entry[i].address), profile.entry[i].prologue, 16)) return 0;
    }
    for (unsigned i = 0; i < 5; ++i)
        if (!install(profile.entry[i].address, hooks[i], &originals[i]) || !originals[i]) return 0;
    enabled.store(true, std::memory_order_release);
    install_recall(build, bias, install);
    if (connected.load(std::memory_order_acquire))
        for (unsigned d = 0; d < 4; ++d) azn_capability(d, AZN_CAP_MEMORY | (recall_ready ? AZN_CAP_MEMORY_LINK : 0), 1);
    return 1;
#else
    (void) build;
    (void) install;
    return 0;
#endif
}
extern "C" void azn_memory_bind(unsigned deck, uint64_t unit, uint64_t epoch, uint64_t load,
                                uintptr_t pack, const unsigned char track[12]) {
    if (deck >= 4 || !epoch || !unit || !load || !pack_profile_ready.load(std::memory_order_acquire) ||
        (!observing() && !pack_observer.load(std::memory_order_acquire)) ||
        now_epoch(deck) != epoch || !typed(pack, profile.pack) || !scalar<uint32_t>(pack + 0x30) ||
        scalar<unsigned char>(pack + 0x78) != 1 || std::memcmp(reinterpret_cast<const void *>(pack + 0x38), track, 12) ||
        std::memcmp(reinterpret_cast<const void *>(pack + 0x80), track, 12)) return;
    uint64_t token = make_pin(pack, epoch, load, unit);
    if (!token) return;
    uint64_t previous = active[deck].load(std::memory_order_acquire);
    bool installed = false;
    for (unsigned n = 0; n < 8 && now_epoch(deck) == epoch; ++n) {
        Hold old(previous);
        if (old.token && old.get()->epoch >= epoch) break;
        if (active[deck].compare_exchange_strong(previous, token, std::memory_order_acq_rel)) {
            installed = true;
            break;
        }
    }
    if (!installed) {
        drop(token);
        return;
    }
    if (previous) drop(previous);
    if (now_epoch(deck) != epoch) {
        uint64_t expected = token;
        if (active[deck].compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) drop(token);
        return;
    }
    auto observer = pack_observer.load(std::memory_order_acquire);
    if (observer) observer(deck, unit, epoch, load);
    if (observing()) seed_active(deck);
}
extern "C" void azn_memory_retire(unsigned deck, uint64_t fence) {
    if (deck >= 4) return;
    Hold current(active[deck].load(std::memory_order_acquire));
    if (!current.token || (fence && current.get()->epoch >= fence)) return;
    uint64_t token = current.token;
    if (active[deck].compare_exchange_strong(token, 0, std::memory_order_acq_rel)) {
        drop(current.token);
        auto observer = pack_observer.load(std::memory_order_acquire);
        if (observer) observer(deck, current.get()->unit, fence, 0);
    }
}
extern "C" void azn_memory_maintenance(void) {
    if (!reap_needed.exchange(false, std::memory_order_acq_rel)) return;
    for (auto &p : pool) {
        uint64_t s = p.state.load(std::memory_order_acquire);
        if (!s || s == busy || (s & refs_mask) || !p.state.compare_exchange_strong(s, busy, std::memory_order_acq_rel)) continue;
        uint64_t h = p.holder_pin.exchange(0, std::memory_order_relaxed), c = p.latest_pin.exchange(0, std::memory_order_relaxed);
        if (h) drop(h);
        if (c) drop(c);
        azn_ref_release(p.object);
        p.state.store(0, std::memory_order_release);
    }
}
extern "C" void azn_file_row_hold(uint64_t token, int acquire) {
    const auto cb = file_callbacks.load(std::memory_order_acquire);
    if (token && cb && cb->size >= offsetof(azn_file_cue_callbacks, hold) + sizeof cb->hold && cb->hold)
        cb->hold(&token, 1, acquire);
}
extern "C" const azn_file_cue_api *azn_file_service(void) {
    static const azn_file_cue_api api = {sizeof api, 1, file_install, file_transport, observe_file_input,
                                       file_maps_install, file_maps_origins, file_maps_hold};
    return &api;
}
extern "C" const azn_memory_api *azn_memory_service(void) {
    static const azn_memory_api api = {sizeof api, 1, read_rows, drop, seed, [] {
                                           connected.store(true, std::memory_order_release);
                                           if (enabled.load(std::memory_order_acquire))
                                               for (unsigned d = 0; d < 4; ++d) azn_capability(d, AZN_CAP_MEMORY | (recall_ready ? AZN_CAP_MEMORY_LINK : 0), 1);
                                       }};
    return &api;
}
