/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcuepersist.h"
#include "../azmod/azmod-mod.h"
#include <atomic>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <time.h>
#if defined(AZSDK_CUE_DIAGNOSTIC)
#include <cstdio>
#endif

/* Standalone persistence fixtures do not link the command actor. */
extern "C" void azsdk_cue_commands_persist_retired(unsigned, uint64_t) noexcept __attribute__((weak));

namespace {
enum {
    FREE,
    WRITING,
    ACTIVE,
    RECEIPT,
    POLLING,
    HELPING
};
struct Slot {
    std::atomic<unsigned> phase {FREE}, invalid {0}, mutated {0}, retired {0};
    std::atomic<uintptr_t> handler {0}, receiver {0};
    azexit_cue_request request = {};
    std::atomic<void *> manager {nullptr};
    void *maintainer = nullptr;
    uint64_t cookie = 0;
    unsigned kind = 0;
    unsigned char property_identity[12] = {};
    uint64_t property_in = 0;
    std::atomic<int> native_result {-1};
    std::atomic<unsigned> receipt_seen {0};
    std::atomic<unsigned> diagnostic {0}, no_effect {0}, request_entered {0};
    std::atomic<uint64_t> submit_time {0}, request_time {0}, handler_time {0};
    uintptr_t property_source = 0;
    uint32_t property_rate = 0;
    uintptr_t request_source = 0;
    uint32_t request_rate = 0;
    azcuepersist_witness witness = {};
    uintptr_t own_content = 0;
    uintptr_t reply_memory_set = 0, reply_hot_set = 0, reply_memory_content = 0, reply_hot_content = 0;
    std::atomic<unsigned> reply_captured {0}, metadata_applied {0};
    uint32_t native_serial = 0;
    unsigned char native_track[12] = {};
    azcue_native_timestamps native_times;
    std::atomic<unsigned> helper_claim {0};
};
struct Receipt {
    std::atomic<unsigned> phase {FREE};
    uint64_t sequence = 0, token = 0;
    uintptr_t maintainer = 0;
    uint32_t tag = 0;
    int result = -1;
    unsigned deck = 4;
    unsigned char track[16] = {};
};
struct State {
    azcuepersist_profile profile = {};
    azcuepersist_callbacks callbacks = {};
    Slot slots[4];
    Receipt receipts[64];
    std::atomic<uint64_t> sequence {0};
    std::atomic<unsigned> enabled {0}, overflow {0}, attempted {0};
    void *request_original = nullptr, *handler_original = nullptr;
    void *reply_original = nullptr, *registered_original = nullptr, *memory_original = nullptr, *hot_original = nullptr;
};
State &state() {
    static State value;
    return value;
}
thread_local Slot *submitting = nullptr;
struct Scope {
    Slot *previous;
    explicit Scope(Slot *slot) : previous(submitting) { submitting = slot; }
    ~Scope() { submitting = previous; }
};
uintptr_t word(uintptr_t address) {
    uintptr_t out;
    std::memcpy(&out, reinterpret_cast<const void *>(address), sizeof out);
    return out;
}
uint32_t u32(uintptr_t address) {
    uint32_t out;
    std::memcpy(&out, reinterpret_cast<const void *>(address), sizeof out);
    return out;
}
bool pointer(uintptr_t address) { return address >= 0x10000 && !(address & 7); }
uint64_t now() noexcept {
    timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = uint64_t(value.tv_sec);
    return seconds <= (UINT64_MAX - uint64_t(value.tv_nsec)) / UINT64_C(1000000000)
               ? seconds * UINT64_C(1000000000) + uint64_t(value.tv_nsec)
               : 0;
}
#if defined(AZSDK_CUE_DIAGNOSTIC)
void persist_diagnostic(unsigned deck, uint64_t token, const char *stage, int result) {
    std::fprintf(stderr, "AZCUE persist deck=%u token=%llu stage=%s clock=%llu result=%d\n", deck,
                 (unsigned long long) token, stage, (unsigned long long) now(), result);
}
#define PERSIST_DIAG(...) persist_diagnostic(__VA_ARGS__)
#else
#define PERSIST_DIAG(...) ((void) 0)
#endif
bool creating(unsigned operation) { return operation == AZ_CMD_CUE_POINT_SET || operation == AZ_CMD_CUE_LOOP_SET; }
bool supported(unsigned operation) {
    switch (operation) {
    case AZ_CMD_HOT_SET:
    case AZ_CMD_HOT_CLEAR:
    case AZ_CMD_MEMORY_SET:
    case AZ_CMD_MEMORY_DELETE:
    case AZ_CMD_SAVED_SAVE:
    case AZ_CMD_SAVED_DELETE:
    case AZ_CMD_SAVED_AUTO:
    case AZ_CMD_CUE_COMMENT:
    case AZ_CMD_CUE_COLOR:
    case AZ_CMD_CUE_POINT_SET:
    case AZ_CMD_CUE_LOOP_SET: return true;
    default: return false;
    }
}
bool deleting(unsigned operation) { return operation == AZ_CMD_HOT_CLEAR || operation == AZ_CMD_MEMORY_DELETE || operation == AZ_CMD_SAVED_DELETE; }
bool utf8(const char *text, size_t &length) {
    length = 0;
    while (length < 128 && text[length]) ++length;
    if (length == 128) return false;
    for (size_t i = 0; i < length;) {
        unsigned char c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) continue;
        unsigned count, minimum, value;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            minimum = 0x80;
            value = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            minimum = 0x800;
            value = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            minimum = 0x10000;
            value = c & 7;
        } else return false;
        if (count > length - i) return false;
        while (count--) {
            unsigned char tail = static_cast<unsigned char>(text[i++]);
            if ((tail & 0xc0) != 0x80) return false;
            value = (value << 6) | (tail & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
bool valid(Slot &slot) noexcept {
    uint64_t time = now();
    if (slot.invalid.load(std::memory_order_acquire) || !time || compare_timestamps(time, slot.request.identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL) return false;
    try {
        return state().callbacks.validate(&slot.request, slot.manager.load(std::memory_order_acquire), slot.cookie, state().callbacks.context) != 0;
    } catch (...) { return false; }
}
template <class F>
F original(void *const *p) {
    void *entry = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    if (!entry) std::terminate();
    return reinterpret_cast<F>(entry);
}
Slot *for_handler(uintptr_t handler) {
    for (auto &slot : state().slots) {
        unsigned phase = slot.phase.load(std::memory_order_acquire);
        if ((phase == ACTIVE || phase == RECEIPT) && slot.handler.load(std::memory_order_acquire) == handler) return &slot;
    }
    return nullptr;
}
/* Nontrivial return selects the native AArch64 hidden-X8 aggregate ABI. Native
 * allocation creates the actual counted string header; never synthesize it.
 * This local destructor is deliberately empty; the native destructor owns it. */
struct NativeString {
    void *data;
    ~NativeString() {}
};
void mutate_row(Slot &slot, uintptr_t row) {
    const auto &profile = state().profile;
    if (slot.request.operation == AZ_CMD_CUE_COMMENT || creating(slot.request.operation)) {
        size_t length = 0;
        if (!utf8(slot.request.comment, length)) throw std::exception();
        NativeString text = reinterpret_cast<NativeString (*)(uint64_t)>(profile.string_allocate)(length + 1);
        if (!text.data) throw std::exception();
        struct Release {
            NativeString &text;
            uintptr_t destroy;
            ~Release() { reinterpret_cast<void (*)(void *)>(destroy)(&text); }
        } release {text, profile.string_destroy};
        std::memcpy(text.data, slot.request.comment, length + 1);
        reinterpret_cast<void (*)(void *, const void *)>(profile.string_assign)(reinterpret_cast<void *>(row + 0x70), &text);
    }
    if (slot.request.operation == AZ_CMD_CUE_COLOR || slot.request.operation == AZ_CMD_HOT_SET || creating(slot.request.operation)) {
        unsigned rgb = slot.request.rgb;
        reinterpret_cast<void (*)(void *, unsigned, unsigned, unsigned)>(profile.color_construct)(
            reinterpret_cast<void *>(row + 0x78), (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    }
    if (slot.request.operation == AZ_CMD_SAVED_AUTO) {
        unsigned char automatic = slot.request.position_s == 1.0;
        std::memcpy(reinterpret_cast<void *>(row + 0x28), &automatic, 1);
    }
}
bool metadata(unsigned operation) { return operation == AZ_CMD_SAVED_AUTO || operation == AZ_CMD_CUE_COMMENT || operation == AZ_CMD_CUE_COLOR || operation == AZ_CMD_HOT_SET || creating(operation); }
bool vector_bounds(uintptr_t content, uintptr_t &begin, uintptr_t &end, uint64_t &count) {
    begin = end = count = 0;
    if (!content) return true;
    if (!pointer(content)) return false;
    begin = word(content + 0x28);
    end = word(content + 0x30);
    if (end < begin || (end - begin) % 0x80 || (end - begin) / 0x80 > 10 || (end != begin && !pointer(begin))) return false;
    count = (end - begin) / 0x80;
    return true;
}
uint64_t witness_hash(const azcuepersist_witness &w) {
    uint64_t count, hot_count;
    uintptr_t begin, end, hot_begin, hot_end;
    if (!vector_bounds(w.memory_content, begin, end, count) || !vector_bounds(w.hot_content, hot_begin, hot_end, hot_count) || hot_count > 8) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    auto mix = [&hash](const void *p, size_t n) {
        const auto *bytes = static_cast<const unsigned char *>(p);
        for (size_t i = 0; i < n; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    };
    mix(w.source_id, 12);
    mix(&count, sizeof count);
    mix(reinterpret_cast<const void *>(begin), end - begin);
    mix(reinterpret_cast<const void *>(hot_begin), hot_end - hot_begin);
    return hash;
}
bool witness_current(const Slot &s) {
    const auto &w = s.witness;
    return w.source_info && (creating(s.request.operation) || (w.memory_set && w.hot_set)) && w.revision && (s.request.operation == AZ_CMD_HOT_SET || w.revision == s.request.expected_revision) && w.fingerprint &&
           (w.memory_set ? word(w.memory_set + 0x28) == w.memory_content : !w.memory_content) &&
           (w.hot_set ? word(w.hot_set + 0x28) == w.hot_content : !w.hot_content) &&
           witness_hash(w) == w.fingerprint;
}
void release_witness(Slot &slot) {
    uintptr_t *reply_refs[] = {&slot.reply_memory_set, &slot.reply_hot_set, &slot.reply_memory_content, &slot.reply_hot_content};
    for (auto ref : reply_refs)
        if (*ref) {
            reinterpret_cast<void (*)(void *)>(state().profile.content_release)(ref);
            *ref = 0;
        }
    if (slot.own_content) {
        reinterpret_cast<void (*)(void *)>(state().profile.content_release)(&slot.own_content);
        slot.own_content = 0;
    }
    uintptr_t *refs[] = {&slot.witness.source_info, &slot.witness.memory_set, &slot.witness.hot_set, &slot.witness.memory_content, &slot.witness.hot_content};
    for (auto ref : refs)
        if (*ref) {
            reinterpret_cast<void (*)(void *)>(state().profile.content_release)(ref);
            *ref = 0;
        }
}
void retire_receipt(Slot &slot) {
    unsigned deck = slot.request.identity.deck;
    uint64_t token = slot.request.identity.token;
    release_witness(slot);
    slot.phase.store(FREE, std::memory_order_release);
    if (azsdk_cue_commands_persist_retired) azsdk_cue_commands_persist_retired(deck, token);
}
void complete_receipt(Slot &slot) {
    if (!slot.receipt_seen.load(std::memory_order_acquire)) return;
    unsigned active = ACTIVE;
    if (!slot.phase.compare_exchange_strong(active, RECEIPT, std::memory_order_acq_rel)) return;
    if (slot.retired.load(std::memory_order_acquire)) {
        unsigned receipt = RECEIPT;
        if (slot.phase.compare_exchange_strong(receipt, POLLING, std::memory_order_acq_rel)) {
            retire_receipt(slot);
        }
    } else state().callbacks.ready(slot.request.identity.deck, slot.request.identity.token, state().callbacks.context);
}
void leave_helper(Slot &slot) {
    slot.phase.store(ACTIVE, std::memory_order_release);
    complete_receipt(slot);
}
/* This helper executes on the native repository queue, after doRequest returns.
 * Match its copied native RequestID serial and TrackID, never thread-local state. */
int metadata_hook(bool hot, void *cache, const void *request, const void *track, const void *cue, const void *set_ref, unsigned priority) {
    using Helper = int (*)(void *, const void *, const void *, const void *, const void *, unsigned);
    Slot *slot = nullptr;
    if (state().enabled.load(std::memory_order_acquire) && request && track && cue && set_ref) {
        for (auto &candidate : state().slots) {
            unsigned active = ACTIVE;
            if (!candidate.phase.compare_exchange_strong(active, HELPING, std::memory_order_acq_rel)) continue;
            bool match = candidate.mutated.load(std::memory_order_acquire) == 2 && metadata(candidate.request.operation) &&
                         bool(candidate.kind) == hot && candidate.native_serial &&
                         candidate.native_serial == u32(reinterpret_cast<uintptr_t>(request) + 8) && !std::memcmp(candidate.native_track, track, 12);
            if (!match) {
                leave_helper(candidate);
                continue;
            }
            if (slot) {
                slot->invalid.store(1, std::memory_order_release);
                candidate.invalid.store(1, std::memory_order_release);
                leave_helper(candidate);
                leave_helper(*slot);
                return 1;
            }
            slot = &candidate;
        }
    }
    if (!slot) return original<Helper>(hot ? &state().hot_original : &state().memory_original)(cache, request, track, cue, set_ref, priority);
    struct Leave {
        Slot &slot;
        ~Leave() { leave_helper(slot); }
    } leave {*slot};
    unsigned expected = 0;
    if (!slot->helper_claim.compare_exchange_strong(expected, 1, std::memory_order_acq_rel) || !valid(*slot) || !witness_current(*slot) ||
        static_cast<const unsigned char *>(cue)[0] != slot->kind || !azcue_timestamps_equal(azcue_read_timestamps(cue), slot->native_times, slot->property_rate) ||
        std::memcmp(slot->native_track, slot->witness.source_id, 12) ||
        (!creating(slot->request.operation) && word(reinterpret_cast<uintptr_t>(set_ref)) != (hot ? slot->witness.hot_set : slot->witness.memory_set))) {
        slot->invalid.store(1, std::memory_order_release);
        return 1;
    }
    if (creating(slot->request.operation)) {
        uintptr_t actual = word(reinterpret_cast<uintptr_t>(set_ref));
        uintptr_t &root = hot ? slot->witness.hot_set : slot->witness.memory_set;
        uintptr_t &content = hot ? slot->witness.hot_content : slot->witness.memory_content;
        if (!root) {
            // The native repository may construct the first publication root
            // after an empty-source admission. Bind only its still-empty set,
            // under this exact correlated source/request helper lease.
            uintptr_t begin = 0, end = 0; uint64_t count = 0;
            uintptr_t actual_content = actual ? word(actual + 0x28) : 0;
            if (!actual || !vector_bounds(actual_content, begin, end, count) || count) {
                slot->invalid.store(1, std::memory_order_release); return 1;
            }
            auto copy = reinterpret_cast<void (*)(void *, const void *)>(state().profile.content_copy);
            copy(&root, &actual); if (actual_content) copy(&content, &actual_content);
            if (root != actual || content != actual_content || !witness_current(*slot)) {
                slot->invalid.store(1, std::memory_order_release); return 1;
            }
        } else if (actual != root) {
            slot->invalid.store(1, std::memory_order_release); return 1;
        }
    }
    uintptr_t target_content = hot ? slot->witness.hot_content : slot->witness.memory_content;
    uintptr_t target_begin, target_end;
    uint64_t target_count;
    if (!vector_bounds(target_content, target_begin, target_end, target_count)) {
        slot->invalid.store(1, std::memory_order_release);
        return 1;
    }
    if (creating(slot->request.operation)) {
        if (!hot && target_count >= 10) { slot->invalid.store(1, std::memory_order_release); return 1; }
        const uint64_t absent = UINT64_C(0x7fffffffffffffff);
        for (uintptr_t row = target_begin; row != target_end; row += 0x80) {
            const unsigned kind = *reinterpret_cast<const unsigned char *>(row);
            const auto times = azcue_read_timestamps(reinterpret_cast<const void *>(row));
            if (kind == 0xff || times.in_us == absent || times.in_sample == absent) continue;
            const int collision = compare_timestamps(times.in_us, slot->native_times.in_us, 1000000, slot->property_rate, 1);
            if ((hot && kind == slot->kind) || (!hot && (collision == AZ_TIME_INVALID || collision == AZ_TIME_EQUAL))) {
                slot->invalid.store(1, std::memory_order_release); return 1;
            }
        }
        int result = original<Helper>(hot ? &state().hot_original : &state().memory_original)(cache, request, track, cue, set_ref, priority);
        if (result == 0) slot->metadata_applied.store(1, std::memory_order_release);
        else slot->invalid.store(1, std::memory_order_release);
        return result;
    }
    unsigned target_matches = 0;
    for (uintptr_t row = target_begin; row != target_end; row += 0x80)
        if (*reinterpret_cast<const unsigned char *>(row) == slot->kind && azcue_timestamps_equal(azcue_read_timestamps(reinterpret_cast<const void *>(row)), slot->native_times, slot->property_rate)) ++target_matches;
    if (target_matches > 1 || (slot->request.operation != AZ_CMD_HOT_SET && target_matches != 1)) {
        slot->invalid.store(1, std::memory_order_release);
        return 1;
    }
    int result;
    try {
        result = original<Helper>(hot ? &state().hot_original : &state().memory_original)(cache, request, track, cue, set_ref, priority);
    } catch (...) {
        slot->invalid.store(1, std::memory_order_release);
        return 1;
    }
    if (slot->request.operation == AZ_CMD_HOT_SET && result == 0) {
        slot->metadata_applied.store(1, std::memory_order_release);
        return result;
    }
    if (result != 1 || !valid(*slot) || !witness_current(*slot)) {
        slot->invalid.store(1, std::memory_order_release);
        return result;
    }
    uintptr_t collection = word(reinterpret_cast<uintptr_t>(set_ref));
    uintptr_t retained = 0, built = 0, vector[3] = {};
    struct Cleanup {
        uintptr_t &retained, &built, *vector;
        ~Cleanup() {
            const auto &p = state().profile;
            reinterpret_cast<void (*)(void *)>(p.vector_destroy)(vector);
            if (built) reinterpret_cast<void (*)(void *)>(p.content_release)(&built);
            if (retained) reinterpret_cast<void (*)(void *)>(p.content_release)(&retained);
        }
    } cleanup {retained, built, vector};
    try {
        if (!pointer(collection)) throw std::exception();
        reinterpret_cast<void (*)(void *, const void *)>(state().profile.content_copy)(&retained, reinterpret_cast<const void *>(collection + 0x28));
        if (retained != (hot ? slot->witness.hot_content : slot->witness.memory_content) || !witness_current(*slot)) throw std::exception();
        if (slot->request.operation == AZ_CMD_HOT_SET) {
            uintptr_t original_row = 0;
            for (uintptr_t row = target_begin; row != target_end; row += 0x80)
                if (*reinterpret_cast<const unsigned char *>(row) == slot->kind && azcue_timestamps_equal(azcue_read_timestamps(reinterpret_cast<const void *>(row)), slot->native_times, slot->property_rate)) original_row = row;
            if (!original_row) {
                slot->invalid.store(1, std::memory_order_release);
                return result;
            }
            /* registerCue owns this queued/inline copy and constructs the actual
             * write ticket from it only after the helper returns. */
            void *owned_cue = const_cast<void *>(cue);
            reinterpret_cast<void (*)(void *)>(state().profile.cue_destroy)(owned_cue);
            reinterpret_cast<void (*)(void *, const void *)>(state().profile.cue_copy)(owned_cue, reinterpret_cast<const void *>(original_row));
            mutate_row(*slot, reinterpret_cast<uintptr_t>(owned_cue));
        }

        uintptr_t begin = word(retained + 0x28), end = word(retained + 0x30);
        unsigned matches = 0;
        for (uintptr_t at = begin; at != end; at += 0x80) {
            bool target = *reinterpret_cast<const unsigned char *>(at) == slot->kind && azcue_timestamps_equal(azcue_read_timestamps(reinterpret_cast<const void *>(at)), slot->native_times, slot->property_rate);
            reinterpret_cast<void (*)(void *, const void *)>(state().profile.vector_append)(vector, reinterpret_cast<const void *>(at));
            if (target) {
                ++matches;
                mutate_row(*slot, vector[1] - 0x80);
            }
        }
        if (matches != 1 || vector[1] - vector[0] != end - begin || !valid(*slot) ||
            word(collection + 0x28) != retained || !witness_current(*slot)) throw std::exception();
        reinterpret_cast<void (*)(void *, const void *)>((hot ? state().profile.hot_content_from_vector : state().profile.content_from_vector))(&built, vector);
        if (!built || word(built + 0x30) - word(built + 0x28) != end - begin || !valid(*slot) ||
            word(collection + 0x28) != retained || !witness_current(*slot)) throw std::exception();
        reinterpret_cast<void (*)(void *, const void *)>(state().profile.content_copy)(&slot->own_content, &built);
        if (slot->own_content != built) throw std::exception();
        if (!reinterpret_cast<bool (*)(void *, const void *, void *)>((hot ? state().profile.hot_content_publish : state().profile.content_publish))(reinterpret_cast<void *>(collection), &built, nullptr) ||
            word(collection + 0x28) != built) throw std::exception();
        slot->metadata_applied.store(1, std::memory_order_release);
        slot->diagnostic.store(6, std::memory_order_release);
        return 0; /* official caller persists the selected cue through its native ticket */
    } catch (...) {
        slot->invalid.store(1, std::memory_order_release);
        return result;
    }
}
int memory_hook(void *a, const void *b, const void *c, const void *d, const void *e, unsigned f) { return metadata_hook(false, a, b, c, d, e, f); }
int hot_hook(void *a, const void *b, const void *c, const void *d, const void *e, unsigned f) { return metadata_hook(true, a, b, c, d, e, f); }
void request_hook(void *maintainer, const void *record) {
    Slot *slot = submitting;
    if (slot && slot->maintainer == maintainer) {
        uintptr_t input = reinterpret_cast<uintptr_t>(record);
        slot->request_time.store(now(), std::memory_order_release);
        if (pointer(input)) {
            slot->request_source = word(input + 8 + 0x50);
            slot->request_rate = u32(input + 8 + 0x60);
        }
        unsigned reason = !pointer(input) ? 11 : static_cast<const unsigned char *>(record)[0] != unsigned(!deleting(slot->request.operation))                             ? 12
                                             : u32(input + 4) != slot->kind                                                                                                ? 13
                                             : std::memcmp(reinterpret_cast<const void *>(input + 8 + 0x30), slot->property_identity, 12)                                  ? 14
                                             : compare_timestamps(word(input + 8 + 0x40), slot->property_in, slot->property_rate, slot->property_rate, 1) != AZ_TIME_EQUAL ? 15
                                             : !valid(*slot)                                                                                                               ? 16
                                                                                                                                                                           : 0;
        if (reason) {
            slot->diagnostic.store(reason, std::memory_order_release);
            slot->invalid.store(1, std::memory_order_release);
            slot->no_effect.store(1, std::memory_order_release);
            return; /* exact intercepted request was suppressed before repository work */
        }
        slot->diagnostic.store(2, std::memory_order_release);
        slot->request_entered.store(1, std::memory_order_release);
    } else slot = nullptr;
    Scope scope(slot);
    original<void (*)(void *, const void *)> (&state().request_original)(maintainer, record);
    if (slot) {
        unsigned entered = 2;
        slot->diagnostic.compare_exchange_strong(entered, 5, std::memory_order_acq_rel);
    }
}
bool handler_hook(void *self) {
    if (!state().enabled.load(std::memory_order_acquire)) return original<bool (*)(void *)>(&state().handler_original)(self);
    uintptr_t handler = reinterpret_cast<uintptr_t>(self);
    Slot *slot = for_handler(handler);
    if (!slot && submitting) {
        Slot &candidate = *submitting;
        uintptr_t maintainer = reinterpret_cast<uintptr_t>(candidate.maintainer);
        if (word(handler) == state().profile.handler_table && word(maintainer + 0x160) == handler) {
            uintptr_t receiver = word(handler + 0xc0);
            if (pointer(receiver) && word(receiver) == state().profile.reception_table) {
                candidate.receiver.store(receiver, std::memory_order_release);
                candidate.handler.store(handler, std::memory_order_release);
                slot = &candidate;
            }
        }
    }
    if (slot) {
        slot->handler_time.store(now(), std::memory_order_release);
        slot->diagnostic.store(3, std::memory_order_release);
        if (!valid(*slot) || state().overflow.load(std::memory_order_acquire)) {
            slot->invalid.store(1, std::memory_order_release);
            return false; /* suppress late retries as well */
        }
        unsigned expected = 0;
        if (slot->mutated.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            try {
                if (slot->request.operation == AZ_CMD_SAVED_AUTO || slot->request.operation == AZ_CMD_CUE_COMMENT || slot->request.operation == AZ_CMD_CUE_COLOR) {
                    if (!slot->witness.selected_cue || !witness_current(*slot)) throw std::exception();
                    reinterpret_cast<void (*)(void *)>(state().profile.cue_destroy)(reinterpret_cast<void *>(handler + 0x38));
                    reinterpret_cast<void (*)(void *, const void *)>(state().profile.cue_copy)(reinterpret_cast<void *>(handler + 0x38), reinterpret_cast<const void *>(slot->witness.selected_cue));
                }
                slot->native_serial = u32(handler + 0x30);
                std::memcpy(slot->native_track, reinterpret_cast<const void *>(handler + 0x10), 12);
                slot->native_times = azcue_read_timestamps(reinterpret_cast<const void *>(handler + 0x38));
                mutate_row(*slot, handler + 0x38);
                slot->mutated.store(2, std::memory_order_release);
                slot->diagnostic.store(4, std::memory_order_release);
            } catch (...) {
                slot->invalid.store(1, std::memory_order_release);
                return false;
            }
        } else if (expected != 2) return false;
    }
    return original<bool (*)(void *)>(&state().handler_original)(self);
}
void reply_hook(void *self, const void *request_id, const void *track, const void *memory,
                const void *hot, const int *result) {
    if (!state().enabled.load(std::memory_order_acquire)) {
        original<void (*)(void *, const void *, const void *, const void *, const void *, const int *)> (&state().reply_original)(self, request_id, track, memory, hot, result);
        return;
    }
    uintptr_t receiver = reinterpret_cast<uintptr_t>(self);
    Slot *owner = nullptr;
    for (auto &slot : state().slots) {
        if ((slot.phase.load(std::memory_order_acquire) == ACTIVE || slot.phase.load(std::memory_order_acquire) == HELPING) && slot.receiver.load(std::memory_order_acquire) == receiver) {
            owner = &slot;
            break;
        }
    }
    if (owner && metadata(owner->request.operation) && result && (*result == 0 || *result == 5)) {
        unsigned absent = 0;
        if (!request_id || !track || !owner->metadata_applied.load(std::memory_order_acquire) ||
            u32(reinterpret_cast<uintptr_t>(request_id) + 8) != owner->native_serial || std::memcmp(track, owner->native_track, 12) ||
            !owner->reply_captured.compare_exchange_strong(absent, 1, std::memory_order_acq_rel)) {
            owner->invalid.store(1, std::memory_order_release);
        } else {
            try {
                auto copy = reinterpret_cast<void (*)(void *, const void *)>(state().profile.content_copy);
                bool memory_present = memory && word(reinterpret_cast<uintptr_t>(memory));
                bool hot_present = hot && word(reinterpret_cast<uintptr_t>(hot));
                copy(&owner->reply_memory_set, memory_present ? memory : &owner->witness.memory_set);
                copy(&owner->reply_hot_set, hot_present ? hot : &owner->witness.hot_set);
                if (!creating(owner->request.operation) && (!owner->reply_memory_set || !owner->reply_hot_set)) throw std::exception();
                if (memory_present) copy(&owner->reply_memory_content, reinterpret_cast<const void *>(owner->reply_memory_set + 0x28));
                if (hot_present) copy(&owner->reply_hot_content, reinterpret_cast<const void *>(owner->reply_hot_set + 0x28));
                owner->reply_captured.store(2, std::memory_order_release);
            } catch (...) { owner->invalid.store(1, std::memory_order_release); }
        }
    }
    if (owner) PERSIST_DIAG(owner->request.identity.deck, owner->request.identity.token, "reply", result ? *result : -1);
    Receipt *receipt = nullptr;
    if (track && result && pointer(receiver)) {
        for (auto &entry : state().receipts) {
            unsigned free = FREE;
            if (entry.phase.compare_exchange_strong(free, WRITING, std::memory_order_acq_rel)) {
                receipt = &entry;
                break;
            }
        }
    }
    if (receipt) {
        uint64_t sequence = state().sequence.fetch_add(1, std::memory_order_acq_rel);
        if (sequence == UINT64_MAX) state().overflow.store(1, std::memory_order_release);
        receipt->sequence = sequence;
        receipt->maintainer = word(receiver + 0x10);
        receipt->tag = u32(receiver + 8);
        receipt->result = *result;
        std::memcpy(receipt->track, track, 16);
        receipt->token = owner ? owner->request.identity.token : 0;
        receipt->deck = owner ? owner->request.identity.deck : 4;
        receipt->phase.store(ACTIVE, std::memory_order_release);
    } else state().overflow.store(1, std::memory_order_release);
    original<void (*)(void *, const void *, const void *, const void *, const void *, const int *)> (&state().reply_original)(
        self, request_id, track, memory, hot, result);
}
void registered_hook(void *payload) {
    if (!state().enabled.load(std::memory_order_acquire)) {
        original<void (*)(void *)> (&state().registered_original)(payload);
        return;
    }
    uintptr_t p = reinterpret_cast<uintptr_t>(payload);
    Receipt *receipt = nullptr;
    uint64_t sequence = UINT64_MAX;
    for (auto &entry : state().receipts) {
        unsigned expected = ACTIVE;
        if (!entry.phase.compare_exchange_strong(expected, POLLING, std::memory_order_acq_rel)) continue;
        if (entry.maintainer == word(p) && entry.tag == u32(p + 12) && entry.result == int(u32(p + 8)) &&
            !std::memcmp(entry.track, reinterpret_cast<const void *>(p + 16), 16) && entry.sequence < sequence) {
            if (receipt) receipt->phase.store(ACTIVE, std::memory_order_release);
            receipt = &entry;
            sequence = entry.sequence;
        } else entry.phase.store(ACTIVE, std::memory_order_release);
    }
    /* Actual native task disposes its active request/handler before notifying
     * our actor continuation. Merely seeing admission never completes a command. */
#if defined(AZSDK_CUE_DIAGNOSTIC)
    unsigned diagnostic_deck = receipt ? receipt->deck : 4;
    uint64_t diagnostic_token = receipt ? receipt->token : 0;
    if (diagnostic_deck < 4) PERSIST_DIAG(diagnostic_deck, diagnostic_token, "registered-enter", receipt->result);
#endif
    original<void (*)(void *)> (&state().registered_original)(payload);
#if defined(AZSDK_CUE_DIAGNOSTIC)
    if (diagnostic_deck < 4) PERSIST_DIAG(diagnostic_deck, diagnostic_token, "registered-return", receipt->result);
#endif
    if (!receipt) return;
    unsigned deck = receipt->deck;
    uint64_t token = receipt->token;
    int result = receipt->result;
    receipt->phase.store(FREE, std::memory_order_release);
    if (deck >= 4 || !token) return;
    Slot &slot = state().slots[deck];
    unsigned phase = slot.phase.load(std::memory_order_acquire);
    if ((phase != ACTIVE && phase != HELPING) || slot.request.identity.token != token) return;
    slot.native_result.store(result, std::memory_order_relaxed);
    slot.receipt_seen.store(1, std::memory_order_release);
    complete_receipt(slot);
}
} // namespace

extern "C" int azsdk_cue_persist_configure(const azcuepersist_profile *profile, azexit_install_fn install,
                                           const azcuepersist_callbacks *callbacks) noexcept {
    if (!profile || !callbacks || !install || !callbacks->validate || !callbacks->confirm || !callbacks->ready ||
        !profile->save || !profile->erase || !profile->request || !profile->handler_request || !profile->reception_reply ||
        !profile->registered_task || !profile->handler_table || !profile->reception_table || !profile->string_allocate ||
        !profile->string_assign || !profile->string_destroy || !profile->color_construct || !profile->memory_helper || !profile->content_copy || !profile->content_release ||
        !profile->vector_append || !profile->vector_destroy || !profile->content_from_vector || !profile->content_publish || !profile->hot_helper || !profile->hot_content_from_vector || !profile->hot_content_publish || !profile->cue_copy || !profile->cue_destroy) return 0;
    unsigned attempted = 0;
    if (!state().attempted.compare_exchange_strong(attempted, 1, std::memory_order_acq_rel)) return 0;
    state().profile = *profile;
    state().callbacks = *callbacks;
    try {
        if (!install(profile->request, reinterpret_cast<void *>(request_hook), &state().request_original) ||
            !install(profile->handler_request, reinterpret_cast<void *>(handler_hook), &state().handler_original) ||
            !install(profile->reception_reply, reinterpret_cast<void *>(reply_hook), &state().reply_original) ||
            !install(profile->registered_task, reinterpret_cast<void *>(registered_hook), &state().registered_original) ||
            !install(profile->memory_helper, reinterpret_cast<void *>(memory_hook), &state().memory_original) ||
            !install(profile->hot_helper, reinterpret_cast<void *>(hot_hook), &state().hot_original)) return 0;
    } catch (...) { return 0; }
    state().enabled.store(1, std::memory_order_release);
    return 1;
}
extern "C" uint64_t azsdk_cue_persist_capabilities(void) noexcept {
    if (!state().enabled.load(std::memory_order_acquire) || state().overflow.load(std::memory_order_acquire)) return 0;
    uint64_t result = 0;
    for (unsigned operation = AZ_CMD_SAVED_SAVE; operation <= AZ_CMD_CUE_LOOP_SET; ++operation)
        if (supported(operation)) result |= UINT64_C(1) << operation;
    return result;
}
extern "C" int azsdk_cue_persist_submit(const azexit_cue_request *request, void *manager, void *maintainer,
                                        unsigned kind, const void *property, uint64_t cookie, const azcuepersist_witness *witness) noexcept {
    if (!request || request->size < sizeof *request || request->identity.deck >= 4 || !request->identity.token ||
        !request->identity.load_generation || !request->identity.deadline_ns || request->identity.reserved ||
        !manager || !maintainer || !property || !cookie || kind > 8) return AZ_INVALID;
    uintptr_t prop = reinterpret_cast<uintptr_t>(property);
    const uint32_t property_rate = u32(prop + 0x60);
    if (!supported(request->operation) || !(azsdk_cue_persist_capabilities() & (UINT64_C(1) << request->operation))) return AZ_UNSUPPORTED;
    if ((request->operation == AZ_CMD_HOT_SET || request->operation == AZ_CMD_HOT_CLEAR) && (!kind || kind != request->entry_id)) return AZ_INVALID;
    if (!(request->flags & AZ_COMMAND_PERSIST) || request->rgb > 0xffffffu ||
        (request->operation == AZ_CMD_SAVED_AUTO && request->position_s != 0.0 && request->position_s != 1.0)) return AZ_INVALID;
    size_t length;
    if (!utf8(request->comment, length)) return AZ_INVALID;
    if (request->operation == AZ_CMD_CUE_COLOR && !kind) {
        bool supported_color = false;
        for (unsigned color : {0x000000u, 0xff00c6u, 0xff0000u, 0xff7100u, 0xffff00u, 0x00e300u, 0x00c3ffu, 0x0051ffu, 0x9c08ffu}) supported_color = supported_color || request->rgb == color;
        if (!supported_color) return AZ_UNSUPPORTED;
    }
    Slot &slot = state().slots[request->identity.deck];
    unsigned free = FREE;
    if (!slot.phase.compare_exchange_strong(free, WRITING, std::memory_order_acq_rel)) return AZ_BUSY;
    slot.request = *request;
    slot.helper_claim.store(0, std::memory_order_relaxed);
    slot.native_serial = 0;
    slot.reply_captured.store(0, std::memory_order_relaxed);
    slot.metadata_applied.store(0, std::memory_order_relaxed);
    slot.witness = {};
    if (metadata(request->operation)) {
        if (!witness || !witness->source_info || !witness->fingerprint || !witness->revision || (request->operation != AZ_CMD_HOT_SET && witness->revision != request->expected_revision)) {
            slot.phase.store(FREE, std::memory_order_release);
            return AZ_STALE;
        }
        slot.witness = *witness;
        uintptr_t *refs[] = {&slot.witness.source_info, &slot.witness.memory_set, &slot.witness.hot_set, &slot.witness.memory_content, &slot.witness.hot_content};
        uintptr_t input[] = {witness->source_info, witness->memory_set, witness->hot_set, witness->memory_content, witness->hot_content};
        for (auto ref : refs) *ref = 0;
        for (unsigned i = 0; i < 5; ++i)
            if (input[i])
                reinterpret_cast<void (*)(void *, const void *)>(state().profile.content_copy)(refs[i], &input[i]);
        bool same = true;
        for (unsigned i = 0; i < 5; ++i) same = same && *refs[i] == input[i];
        if (!same || !witness_current(slot)) {
            slot.diagnostic.store(40, std::memory_order_relaxed);
            PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-witness", AZ_STALE);
            release_witness(slot);
            slot.phase.store(FREE, std::memory_order_release);
            return AZ_STALE;
        }
    }
    if (request->operation == AZ_CMD_SAVED_AUTO || request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR) {
        uintptr_t locator = witness ? witness->selected_cue : 0;
        uintptr_t content = kind ? slot.witness.hot_content : slot.witness.memory_content, begin, end;
        uint64_t count;
        uintptr_t selected = 0;
        if (pointer(locator) && vector_bounds(content, begin, end, count)) {
            for (uintptr_t row = begin; row != end; row += 0x80) {
                if (*reinterpret_cast<const unsigned char *>(row) != kind || *reinterpret_cast<const unsigned char *>(locator) != kind ||
                    !azcue_timestamps_equal(azcue_read_timestamps(reinterpret_cast<const void *>(row)), azcue_read_timestamps(reinterpret_cast<const void *>(locator)), property_rate)) continue;
                if (selected) {
                    selected = 0;
                    break;
                }
                selected = row;
            }
        }
        if (!selected) {
            release_witness(slot);
            slot.phase.store(FREE, std::memory_order_release);
            return AZ_STALE;
        }
        slot.witness.selected_cue = selected;
    }
    slot.manager.store(manager, std::memory_order_release);
    slot.maintainer = maintainer;
    slot.cookie = cookie;
    slot.kind = kind;
    slot.handler.store(0, std::memory_order_relaxed);
    slot.receiver.store(0, std::memory_order_relaxed);
    slot.invalid.store(0, std::memory_order_relaxed);
    slot.retired.store(0, std::memory_order_relaxed);
    slot.mutated.store(0, std::memory_order_relaxed);
    slot.native_result.store(-1, std::memory_order_relaxed);
    slot.receipt_seen.store(0, std::memory_order_relaxed);
    slot.diagnostic.store(1, std::memory_order_relaxed);
    slot.no_effect.store(0, std::memory_order_relaxed);
    slot.request_entered.store(0, std::memory_order_relaxed);
    slot.submit_time.store(now(), std::memory_order_relaxed);
    slot.request_time.store(0, std::memory_order_relaxed);
    slot.handler_time.store(0, std::memory_order_relaxed);
    slot.request_source = 0;
    slot.request_rate = 0;
    uintptr_t m = reinterpret_cast<uintptr_t>(maintainer), q = word(m + 0x150);
    if (!valid(slot)) {
        slot.diagnostic.store(41, std::memory_order_relaxed);
        PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-invalid", AZ_STALE);
        release_witness(slot);
        slot.phase.store(FREE, std::memory_order_release);
        return AZ_STALE;
    }
    if (!pointer(q) || word(m + 0x158) || word(m + 0x160) || word(q + 0x48) != word(q + 0x68)) {
        unsigned busy = (!pointer(q) ? 1u : 0u) | (word(m + 0x158) ? 2u : 0u) | (word(m + 0x160) ? 4u : 0u) |
                        (pointer(q) && word(q + 0x48) != word(q + 0x68) ? 8u : 0u);
        slot.diagnostic.store(48 + busy, std::memory_order_relaxed);
        PERSIST_DIAG(request->identity.deck, request->identity.token, "preflight-queue", int(busy));
        release_witness(slot);
        slot.phase.store(FREE, std::memory_order_release);
        return AZ_BUSY;
    }
    std::memcpy(slot.property_identity, reinterpret_cast<const void *>(prop + 0x30), 12);
    slot.property_in = word(prop + 0x40);
    slot.property_source = word(prop + 0x50);
    slot.property_rate = property_rate;
    slot.phase.store(ACTIVE, std::memory_order_release);
    Scope scope(&slot);
    try {
        slot.diagnostic.store(20, std::memory_order_release);
        uintptr_t producer = deleting(request->operation) ? state().profile.erase : state().profile.save;
        PERSIST_DIAG(request->identity.deck, request->identity.token, "producer-enter", 0);
        reinterpret_cast<void (*)(void *, unsigned, const void *)>(producer)(maintainer, kind, property);
        PERSIST_DIAG(request->identity.deck, request->identity.token, "producer-return", 0);
    } catch (const std::exception &) {
        slot.diagnostic.store(31, std::memory_order_release);
        slot.invalid.store(1, std::memory_order_release);
    } catch (...) {
        slot.diagnostic.store(32, std::memory_order_release);
        slot.invalid.store(1, std::memory_order_release);
    }
    if (slot.no_effect.load(std::memory_order_acquire) && !slot.request_entered.load(std::memory_order_acquire) &&
        !slot.handler.load(std::memory_order_acquire)) {
        release_witness(slot);
        slot.phase.store(FREE, std::memory_order_release);
        return AZ_STALE;
    }
    unsigned producer_entered = 20;
    slot.diagnostic.compare_exchange_strong(producer_entered, 21, std::memory_order_acq_rel);
    /* If a foreign native path bypassed the checked request/handler route, keep
     * this deck fenced; we cannot attest that the producer had no effect. */
    if (!slot.handler.load(std::memory_order_acquire)) slot.invalid.store(1, std::memory_order_release);
    return AZ_PENDING;
}
extern "C" int azsdk_cue_persist_confirm_publication(const azexit_cue_request *request, const azcuepersist_witness *fresh) noexcept {
    if (!request || !fresh || request->identity.deck >= 4) return 0;
    Slot &slot = state().slots[request->identity.deck];
    int result = slot.native_result.load(std::memory_order_acquire);
    if (slot.phase.load(std::memory_order_acquire) != POLLING || slot.reply_captured.load(std::memory_order_acquire) != 2 ||
        !slot.metadata_applied.load(std::memory_order_acquire) || (result != 0 && result != 5) ||
        slot.request.identity.token != request->identity.token || slot.request.identity.load_generation != request->identity.load_generation ||
        slot.request.operation != request->operation || slot.request.expected_revision != request->expected_revision ||
        slot.request.entry_id != request->entry_id || slot.request.classification != request->classification ||
        compare_timestamps(slot.request.position_s, request->position_s, 1, slot.property_rate, 1) != AZ_TIME_EQUAL ||
        compare_timestamps(slot.request.end_s, request->end_s, 1, slot.property_rate, 1) != AZ_TIME_EQUAL || !valid(slot)) return 0;
    if (!fresh->revision || fresh->source_info != slot.witness.source_info || fresh->memory_set != slot.reply_memory_set ||
        fresh->hot_set != slot.reply_hot_set || std::memcmp(fresh->source_id, slot.native_track, 12) ||
        !fresh->fingerprint || witness_hash(*fresh) != fresh->fingerprint ||
        (fresh->memory_set ? word(fresh->memory_set + 0x28) != fresh->memory_content : bool(fresh->memory_content)) ||
        (fresh->hot_set ? word(fresh->hot_set + 0x28) != fresh->hot_content : bool(fresh->hot_content)) || !valid(slot)) return 0;
    return fresh->memory_content == slot.reply_memory_content && fresh->hot_content == slot.reply_hot_content ? 1 : 2;
}
extern "C" int azsdk_cue_persist_take(const azexit_cue_request *request, void *manager, azexit_result *out) noexcept {
    if (!request || !out || request->identity.deck >= 4 || !manager) return AZ_INVALID;
    Slot &slot = state().slots[request->identity.deck];
    unsigned phase = RECEIPT;
    if (!slot.phase.compare_exchange_strong(phase, POLLING, std::memory_order_acq_rel)) return phase == FREE ? AZ_STALE : AZ_PENDING;
    if (slot.request.identity.token != request->identity.token || slot.manager.load(std::memory_order_acquire) != manager) {
        slot.phase.store(RECEIPT, std::memory_order_release);
        return AZ_STALE;
    }
    int native_result = slot.native_result.load(std::memory_order_acquire);
    bool native_success = native_result == 0 || native_result == 5;
    bool eligible = !state().overflow.load(std::memory_order_acquire) && valid(slot) && native_success;
    bool applied = false;
    uint64_t revision = 0;
    if (eligible) {
        try {
            applied = state().callbacks.confirm(&slot.request, manager, slot.cookie, &revision, state().callbacks.context) && revision;
        } catch (...) { applied = false; }
        /* The repository reply can precede publication of the source-qualified
         * CueSet. Observe on fresh owner continuations until the ORIGINAL
         * deadline; never submit the native save/delete producer again. */
        eligible = !state().overflow.load(std::memory_order_acquire) && valid(slot);
        if (!applied && eligible) {
            slot.phase.store(RECEIPT, std::memory_order_release);
            return AZ_PENDING;
        }
        applied = applied && eligible;
    }
    *out = {slot.request.identity.token, slot.request.identity.load_generation, applied ? revision : 0,
            applied ? AZ_COMMAND_APPLIED : AZ_COMMAND_UNCERTAIN, applied ? AZ_OK : AZ_FAILED};
    release_witness(slot);
    slot.phase.store(FREE, std::memory_order_release);
    return AZ_OK;
}
extern "C" int azsdk_cue_persist_poll(const azexit_cue_request *request, void *manager, azexit_result *out) noexcept {
    return azsdk_cue_persist_take(request, manager, out);
}
extern "C" void azsdk_cue_persist_retire(void *manager) noexcept {
    for (auto &slot : state().slots) {
        unsigned phase = slot.phase.load(std::memory_order_acquire);
        if ((phase == ACTIVE || phase == HELPING || phase == RECEIPT || phase == POLLING) && slot.manager.load(std::memory_order_acquire) == manager) {
            slot.invalid.store(1, std::memory_order_release);
            slot.retired.store(1, std::memory_order_release);
            unsigned receipt_phase = RECEIPT;
            if (slot.phase.compare_exchange_strong(receipt_phase, POLLING, std::memory_order_acq_rel)) {
                retire_receipt(slot);
            }
        }
    }
}
