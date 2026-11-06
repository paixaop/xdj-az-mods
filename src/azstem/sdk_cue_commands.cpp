/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/aztimestamp.h"
#include "../common/azcuecommands.h"
#include "../common/azcueactor.h"
#include "../common/azcueobserver.h"
#include "../common/azcuestore.h"
#include "../common/azloadobserver.h"
#include "../common/azcueprofile.h"
#include "../common/azcuepersist.h"
#include "../common/azcuerecall.h"
#include "../common/azcuerecallbridge.h"
#include "../azmod/azmod-mod.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>
#include <time.h>
#if defined(AZSDK_CUE_DIAGNOSTIC)
#include <cstdio>
#endif
namespace {
constexpr uint64_t INTERNAL = UINT64_C(1) << 63;
constexpr uint64_t bit(unsigned operation) { return UINT64_C(1) << operation; }
struct Cache {
    std::mutex mutex;
    std::atomic<bool> waiter {false};
    az_native_cue_snapshot value = {};
    uint64_t observed = 0;
    std::atomic<uint64_t> refresh {0}, actor_finished {0};
    azcuestore::RevisionState revision;
    /* One copied public command owns admission through release, including
     * contention with another deck's owner frame or an internal snapshot query.
     * 0 empty,1 exclusive write/read,2 queued,3 terminal,4 management posting,
     * 5 active public command in the actor. */
    std::atomic<unsigned> waiting {0};
    azexit_cue_request waiting_request = {};
    azexit_result waiting_result = {};
};
struct AutoPlan {
    bool active = false, final = false, effect_seen = false;
    unsigned steps = 0;
    uint64_t token = 0, revision = 0;
    az_cue_entry target = {};
    uint32_t target_rate = 0;
};
struct Pending {
    std::atomic<uint64_t> cookie {0}, token {0};
    std::atomic<uint64_t> resume {0}, persist_ready {0}, bridge_ready {0};
    std::atomic<uint64_t> actor_retired {0}, retired_load {0}, persist_retired {0}, stop_resumed {0};
    azexit_result actor_completion = {}; // management dispatcher only
    std::atomic<uintptr_t> manager {0};
    az_cue_entry expected = {};
    uint32_t expected_rate = 0;
    azcuestore::Property retained;
    std::atomic<uint64_t> bridge {0}, bridge_generation {0}, bridge_revision {0};
    std::atomic<unsigned> stage {0};
    std::atomic<uint64_t> stage_token {0};
    azexit_cue_stage_sample staged_cue = {};
    uint64_t before_revision = 0;
    azexit_cue_request active_request = {};
    // Borrowed only inside confirm: persistence pins all five baseline refs
    // until that callback returns, including the original immutable contents.
    azcuepersist_witness before_witness = {};
    AutoPlan automatic;
};
struct State {
    std::atomic<bool> enabled {false};
    std::atomic<uint64_t> capabilities {0}, serial {0};
    Cache decks[4];
    Pending pending[4];
    azcuestore::Profile store;
    int build = 0;
    uintptr_t bias = 0;
    std::atomic<unsigned> notifications {0}, collections {0};
};
static State command_state;
State &state() { return command_state; }
#ifdef AZSDK_CUE_COMMANDS_TESTING
extern "C" void azsdk_exit_delivery_notify(unsigned) noexcept {}
extern "C" int azsdk_exit_delivery_continue(const azexit_request *, uint64_t) noexcept { return AZ_OK; }
extern "C" int azsdk_exit_delivery_publish(const azexit_receipt *) noexcept { return AZ_UNSUPPORTED; }
extern "C" int azsdk_cue_actor_peek(unsigned deck, azexit_result *result) noexcept {
    return azsdk_cue_actor_poll(deck, result);
}
extern "C" int azsdk_cue_actor_take(unsigned deck, azexit_result *result) noexcept {
    return azsdk_cue_actor_peek(deck, result);
}
extern "C" int azsdk_cue_persist_take(const azexit_cue_request *request, void *manager,
                                     azexit_result *result) noexcept {
    return azsdk_cue_persist_poll(request, manager, result);
}
extern "C" int azsdk_exit_recall_take(unsigned deck, azexit_result *result) noexcept {
    return azsdk_exit_recall_poll(deck, result);
}
#endif
bool unfinished_public_work() noexcept {
    State &s = state();
    for (unsigned deck = 0; deck < 4; ++deck) {
        unsigned waiting = s.decks[deck].waiting.load(std::memory_order_acquire);
        if (!waiting || waiting == 3) continue;
        if (waiting != 5) return true;
#ifdef AZSDK_CUE_COMMANDS_TESTING
        azexit_result result = {};
        if (azsdk_cue_actor_peek(deck, &result) != AZ_OK || !result.token ||
            (result.token & INTERNAL) || result.outcome == AZ_COMMAND_ACCEPTED) return true;
#else
        if (s.decks[deck].actor_finished.load(std::memory_order_acquire) !=
            s.decks[deck].waiting_request.identity.token) return true;
#endif
    }
    return false;
}
void retire_internal_reads() noexcept {
    State &s = state();
    for (unsigned deck = 0; deck < 4; ++deck) {
        Cache &cache = s.decks[deck];
        uint64_t refresh = cache.refresh.load(std::memory_order_acquire);
        if ((refresh & INTERNAL) && azsdk_cue_actor_cancel(deck, refresh) == AZ_OK &&
            azsdk_cue_actor_release(deck, refresh) == AZ_OK)
            cache.refresh.compare_exchange_strong(refresh, 0, std::memory_order_acq_rel);
    }
}
#ifdef AZSDK_CUE_COMMANDS_TESTING
std::atomic<uint64_t> test_clock {0};
#endif
uint64_t now() {
#ifdef AZSDK_CUE_COMMANDS_TESTING
    uint64_t injected = test_clock.load(std::memory_order_relaxed);
    if (injected) return injected;
#endif
    timespec t = {};
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0) return 0;
    return uint64_t(t.tv_sec) * UINT64_C(1000000000) + uint64_t(t.tv_nsec);
}
#if defined(AZSDK_CUE_DIAGNOSTIC)
void command_diagnostic(const azexit_cue_request &request, unsigned stage, const char *name) {
    static std::atomic<unsigned> last[4];
    static std::atomic<uint64_t> tokens[4];
    unsigned deck = request.identity.deck;
    if (deck >= 4 || !request.operation) return;
    bool changed = tokens[deck].exchange(request.identity.token, std::memory_order_relaxed) != request.identity.token;
    if (last[deck].exchange(stage + 1, std::memory_order_relaxed) == stage + 1 && !changed) return;
    std::fprintf(stderr, "AZCUE command deck=%u stage=%s op=%u token=%llu load=%llu clock=%llu deadline=%llu\n",
                 deck, name, request.operation, (unsigned long long) request.identity.token, (unsigned long long) request.identity.load_generation,
                 (unsigned long long) now(), (unsigned long long) request.identity.deadline_ns);
}
#define COMMAND_DIAG(request, stage, name) command_diagnostic(request, stage, name)
#else
#define COMMAND_DIAG(...) ((void) 0)
#endif
int current(unsigned deck, uint64_t generation, void *) {
    azsdk_load_sample sample = {};
    return state().enabled.load(std::memory_order_acquire) && azsdk_load_read(int(deck), &sample) &&
           sample.load_generation == generation;
}
void ready(unsigned deck, uint64_t token, void *) {
    if (deck >= 4 || !token) return;
    Pending &pending = state().pending[deck];
    pending.persist_ready.store(token, std::memory_order_release);
    pending.resume.store(token, std::memory_order_release);
    azsdk_cue_commands_notify(deck);
}
void publish_waiting_result(unsigned deck, const azexit_result &result) noexcept {
    azexit_receipt value = {
        sizeof value, AZEXIT_DELIVERY_VERSION, AZEXIT_RECEIPT_TERMINAL, 1, deck,
        result.token, result.load_generation, 0, result.revision, now(), result.outcome, result.reason};
    (void) azsdk_exit_delivery_publish(&value);
}
template <class T>
T read(const void *p, unsigned offset) {
    T value;
    std::memcpy(&value, static_cast<const char *>(p) + offset, sizeof value);
    return value;
}
bool valid_position(const unsigned char *p) {
    return p[0] && (p[4] & 0x1f) && p[6] && read<int32_t>(p, 8) != 0 &&
           read<uint64_t>(p, 16) <= UINT64_C(9007199254740992) && read<uint32_t>(p, 0x30) && read<uintptr_t>(p, 0x20);
}
bool property_scalar(const azcuestore::NativeProperty &property, az_cue_entry &out) {
    const unsigned char *p = property.bytes;
    if (!valid_position(p + 0x30)) return false;
    out.start_s = double(read<uint64_t>(p, 0x40)) / read<uint32_t>(p, 0x60);
    out.valid |= AZ_CUE_START;
    out.type = AZ_CUE_POINT;
    if (valid_position(p + 0x88) && !std::memcmp(p + 0x30, p + 0x88, 12) &&
        read<uint32_t>(p, 0x60) == read<uint32_t>(p, 0xb8) &&
        compare_timestamps(read<uint64_t>(p, 0x98), read<uint64_t>(p, 0x40),
                           read<uint32_t>(p, 0x60), read<uint32_t>(p, 0x60), 1) == AZ_TIME_AFTER) {
        out.end_s = double(read<uint64_t>(p, 0x98)) / read<uint32_t>(p, 0xb8);
        out.valid |= AZ_CUE_END;
        out.type = AZ_CUE_LOOP;
    }
    return true;
}
void copy_utf8_prefix(char (&out)[128], const char *text) {
    unsigned at = 0;
    while (at < 127 && text[at]) {
        unsigned char first = static_cast<unsigned char>(text[at]);
        unsigned width = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
                                        : first >= 0xe0 && first <= 0xef   ? 3
                                        : first >= 0xf0 && first <= 0xf4   ? 4
                                                                           : 0;
        if (!width || width > 127 - at) break;
        uint32_t code = first & (width == 1 ? 0x7f : (1u << (7 - width)) - 1);
        bool valid = true;
        for (unsigned i = 1; i < width; ++i) {
            unsigned char next = static_cast<unsigned char>(text[at + i]);
            if ((next & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            code = (code << 6) | (next & 0x3f);
        }
        if (!valid || (width == 2 && code < 0x80) || (width == 3 && code < 0x800) ||
            (width == 4 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) break;
        std::memcpy(out + at, text + at, width);
        at += width;
    }
    out[at] = 0;
}
/* -1 malformed, 0 native absent HOT slot, 1 populated source-qualified row. */
int scalar(azcuestore::Snapshot &snapshot, size_t ordinal, az_cue_entry &out,
           azcuestore::Property *selected_property = nullptr) {
    azcuestore::OwnedCue cue;
    azcuestore::Property temporary;
    azcuestore::Property &property = selected_property ? *selected_property : temporary;
    if (!snapshot.row(ordinal, cue)) return -1;
    unsigned kind = cue.native.bytes[0];
    const bool hot_collection = ordinal >= snapshot.memory_count();
    /* Exact native validStoredCue guard. Only its fixed HOT collection may
     * contain absent slots; populated conversion/source failures are errors. */
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    if (hot_collection && (kind == 0xff || read<uint64_t>(&cue.native, 8) == absent || read<uint64_t>(&cue.native, 0x10) == absent)) return 0;
    if ((hot_collection ? kind < 1 || kind > 8 : kind != 0) ||
        !snapshot.property(uint32_t(ordinal + 1), snapshot.revision(), property)) return -1;
    out = {};
    out.size = sizeof out;
    out.version = 1;
    out.saved = 1;
    out.valid = AZ_CUE_SAVED;
    out.classification = kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
    out.slot = kind;
    out.entry_id = kind ? kind : uint32_t(ordinal + 1);
    out.rgb = read<uint32_t>(&cue.native, 0x78) & 0xffffff;
    out.valid |= AZ_CUE_COLOR;
    out.auto_active = !!cue.native.bytes[0x28];
    out.valid |= AZ_CUE_AUTO_ACTIVE;
    uintptr_t text = read<uintptr_t>(&cue.native, 0x70);
    if (text) {
        copy_utf8_prefix(out.comment, reinterpret_cast<const char *>(text));
        out.valid |= AZ_CUE_COMMENT;
    }
    return property_scalar(property.native, out) ? 1 : -1;
}
bool snapshot_copy(azcuestore::Snapshot &snapshot, unsigned deck, uint64_t generation, az_native_cue_snapshot &out) {
    out = {};
    out.size = sizeof out;
    out.version = 1;
    out.deck = deck;
    out.native_load_generation = generation;
    out.revision = snapshot.revision();
    for (size_t i = 0; i < snapshot.count(); ++i) {
        int status = scalar(snapshot, i, out.entries[out.count]);
        if (status < 0) return false;
        if (status > 0) ++out.count;
    }
    return true;
}
void publish(unsigned deck, const az_native_cue_snapshot &snapshot) {
    Cache &cache = state().decks[deck];
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.value = snapshot;
    cache.observed = now();
}
int validate_persist(const azexit_cue_request *request, void *manager, uint64_t cookie, void *) {
    if (request->identity.deck >= 4 || !current(request->identity.deck, request->identity.load_generation, nullptr)) {
        COMMAND_DIAG(*request, 6, "persist-current");
        return 0;
    }
    Pending &pending = state().pending[request->identity.deck];
    return cookie && pending.cookie.load(std::memory_order_acquire) == cookie &&
           pending.token.load(std::memory_order_acquire) == request->identity.token &&
           pending.manager.load(std::memory_order_acquire) == uintptr_t(manager) && pending.cookie.load(std::memory_order_acquire) == cookie;
}
bool same_point(const az_cue_entry &a, const az_cue_entry &b, uint32_t rate) {
    return a.classification == b.classification && a.slot == b.slot && a.type == b.type &&
           compare_timestamps(a.start_s, b.start_s, 1, rate, 1) == AZ_TIME_EQUAL &&
           (!(a.valid & AZ_CUE_END) || compare_timestamps(a.end_s, b.end_s, 1, rate, 1) == AZ_TIME_EQUAL);
}
bool staged_point(const azcuestore::NativeProperty &property, const az_cue_entry &entry, const azexit_cue_stage_sample &captured) {
    const unsigned char *p = property.bytes;
    return captured.sample_rate && read<uint32_t>(p, 0x60) == captured.sample_rate &&
           !std::memcmp(p + 0x30, captured.source_id, 12) &&
           bool(entry.valid & AZ_CUE_END) == bool(captured.out_valid) &&
           compare_timestamps(read<uint64_t>(p, 0x40), captured.in_sample, captured.sample_rate, captured.sample_rate, 1) == AZ_TIME_EQUAL &&
           (!captured.out_valid || (read<uint32_t>(p, 0xb8) == captured.sample_rate &&
                                    !std::memcmp(p + 0x88, captured.source_id, 12) &&
                                    compare_timestamps(read<uint64_t>(p, 0x98), captured.out_sample, captured.sample_rate, captured.sample_rate, 1) == AZ_TIME_EQUAL));
}
int confirm_persist(const azexit_cue_request *request, void *manager, uint64_t cookie, uint64_t *revision, void *) {
    if (!validate_persist(request, manager, cookie, nullptr)) return 0;
    State &s = state();
    unsigned deck = request->identity.deck;
    azcuestore::Snapshot snapshot;
    az_native_cue_snapshot copied;
    if (!snapshot.open(s.store, manager, request->identity.load_generation, nullptr, s.decks[deck].revision)) return 0;
    COMMAND_DIAG(*request, 15, "confirm-open");
    Pending &pending = s.pending[deck];
    if (request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR) {
        azcuepersist_witness fresh = {};
        const unsigned kind = request->classification == AZ_CUE_HOT ? request->entry_id : 0;
        if (!snapshot.content_witness(snapshot.revision(), &fresh) ||
            !azsdk_cue_persist_confirm_publication(request, &fresh) ||
            !snapshot.metadata_only_changed(pending.before_witness, kind, request->entry_id,
                                            request->operation == AZ_CMD_CUE_COMMENT ? request->comment : nullptr, request->rgb, pending.expected_rate) ||
            !validate_persist(request, manager, cookie, nullptr)) return 0;
        // The exact receipt and full typed native collection prove the edit.
        // Public position conversion is an independent cache refresh, outside
        // this already-dispatched write's original completion budget.
        COMMAND_DIAG(*request, 17, "confirm-metadata");
        *revision = snapshot.revision();
        return 1;
    }
    if (!snapshot_copy(snapshot, deck, request->identity.load_generation, copied) || !validate_persist(request, manager, cookie, nullptr)) return 0;
    COMMAND_DIAG(*request, 16, "confirm-copied");
    if (request->operation == AZ_CMD_SAVED_AUTO) {
        azcuepersist_witness fresh = {};
        if (!snapshot.content_witness(snapshot.revision(), &fresh) ||
            !azsdk_cue_persist_confirm_publication(request, &fresh) ||
            !snapshot.auto_only_changed(pending.before_witness, request->entry_id, request->position_s != 0, pending.expected_rate)) return 0;
        if (pending.automatic.active && pending.automatic.token == request->identity.token && pending.automatic.final) {
            unsigned active = 0;
            for (unsigned i = 0; i < copied.count; ++i) {
                const az_cue_entry &row = copied.entries[i];
                if (row.classification == AZ_CUE_MEMORY && row.type == AZ_CUE_LOOP && row.auto_active) {
                    if (row.entry_id != pending.automatic.target.entry_id) return 0;
                    ++active;
                }
            }
            if (active != 1) return 0;
        }
    }
    const az_cue_entry &expected = pending.expected;
    bool exists = false, effect = false;
    for (unsigned i = 0; i < copied.count; ++i) {
        const az_cue_entry &row = copied.entries[i];
        bool target = expected.classification == AZ_CUE_HOT ? row.classification == AZ_CUE_HOT && row.slot == expected.slot : same_point(row, expected, pending.expected_rate);
        if (!target) continue;
        exists = true;
        if (request->operation == AZ_CMD_CUE_COMMENT) effect = !std::strcmp(row.comment, request->comment);
        else if (request->operation == AZ_CMD_CUE_COLOR) effect = row.rgb == request->rgb;
        else if (request->operation == AZ_CMD_SAVED_AUTO) effect = row.auto_active == int(request->position_s);
        else if (request->operation == AZ_CMD_HOT_SET) effect = same_point(row, expected, pending.expected_rate) && row.rgb == request->rgb;
        else effect = same_point(row, expected, pending.expected_rate);
    }
    bool deletion = request->operation == AZ_CMD_SAVED_DELETE || request->operation == AZ_CMD_MEMORY_DELETE || request->operation == AZ_CMD_HOT_CLEAR;
    if (!(deletion ? !exists : effect)) return 0;
    publish(deck, copied);
    *revision = copied.revision;
    return 1;
}
bool selector(unsigned operation) {
    return operation == AZ_CMD_SAVED_RECALL || operation == AZ_CMD_SAVED_DELETE || operation == AZ_CMD_SAVED_AUTO ||
           operation == AZ_CMD_MEMORY_RECALL || operation == AZ_CMD_MEMORY_DELETE ||
           operation == AZ_CMD_CUE_COMMENT || operation == AZ_CMD_CUE_COLOR;
}
bool hot(unsigned operation) { return operation == AZ_CMD_HOT_SET || operation == AZ_CMD_HOT_CLEAR; }
bool recall(unsigned operation) { return operation == AZ_CMD_SAVED_RECALL || operation == AZ_CMD_MEMORY_RECALL; }
int execute(const azexit_cue_request *request, void *manager, azexit_result *result, void *) {
    State &s = state();
    const unsigned deck = request->identity.deck;
    Pending &pending = s.pending[deck];
    auto fail = [&](int reason) -> int {
        AutoPlan &plan = pending.automatic;
        if (plan.active && plan.token == request->identity.token && plan.effect_seen) {
            *result = {request->identity.token, request->identity.load_generation, 0, AZ_COMMAND_UNCERTAIN, reason};
            plan.active = false;
            return AZ_OK;
        }
        return reason;
    };
    if (pending.token.load(std::memory_order_acquire) == request->identity.token && pending.cookie.load(std::memory_order_acquire)) {
        int rc = azsdk_cue_persist_take(&pending.active_request, manager, result);
        if (rc == AZ_PENDING) return rc;
        pending.persist_ready.store(0, std::memory_order_release);
        pending.cookie.store(0, std::memory_order_release);
        pending.stage.store(0, std::memory_order_release);
        pending.retained.reset();
        AutoPlan &plan = pending.automatic;
        if (!plan.active || plan.token != request->identity.token) return rc;
        if (rc != AZ_OK) return fail(rc);
        if (result->outcome != AZ_COMMAND_APPLIED || !result->revision) {
            if (plan.effect_seen) result->outcome = AZ_COMMAND_UNCERTAIN;
            plan.active = false;
            return AZ_OK;
        }
        if (plan.final) {
            plan.active = false;
            return AZ_OK;
        }
        plan.effect_seen = true;
        plan.revision = result->revision;
        // Continue on this owned frame only after this exact native receipt.
        // The next step must still have the original public deadline/source.
    }
    uint64_t clock = now();
    if (!clock || compare_timestamps(clock, request->identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL) {
        COMMAND_DIAG(*request, 1, "execute-deadline");
        return fail(AZ_STALE);
    }
    if (!current(deck, request->identity.load_generation, nullptr)) {
        COMMAND_DIAG(*request, 2, "execute-current");
        return fail(AZ_STALE);
    }
    if (request->operation) {
        int allowed = azsdk_exit_delivery_continue(&request->identity, clock);
        if (allowed != AZ_OK) return fail(allowed);
    }
    bool awaiting_hot_view = request->operation == AZ_CMD_HOT_SET &&
                             pending.stage_token.load(std::memory_order_acquire) == request->identity.token && pending.stage.load(std::memory_order_acquire) == 2;
    auto resume_hot_completion = [&]() -> int {
        // The native PlayerState copy and the setter's automatic store work
        // can lag DSP completion. Keep the original deadline and capture.
#ifdef AZSDK_CUE_COMMANDS_TESTING
        ready(deck, request->identity.token, nullptr);
#endif
        return AZ_PENDING;
    };
    azcuestore::Snapshot snapshot;
    if (!snapshot.open(s.store, manager, request->identity.load_generation, nullptr, s.decks[deck].revision)) {
        COMMAND_DIAG(*request, 3, "snapshot-open");
        if (awaiting_hot_view) return resume_hot_completion();
        return fail(AZ_STALE);
    }
    if (!current(deck, request->identity.load_generation, nullptr)) {
        COMMAND_DIAG(*request, 4, "snapshot-after-current");
        return fail(AZ_STALE);
    }
    COMMAND_DIAG(*request, 0, "snapshot-admitted");
    if (request->operation == 0) {
        // Snapshot::open revalidates the source and fingerprints both native
        // immutable collections. Their positions need conversion only after
        // that exact load/revision changes, rather than on every 100ms read.
        {
            Cache &cache = s.decks[deck];
            std::lock_guard<std::mutex> lock(cache.mutex);
            if (cache.value.native_load_generation == request->identity.load_generation &&
                cache.value.revision && cache.value.revision == snapshot.revision()) {
                if (!current(deck, request->identity.load_generation, nullptr)) return fail(AZ_STALE);
                cache.observed = now();
                *result = {request->identity.token, request->identity.load_generation, snapshot.revision(), AZ_COMMAND_APPLIED, AZ_OK};
                return AZ_OK;
            }
        }
        az_native_cue_snapshot copied;
        if (!snapshot_copy(snapshot, deck, request->identity.load_generation, copied)) return fail(AZ_UNSUPPORTED);
        if (!current(deck, request->identity.load_generation, nullptr)) return fail(AZ_STALE);
        publish(deck, copied);
        result->token = request->identity.token;
        result->load_generation = request->identity.load_generation;
        result->revision = snapshot.revision();
        result->outcome = AZ_COMMAND_APPLIED;
        result->reason = AZ_OK;
        return AZ_OK;
    }
    azexit_cue_request automatic_step = {};
    const azexit_cue_request *native_request = request;
    if (request->operation == AZ_CMD_SAVED_AUTO && request->position_s == 1) {
        AutoPlan &plan = pending.automatic;
        az_native_cue_snapshot cues = {};
        if (!snapshot_copy(snapshot, deck, request->identity.load_generation, cues)) return fail(AZ_UNSUPPORTED);
        if (!plan.active || plan.token != request->identity.token) {
            if (snapshot.revision() != request->expected_revision) return fail(AZ_STALE);
            plan = {};
            for (unsigned i = 0; i < cues.count; ++i) {
                const auto &row = cues.entries[i];
                if (row.classification == AZ_CUE_MEMORY && row.entry_id == request->entry_id) {
                    plan.target = row;
                    break;
                }
            }
            if (plan.target.type != AZ_CUE_LOOP || !(plan.target.valid & AZ_CUE_END)) return fail(AZ_INVALID);
            azcuestore::Property target_property;
            if (!snapshot.property(plan.target.entry_id, snapshot.revision(), target_property)) return fail(AZ_STALE);
            plan.target_rate = read<uint32_t>(&target_property.native, 0x60);
            plan.active = true;
            plan.token = request->identity.token;
            plan.revision = snapshot.revision();
        }
        if (snapshot.revision() != plan.revision) return fail(AZ_STALE);
        bool target_present = false;
        unsigned other = 0;
        for (unsigned i = 0; i < cues.count; ++i) {
            const auto &row = cues.entries[i];
            if (row.classification != AZ_CUE_MEMORY || row.type != AZ_CUE_LOOP) continue;
            if (row.entry_id == plan.target.entry_id) {
                target_present = same_point(row, plan.target, plan.target_rate) && row.rgb == plan.target.rgb && !std::strcmp(row.comment, plan.target.comment);
            } else if (row.auto_active && !other) other = row.entry_id;
        }
        if (!target_present) return fail(AZ_STALE);
        if (plan.steps >= AZ_NATIVE_CUE_LIMIT) return fail(AZ_UNSUPPORTED);
        automatic_step = *request;
        automatic_step.entry_id = other ? other : plan.target.entry_id;
        automatic_step.expected_revision = plan.revision;
        automatic_step.position_s = other ? 0 : 1;
        plan.final = !other;
        native_request = &automatic_step;
    }
    azcuestore::Property property;
    azcuestore::OwnedCue selected_stored;
    az_cue_entry expected = {};
    unsigned kind = hot(request->operation) ? request->entry_id : 0;
    if (hot(request->operation) && pending.stage_token.load(std::memory_order_acquire) == request->identity.token && pending.stage.load(std::memory_order_acquire) == 2) {
        if (request->operation == AZ_CMD_HOT_CLEAR) {
            if (!pending.retained.copy_to(property)) return fail(AZ_STALE);
            expected = pending.expected;
        } else {
            if (!snapshot.current_property(kind, property) || !property_scalar(property.native, expected) ||
                !staged_point(property.native, expected, pending.staged_cue)) return resume_hot_completion();
            expected.classification = AZ_CUE_HOT;
            expected.slot = kind;
        }
    } else if (request->operation == AZ_CMD_HOT_SET) {
        pending.retained.reset(); // DSP stage captures its playhead, independent of an existing main cue.
    } else if (selector(request->operation) || request->operation == AZ_CMD_HOT_CLEAR) {
        if (selector(request->operation) && native_request->expected_revision != snapshot.revision()) return fail(AZ_STALE);
        size_t ordinal = snapshot.count();
        // Memory IDs are ordinals in this exact admitted revision. Hot IDs are
        // native slot bytes. Resolve those identities before converting points:
        // conversion can perform source seek work and is needed only once for
        // the selected property, never for every preceding row.
        if (native_request->classification == AZ_CUE_MEMORY) {
            if (native_request->entry_id && native_request->entry_id <= snapshot.memory_count())
                ordinal = native_request->entry_id - 1;
        } else {
            for (size_t i = snapshot.memory_count(); i < snapshot.count(); ++i) {
                azcuestore::OwnedCue row;
                if (!snapshot.row(i, row)) return fail(AZ_STALE);
                if (row.native.bytes[0] == native_request->entry_id) {
                    ordinal = i;
                    break;
                }
            }
        }
        if (ordinal == snapshot.count()) return fail(AZ_STALE);
        int status = scalar(snapshot, ordinal, expected, &property);
        if (status < 0) return fail(AZ_UNSUPPORTED);
        if (!status || expected.classification != native_request->classification || expected.entry_id != native_request->entry_id) return fail(AZ_STALE);
        COMMAND_DIAG(*request, 11, "selected-property");
        if ((request->operation == AZ_CMD_SAVED_AUTO || request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR) &&
            !snapshot.row(ordinal, selected_stored)) return fail(AZ_STALE);
        kind = expected.slot;
        if ((request->operation == AZ_CMD_SAVED_RECALL || request->operation == AZ_CMD_SAVED_DELETE || request->operation == AZ_CMD_SAVED_AUTO) &&
            !(expected.valid & AZ_CUE_END)) return fail(AZ_INVALID);
    } else {
        if (!snapshot.current_property(0, property) || !property_scalar(property.native, expected)) return fail(AZ_UNSUPPORTED);
        expected.classification = kind ? AZ_CUE_HOT : AZ_CUE_MEMORY;
        expected.slot = kind;
    }
    if (hot(request->operation) && !(pending.stage_token.load(std::memory_order_acquire) == request->identity.token && pending.stage.load(std::memory_order_acquire) == 2)) {
        if (request->operation == AZ_CMD_HOT_CLEAR && !property.copy_to(pending.retained)) return fail(AZ_FAILED);
        pending.expected = expected;
        int rc = azsdk_exit_cue_stage_submit(request);
        if (rc == AZ_PENDING) {
            pending.staged_cue = {};
            pending.bridge_generation.store(request->identity.load_generation, std::memory_order_relaxed);
            pending.bridge_revision.store(0, std::memory_order_relaxed);
            pending.stage_token.store(request->identity.token, std::memory_order_release);
            pending.stage.store(1, std::memory_order_release);
            pending.bridge.store(request->identity.token, std::memory_order_release);
            if (pending.bridge_ready.load(std::memory_order_acquire) == request->identity.token)
                azsdk_cue_commands_notify(deck);
        }
        return rc;
    }
    if (request->operation == AZ_CMD_SAVED_SAVE && !(expected.valid & AZ_CUE_END)) return fail(AZ_INVALID);
    if (recall(request->operation)) {
        int rc = azsdk_cue_recall_prepare(request, manager, &property.native);
        if (rc == AZ_PENDING) {
            /* DSP serials are a separate namespace. The selected, source-qualified
             * copied property belongs to this exact store snapshot revision. */
            pending.bridge_generation.store(request->identity.load_generation, std::memory_order_relaxed);
            pending.bridge_revision.store(snapshot.revision(), std::memory_order_relaxed);
            pending.bridge.store(request->identity.token, std::memory_order_release);
            if (pending.bridge_ready.load(std::memory_order_acquire) == request->identity.token)
                azsdk_cue_commands_notify(deck);
        }
        return rc;
    }
    /* Native persistence serializes microseconds, then reconstructs samples.
     * Its exact owned round trip defines the observable saved position; the
     * original property still goes unchanged to the real native producer. */
    azcuestore::Property canonical;
    if (!snapshot.canonical_property(kind, property, canonical)) return fail(AZ_UNSUPPORTED);
    COMMAND_DIAG(*request, 12, "canonical-property");
    expected.valid &= ~(AZ_CUE_START | AZ_CUE_END);
    expected.start_s = expected.end_s = 0;
    if (!property_scalar(canonical.native, expected)) return fail(AZ_UNSUPPORTED);
    if (request->operation == AZ_CMD_SAVED_SAVE && !(expected.valid & AZ_CUE_END)) return fail(AZ_UNSUPPORTED);
    azcuepersist_witness witness = {};
    const azcuepersist_witness *selected_witness = nullptr;
    if (request->operation == AZ_CMD_SAVED_AUTO || request->operation == AZ_CMD_CUE_COMMENT || request->operation == AZ_CMD_CUE_COLOR || request->operation == AZ_CMD_HOT_SET) {
        uint64_t revision = request->operation == AZ_CMD_HOT_SET ? snapshot.revision() : native_request->expected_revision;
        if (!snapshot.content_witness(revision, &witness)) {
            COMMAND_DIAG(*request, 5, "snapshot-witness");
            // HOT_SET's own asynchronous store can publish between the DSP
            // capture and this witness. No repository request exists yet;
            // refresh the view without repeating the setter or its deadline.
            if (awaiting_hot_view) return resume_hot_completion();
            return fail(AZ_STALE);
        }
        if (request->operation != AZ_CMD_HOT_SET) witness.selected_cue = reinterpret_cast<uintptr_t>(&selected_stored.native);
        selected_witness = &witness;
        COMMAND_DIAG(*request, 13, "content-witness");
    }
    uint64_t cookie = s.serial.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!cookie || cookie >= INTERNAL) return fail(AZ_UNSUPPORTED);
    pending.expected = expected;
    pending.active_request = *native_request;
    pending.expected_rate = read<uint32_t>(&canonical.native, 0x60);
    pending.before_witness = witness;
    pending.before_revision = snapshot.revision();
    pending.token.store(request->identity.token, std::memory_order_relaxed);
    pending.manager.store(uintptr_t(manager), std::memory_order_relaxed);
    pending.cookie.store(cookie, std::memory_order_release);
    uintptr_t maintainer = read<uintptr_t>(manager, 0x140);
    COMMAND_DIAG(*request, 14, "persist-submit");
    int rc = azsdk_cue_persist_submit(&pending.active_request, manager, reinterpret_cast<void *>(maintainer), kind, &property.native, cookie, selected_witness);
    if (rc != AZ_PENDING) {
        pending.cookie.store(0, std::memory_order_release);
        // Busy or stale preflight produced no repository request. A native
        // automatic store can invalidate the captured content witness between
        // preparation and admission. Recheck the load, captured property and
        // fresh witness on the next owned frame, keeping this DSP stage and
        // the original deadline rather than repeating the setter.
        if (hot(request->operation) && (rc == AZ_BUSY || (request->operation == AZ_CMD_HOT_SET && rc == AZ_STALE))) return resume_hot_completion();
        return fail(rc);
    }
    if (pending.automatic.active && pending.automatic.token == request->identity.token) ++pending.automatic.steps;
    return rc;
}

int valid(const azexit_cue_request *r) {
    if (!r || r->size < sizeof *r || r->identity.deck >= 4 || !r->identity.token ||
        (r->identity.token & INTERNAL) || !r->identity.load_generation || !r->identity.deadline_ns || r->identity.reserved || r->operation >= 64 ||
        !std::memchr(r->comment, 0, sizeof r->comment) || !std::isfinite(r->position_s) || r->end_s != 0 || r->beats != 0) return AZ_INVALID;
    if (!(azsdk_cue_commands_ready(r->identity.deck) & bit(r->operation))) return AZ_UNSUPPORTED;
    const bool selected = selector(r->operation);
    uint32_t flags = AZ_COMMAND_NATIVE_LOAD | (recall(r->operation) ? 0 : AZ_COMMAND_PERSIST) |
                     (selected ? AZ_COMMAND_NATIVE_CUE_IDS : 0);
    if (r->flags != flags || (selected ? (!r->expected_revision || !r->entry_id || r->entry_id > AZ_NATIVE_CUE_LIMIT) : r->expected_revision != 0)) return AZ_INVALID;
    if (hot(r->operation)) {
        if (r->classification != AZ_CUE_HOT || r->entry_id < 1 || r->entry_id > 8) return AZ_INVALID;
    } else if ((r->classification != AZ_CUE_MEMORY &&
                !((r->operation == AZ_CMD_CUE_COMMENT || r->operation == AZ_CMD_CUE_COLOR) && r->classification == AZ_CUE_HOT)) ||
               (!selected && r->entry_id) || (r->classification == AZ_CUE_HOT && r->entry_id > 8)) return AZ_INVALID;
    if (r->operation == AZ_CMD_SAVED_AUTO) {
        if (r->position_s != 0 && r->position_s != 1) return AZ_INVALID;
    } else if (r->position_s != 0) return AZ_INVALID;
    if (r->operation != AZ_CMD_CUE_COMMENT && r->comment[0]) return AZ_INVALID;
    if (r->operation == AZ_CMD_HOT_SET || r->operation == AZ_CMD_CUE_COLOR) {
        if (r->rgb > 0xffffff) return AZ_INVALID;
        if (r->operation == AZ_CMD_CUE_COLOR && r->classification == AZ_CUE_MEMORY) {
            switch (r->rgb) {
            case 0x000000:
            case 0xff00c6:
            case 0xff0000:
            case 0xff7100:
            case 0xffff00:
            case 0x00e300:
            case 0x00c3ff:
            case 0x0051ff:
            case 0x9c08ff: break;
            default: return AZ_UNSUPPORTED;
            }
        }
    } else if (r->rgb) return AZ_INVALID;
    return current(r->identity.deck, r->identity.load_generation, nullptr) ? AZ_OK : AZ_STALE;
}
} // namespace
extern "C" int azsdk_cue_commands_configure(int on, int build, azexit_install_fn install) noexcept {
    State &s = state();
    if (!on) {
        s.enabled.store(false, std::memory_order_release);
        return AZ_OK;
    }
    if (s.enabled.load(std::memory_order_acquire)) return AZ_BUSY;
    azcue_profile profile = {};
    if (!install || !azsdk_cue_main_profile(build, &profile)) return AZ_UNSUPPORTED;
    s.build = build;
    s.bias = profile.actor.memory_producer - (build == 1 ? 0x81db60 : 0x863460);
    s.store = azcuestore::native_profile(build, s.bias);
    azcue_actor_callbacks callbacks = {execute, current, nullptr};
    int rc = azsdk_cue_actor_configure(&profile.actor, install, &callbacks);
    if (rc != AZ_OK) return rc;
    azcuepersist_callbacks persistence = {validate_persist, confirm_persist, ready, nullptr};
    if (!azsdk_cue_persist_configure(&profile.persist, install, &persistence)) return AZ_UNSUPPORTED;
    rc = azsdk_cue_recall_configure(build, s.bias);
    if (rc != AZ_OK) return rc;
    if (!azsdk_cue_lifecycle_configure(build, install)) return AZ_UNSUPPORTED;
    s.capabilities.store(azsdk_cue_persist_capabilities() | bit(AZ_CMD_SAVED_RECALL) | bit(AZ_CMD_MEMORY_RECALL), std::memory_order_release);
    s.enabled.store(true, std::memory_order_release);
    return AZ_OK;
}
extern "C" uint64_t azsdk_cue_commands_ready(unsigned deck) noexcept {
    if (deck >= 4 || !state().enabled.load(std::memory_order_acquire)) return 0;
    uint64_t mask = state().capabilities.load(std::memory_order_acquire);
#ifndef AZSDK_CUE_COMMANDS_TESTING
    unsigned bit_deck = 1u << deck;
    if (!(azsdk_cue_actor_mask() & bit_deck)) return 0;
    if (!(azsdk_cue_capabilities() & bit_deck))
        mask &= bit(AZ_CMD_SAVED_RECALL) | bit(AZ_CMD_MEMORY_RECALL);
#endif
    return mask;
}
extern "C" uint64_t azsdk_cue_commands_commands(void) noexcept {
    uint64_t mask = 0;
    for (unsigned deck = 0; deck < 4; ++deck) mask |= azsdk_cue_commands_ready(deck);
    return mask;
}
extern "C" int azsdk_cue_commands_native_cues(unsigned deck, az_native_cue_snapshot *out) noexcept {
    if (deck >= 4 || !out || out->size < sizeof *out) return AZ_INVALID;
    Cache &cache = state().decks[deck];
    std::unique_lock<std::mutex> lock(cache.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        cache.waiter.store(true, std::memory_order_release);
        if (!lock.try_lock()) return AZ_BUSY;
    }
    int rc = AZ_STALE;
    if (cache.value.revision && current(deck, cache.value.native_load_generation, nullptr)) {
        uint32_t size = out->size;
        *out = cache.value;
        out->size = size;
        rc = AZ_OK;
    }
    lock.unlock();
    if (cache.waiter.exchange(false, std::memory_order_acq_rel)) azsdk_exit_delivery_notify(deck);
    return rc;
}
extern "C" int azsdk_cue_commands_target_cue(unsigned deck, azexit_cue_target *out) noexcept {
    if (deck >= 4 || !out) return AZ_INVALID;
    azexit_cue_target target = {};
    target.persistent_commands = azsdk_cue_commands_commands() & ~(bit(AZ_CMD_SAVED_RECALL) | bit(AZ_CMD_MEMORY_RECALL));
    az_native_cue_snapshot snapshot = {};
    snapshot.size = sizeof snapshot;
    int rc = azsdk_cue_commands_native_cues(deck, &snapshot);
    if (rc == AZ_BUSY) return AZ_BUSY;
    if (rc == AZ_OK) {
        target.revision = snapshot.revision;
        target.revision_valid = 1;
        for (unsigned operation = 1; operation < 64; ++operation)
            if (selector(operation)) target.revision_commands |= azsdk_cue_commands_commands() & bit(operation);
    }
    azsdk_load_sample load = {};
    if (!azsdk_load_read(int(deck), &load)) return AZ_STALE;
    *out = target;
    return AZ_OK;
}
extern "C" int azsdk_cue_commands_submit(const azexit_cue_request *request) noexcept {
    if (request && request->size >= sizeof *request) COMMAND_DIAG(*request, 10, "public-entry");
    int rc = valid(request);
    if (rc != AZ_OK) return rc;
    COMMAND_DIAG(*request, 7, "public-submit");
    Cache &cache = state().decks[request->identity.deck];
    unsigned empty = 0;
    if (!cache.waiting.compare_exchange_strong(empty, 1, std::memory_order_acq_rel)) return AZ_BUSY;
    cache.waiting_request = *request;
    // All decks share the actor's owner lock. Retire queued internal reads on
    // every deck; their native tasks keep tombstones until destruction. A running
    // owner frame stays protected, and contention uses the copied queue below.
    retire_internal_reads();
    rc = azsdk_cue_actor_enqueue(&cache.waiting_request);
    COMMAND_DIAG(*request, rc == AZ_BUSY ? 8 : 9, rc == AZ_BUSY ? "public-queued" : "actor-enqueued");
    cache.waiting.store(rc == AZ_PENDING ? 5 : rc == AZ_BUSY ? 2
                                                             : 0,
                        std::memory_order_release);
    if (rc == AZ_PENDING || rc == AZ_BUSY) azsdk_cue_commands_notify(request->identity.deck);
    return rc == AZ_BUSY ? AZ_PENDING : rc;
}
extern "C" int azsdk_cue_commands_cancel(unsigned deck, uint64_t token) noexcept {
    if (deck >= 4 || !token) return AZ_INVALID;
    Cache &cache = state().decks[deck];
    unsigned queued = 2;
    if (cache.waiting.compare_exchange_strong(queued, 1, std::memory_order_acq_rel)) {
        if (cache.waiting_request.identity.token != token) {
            cache.waiting.store(2, std::memory_order_release);
            return AZ_STALE;
        }
        cache.waiting_result = {token, cache.waiting_request.identity.load_generation, 0, AZ_COMMAND_CANCELLED, AZ_CANCELLED};
        cache.waiting.store(3, std::memory_order_release);
        publish_waiting_result(deck, cache.waiting_result);
        return AZ_OK;
    }
    if (queued && queued != 5) return AZ_PENDING;
    Pending &pending = state().pending[deck];
    if (pending.bridge.load(std::memory_order_acquire) == token) {
        int rc = azsdk_exit_recall_cancel(deck, token);
        if (rc != AZ_OK) return rc;
    }
    if ((pending.cookie.load(std::memory_order_acquire) && pending.token.load(std::memory_order_acquire) == token) ||
        (pending.stage_token.load(std::memory_order_acquire) == token && pending.stage.load(std::memory_order_acquire) == 2)) {
        if ((pending.persist_ready.load(std::memory_order_acquire) == token ||
             !pending.cookie.load(std::memory_order_acquire)) &&
            pending.stop_resumed.exchange(token, std::memory_order_acq_rel) != token) {
            pending.resume.store(token, std::memory_order_release);
            azsdk_cue_commands_notify(deck);
        }
        return AZ_PENDING;
    }
    int rc = azsdk_cue_actor_cancel(deck, token);
    if (rc == AZ_OK && pending.stage_token.load(std::memory_order_acquire) == token) pending.stage.store(0, std::memory_order_release);
    return rc;
}
extern "C" int azsdk_cue_commands_poll(unsigned deck, azexit_result *result) noexcept {
    if (deck >= 4 || !result) return AZ_INVALID;
    Cache &cache = state().decks[deck];
    unsigned ready = 3;
    if (cache.waiting.compare_exchange_strong(ready, 1, std::memory_order_acq_rel)) {
        *result = cache.waiting_result;
        cache.waiting.store(3, std::memory_order_release);
        return AZ_OK;
    }
    if ((ready && ready != 5) || cache.refresh.load(std::memory_order_acquire)) return AZ_PENDING;
    return azsdk_cue_actor_take(deck, result);
}
extern "C" int azsdk_cue_commands_release(unsigned deck, uint64_t token) noexcept {
    if (deck >= 4 || !token) return AZ_INVALID;
    Cache &cache = state().decks[deck];
    unsigned ready = 3;
    if (cache.waiting.compare_exchange_strong(ready, 1, std::memory_order_acq_rel)) {
        if (cache.waiting_result.token != token) {
            cache.waiting.store(3, std::memory_order_release);
            return AZ_STALE;
        }
        cache.waiting.store(0, std::memory_order_release);
        return AZ_OK;
    }
    if (ready && ready != 5) return AZ_BUSY;
    int rc = azsdk_cue_actor_release(deck, token);
    if (rc == AZ_OK && ready == 5) cache.waiting.store(0, std::memory_order_release);
    if (rc == AZ_OK && deck < 4 && state().pending[deck].stage_token.load(std::memory_order_acquire) == token)
        state().pending[deck].stage.store(0, std::memory_order_release);
    return rc;
}
extern "C" void azsdk_cue_commands_tick(void) noexcept {
    State &s = state();
    if (!s.enabled.load(std::memory_order_acquire)) return;
    uint64_t clock = now();
    if (!clock) return;
    if (unfinished_public_work()) retire_internal_reads();
    unsigned collections = s.collections.exchange(0, std::memory_order_acq_rel);
    for (unsigned deck = 0; deck < 4; ++deck) {
        Pending &pending = s.pending[deck];
        uint64_t bridge = pending.bridge.load(std::memory_order_acquire);
        if (bridge
#ifndef AZSDK_CUE_COMMANDS_TESTING
            && pending.bridge_ready.load(std::memory_order_acquire) == bridge
#endif
        ) {
            azexit_result result = {};
            if (azsdk_exit_recall_take(deck, &result) == AZ_OK && result.token == bridge) {
                uint64_t generation = pending.bridge_generation.load(std::memory_order_acquire);
                if (result.load_generation != generation || !current(deck, generation, nullptr)) {
                    result.load_generation = generation;
                    result.revision = 0;
                    result.outcome = AZ_COMMAND_UNCERTAIN;
                    result.reason = AZ_STALE;
                } else if (result.outcome == AZ_COMMAND_APPLIED) {
                    uint64_t revision = pending.bridge_revision.load(std::memory_order_acquire);
                    if (revision) result.revision = revision;
                    if (pending.stage.load(std::memory_order_acquire) == 1 &&
                        azsdk_exit_cue_stage_capture(deck, bridge, &pending.staged_cue) != AZ_OK) {
                        result.revision = 0;
                        result.outcome = AZ_COMMAND_UNCERTAIN;
                        result.reason = AZ_FAILED;
                    }
                }
                if (azsdk_exit_recall_release(deck, bridge) != AZ_OK) continue;
                azsdk_cue_recall_release(deck, bridge);
                pending.bridge_ready.store(0, std::memory_order_release);
                pending.bridge.store(0, std::memory_order_release);
                if (pending.stage.load(std::memory_order_acquire) == 1 && result.outcome == AZ_COMMAND_APPLIED) {
                    pending.stage.store(2, std::memory_order_release);
                    pending.resume.store(bridge, std::memory_order_release);
                    azsdk_cue_commands_notify(deck);
                } else {
                    pending.stage.store(0, std::memory_order_release);
                    pending.actor_completion = result;
                }
            }
        }
        if (pending.actor_completion.token &&
            azsdk_cue_actor_complete(deck, pending.actor_completion.token, &pending.actor_completion) != AZ_BUSY)
            pending.actor_completion = {};
        uint64_t retired = pending.actor_retired.load(std::memory_order_acquire);
        if (retired) {
            if (pending.persist_retired.load(std::memory_order_acquire) == retired) {
                pending.persist_ready.store(0, std::memory_order_release);
                pending.cookie.store(0, std::memory_order_release);
                pending.stage.store(0, std::memory_order_release);
            }
            if (!pending.cookie.load(std::memory_order_acquire) &&
                !pending.bridge.load(std::memory_order_acquire) &&
                pending.stage.load(std::memory_order_acquire) != 2) {
                azexit_receipt value = {
                    sizeof value, AZEXIT_DELIVERY_VERSION, AZEXIT_RECEIPT_SETTLED, 1, deck,
                    retired, pending.retired_load.load(std::memory_order_acquire), 0, 0, now(), 0, 0};
                pending.actor_retired.store(0, std::memory_order_release);
                (void) azsdk_exit_delivery_publish(&value);
            }
        }
        uint64_t notice = pending.resume.exchange(0, std::memory_order_acq_rel);
        if (!notice && (collections & (1u << deck))) {
            notice = pending.persist_ready.load(std::memory_order_acquire);
            if (!notice && pending.stage.load(std::memory_order_acquire) == 2)
                notice = pending.stage_token.load(std::memory_order_acquire);
        }
        if (notice && azsdk_cue_actor_resume(deck, notice) == AZ_BUSY) {
            uint64_t empty = 0;
            pending.resume.compare_exchange_strong(empty, notice, std::memory_order_acq_rel);
        }
        Cache &cache = s.decks[deck];
        uint64_t token = cache.refresh.load(std::memory_order_acquire);
        if (token
#ifndef AZSDK_CUE_COMMANDS_TESTING
            && cache.actor_finished.load(std::memory_order_acquire) == token
#endif
        ) {
            azexit_result result = {};
            if (azsdk_cue_actor_take(deck, &result) == AZ_OK && result.token == token) {
                if (azsdk_cue_actor_release(deck, token) == AZ_OK) cache.refresh.store(0, std::memory_order_release);
            }
        }
        bool posted_public = false;
        if (!cache.refresh.load(std::memory_order_acquire)) {
            unsigned queued = 2;
            if (cache.waiting.compare_exchange_strong(queued, 4, std::memory_order_acq_rel)) {
                const azexit_cue_request &request = cache.waiting_request;
                int rc = compare_timestamps(clock, request.identity.deadline_ns, 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL ? AZ_STALE : valid(&request);
                if (rc == AZ_OK) rc = azsdk_cue_actor_enqueue(&request);
                if (rc == AZ_PENDING) {
                    COMMAND_DIAG(request, 9, "actor-enqueued");
                    cache.waiting.store(5, std::memory_order_release);
                    posted_public = true;
                } else if (rc == AZ_BUSY) cache.waiting.store(2, std::memory_order_release);
                else {
                    cache.waiting_result = {request.identity.token, request.identity.load_generation, 0, AZ_COMMAND_REJECTED, rc};
                    cache.waiting.store(3, std::memory_order_release);
                    publish_waiting_result(deck, cache.waiting_result);
                }
            }
        }
        unsigned waiting = cache.waiting.load(std::memory_order_acquire);
        if (!posted_public && (!waiting || waiting == 3) && !cache.refresh.load(std::memory_order_acquire) && !unfinished_public_work()) {
            azsdk_load_sample load = {};
            bool refresh = false;
            if (azsdk_load_read(int(deck), &load)) {
                {
                    std::lock_guard<std::mutex> lock(cache.mutex);
                    refresh = cache.value.native_load_generation != load.load_generation || compare_timestamps(clock, cache.observed, 1000000000, 10, 1) != AZ_TIME_EQUAL;
                }
                if (refresh) {
                    uint64_t serial = s.serial.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (serial >= INTERNAL) {
                        s.enabled.store(false, std::memory_order_release);
                        return;
                    }
                    azexit_cue_request request = {};
                    request.size = sizeof request;
                    request.identity.deck = deck;
                    request.identity.token = INTERNAL | serial;
                    request.identity.load_generation = load.load_generation;
                    request.identity.deadline_ns = clock + UINT64_C(1000000000);
                    if (azsdk_cue_actor_enqueue(&request) == AZ_PENDING) {
                        cache.refresh.store(request.identity.token, std::memory_order_release);
                        // Cover a public admission that raced this publication.
                        if (unfinished_public_work()) retire_internal_reads();
                    }
                }
            }
        }
        azsdk_cue_actor_drive(deck);
    }
}

extern "C" void azsdk_cue_commands_notify(unsigned deck) noexcept {
    if (deck >= 4) return;
    state().notifications.fetch_or(1u << deck, std::memory_order_release);
    azsdk_exit_delivery_notify(deck);
}
extern "C" void azsdk_cue_commands_actor_finished(unsigned deck, uint64_t token) noexcept {
    if (deck >= 4) return;
    state().decks[deck].actor_finished.store(token, std::memory_order_release);
    azsdk_cue_commands_notify(deck);
}
extern "C" void azsdk_cue_commands_actor_retired(unsigned deck, uint64_t token, uint64_t load) noexcept {
    if (deck >= 4) return;
    Pending &pending = state().pending[deck];
    pending.retained.reset(); // manager's retirement still holds the actor lease
    pending.retired_load.store(load, std::memory_order_relaxed);
    pending.actor_retired.store(token, std::memory_order_release);
    azsdk_cue_commands_notify(deck);
}
extern "C" void azsdk_cue_commands_persist_retired(unsigned deck, uint64_t token) noexcept {
    if (deck >= 4) return;
    state().pending[deck].persist_retired.store(token, std::memory_order_release);
    azsdk_cue_commands_notify(deck);
}
extern "C" void azsdk_cue_commands_recall_finished(unsigned deck, uint64_t token) noexcept {
    if (deck >= 4) return;
    state().pending[deck].bridge_ready.store(token, std::memory_order_release);
    azsdk_cue_commands_notify(deck);
}
extern "C" void azsdk_cue_commands_collection_changed(unsigned deck) noexcept {
    if (deck >= 4) return;
    state().collections.fetch_or(1u << deck, std::memory_order_release);
    azsdk_cue_commands_notify(deck);
}
extern "C" void azsdk_cue_commands_drive(void) noexcept {
    if (!state().notifications.exchange(0, std::memory_order_acq_rel)) return;
    azsdk_cue_commands_tick();
}

#ifdef AZSDK_CUE_COMMANDS_TESTING
extern "C" void azsdk_cue_commands_test_time(uint64_t value) noexcept { test_clock.store(value, std::memory_order_relaxed); }
extern "C" void azsdk_cue_commands_test_cache_lock(unsigned deck, void (*held)(void *), void *context) {
    std::lock_guard<std::mutex> lock(state().decks[deck].mutex);
    held(context);
}
#endif
