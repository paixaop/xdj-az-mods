/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <stdbool.h>
#include "../common/aznative_c.h"
#if defined(__linux__)
#include <link.h>
#endif


typedef struct Entry Entry;
typedef struct MemProfile MemProfile;
struct Entry {
    uintptr_t address;
    uint32_t prologue[4];
};
struct MemProfile {
    uintptr_t pack, holder, content;
    Entry entry[5];
};
typedef struct Pin Pin;
typedef struct Writing Writing;
typedef struct Hold Hold;
typedef struct Writer Writer;
typedef struct HolderScope HolderScope;
typedef struct HolderStage HolderStage;
typedef struct RecallProfile RecallProfile;
typedef struct MemoryEdition MemoryEdition;
typedef struct RecallCandidates RecallCandidates;
typedef struct RecallFrame RecallFrame;
typedef struct PointerScope PointerScope;
typedef struct RecallJob RecallJob;
typedef struct FileInputOrigin FileInputOrigin;
typedef struct FileInputUse FileInputUse;
typedef struct FileFreshInput FileFreshInput;
typedef struct FileCueProfile FileCueProfile;
typedef struct FileWireRow FileWireRow;
typedef struct FileServe FileServe;
typedef struct FileAtomOwner FileAtomOwner;
typedef struct FileRead FileRead;
typedef struct FileCacheCall FileCacheCall;
typedef struct FileOrigins FileOrigins;
typedef struct FileRoute FileRoute;
typedef struct FileGuard FileGuard;
typedef struct FileKey FileKey;
typedef struct FileBank FileBank;
typedef struct FileImport FileImport;
typedef struct FileContent FileContent;
typedef struct FileMapProfile FileMapProfile;
typedef struct MapBytes MapBytes;
typedef struct MapServe MapServe;
typedef struct MapRoute MapRoute;
typedef struct MapGuard MapGuard;
typedef struct MapImport MapImport;
typedef struct MapPending MapPending;
typedef struct MapContentOrigin MapContentOrigin;
#include "sdk_status_memory_profiles.h"
MemProfile profile = {0};
void *originals[5];
_Atomic(bool) enabled = false;
_Atomic(bool) connected = false;
_Atomic(bool) pack_profile_ready = false;
_Atomic(azn_pack_observer) pack_observer = NULL;
static bool observing() { return atomic_load_explicit(&(enabled), memory_order_acquire) && atomic_load_explicit(&(connected), memory_order_acquire); }
enum {
    pool_count = 256,
    cache_count = 32
};
enum {
    refbits = 16,
    refs_mask = (1u << refbits) - 1
};
static const uint64_t busy = UINT64_MAX;
struct Pin {
    /* High bits are a nonreused incarnation. A zero low count is retired.
     * Only the reducer can recycle a retired object's storage. */
    _Atomic(uint64_t) state;
    uintptr_t object;
    uint64_t epoch, load, unit;
    _Atomic(uintptr_t) holder;
    _Atomic(uint64_t) provider, serial, holder_pin, latest_pin;
    bool confirmed_empty; // protected by publishing
    atomic_flag publishing;
};
Pin pool[pool_count] = {[0 ... pool_count - 1] = {.publishing = ATOMIC_FLAG_INIT}};
_Atomic(uint64_t) sequence = 0, providers = 0;
_Atomic(bool) reap_needed = false;
_Atomic(uint64_t) active[4] = {0};
_Atomic(uint64_t) cached_holder[cache_count] = {0}, cached_content[cache_count] = {0};
struct Writing {
    _Atomic(unsigned) count;
    _Atomic(uint64_t) interference;
};
Writing writers[cache_count];
Writing holder_writers[cache_count];

#define SCALAR(type, address) ({ type scalar_value; memcpy(&scalar_value, (const void *)(address), sizeof scalar_value); scalar_value; })
static bool ptr(uintptr_t addr) { return addr >= 0x10000 && addr <= UINT64_C(0x0000ffffffffffff) && !(addr & 7); }
static uintptr_t object_word(uintptr_t addr) {
    return __atomic_load_n(((const uintptr_t *) (addr)), __ATOMIC_ACQUIRE);
}
static bool typed(uintptr_t obj, uintptr_t table) { return ptr(obj) && object_word(obj) == table; }
static uint64_t new_id(_Atomic(uint64_t) *seq, uint64_t maximum) {
    uint64_t value = atomic_load_explicit(seq, memory_order_relaxed);
    for (unsigned n = 0; n < 8 && value < maximum; ++n)
        if (atomic_compare_exchange_strong_explicit(seq, &value, value + 1, memory_order_relaxed, memory_order_relaxed)) return value + 1;
    return 0;
}
Pin *pin_for(uint64_t token) {
    unsigned index = (unsigned) (token & 511u);
    return token && index < pool_count ? &pool[index] : NULL;
}
static bool hold(uint64_t token) {
    Pin *p = pin_for(token);
    if (!p) return false;
    const uint64_t identity = token >> 9;
    uint64_t state = atomic_load_explicit(&(p->state), memory_order_acquire);
    for (unsigned n = 0; n < 8 && state >> refbits == identity && (state & refs_mask) && (state & refs_mask) < refs_mask; ++n)
        if (atomic_compare_exchange_strong_explicit(&(p->state), &(state), state + 1, memory_order_acq_rel, memory_order_acquire)) return true;
    return false;
}
static void drop(uint64_t token) {
    Pin *p = pin_for(token);
    if (!p) return;
    /* Callers own exactly one reference, so neither retirement nor reuse can race this RMW. */
    uint64_t old = atomic_fetch_sub_explicit(&(p->state), 1, memory_order_acq_rel);
    if ((old & refs_mask) == 1 && !atomic_exchange_explicit(&(reap_needed), true, memory_order_acq_rel)) azn_notify();
}
struct Hold {
    uint64_t token;
};
static void hold_drop(struct Hold *held) {
    if (held->token) drop(held->token);
}
#define HOLD(name, id) struct Hold name __attribute__((cleanup(hold_drop))) = {hold(id) ? (id) : 0}
/* Evaluate the requested incarnation exactly once before attempting a hold. */
static struct Hold hold_acquire(uint64_t id) { return (struct Hold) {hold(id) ? id : 0}; }
#undef HOLD
#define HOLD(name, id) struct Hold name __attribute__((cleanup(hold_drop))) = hold_acquire(id)
static void publishing_unlock(atomic_flag **flag) { atomic_flag_clear_explicit(*flag, memory_order_release); }
#define PUBLISHING_GUARD(name, pin) atomic_flag *name __attribute__((cleanup(publishing_unlock))) = &(pin)->publishing
static uint64_t make_pin(uintptr_t object, uint64_t epoch, uint64_t load, uint64_t unit) {
    uint64_t id = new_id(&(sequence), (UINT64_C(1) << 46) - 1);
    if (!id) return 0;
    for (unsigned n = 0; n < pool_count; ++n) {
        unsigned i = (unsigned) (id + n) % pool_count;
        uint64_t empty = 0;
        if (!atomic_compare_exchange_strong_explicit(&(pool[i].state), &(empty), busy, memory_order_acq_rel, memory_order_acquire)) continue;
        Pin *p = &(pool[i]);
        if (!azn_ref_pin(object)) {
            atomic_store_explicit(&(p->state), 0, memory_order_release);
            return 0;
        }
        p->object = object;
        p->epoch = epoch;
        p->load = load;
        p->unit = unit;
        p->confirmed_empty = false;
        atomic_store_explicit(&(p->holder), 0, memory_order_relaxed);
        atomic_store_explicit(&(p->provider), 0, memory_order_relaxed);
        atomic_store_explicit(&(p->serial), 0, memory_order_relaxed);
        atomic_store_explicit(&(p->holder_pin), 0, memory_order_relaxed);
        atomic_store_explicit(&(p->latest_pin), 0, memory_order_relaxed);
        atomic_flag_clear_explicit(&(p->publishing), memory_order_relaxed);
        atomic_store_explicit(&(p->state), (id << refbits) | 1, memory_order_release);
        return (id << 9) | i;
    }
    return 0;
}
static unsigned cache_slot(uintptr_t obj) { return (unsigned) ((obj >> 4) ^ (obj >> 12)) % cache_count; }
static uint64_t known(_Atomic(uint64_t) *cache, uintptr_t obj) {
    for (unsigned i = 0; i < cache_count; i++) {
        uint64_t token = atomic_load_explicit(&(cache[(cache_slot(obj) + i) % cache_count]), memory_order_acquire);
        if (!hold(token)) continue;
        if (pin_for(token)->object == obj) return token;
        drop(token);
    }
    return 0;
}
/* The caller owns object. Keep a small bounded seed cache even before a pack
 * matches a deck. A numeric address lookup succeeds only while the cache
 * holds a genuine reference to this exact object. */
static uint64_t remember(_Atomic(uint64_t) *cache, uintptr_t object, uintptr_t table) {
    if (!typed(object, table)) return 0;
    uint64_t prior = known(cache, object);
    if (prior) return prior;
    uint64_t token = make_pin(object, 0, 0, 0);
    if (!token || !hold(token)) {
        if (token) drop(token);
        return 0;
    }
    for (unsigned i = 0; i < cache_count; i++) {
        uint64_t empty = 0;
        if (atomic_compare_exchange_strong_explicit(&(cache[(cache_slot(object) + i) % cache_count]), &(empty), token, memory_order_acq_rel, memory_order_acquire)) return token;
    }
    prior = atomic_exchange_explicit(&(cache[cache_slot(object)]), token, memory_order_acq_rel);
    if (prior) drop(prior);
    return token;
}
static bool current(unsigned deck, const Pin *bind) {
    struct azsdk_load_sample load = {0};
    return azsdk_load_read((int) (deck), &load) && load.unit_generation == bind->unit && load.load_generation == bind->load &&
           load.invalidation_epoch == bind->epoch;
}
static void memory_loss(unsigned deck, const Pin *bind) {
    if (!(azn_interest(deck) & AZ_STATUS_TOPIC(AZ_STATUS_CUES))) return;
    struct azstatus_native_receipt r = {0};
    r.kind = AZN_MEMORY;
    r.deck = deck;
    r.unit = bind->unit;
    r.epoch = bind->epoch;
    r.load_generation = bind->load;
    r.valid = AZ_AVAIL_STALE;
    r.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_CUES);
    r.attempt = atomic_load_explicit(&(bind->provider), memory_order_acquire);
    r.memory_serial = atomic_load_explicit(&(bind->serial), memory_order_acquire);
    azn_emit(&r);
}
/* owned_holder/content_pin are supplied by a real setter/factory, or by the
 * cache of still-pinned objects. A raw pointer read from the pack is used
 * only for comparison, never to borrow or resurrect the target. */
static bool fill(unsigned deck, Pin *binding, uint64_t holder_token, uint64_t content_token, bool explicit_empty, bool committed, struct azstatus_native_receipt *out) {
    if (!current(deck, binding)) return false;
    const uintptr_t holder = object_word(binding->object + 0xe8);
    Pin *hp = pin_for(holder_token), *cp = pin_for(content_token);
    if ((hp && hp->object != holder) || (cp && (!hp || object_word(holder + 0x28) != cp->object))) return false;
    if (explicit_empty && (!hp || object_word(holder + 0x28) != 0)) return false;
    if (atomic_flag_test_and_set_explicit(&(binding->publishing), memory_order_acquire)) return false;
    PUBLISHING_GUARD(guard, binding);
    if (!current(deck, binding) || object_word(binding->object + 0xe8) != holder ||
        (cp && object_word(holder + 0x28) != cp->object) || (explicit_empty && object_word(holder + 0x28))) return false;
    uintptr_t previous = atomic_load_explicit(&(binding->holder), memory_order_relaxed);
    uint64_t provider = atomic_load_explicit(&(binding->provider), memory_order_relaxed);
    if (!provider || previous != holder) {
        uint64_t n = new_id(&(providers), AZSP_SERIAL_MAX);
        if (!n) {
            memory_loss(deck, binding);
            return false;
        }
        provider = AZSP_MEMORY | n;
        atomic_store_explicit(&(binding->holder), holder, memory_order_relaxed);
        atomic_store_explicit(&(binding->provider), provider, memory_order_relaxed);
        binding->confirmed_empty = false;
        committed = true;
    }
    if (holder_token && hold(holder_token)) {
        uint64_t old = atomic_exchange_explicit(&(binding->holder_pin), holder_token, memory_order_acq_rel);
        if (old) drop(old);
    } else if (previous != holder) {
        uint64_t old = atomic_exchange_explicit(&(binding->holder_pin), 0, memory_order_acq_rel);
        if (old) drop(old);
    }
    if (content_token && hold(content_token)) {
        uint64_t old = atomic_exchange_explicit(&(binding->latest_pin), content_token, memory_order_acq_rel);
        if (old) drop(old);
    } else if (explicit_empty || previous != holder) {
        uint64_t old = atomic_exchange_explicit(&(binding->latest_pin), 0, memory_order_acq_rel);
        if (old) drop(old);
    }
    uint64_t revision = atomic_load_explicit(&(binding->serial), memory_order_relaxed);
    if (content_token || (hp && object_word(holder + 0x28))) binding->confirmed_empty = false;
    else if (explicit_empty) binding->confirmed_empty = true;
    if (committed) {
        if (revision == UINT64_MAX) {
            memory_loss(deck, binding);
            return false;
        }
        atomic_store_explicit(&(binding->serial), ++revision, memory_order_relaxed);
    }
    (*out) = (struct azstatus_native_receipt) {0};
    out->size = sizeof(*out);
    out->version = 1;
    out->kind = AZN_MEMORY;
    out->deck = deck;
    out->unit = binding->unit;
    out->epoch = binding->epoch;
    out->load_generation = binding->load;
    out->attempt = provider;
    out->memory_serial = revision;
    if (content_token && hold(content_token)) {
        out->memory_pin = content_token;
        out->valid = AZ_AVAIL_READY;
    } else out->valid = hp && binding->confirmed_empty ? AZ_AVAIL_EMPTY : AZ_AVAIL_LOADING;
    return true;
}
static int seed(unsigned deck, struct azstatus_native_receipt *out) {
    if (deck >= 4 || !out || out->size < sizeof *out || out->version != 1) return 0;
    HOLD(pack, atomic_load_explicit(&(active[deck]), memory_order_acquire));
    Pin *binding = pin_for(pack.token);
    if (!binding || !current(deck, binding)) return 0;
    __auto_type replacement = &(holder_writers[cache_slot(binding->object)]);
    if (atomic_load_explicit(&(replacement->count), memory_order_acquire)) return 0;
    uint64_t replace_cut = atomic_load_explicit(&(replacement->interference), memory_order_acquire);
    uintptr_t holder = object_word(binding->object + 0xe8);
    __auto_type write = &(writers[cache_slot(holder)]);
    if (atomic_load_explicit(&(write->count), memory_order_acquire)) return 0;
    uint64_t cut = atomic_load_explicit(&(write->interference), memory_order_acquire);
    uint64_t h = holder ? known(cached_holder, holder) : 0;
    if (!h && holder) {
        HOLD(prior, atomic_load_explicit(&(binding->holder_pin), memory_order_acquire));
        if (prior.token && pin_for(prior.token)->object == holder && hold(prior.token)) h = prior.token;
    }
    uintptr_t content = h ? object_word(holder + 0x28) : 0;
    uint64_t c = content ? known(cached_content, content) : 0;
    if (!c && content) {
        HOLD(prior, atomic_load_explicit(&(binding->latest_pin), memory_order_acquire));
        if (prior.token && pin_for(prior.token)->object == content && hold(prior.token)) c = prior.token;
    }
    bool ok = fill(deck, binding, h, c, false, false, out);
    if (ok && (atomic_load_explicit(&(write->count), memory_order_acquire) || cut != atomic_load_explicit(&(write->interference), memory_order_acquire) ||
               atomic_load_explicit(&(replacement->count), memory_order_acquire) || replace_cut != atomic_load_explicit(&(replacement->interference), memory_order_acquire))) {
        if (out->memory_pin) drop(out->memory_pin);
        ok = false;
    }
    if (c) drop(c);
    if (h) drop(h);
    return ok;
}
static void send(struct azstatus_native_receipt *r) {
    if ((!(azn_interest(r->deck) & AZ_STATUS_TOPIC(AZ_STATUS_CUES)) || !azn_offer(r)) && r->memory_pin) drop(r->memory_pin);
}
static void seed_active(unsigned deck) {
    struct azstatus_native_receipt r = {0};
    r.size = sizeof r;
    r.version = 1;
    if (seed(deck, &r)) send(&(r));
}
struct Writer {
    struct Writer *previous;
    uintptr_t holder, content;
    uint64_t holder_token, content_token;
    Writing *bucket;
    uint64_t cut;
    bool nested, contended, witnessed;
};
static _Thread_local struct Writer *writer_top;
static bool writer_uncertain(const struct Writer *w) {
    return w->contended || w->cut != atomic_load_explicit(&w->bucket->interference, memory_order_acquire);
}
static void writer_begin(struct Writer *w, uintptr_t holder, uintptr_t content) {
    *w = (struct Writer) {.previous = writer_top, .holder = holder, .content = content, .holder_token = remember(cached_holder, holder, profile.holder), .content_token = content ? remember(cached_content, content, profile.content) : 0, .bucket = &writers[cache_slot(holder)]};
    w->cut = atomic_load_explicit(&w->bucket->interference, memory_order_acquire);
    writer_top = w;
    for (struct Writer *p = w->previous; p; p = p->previous) {
        if (p->holder == holder && p->witnessed) {
            w->nested = true;
            w->contended = writer_uncertain(p);
            w->cut = p->cut;
            break;
        }
    }
    if (!w->nested && atomic_fetch_add_explicit(&w->bucket->count, 1, memory_order_acq_rel)) {
        w->contended = true;
        atomic_fetch_add_explicit(&w->bucket->interference, 1, memory_order_acq_rel);
    }
}
static void writer_drop(struct Writer *w) {
    writer_top = w->previous;
    if (!w->nested) {
        bool last = atomic_fetch_sub_explicit(&w->bucket->count, 1, memory_order_acq_rel) == 1;
        if (writer_uncertain(w)) {
            for (unsigned d = 0; d < 4; ++d) {
                HOLD(bind, atomic_load_explicit(&active[d], memory_order_acquire));
                Pin *b = pin_for(bind.token);
                if (!b || !current(d, b)) continue;
                uintptr_t h = object_word(b->object + 0xe8);
                if (h == w->holder) memory_loss(d, b);
                if (last && cache_slot(h) == cache_slot(w->holder)) seed_active(d);
            }
        }
    }
    if (w->holder_token) drop(w->holder_token);
    if (w->content_token) drop(w->content_token);
}
#define WRITER(name, holder, content)                               \
    struct Writer name __attribute__((cleanup(writer_drop))) = {0}; \
    writer_begin(&name, holder, content)
#define ORIGINAL(type, index) ((type) __atomic_load_n(originals + (index), __ATOMIC_ACQUIRE))
static void on_broadcast(void *self, void *listener) {
    uintptr_t holder = ((uintptr_t) (self));
    Writer *w = writer_top;
    while (w && w->holder != holder) w = w->previous;
    if (w && !w->witnessed) {
        w->witnessed = true;
        for (unsigned d = 0; d < 4; ++d) {
            HOLD(bind, atomic_load_explicit(&(active[d]), memory_order_acquire));
            Pin *b = pin_for(bind.token);
            if (!b || !current(d, b) || object_word(b->object + 0xe8) != holder) continue;
            __auto_type replacement = &(holder_writers[cache_slot(b->object)]);
            if (atomic_load_explicit(&(replacement->count), memory_order_acquire)) {
                atomic_fetch_add_explicit(&(replacement->interference), 1, memory_order_acq_rel);
                memory_loss(d, b);
                continue;
            }
            if (writer_uncertain(w)) continue; // last writer emits a scoped cut and an owned seed
            if (!w->holder_token || (w->content && !w->content_token) || object_word(holder + 0x28) != w->content) {
                memory_loss(d, b);
                continue;
            }
            struct azstatus_native_receipt r = {0};
            if (fill(d, b, w->holder_token, w->content_token, !w->content, true, &(r))) send(&(r));
            else memory_loss(d, b);
        }
    }
    ORIGINAL(void (*)(void *, void *), 2)(self, listener);
}
static bool set_content(void *self, const uintptr_t *input, void *listener) {
    __auto_type fn = ORIGINAL(bool (*)(void *, const uintptr_t *, void *), 1);
    if (!observing() || !input) return fn(self, input, listener);
    WRITER(w, (uintptr_t) self, *input);
    bool changed = fn(self, input, listener);
    if (changed && !w.witnessed) {
        for (unsigned d = 0; d < 4; ++d) {
            HOLD(bind, atomic_load_explicit(&(active[d]), memory_order_acquire));
            Pin *b = pin_for(bind.token);
            if (b && current(d, b) && object_word(b->object + 0xe8) == w.holder) memory_loss(d, b);
        }
    }
    return changed;
}
struct HolderScope {
    Writing *gate;
    uintptr_t pack, target;
    uint64_t pin, cut;
    bool overlap;
};
static void holder_scope_drop(struct HolderScope *scope) {
    bool last = atomic_fetch_sub_explicit(&(scope->gate->count), 1, memory_order_acq_rel) == 1;
    bool lost = scope->overlap || scope->cut != atomic_load_explicit(&(scope->gate->interference), memory_order_acquire);
    for (unsigned d = 0; d < 4; ++d) {
        HOLD(bind, atomic_load_explicit(&(active[d]), memory_order_acquire));
        Pin *b = pin_for(bind.token);
        if (!b || !current(d, b)) continue;
        if (b->object == scope->pack && (lost || object_word(scope->pack + 0xe8) != scope->target)) memory_loss(d, b);
        if (last && (b->object == scope->pack || (lost && cache_slot(b->object) == cache_slot(scope->pack)))) seed_active(d);
    }
    if (scope->pin) drop(scope->pin);
}
static void set_holder(void *builder, const uintptr_t *where, const uintptr_t *input) {
    __auto_type fn = ORIGINAL(void (*)(void *, const uintptr_t *, const uintptr_t *), 0);
    if (!observing() || !where || !input || !typed(*where, profile.pack)) {
        fn(builder, where, input);
        return;
    }
    const uintptr_t pack = *where, target = *input;
    uint64_t h = target ? remember(cached_holder, target, profile.holder) : 0;
    // advance() can invoke another setter before returning. A post-call seed
    // alone would silently collapse A/B/A. Mark overlap, then reseed only when
    // every owner in this bucket has left its replacement call.
    __auto_type gate = &(holder_writers[cache_slot(pack)]);
    uint64_t cut = atomic_load_explicit(&(gate->interference), memory_order_acquire);
    bool overlap = atomic_fetch_add_explicit(&(gate->count), 1, memory_order_acq_rel) != 0;
    if (overlap) atomic_fetch_add_explicit(&(gate->interference), 1, memory_order_acq_rel);
    struct HolderScope scope __attribute__((cleanup(holder_scope_drop))) = {gate, pack, target, h, cut, overlap};
    fn(builder, where, input);
}
static uintptr_t create_holder(void *manager, const void *id, const uintptr_t *input) {
    /* This factory constructs a holder with the caller's existing Content ref,
     * before it is attached to any deck's pack. */
    uint64_t c = observing() && input && *input ? remember(cached_content, *input, profile.content) : 0;
    __auto_type result = ORIGINAL(uintptr_t (*)(void *, const void *, const uintptr_t *), 3)(manager, id, input);
    if (c) drop(c);
    return result;
}
/* Native holder results use x8 on AArch64. The assembly entry forwards that
 * destination to C without changing ownership of the returned native object. */
struct HolderStage {
    struct Writer writers[4];
    unsigned count;
    uint64_t token;
};
static void holder_stage_drop(struct HolderStage *stage) {
    while (stage->count) writer_drop(&stage->writers[--stage->count]);
    if (stage->token) drop(stage->token);
}
extern void azn_cache_update_entry(void);
extern void azn_invoke_cache_update(void *function, uintptr_t *result, const unsigned char *track, const uintptr_t *input, void *repository);
__attribute__((visibility("hidden"))) uintptr_t *azn_cache_update(uintptr_t *result, const unsigned char *track, const uintptr_t *input, void *repository) {
    void *fn = __atomic_load_n(originals + 4, __ATOMIC_ACQUIRE);
    if (!observing() || !track || !input) {
        azn_invoke_cache_update(fn, result, track, input, repository);
        return result;
    }
    const uintptr_t target = *input;
    uint64_t c = target ? remember(cached_content, target, profile.content) : 0;
    struct HolderStage stage __attribute__((cleanup(holder_stage_drop))) = {.token = c};
    for (unsigned d = 0; d < 4; ++d) {
        HOLD(bind, atomic_load_explicit(&(active[d]), memory_order_acquire));
        Pin *p = pin_for(bind.token);
        if (!p || !current(d, p) || memcmp(((const void *) (p->object + 0x38)), track, 12)) continue;
        uintptr_t holder = object_word(p->object + 0xe8);
        uint64_t hp = known(cached_holder, holder);
        if (!hp) continue;
        bool present = false;
        for (unsigned i = 0; i < stage.count; ++i)
            if (stage.writers[i].holder == holder) present = true;
        if (!present) {
            writer_begin(&stage.writers[stage.count], holder, target);
            ++stage.count;
        }
        drop(hp);
    }
    azn_invoke_cache_update(fn, result, track, input, repository);
    if (typed((*result), profile.holder)) {
        uint64_t h = remember(cached_holder, (*result), profile.holder);
        bool captured = false;
        for (unsigned i = 0; i < stage.count; ++i)
            if (stage.writers[i].holder == (*result) && stage.writers[i].witnessed) captured = true;
        if (!captured && h && (!target || c) && object_word((*result) + 0x28) == target) {
            for (unsigned d = 0; d < 4; ++d) {
                HOLD(bind, atomic_load_explicit(&(active[d]), memory_order_acquire));
                Pin *p = pin_for(bind.token);
                if (!p || !current(d, p) || object_word(p->object + 0xe8) != (*result)) continue;
                struct azstatus_native_receipt r = {0};
                if (fill(d, p, h, c, !target, false, &(r))) {
                    if (atomic_load_explicit(&(p->serial), memory_order_acquire) > 1) r.lost_topics = AZ_STATUS_TOPIC(AZ_STATUS_CUES);
                    send(&(r));
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
static bool coordinate(uint64_t samples, uint64_t us, uint32_t rate, double *seconds) {
    if (!rate || rate >= 0x7fffffffu) return false;
    const int sample_range = compare_timestamps(samples, UINT64_C(9007199254740992), rate, rate, 1);
    const int us_range = compare_timestamps(us, UINT64_C(9007199254740992), 1000000, rate, 1);
    if (sample_range == AZ_TIME_INVALID || us_range == AZ_TIME_INVALID ||
        sample_range > AZ_TIME_EQUAL || us_range > AZ_TIME_EQUAL) return false;
    typedef unsigned __int128 W;
    const int consistent = compare_timestamps((W) (samples) * 1000000u, (W) (us) *rate, (uint64_t) (rate) * 1000000u, rate, 1);
    if (consistent == AZ_TIME_INVALID || consistent != AZ_TIME_EQUAL) return false;
    (*seconds) = (double) (samples) / rate;
    return true;
}
static int read_rows(uint64_t token, struct azn_memory_rows *out) {
    if (!out || out->size < offsetof(struct azn_memory_rows, clock_version) || out->version != 1) return 0;
    const bool clocks = out->size >= offsetof(struct azn_memory_rows, stored_ms);
    const bool storage_keys = out->size >= sizeof *out;
    if (clocks) out->clock_version = 0;
    HOLD(content, token);
    Pin *p = pin_for(content.token);
    if (!p || !typed(p->object, profile.content)) return 0;
    uintptr_t start = object_word(p->object + 0x28), end = object_word(p->object + 0x30);
    out->count = 0;
    out->availability = AZ_AVAIL_STALE;
    if (end < start || (end - start) % 0x80 || (end - start) / 0x80 > AZ_NATIVE_CUE_LIMIT || (start != end && !ptr(start))) return 1;
    unsigned count = (unsigned) ((end - start) / 0x80);
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t row = start + i * 0x80;
        __auto_type entry = &(out->rows[i]);
        (*entry) = (struct az_cue_entry) {0};
        if (SCALAR(unsigned char, row) != 0) return 1;
        __auto_type t = azcue_read_timestamps(((const void *) (row)));
        if (!coordinate(t.in_sample, t.in_us, t.sample_rate, &(entry->start_s))) return 1;
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
                !coordinate(t.out_sample, t.out_us, t.sample_rate, &(entry->end_s))) return 1;
        }
        if (clocks) {
            out->points[i][0] = (__typeof__(out->points[i][0])) {AZN_COORD, t.sample_rate, ((int64_t) (t.in_sample))};
            out->points[i][1] = (__typeof__(out->points[i][1])) {loop ? AZN_COORD : AZN_ABSENT, t.sample_rate,
                                                                 loop ? ((int64_t) (t.out_sample)) : 0};
        }
        if (storage_keys) {
            out->stored_ms[i][0] = t.in_us / 1000;
            out->stored_ms[i][1] = loop ? t.out_us / 1000 : 0;
        }
        entry->size = sizeof(*entry);
        entry->version = AZ_DATA_VERSION;
        entry->entry_id = i + 1;
        entry->classification = AZ_CUE_MEMORY;
        entry->type = loop ? AZ_CUE_LOOP : AZ_CUE_POINT;
        entry->valid = AZ_CUE_START | AZ_CUE_COLOR | (loop ? AZ_CUE_END : 0);
        entry->saved = entry->modified = -1;
        entry->rgb = SCALAR(uint32_t, row + 0x78) & 0xffffff;
        unsigned automatic = SCALAR(unsigned char, row + 0x28);
        entry->auto_active = -1;
        if (automatic <= 1) {
            entry->auto_active = automatic;
            entry->valid |= AZ_CUE_AUTO_ACTIVE;
        }
        unsigned palette = SCALAR(unsigned char, row + 0x7c);
        if (palette <= 8) {
            entry->valid |= AZ_CUE_COLOR_ID;
            entry->color_id = palette;
        }
        uintptr_t comment = object_word(row + 0x70);
        if (comment) {
            azcue_copy_utf8_prefix(entry->comment, ((const char *) (comment)));
            entry->valid |= AZ_CUE_COMMENT;
        }
    }
    out->count = count;
    out->availability = count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
    if (clocks) out->clock_version = storage_keys ? 2 : 1;
    return 1;
}
static uint64_t now_epoch(unsigned deck) {
    return azn_lifecycle_fence(deck);
}
#include "sdk_status_memory_recall.inc"
#include "sdk_status_file_cues.inc"
#if defined(__linux__)
static int main_image(struct dl_phdr_info *info, size_t bytes __attribute__((unused)), void *result) {
    if (info->dlpi_name && *info->dlpi_name) return 0;
    *((uintptr_t *) (result)) = info->dlpi_addr;
    return 1;
}
#endif


struct azn_hot_link azn_memory_copied(unsigned deck, uint64_t fence, uint64_t load, uintptr_t context,
                                      uintptr_t destination, uintptr_t source, uint32_t previous_revision, uint64_t *file_row) {
    if (file_row) *file_row = 0;
    __auto_type frame = recall_frame;
    if (!frame || deck >= 4 || !context || !frame->owned || !frame->candidates ||
        !typed(frame->facade, recall_bias + recall_profile->facade_table) || object_word(frame->facade + 0x60) != context) return (struct azn_hot_link) {0};
    __auto_type edition = &(frame->candidates->deck[deck]);
    if (!edition->entry || edition->epoch != fence || edition->load != load ||
        SCALAR(uint32_t, destination + 8) != previous_revision + 1u) return (struct azn_hot_link) {0};
    const uintptr_t retained = object_word(context + 0x18);
    if (destination == retained) {
        frame->retained = source == frame->owned ? retained : 0;
        frame->retained_revision = previous_revision + 1u;
        return (struct azn_hot_link) {0};
    }
    /* The stock CueLoadFacade first copies the task property to kind zero.
     * Its loader increments the retained revision once more on committing the
     * kind. Only that same retained property can establish the working copy. */
    if (destination != context + 0x160 || !frame->retained || frame->retained != source || source != retained ||
        SCALAR(uint32_t, source + 8) != frame->retained_revision + 1u) return (struct azn_hot_link) {0};
    HOLD(a, edition->pack_token);
    HOLD(c, edition->content_token);
    if (!a.token || !c.token) return (struct azn_hot_link) {0};
    __auto_type now = selected_edition(deck, pin_for(a.token)->object, pin_for(c.token)->object, edition->entry);
    if (now.unit != edition->unit || now.epoch != fence || now.load != load || now.provider != edition->provider ||
        now.serial != edition->serial || now.pack_token != a.token || now.content_token != c.token) return (struct azn_hot_link) {0};
    if (file_row) *file_row = imported_memory_row(pin_for(c.token)->object, edition->entry);
    return (struct azn_hot_link) {edition->provider, edition->serial, edition->entry, AZN_MEMORY_LINK_READY};
}

uint64_t azn_owned_pin(uintptr_t owned) { return ptr(owned) ? make_pin(owned, 0, 0, 0) : 0; }
int azn_pin_hold(uint64_t token, uintptr_t *object) {
    if (!object || !hold(token)) return 0;
    *object = pin_for(token)->object;
    return 1;
}
void azn_pin_drop(uint64_t token) {
    if (token) drop(token);
}
int azn_pack_current(unsigned deck, const struct azn_pack_view *view) {
    if (deck >= 4 || !view || !view->token || atomic_load_explicit(&(active[deck]), memory_order_acquire) != view->token) return 0;
    Pin *pin = pin_for(view->token);
    return pin && pin->object == view->object && pin->epoch == view->fence && pin->load == view->load && pin->unit == view->unit &&
           current(deck, pin) && atomic_load_explicit(&(active[deck]), memory_order_acquire) == view->token;
}
int azn_pack_acquire(unsigned deck, struct azn_pack_view *out) {
    if (deck >= 4 || !out) return 0;
    uint64_t token = atomic_load_explicit(&(active[deck]), memory_order_acquire);
    if (!hold(token)) return 0;
    Pin *p = pin_for(token);
    const struct azn_pack_view view = {token, p->unit, p->epoch, p->load, p->object};
    if (!azn_pack_current(deck, &view)) {
        drop(token);
        return 0;
    }
    *out = view;
    return 1;
}
int azn_pack_retains(uintptr_t pack) {
    if (!pack) return 0;
    for (unsigned slot_index = 0; slot_index < sizeof(active) / sizeof(active)[0]; ++slot_index) {
        const _Atomic(uint64_t) *slot = &active[slot_index];
        __auto_type token = atomic_load_explicit(slot, memory_order_acquire);
        if (!token) continue;
        HOLD(held, token);
        if (!held.token || pin_for(held.token)->object == pack || atomic_load_explicit(slot, memory_order_acquire) != token)
            return 1;
    }
    return 0;
}
int azn_pack_observe(azn_pack_observer observer) {
    if (!observer) return 0;
    azn_pack_observer expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&(pack_observer), &(expected), observer, memory_order_acq_rel, memory_order_acquire))
        return expected == observer;
    for (unsigned d = 0; d < 4; ++d) {
        struct azn_pack_view view = {0};
        if (azn_pack_acquire(d, &view)) {
            observer(d, view.unit, view.fence, view.load);
            drop(view.token);
        }
    }
    return 1;
}
int azn_memory_configure(int build, azexit_install_fn install) {
#if defined(__linux__) && defined(__aarch64__)
    if (build < 1 || build > 2 || !install || atomic_load_explicit(&(enabled), memory_order_acquire)) return 0;
    uintptr_t bias = 0;
    dl_iterate_phdr(main_image, &bias);
    profile = mem_profiles[build - 1];
    profile.pack += bias;
    profile.holder += bias;
    profile.content += bias;
    atomic_store_explicit(&(pack_profile_ready), true, memory_order_release);
    void *hooks[] = {((void *) (set_holder)), ((void *) (set_content)),
                     ((void *) (on_broadcast)), ((void *) (create_holder)), (void *) azn_cache_update_entry};
    for (unsigned i = 0; i < 5; ++i) {
        profile.entry[i].address += bias;
        if (memcmp(((const void *) (profile.entry[i].address)), profile.entry[i].prologue, 16)) return 0;
    }
    for (unsigned i = 0; i < 5; ++i)
        if (!install(profile.entry[i].address, hooks[i], &originals[i]) || !originals[i]) return 0;
    atomic_store_explicit(&(enabled), true, memory_order_release);
    install_recall(build, bias, install);
    if (atomic_load_explicit(&(connected), memory_order_acquire))
        for (unsigned d = 0; d < 4; ++d) azn_capability(d, AZN_CAP_MEMORY | (recall_ready ? AZN_CAP_MEMORY_LINK : 0), 1);
    return 1;
#else
    (void) build;
    (void) install;
    return 0;
#endif
}
void azn_memory_bind(unsigned deck, uint64_t unit, uint64_t epoch, uint64_t load,
                     uintptr_t pack, const unsigned char track[12]) {
    if (deck >= 4 || !epoch || !unit || !load || !atomic_load_explicit(&(pack_profile_ready), memory_order_acquire) ||
        (!observing() && !atomic_load_explicit(&(pack_observer), memory_order_acquire)) ||
        now_epoch(deck) != epoch || !typed(pack, profile.pack) || !SCALAR(uint32_t, pack + 0x30) ||
        SCALAR(unsigned char, pack + 0x78) != 1 || memcmp(((const void *) (pack + 0x38)), track, 12) ||
        memcmp(((const void *) (pack + 0x80)), track, 12)) return;
    uint64_t token = make_pin(pack, epoch, load, unit);
    if (!token) return;
    uint64_t previous = atomic_load_explicit(&(active[deck]), memory_order_acquire);
    bool installed = false;
    for (unsigned n = 0; n < 8 && now_epoch(deck) == epoch; ++n) {
        HOLD(old, previous);
        if (old.token && pin_for(old.token)->epoch >= epoch) break;
        if (atomic_compare_exchange_strong_explicit(&(active[deck]), &(previous), token, memory_order_acq_rel, memory_order_acquire)) {
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
        if (atomic_compare_exchange_strong_explicit(&(active[deck]), &(expected), 0, memory_order_acq_rel, memory_order_acquire)) drop(token);
        return;
    }
    __auto_type observer = atomic_load_explicit(&(pack_observer), memory_order_acquire);
    if (observer) observer(deck, unit, epoch, load);
    if (observing()) seed_active(deck);
}
void azn_memory_retire(unsigned deck, uint64_t fence) {
    if (deck >= 4) return;
    HOLD(current, atomic_load_explicit(&(active[deck]), memory_order_acquire));
    if (!current.token || (fence && pin_for(current.token)->epoch >= fence)) return;
    uint64_t token = current.token;
    if (atomic_compare_exchange_strong_explicit(&(active[deck]), &(token), 0, memory_order_acq_rel, memory_order_acquire)) {
        drop(current.token);
        __auto_type observer = atomic_load_explicit(&(pack_observer), memory_order_acquire);
        if (observer) observer(deck, pin_for(current.token)->unit, fence, 0);
    }
}
void azn_memory_maintenance(void) {
    if (!atomic_exchange_explicit(&(reap_needed), false, memory_order_acq_rel)) return;
    for (unsigned p_index = 0; p_index < sizeof(pool) / sizeof(pool)[0]; ++p_index) {
        Pin *p = &(pool[p_index]);
        uint64_t s = atomic_load_explicit(&(p->state), memory_order_acquire);
        if (!s || s == busy || (s & refs_mask) || !atomic_compare_exchange_strong_explicit(&(p->state), &(s), busy, memory_order_acq_rel, memory_order_acquire)) continue;
        uint64_t h = atomic_exchange_explicit(&(p->holder_pin), 0, memory_order_relaxed), c = atomic_exchange_explicit(&(p->latest_pin), 0, memory_order_relaxed);
        if (h) drop(h);
        if (c) drop(c);
        azn_ref_release(p->object);
        atomic_store_explicit(&(p->state), 0, memory_order_release);
    }
}
void azn_file_row_hold(uint64_t token, int acquire) {
    __auto_type cb = atomic_load_explicit(&(file_callbacks), memory_order_acquire);
    if (token && cb && cb->size >= offsetof(struct azn_file_cue_callbacks, hold) + sizeof cb->hold && cb->hold)
        cb->hold(&token, 1, acquire);
}
const struct azn_file_cue_api *azn_file_service(void) {
    static const struct azn_file_cue_api api = {sizeof api, 1, file_install, file_transport, observe_file_input,
                                                file_maps_install, file_maps_origins, file_maps_hold};
    return &api;
}
static void memory_connect(void) {
    atomic_store_explicit(&(connected), true, memory_order_release);
    if (atomic_load_explicit(&(enabled), memory_order_acquire))
        for (unsigned d = 0; d < 4; ++d) azn_capability(d, AZN_CAP_MEMORY | (recall_ready ? AZN_CAP_MEMORY_LINK : 0), 1);
}
const struct azn_memory_api *azn_memory_service(void) {
    static const struct azn_memory_api api = {sizeof api, 1, read_rows, drop, seed, memory_connect};
    return &api;
}
