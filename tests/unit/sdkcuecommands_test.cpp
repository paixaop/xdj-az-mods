/* SPDX-License-Identifier: GPL-3.0-only */
/* Orchestration fixture: native receipts, store revisions and DSP settlement are
 * independently controlled. No firmware is executed. */
#include "../../src/common/azcuecommands.h"
#include "../../src/common/azcueactor.h"
#include "../../src/common/azcuepersist.h"
#include "../../src/common/azcueprofile.h"
#include "../../src/common/azcuerecall.h"
#include "../../src/common/azcuerecallbridge.h"
#include "../../src/common/azcuestore.h"
#include "../../src/common/aztimestamp.h"
#include "../../src/common/azloadobserver.h"
#include "../../src/azmod/azmod-mod.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <cstdio>
#include <atomic>
#include <thread>
extern "C" void azsdk_cue_commands_test_cache_lock(unsigned, void (*)(void *), void *);
struct Row {
    azcuestore::NativeCue cue = {};
    azcuestore::NativeProperty property = {};
};
static std::vector<Row> rows;
struct WitnessRows {
    std::vector<Row> rows;
    size_t memory_count;
};
static std::map<uint64_t, WitnessRows> witnessed_rows;
static uint64_t generation = 42, revision = 1;
static bool main_valid = true;
static int stored_memory_count = -1, fail_property = -1;
static unsigned property_calls = 0, canonical_calls = 0, witness_calls = 0;
static bool fail_witness = false;
static azcuepersist_witness submitted_witness = {};
static azcuestore::NativeProperty main_property;
static uintptr_t manager[64];
static azcue_actor_callbacks actor;
static azcuepersist_callbacks persist;
static azexit_cue_request mailbox = {}, active_persist = {};
static azexit_result terminal = {}, bridge_result = {};
static unsigned phase, persist_calls, stages, recalls, native_kind;
static unsigned resume_busy;
static unsigned persist_busy;
static unsigned persist_stale;
static unsigned enqueue_busy;
static bool hold_background_query;
static unsigned loaded_decks = 1, background_attempts[4];
static uint64_t cancelled_background_query;
static unsigned background_cancel_busy, background_release_busy;
static uint64_t cookie, submitted_sample;
static bool receipt, bridge_done;
static azexit_cue_request stage_request = {};
static bool override_hot_view = false, missing_hot_view = false, override_stage_sample = false;
static bool fail_snapshot = false, fail_stage_capture = false;
static azcuestore::NativeProperty hot_view = {};
static azexit_cue_stage_sample stage_sample = {};
static bool publication_matches = true, force_native_failure = false;
static int publication_mode = 1;
static bool inject_foreign_after_confirm = false, expire_after_confirm = false;
static std::vector<azexit_cue_request> submissions;
static std::vector<uint64_t> submitted_cookies;
template <class T>
static void put(void *p, unsigned offset, T value) { std::memcpy(static_cast<char *>(p) + offset, &value, sizeof value); }
static void property(azcuestore::NativeProperty &p, uint64_t sample, bool loop) {
    p = {};
    for (unsigned offset : {0x30u, 0x88u}) {
        p.bytes[offset] = 1;
        p.bytes[offset + 4] = 1;
        p.bytes[offset + 6] = 1;
        put<int32_t>(&p, offset + 8, 1);
        put<uintptr_t>(&p, offset + 0x20, 0x10000);
        put<uint32_t>(&p, offset + 0x30, 48000);
    }
    put<uint64_t>(&p, 0x40, sample);
    put<uint64_t>(&p, 0x68, sample * 1000000 / 48000);
    put<uint64_t>(&p, 0x98, loop ? sample + 48000 : 0);
    put<uint64_t>(&p, 0xc0, loop ? (sample + 48000) * 1000000 / 48000 : UINT64_C(0x7fffffffffffffff));
}
static Row row(unsigned kind, uint64_t sample, bool loop, const char *comment = "", unsigned rgb = 0) {
    Row r;
    r.cue.bytes[0] = static_cast<unsigned char>(kind);
    put<uintptr_t>(&r.cue, 0x70, uintptr_t(comment));
    put<uint32_t>(&r.cue, 0x78, (kind || rgb ? 0xff000000u : 0u) | rgb);
    property(r.property, sample, loop);
    return r;
}
static uint64_t all_persist() {
    uint64_t mask = 0;
    for (unsigned operation : {AZ_CMD_SAVED_SAVE, AZ_CMD_SAVED_DELETE, AZ_CMD_SAVED_AUTO, AZ_CMD_HOT_SET, AZ_CMD_HOT_CLEAR,
                               AZ_CMD_MEMORY_SET, AZ_CMD_MEMORY_DELETE, AZ_CMD_CUE_COMMENT, AZ_CMD_CUE_COLOR}) mask |= UINT64_C(1) << operation;
    return mask;
}
namespace azcuestore {
Profile native_profile(int, uintptr_t) noexcept { return {}; }
void OwnedCue::reset() noexcept {
    if (destroy_) destroy_(&native);
    destroy_ = nullptr;
    native = {};
}
void Property::reset() noexcept {
    if (copy_) copy_ = nullptr;
    native = {};
    destroy_ = nullptr;
}
static void dummy_destroy(void *) {}
bool Property::copy_to(Property &out) const noexcept {
    if (!valid()) return false;
    out.native = native;
    out.destroy_ = dummy_destroy;
    return true;
}
void Snapshot::reset() noexcept {
    player_ = source_ = where_from_ = memory_set_ = hot_set_ = content_ = hot_content_ = pack_ = 0;
    count_ = memory_count_ = 0;
    revision_ = fingerprint_ = 0;
}
bool Snapshot::open(const Profile &, void *, uint64_t load, const unsigned char *, RevisionState &s) noexcept {
    if (load != generation || fail_snapshot) return false;
    count_ = rows.size();
    memory_count_ = 0;
    while (memory_count_ < rows.size() && rows[memory_count_].cue.bytes[0] == 0) ++memory_count_;
    if (stored_memory_count >= 0) memory_count_ = size_t(stored_memory_count);
    revision_ = ::revision;
    fingerprint_ = revision_ * 1931 + rows.size();
    s.revision = ::revision;
    return true;
}
bool Snapshot::row(size_t i, OwnedCue &out) const noexcept {
    if (i >= rows.size()) return false;
    out.native = rows[i].cue;
    return true;
}
bool Snapshot::property(uint32_t id, uint64_t rev, Property &out) const noexcept {
    ++property_calls;
    if (!id || id > rows.size() || rev != ::revision || int(id) == fail_property) return false;
    out.native = rows[id - 1].property;
    out.destroy_ = dummy_destroy;
    return true;
}
bool Snapshot::content_witness(uint64_t expected, azcuepersist_witness *out) const noexcept {
    ++witness_calls;
    if (!out || expected != revision_ || fail_witness) return false;
    *out = {};
    out->source_info = 0x50000;
    out->memory_set = 0x60000;
    out->hot_set = 0x70000;
    out->memory_content = 0x80000;
    out->hot_content = 0x90000;
    out->fingerprint = fingerprint_;
    out->revision = revision_;
    out->source_id[0] = 1;
    witnessed_rows[out->fingerprint] = {rows, memory_count_};
    return true;
}
bool Snapshot::auto_only_changed(const azcuepersist_witness &before, uint32_t entry, bool desired, uint32_t source_rate) const noexcept {
    assert(source_rate);
    auto found = witnessed_rows.find(before.fingerprint);
    if (found == witnessed_rows.end() || found->second.rows.size() != rows.size() || found->second.memory_count != memory_count_ ||
        !entry || entry > memory_count_) return false;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row &a = found->second.rows[i], &b = rows[i];
        if (a.cue.bytes[0] != b.cue.bytes[0] || std::memcmp(a.cue.bytes + 4, b.cue.bytes + 4, 0x24) ||
            std::memcmp(a.cue.bytes + 0x2a, b.cue.bytes + 0x2a, 4) || std::memcmp(a.cue.bytes + 0x30, b.cue.bytes + 0x30, 4) || std::memcmp(a.cue.bytes + 0x50, b.cue.bytes + 0x50, 4) || std::memcmp(a.cue.bytes + 0x48, b.cue.bytes + 0x48, 4) ||
            std::memcmp(a.cue.bytes + 0x68, b.cue.bytes + 0x68, 4) || std::memcmp(a.cue.bytes + 0x78, b.cue.bytes + 0x78, 5)) return false;
        if (i == entry - 1) {
            if (b.cue.bytes[0] != 0 || b.cue.bytes[0x28] != unsigned(desired)) return false;
        } else if (a.cue.bytes[0x28] != b.cue.bytes[0x28]) return false;
        uintptr_t text_a = 0, text_b = 0;
        std::memcpy(&text_a, a.cue.bytes + 0x70, 8);
        std::memcpy(&text_b, b.cue.bytes + 0x70, 8);
        if (std::strcmp(text_a ? reinterpret_cast<const char *>(text_a) : "", text_b ? reinterpret_cast<const char *>(text_b) : "")) return false;
        for (unsigned base : {0x30u, 0x88u})
            if (std::memcmp(a.property.bytes + base, b.property.bytes + base, 12) ||
                std::memcmp(a.property.bytes + base + 0x10, b.property.bytes + base + 0x10, 8) ||
                std::memcmp(a.property.bytes + base + 0x30, b.property.bytes + base + 0x30, 4) ||
                std::memcmp(a.property.bytes + base + 0x38, b.property.bytes + base + 0x38, 8)) return false;
    }
    return true;
}
bool Snapshot::metadata_only_changed(const azcuepersist_witness &before, unsigned kind, uint32_t entry,
                                     const char *comment, uint32_t rgb, uint32_t source_rate) const noexcept {
    auto found = witnessed_rows.find(before.fingerprint);
    if (!source_rate || found == witnessed_rows.end() || found->second.rows.size() != rows.size() ||
        found->second.memory_count != memory_count_ || !entry || (kind ? kind != entry : entry > memory_count_)) return false;
    unsigned matches = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row &a = found->second.rows[i], &b = rows[i];
        bool selected = kind ? i >= memory_count_ && a.cue.bytes[0] == kind : i + 1 == entry;
        if (selected) ++matches;
        if (a.cue.bytes[0] != b.cue.bytes[0] || a.cue.bytes[0x28] != b.cue.bytes[0x28]) return false;
        uintptr_t old_text = 0, new_text = 0;
        uint32_t old_color = 0, new_color = 0;
        std::memcpy(&old_text, a.cue.bytes + 0x70, 8); std::memcpy(&new_text, b.cue.bytes + 0x70, 8);
        std::memcpy(&old_color, a.cue.bytes + 0x78, 4); std::memcpy(&new_color, b.cue.bytes + 0x78, 4);
        const char *wanted = selected && comment ? comment : old_text ? reinterpret_cast<const char *>(old_text) : "";
        if (std::strcmp(wanted, new_text ? reinterpret_cast<const char *>(new_text) : "") ||
            (selected && !comment ? new_color != (0xff000000u | rgb) && (kind || rgb || new_color) : new_color != old_color)) return false;
        auto times = [](const azcuestore::NativeProperty &p) {
            azcue_native_timestamps t;
            std::memcpy(&t.sample_rate, p.bytes + 0x60, 4);
            std::memcpy(&t.in_sample, p.bytes + 0x40, 8); std::memcpy(&t.in_us, p.bytes + 0x68, 8);
            std::memcpy(&t.out_sample, p.bytes + 0x98, 8); std::memcpy(&t.out_us, p.bytes + 0xc0, 8);
            return t;
        };
        if (!azcue_timestamps_equal(times(a.property), times(b.property), source_rate) ||
            std::memcmp(a.property.bytes + 0x30, b.property.bytes + 0x30, 12) ||
            std::memcmp(a.property.bytes + 0x88, b.property.bytes + 0x88, 12)) return false;
    }
    return matches == 1;
}
bool Snapshot::canonical_property(unsigned, const Property &original, Property &out) const noexcept {
    ++canonical_calls;
    if (!original.copy_to(out)) return false;
    for (unsigned offset : {0x30u, 0x88u}) {
        uint32_t rate;
        uint64_t usec;
        std::memcpy(&rate, original.native.bytes + offset + 0x30, 4);
        std::memcpy(&usec, original.native.bytes + offset + 0x38, 8);
        if (usec == UINT64_C(0x7fffffffffffffff)) continue;
        // Independent fixture for the native 2.00 integer-usec serialization.
        uint64_t sample = uint64_t(double(rate * usec) * 0.000001);
        put<uint64_t>(&out.native, offset + 0x10, sample);
    }
    return true;
}
bool Snapshot::current_property(unsigned kind, Property &out) const noexcept {
    if (kind && override_hot_view) {
        if (missing_hot_view) return false;
        out.native = hot_view;
        out.destroy_ = dummy_destroy;
        return true;
    }
    if (!kind) {
        if (!main_valid) return false;
        out.native = main_property;
        out.destroy_ = dummy_destroy;
        return true;
    }
    for (const Row &r : rows)
        if (r.cue.bytes[0] == kind) {
            out.native = r.property;
            out.destroy_ = dummy_destroy;
            return true;
        }
    return false;
}
} // namespace azcuestore
extern "C" int azsdk_load_read(int deck, azsdk_load_sample *out) noexcept {
    if (deck < 0 || deck >= 4 || !(loaded_decks & (1u << deck)) || !generation) return 0;
    *out = {};
    out->load_generation = generation;
    return 1;
}
extern "C" int azsdk_cue_main_profile(int, azcue_profile *p) noexcept {
    *p = {};
    p->actor.memory_producer = 0x81db60;
    return 1;
}
extern "C" int azsdk_cue_lifecycle_configure(int, azexit_install_fn) noexcept { return 1; }
extern "C" int azsdk_cue_actor_configure(const azcue_actor_profile *, azexit_install_fn, const azcue_actor_callbacks *c) noexcept {
    actor = *c;
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_enqueue(const azexit_cue_request *r) noexcept {
    if (!r->operation) ++background_attempts[r->identity.deck];
    if (enqueue_busy) { --enqueue_busy; return AZ_BUSY; }
    if (phase) return AZ_BUSY;
    mailbox = *r;
    phase = 1;
    return AZ_PENDING;
}
extern "C" void azsdk_cue_actor_drive(unsigned deck) noexcept {
    if (deck != mailbox.identity.deck || phase != 1) return;
    if (hold_background_query && mailbox.operation == 0) return;
    phase = 2;
    azexit_result result = {};
    int rc = actor.execute(&mailbox, manager, &result, nullptr);
    if (phase == 1) return;
    if (rc == AZ_PENDING) phase = 3;
    else {
        terminal = result;
        if (rc != AZ_OK) terminal = {mailbox.identity.token, mailbox.identity.load_generation, 0, AZ_COMMAND_REJECTED, rc};
        phase = 4;
    }
}
extern "C" int azsdk_cue_actor_resume(unsigned deck, uint64_t token) noexcept {
    if (resume_busy) { --resume_busy; return AZ_BUSY; }
    if (deck != mailbox.identity.deck || token != mailbox.identity.token) return AZ_STALE;
    phase = 1;
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_poll(unsigned deck, azexit_result *r) noexcept {
    if (deck != mailbox.identity.deck || phase != 4) return AZ_PENDING;
    *r = terminal;
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_release(unsigned deck, uint64_t token) noexcept {
    if (deck != mailbox.identity.deck || phase != 4 || token != terminal.token) return AZ_BUSY;
    if (mailbox.operation == 0 && background_release_busy) { --background_release_busy; return AZ_BUSY; }
    phase = 0;
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_cancel(unsigned deck, uint64_t token) noexcept {
    if (deck != mailbox.identity.deck || token != mailbox.identity.token) return AZ_STALE;
    if (mailbox.operation == 0 && background_cancel_busy) { --background_cancel_busy; return AZ_PENDING; }
    if (mailbox.operation == 0) cancelled_background_query = token;
    terminal = {token, mailbox.identity.load_generation, 0, AZ_COMMAND_CANCELLED, AZ_CANCELLED};
    phase = 4;
    return AZ_OK;
}
extern "C" int azsdk_cue_actor_complete(unsigned deck, uint64_t token, const azexit_result *r) noexcept {
    if (deck != mailbox.identity.deck || token != mailbox.identity.token) return AZ_STALE;
    terminal = *r;
    phase = 4;
    return AZ_OK;
}
extern "C" int azsdk_cue_persist_configure(const azcuepersist_profile *, azexit_install_fn, const azcuepersist_callbacks *c) noexcept {
    persist = *c;
    return 1;
}
extern "C" uint64_t azsdk_cue_persist_capabilities() noexcept { return all_persist(); }
extern "C" int azsdk_cue_persist_submit(const azexit_cue_request *r, void *m, void *, unsigned kind, const void *property_input, uint64_t source_cookie, const azcuepersist_witness *witness) noexcept {
    assert(persist.validate(r, m, source_cookie, nullptr));
    if (persist_busy) { --persist_busy; return AZ_BUSY; } // No producer/request/reply was created.
    if (persist_stale) { --persist_stale; return AZ_STALE; } // Preflight witness changed; no producer/request/reply.
    bool metadata = r->operation == AZ_CMD_SAVED_AUTO || r->operation == AZ_CMD_CUE_COMMENT || r->operation == AZ_CMD_CUE_COLOR || r->operation == AZ_CMD_HOT_SET;
    assert(metadata == (witness != nullptr));
    if (witness) {
        assert(witness->revision == (r->operation == AZ_CMD_HOT_SET ? ::revision : r->expected_revision) && witness->fingerprint);
        assert(witness->source_info == 0x50000 && witness->memory_set == 0x60000 && witness->hot_set == 0x70000);
        assert(witness->memory_content == 0x80000 && witness->hot_content == 0x90000 && witness->source_id[0] == 1);
        assert(bool(witness->selected_cue) == (r->operation != AZ_CMD_HOT_SET));
        if (witness->selected_cue) {
            const void *native = reinterpret_cast<const void *>(witness->selected_cue);
            bool found = false;
            for (const auto &row : rows)
                if (!std::memcmp(native, &row.cue, sizeof(row.cue))) found = true;
            assert(found);
        }
        submitted_witness = *witness;
    }
    ++persist_calls;
    submissions.push_back(*r);
    submitted_cookies.push_back(source_cookie);
    std::memcpy(&submitted_sample, static_cast<const char *>(property_input) + 0x40, 8);
    active_persist = *r;
    native_kind = kind;
    cookie = source_cookie;
    receipt = false;
    return AZ_PENDING;
}
extern "C" int azsdk_cue_persist_poll(const azexit_cue_request *r, void *m, azexit_result *out) noexcept {
    if (!receipt) return AZ_PENDING;
    uint64_t rev = 0;
    bool applied = !force_native_failure && persist.confirm(r, m, cookie, &rev, nullptr);
    *out = {r->identity.token, r->identity.load_generation, rev, applied ? AZ_COMMAND_APPLIED : AZ_COMMAND_UNCERTAIN, applied ? AZ_OK : AZ_FAILED};
    if (applied && inject_foreign_after_confirm) {
        put<uint32_t>(&rows.back().cue, 0x78, 0xff135790);
        ++revision;
        inject_foreign_after_confirm = false;
    }
    if (applied && expire_after_confirm) {
        azsdk_cue_commands_test_time(r->identity.deadline_ns);
        expire_after_confirm = false;
    }
    return AZ_OK;
}
extern "C" int azsdk_cue_persist_confirm_publication(const azexit_cue_request *r, const azcuepersist_witness *fresh) noexcept {
    assert(r->identity.token == active_persist.identity.token && fresh->revision == ::revision);
    return publication_matches ? publication_mode : 0;
}
extern "C" int azsdk_cue_recall_configure(int, uintptr_t) noexcept { return AZ_OK; }
extern "C" int azsdk_cue_recall_prepare(const azexit_cue_request *r, void *, const void *) noexcept {
    ++recalls;
    bridge_result = {r->identity.token, r->identity.load_generation, 1, AZ_COMMAND_APPLIED, AZ_OK};
    bridge_done = false;
    return AZ_PENDING;
}
extern "C" void azsdk_cue_recall_release(unsigned, uint64_t) noexcept {}
extern "C" int azsdk_exit_cue_stage_submit(const azexit_cue_request *r) noexcept {
    ++stages;
    stage_request = *r;
    bridge_result = {r->identity.token, r->identity.load_generation, 0, AZ_COMMAND_APPLIED, AZ_OK};
    bridge_done = false;
    return AZ_PENDING;
}
extern "C" int azsdk_exit_cue_stage_capture(unsigned deck, uint64_t token, azexit_cue_stage_sample *out) noexcept {
    assert(!deck && token == stage_request.identity.token && bridge_done && out);
    if (fail_stage_capture) return AZ_STALE;
    *out = {};
    if (stage_request.operation == AZ_CMD_HOT_CLEAR) return AZ_OK;
    if (override_stage_sample) { *out = stage_sample; return AZ_OK; }
    for (const auto &r : rows) if (r.cue.bytes[0] == stage_request.entry_id) {
        std::memcpy(out->source_id, r.property.bytes + 0x30, 12);
        std::memcpy(&out->in_sample, r.property.bytes + 0x40, 8);
        std::memcpy(&out->out_sample, r.property.bytes + 0x98, 8);
        std::memcpy(&out->sample_rate, r.property.bytes + 0x60, 4);
        out->out_valid = r.property.bytes[0x88] && compare_timestamps(out->out_sample, out->in_sample, out->sample_rate, out->sample_rate, 1) == AZ_TIME_AFTER;
        return AZ_OK;
    }
    return AZ_STALE;
}
extern "C" int azsdk_exit_recall_cancel(unsigned, uint64_t) noexcept { return AZ_PENDING; }
extern "C" int azsdk_exit_recall_poll(unsigned deck, azexit_result *r) noexcept {
    if (deck || !bridge_done) return AZ_PENDING;
    *r = bridge_result;
    return AZ_OK;
}
extern "C" int azsdk_exit_recall_release(unsigned, uint64_t) noexcept {
    bridge_done = false;
    return AZ_OK;
}
static void *install(uintptr_t, void *, void **) { return nullptr; }
static azexit_cue_request request(uint64_t token, unsigned operation, unsigned classification = AZ_CUE_MEMORY, unsigned id = 0, bool selected = false) {
    azexit_cue_request r = {};
    r.size = sizeof r;
    r.operation = operation;
    r.identity.token = token;
    r.identity.load_generation = generation;
    r.identity.deadline_ns = UINT64_MAX;
    r.classification = classification;
    r.entry_id = id;
    r.flags = AZ_COMMAND_NATIVE_LOAD;
    if (operation != AZ_CMD_MEMORY_RECALL && operation != AZ_CMD_SAVED_RECALL) r.flags |= AZ_COMMAND_PERSIST;
    if (selected) {
        r.flags |= AZ_COMMAND_NATIVE_CUE_IDS;
        r.expected_revision = revision;
    }
    return r;
}
static void dispatch(const azexit_cue_request &r) {
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
}
static azexit_result result(uint64_t token) {
    azexit_result r = {};
    assert(azsdk_cue_commands_poll(0, &r) == AZ_OK);
    assert(r.token == token);
    return r;
}
static void release(uint64_t token) { assert(azsdk_cue_commands_release(0, token) == AZ_OK); }
static void reply() {
    receipt = true;
    persist.ready(0, active_persist.identity.token, nullptr);
    azsdk_cue_actor_drive(0);
}
static void background_query_priority() {
    // A delayed read must not consume the public write's action budget. The
    // native actor retains the cancelled read's tombstone through destruction.
    ++generation;
    hold_background_query = true;
    azsdk_cue_commands_tick();
    assert(phase == 1 && mailbox.operation == 0);
    const uint64_t query = mailbox.identity.token;
    const unsigned before = persist_calls;
    auto r = request(9100, AZ_CMD_MEMORY_SET);
    const uint64_t deadline = r.identity.deadline_ns;
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(cancelled_background_query == query);
    assert(persist_calls == before + 1 && active_persist.identity.token == 9100);
    assert(compare_timestamps(active_persist.identity.deadline_ns, deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    auto second = request(9101, AZ_CMD_MEMORY_SET);
    assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
    reply();
    assert(result(9100).outcome == AZ_COMMAND_APPLIED);
    release(9100);
    hold_background_query = false;
    ++generation;
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick();
    // Failure to acquire or release the query's owner must retain both the
    // query identity and the copied public request until the existing drain.
    for (unsigned fault = 0; fault < 2; ++fault) {
        ++generation;
        hold_background_query = true;
        azsdk_cue_commands_tick();
        assert(phase == 1 && mailbox.operation == 0);
        background_cancel_busy = fault == 0;
        background_release_busy = fault == 1;
        const unsigned effects = persist_calls;
        r = request(9102 + fault, AZ_CMD_MEMORY_SET);
        assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
        assert(persist_calls == effects);
        second = request(9200 + fault, AZ_CMD_MEMORY_SET);
        assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
        hold_background_query = false;
        for (unsigned attempt = 0; attempt < 4 && persist_calls == effects; ++attempt) azsdk_cue_commands_tick();
        assert(persist_calls == effects + 1 && active_persist.identity.token == r.identity.token);
        assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
        reply();
        assert(result(r.identity.token).outcome == AZ_COMMAND_APPLIED);
        release(r.identity.token);
    }
    // Native actor frames share their owner lock/repository queue across decks.
    // Another deck's queued read must also yield to this public write.
    const auto saved_rows = rows;
    ++generation;
    loaded_decks = 2;
    hold_background_query = true;
    azsdk_cue_commands_tick();
    assert(phase == 1 && mailbox.operation == 0 && mailbox.identity.deck == 1);
    const uint64_t other_query = mailbox.identity.token;
    loaded_decks = 3;
    const unsigned effects = persist_calls;
    auto cross = request(9300, AZ_CMD_MEMORY_SET);
    assert(azsdk_cue_commands_submit(&cross) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(cancelled_background_query == other_query);
    assert(persist_calls == effects + 1 && active_persist.identity.token == cross.identity.token);
    const unsigned attempts = background_attempts[1];
    for (unsigned tick = 0; tick < 4; ++tick) azsdk_cue_commands_tick();
    assert(background_attempts[1] == attempts);
    assert(compare_timestamps(active_persist.identity.deadline_ns, cross.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    reply();
    assert(result(cross.identity.token).outcome == AZ_COMMAND_APPLIED);
    // A terminal public result held by a mod must not suppress other decks'
    // snapshot attempts after the actual native work has settled.
    azsdk_cue_commands_tick();
    assert(background_attempts[1] > attempts);
    release(cross.identity.token);
    hold_background_query = false;
    loaded_decks = 1;
    rows = saved_rows;
    ++generation;
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick();
    std::puts("PASS public cue writes retire queued reads across decks, retain their deadline and restore background reads after native completion");
}
static void management_admission() {
    auto r = request(9000, AZ_CMD_MEMORY_SET);
    const uint64_t deadline = r.identity.deadline_ns;
    const unsigned before = persist_calls;
    enqueue_busy = 3; // Shared owner lock busy, without this deck's refresh flag.
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    r.operation = AZ_CMD_HOT_CLEAR; // Admission owns a complete request copy.
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        azsdk_cue_commands_tick();
        azexit_result out = {};
        assert(azsdk_cue_commands_poll(0, &out) == AZ_PENDING);
        assert(persist_calls == before);
    }
    for (unsigned attempt = 0; attempt < 8 && persist_calls == before; ++attempt) azsdk_cue_commands_tick();
    assert(persist_calls == before + 1);
    assert(active_persist.operation == AZ_CMD_MEMORY_SET);
    assert(compare_timestamps(active_persist.identity.deadline_ns, deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    auto second = request(9001, AZ_CMD_MEMORY_SET);
    assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
    reply();
    assert(result(9000).outcome == AZ_COMMAND_APPLIED);
    release(9000);
    assert(persist_calls == before + 1);
    // The admission slot protects both directly accepted and queued commands
    // until release; a second public request cannot hide the first result.
    r = request(9002, AZ_CMD_MEMORY_SET);
    dispatch(r);
    assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
    reply();
    assert(result(9002).outcome == AZ_COMMAND_APPLIED);
    assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
    assert(azsdk_cue_commands_release(0, 9001) != AZ_OK);
    release(9002);
    const unsigned effects = persist_calls;
    enqueue_busy = 1;
    r = request(9003, AZ_CMD_MEMORY_SET);
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    assert(azsdk_cue_commands_cancel(0, 9004) == AZ_STALE);
    assert(azsdk_cue_commands_cancel(0, 9003) == AZ_OK);
    assert(result(9003).outcome == AZ_COMMAND_CANCELLED);
    assert(azsdk_cue_commands_release(0, 9004) == AZ_STALE);
    release(9003);
    assert(persist_calls == effects);
    // Waiting never renews the accepted deadline or follows a replacement load.
    enqueue_busy = 1;
    azsdk_cue_commands_test_time(100);
    r = request(9004, AZ_CMD_MEMORY_SET);
    r.identity.deadline_ns = 200;
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    azsdk_cue_commands_test_time(200);
    azsdk_cue_commands_tick();
    assert(result(9004).outcome == AZ_COMMAND_REJECTED && result(9004).reason == AZ_STALE);
    release(9004);
    assert(persist_calls == effects);
    azsdk_cue_commands_test_time(0);
    enqueue_busy = 1;
    r = request(9005, AZ_CMD_MEMORY_SET);
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    ++generation;
    azsdk_cue_commands_tick();
    assert(result(9005).outcome == AZ_COMMAND_REJECTED && result(9005).reason == AZ_STALE);
    release(9005);
    assert(persist_calls == effects);
    std::puts("PASS public cue admission retains one copied request through contention, cancellation, deadline, load change and release");
}
static void unchanged_snapshot_refresh() {
    auto query = request(UINT64_C(0x8000000000009500), 0);
    azexit_result out = {};
    const unsigned converted = property_calls;
    fail_property = 1;
    assert(actor.execute(&query, manager, &out, nullptr) == AZ_OK);
    assert(property_calls == converted && out.revision == revision);
    fail_property = -1;
    ++revision;
    assert(actor.execute(&query, manager, &out, nullptr) == AZ_OK);
    assert(property_calls == converted + rows.size());
    ++generation;
    query.identity.load_generation = generation;
    const unsigned before_load = property_calls;
    assert(actor.execute(&query, manager, &out, nullptr) == AZ_OK);
    assert(property_calls == before_load + rows.size());
    fail_snapshot = true;
    assert(actor.execute(&query, manager, &out, nullptr) == AZ_STALE);
    fail_snapshot = false;
    std::puts("PASS unchanged native cue publications renew observations without seek conversion; revision/load/source changes require fresh capture");
}
static void selected_property_conversion() {
    const auto saved = rows;
    rows.clear();
    for (unsigned entry = 1; entry <= 8; ++entry) rows.push_back(row(0, entry * 48000, false));
    for (unsigned slot = 1; slot <= 8; ++slot) rows.push_back(row(slot, (slot + 8) * 48000, false));
    ++revision;
    for (unsigned classification : {AZ_CUE_MEMORY, AZ_CUE_HOT}) {
        const size_t ordinal = classification == AZ_CUE_MEMORY ? 7 : 15;
        auto r = request(9400 + classification, AZ_CMD_CUE_COMMENT, classification, 8, true);
        std::strcpy(r.comment, "selected note");
        const unsigned converted = property_calls, projected = canonical_calls;
        fail_property = 1; // An unrelated row must not enter native conversion.
        dispatch(r);
        assert(property_calls == converted + 1 && canonical_calls == projected + 1);
        assert(active_persist.identity.token == r.identity.token && active_persist.entry_id == 8);
        assert(native_kind == (classification == AZ_CUE_HOT ? 8u : 0u));
        assert(compare_timestamps(submitted_sample, (ordinal + 1) * 48000, 48000, 48000, 1) == AZ_TIME_EQUAL);
        assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
        fail_property = -1;
        put<uintptr_t>(&rows[ordinal].cue, 0x70, uintptr_t("selected note"));
        ++revision;
        const unsigned before_confirmation = property_calls;
        reply();
        assert(result(r.identity.token).outcome == AZ_COMMAND_APPLIED);
        assert(property_calls == before_confirmation);
        release(r.identity.token);
    }
    // Even a correlated root-only reply cannot acknowledge an unrelated full
    // comment change beyond the public snapshot's 127-byte prefix.
    std::string old_tail(130, 'x'), new_tail(130, 'x');
    old_tail += 'A'; new_tail += 'B';
    put<uintptr_t>(&rows[8].cue, 0x70, uintptr_t(old_tail.c_str()));
    ++revision;
    for (unsigned fault = 0; fault < 2; ++fault) {
        auto r = request(9420 + fault, AZ_CMD_CUE_COMMENT, AZ_CUE_MEMORY, 8, true);
        std::strcpy(r.comment, "next selected note");
        dispatch(r);
        put<uintptr_t>(&rows[7].cue, 0x70, uintptr_t("next selected note"));
        publication_matches = fault != 0;
        publication_mode = 2;
        if (fault) put<uintptr_t>(&rows[8].cue, 0x70, uintptr_t(new_tail.c_str()));
        ++revision;
        reply();
        assert(result(r.identity.token).outcome == AZ_COMMAND_UNCERTAIN);
        release(r.identity.token);
        publication_matches = true;
        publication_mode = 1;
        put<uintptr_t>(&rows[8].cue, 0x70, uintptr_t(old_tail.c_str()));
        ++revision;
    }
    const unsigned converted = property_calls, effects = persist_calls;
    auto absent = request(9410, AZ_CMD_CUE_COMMENT, AZ_CUE_MEMORY, 9, true);
    dispatch(absent);
    assert(result(absent.identity.token).reason == AZ_STALE && persist_calls == effects && property_calls == converted);
    release(absent.identity.token);
    rows = saved;
    ++generation;
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick();
    std::puts("PASS revision-qualified selectors convert only the selected native property and preserve identity/deadline");
}
static void hot_set_publication() {
    override_hot_view = override_stage_sample = true;
    for (unsigned rate : {44100u, 48000u, 96000u, 192000u}) for (unsigned fault = 0; fault < 7; ++fault) {
        Row target = row(8, rate == 44100 ? 115274 : 3u * rate, false);
        uint64_t sample;
        std::memcpy(&sample, target.property.bytes + 0x40, 8);
        put<uint32_t>(&target.property, 0x60, rate);
        put<uint32_t>(&target.property, 0xb8, rate);
        put<uint64_t>(&target.property, 0x68, rate == 44100 ? 2613923 : 3000000);
        stage_sample = {};
        stage_sample.in_sample = sample;
        stage_sample.sample_rate = rate;
        std::memcpy(stage_sample.source_id, target.property.bytes + 0x30, 12);
        rows.back() = target;
        hot_view = target.property;
        missing_hot_view = fault == 0;
        if (fault == 1) put<uint64_t>(&hot_view, 0x40, sample + 1); // Exact sample boundary is not equal.
        if (fault == 2) hot_view.bytes[0x30] ^= 2;
        if (fault == 3) put<uint32_t>(&hot_view, 0x60, rate * 2);
        if (fault == 4) put<uint64_t>(&hot_view, 0x98, sample + rate);
        auto r = request(1000 + rate + fault, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8);
        unsigned before = persist_calls, before_stages = stages;
        dispatch(r);
        fail_snapshot = fault == 5;
        fail_witness = fault == 6;
        bridge_done = true;
        azsdk_cue_commands_tick();
        azexit_result pending = {};
        assert(azsdk_cue_commands_poll(0, &pending) == AZ_PENDING && persist_calls == before && stages == before_stages + 1);
        missing_hot_view = false;
        fail_snapshot = false;
        fail_witness = false;
        hot_view = target.property;
        azsdk_cue_commands_tick();
        assert(persist_calls == before + 1 && stages == before_stages + 1 && compare_timestamps(submitted_sample, sample, rate, rate, 1) == AZ_TIME_EQUAL);
        assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
        reply();
        assert(result(r.identity.token).outcome == AZ_COMMAND_APPLIED);
        release(r.identity.token);
    }
    // The DSP setter can queue its own native save before the SDK's correlated
    // persistence step. Busy retries must not repeat that setter or its save.
    for (int transient : {AZ_BUSY, AZ_STALE}) {
        auto r = request(3000 + unsigned(-transient), AZ_CMD_HOT_SET, AZ_CUE_HOT, 8);
        unsigned before = persist_calls, before_stages = stages;
        dispatch(r);
        persist_busy = transient == AZ_BUSY ? 3 : 0;
        persist_stale = transient == AZ_STALE ? 3 : 0;
        bridge_done = true;
        for (unsigned retry = 0; retry < 3; ++retry) {
            azsdk_cue_commands_tick();
            azexit_result pending = {};
            assert(azsdk_cue_commands_poll(0, &pending) == AZ_PENDING);
            assert(persist_calls == before && stages == before_stages + 1);
            ++revision;
        }
        azsdk_cue_commands_tick();
        assert(persist_calls == before + 1 && stages == before_stages + 1);
        assert(submitted_witness.revision == revision);
        assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
        reply();
        assert(result(r.identity.token).outcome == AZ_COMMAND_APPLIED);
        release(r.identity.token);
    }
    for (unsigned failure = 0; failure < 8; ++failure) {
        azsdk_cue_commands_test_time(100);
        auto r = request(2000 + failure, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8);
        r.identity.deadline_ns = 110;
        unsigned before = persist_calls;
        missing_hot_view = failure < 3;
        dispatch(r);
        fail_stage_capture = failure == 2;
        persist_busy = failure == 3 ? 3 : 0;
        persist_stale = failure == 4 || failure == 5 ? 3 : 0;
        fail_witness = failure >= 6;
        bridge_done = true;
        azsdk_cue_commands_tick();
        if (failure != 2) {
            azexit_result pending = {};
            assert(azsdk_cue_commands_poll(0, &pending) == AZ_PENDING);
            if (failure == 0 || failure == 3 || failure == 4 || failure == 6) azsdk_cue_commands_test_time(r.identity.deadline_ns);
            else ++generation;
            azsdk_cue_commands_tick();
        }
        auto out = result(r.identity.token);
        assert(out.outcome != AZ_COMMAND_APPLIED && out.reason == (failure == 2 ? AZ_FAILED : AZ_STALE));
        assert(persist_calls == before);
        release(r.identity.token);
        if (failure == 1 || failure == 5 || failure == 7) --generation;
        fail_stage_capture = false;
        persist_busy = 0;
        persist_stale = 0;
        fail_witness = false;
        azsdk_cue_commands_test_time(0);
        azsdk_cue_commands_tick();
        azsdk_cue_commands_tick();
    }
    missing_hot_view = false;
    override_hot_view = override_stage_sample = false;
    std::puts("PASS HOT_SET waits for its DSP-captured source point, rate and shape before persistence");
}
int main(int argc, char **argv) {
    property(main_property, 48000, true);
    std::string long_comment(126, 'x');
    long_comment += "\xe2\x82\xac";
    rows = {row(0, 48000, true), row(8, 96000, false, long_comment.c_str(), 0x123456)};
    assert(azsdk_cue_commands_configure(1, 1, install) == AZ_OK);
    assert(azsdk_cue_commands_commands() == (all_persist() | (UINT64_C(1) << AZ_CMD_SAVED_RECALL) | (UINT64_C(1) << AZ_CMD_MEMORY_RECALL)));
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick(); // refresh captures scalar IDs; second releases internal result
    if (argc == 2) {
        if (!std::strcmp(argv[1], "priority")) {
            background_query_priority();
            return 0;
        }
        if (!std::strcmp(argv[1], "admission")) {
            management_admission();
            return 0;
        }
        if (!std::strcmp(argv[1], "selection")) {
            selected_property_conversion();
            return 0;
        }
        if (!std::strcmp(argv[1], "refresh")) {
            unchanged_snapshot_refresh();
            return 0;
        }
        assert(!std::strcmp(argv[1], "hot-set"));
        hot_set_publication();
        return 0;
    }
    assert(argc == 1);
    az_native_cue_snapshot snapshot = {};
    snapshot.size = sizeof snapshot;
    assert(azsdk_cue_commands_native_cues(0, &snapshot) == AZ_OK && snapshot.count == 2);
    assert(snapshot.entries[0].entry_id == 1 && snapshot.entries[0].type == AZ_CUE_LOOP &&
           compare_timestamps(snapshot.entries[0].start_s, 1., 1, 48000, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(snapshot.entries[0].end_s, 2., 1, 48000, 1) == AZ_TIME_EQUAL);
    assert(std::strlen(snapshot.entries[1].comment) == 126);
    assert(snapshot.entries[1].entry_id == 8 && snapshot.entries[1].slot == 8 && snapshot.entries[1].classification == AZ_CUE_HOT);
    azexit_cue_target target = {};
    assert(azsdk_cue_commands_target_cue(0, &target) == AZ_OK);
    assert(!(target.persistent_commands & (UINT64_C(1) << AZ_CMD_MEMORY_RECALL)) && !(target.revision_commands & (UINT64_C(1) << AZ_CMD_HOT_SET)));
    std::atomic<unsigned> target_lock {0};
    std::thread lock_holder([&]() {
        azsdk_cue_commands_test_cache_lock(0, [](void *context) {
            auto &signal = *static_cast<std::atomic<unsigned> *>(context);
            signal.store(1, std::memory_order_release);
            while (signal.load(std::memory_order_acquire) != 2) std::this_thread::yield();
        }, &target_lock);
    });
    while (!target_lock.load(std::memory_order_acquire)) std::this_thread::yield();
    int target_status = azsdk_cue_commands_target_cue(0, &target);
    target_lock.store(2, std::memory_order_release);
    lock_holder.join();
    assert(target_status == AZ_BUSY);
    assert(azsdk_cue_commands_target_cue(0, &target) == AZ_OK && target.revision_valid && target.revision == revision);
    auto fence_request = request(80, AZ_CMD_CUE_COMMENT, AZ_CUE_MEMORY, 1, true);
    std::strcpy(fence_request.comment, "fenced");
    fail_witness = true;
    dispatch(fence_request);
    assert(result(80).reason == AZ_STALE && persist_calls == 0);
    release(80);
    fail_witness = false;
    auto r = request(1, AZ_CMD_CUE_COLOR, AZ_CUE_HOT, 8, true);
    r.rgb = 0xaabbcc;
    dispatch(r);
    assert(native_kind == 8 && persist_calls == 1);
    assert(submitted_witness.revision == r.expected_revision && witness_calls == 2);
    assert(azsdk_cue_commands_cancel(0, 1) == AZ_PENDING);
    put<uint32_t>(&rows[1].cue, 0x78, 0xffaabbcc);
    ++revision;
    // A receipt must not be lost when the owner frame is still executing.
    receipt = true;
    resume_busy = 2;
    persist.ready(0, active_persist.identity.token, nullptr);
    azexit_result deferred = {};
    assert(azsdk_cue_commands_poll(0, &deferred) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(azsdk_cue_commands_poll(0, &deferred) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(persist_calls == 1 && compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    assert(result(1).outcome == AZ_COMMAND_APPLIED);
    release(1);
    r = request(2, AZ_CMD_CUE_COMMENT, AZ_CUE_HOT, 8, true);
    std::strcpy(r.comment, "edited");
    dispatch(r);
    // A receipt alone cannot claim an effect absent from the refreshed store.
    reply();
    assert(result(2).outcome == AZ_COMMAND_UNCERTAIN);
    release(2);
    r = request(3, AZ_CMD_MEMORY_DELETE, AZ_CUE_MEMORY, 1, true);
    r.expected_revision = revision - 1;
    dispatch(r);
    assert(result(3).reason == AZ_STALE);
    release(3);
    // Hot SET must run the DSP stage even when no main cue exists.
    main_valid = false;
    r = request(4, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8);
    dispatch(r);
    assert(stages == 1 && persist_calls == 2);
    rows[1] = row(8, 144000, false);
    ++revision;
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(persist_calls == 3 && native_kind == 8);
    reply();
    assert(result(4).outcome == AZ_COMMAND_APPLIED);
    release(4);
    // Hot CLEAR retains the old native property until after its live DSP clear.
    r = request(5, AZ_CMD_HOT_CLEAR, AZ_CUE_HOT, 8);
    dispatch(r);
    assert(stages == 2);
    rows.pop_back();
    ++revision;
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(persist_calls == 4 && native_kind == 8);
    reply();
    assert(result(5).outcome == AZ_COMMAND_APPLIED);
    release(5);
    main_valid = true;
    r = request(6, AZ_CMD_MEMORY_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(r);
    assert(recalls == 1);
    azexit_result pending_result = {};
    assert(azsdk_cue_commands_poll(0, &pending_result) == AZ_PENDING);
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(result(6).outcome == AZ_COMMAND_APPLIED && result(6).revision == r.expected_revision);
    release(6);
    r = request(7, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 1, true);
    r.position_s = 1;
    dispatch(r);
    rows[0].cue.bytes[0x28] = 1;
    ++revision;
    reply();
    assert(result(7).outcome == AZ_COMMAND_APPLIED);
    release(7);
    r = request(8, AZ_CMD_MEMORY_DELETE, AZ_CUE_MEMORY, 1, true);
    dispatch(r);
    rows.clear();
    ++revision;
    reply();
    assert(result(8).outcome == AZ_COMMAND_APPLIED);
    release(8);
    r = request(9, AZ_CMD_MEMORY_SET);
    r.position_s = 1;
    assert(azsdk_cue_commands_submit(&r) == AZ_INVALID);
    r = request(10, AZ_CMD_HOT_SET, AZ_CUE_HOT, 9);
    assert(azsdk_cue_commands_submit(&r) == AZ_INVALID);
    r = request(11, AZ_CMD_CUE_COLOR, AZ_CUE_MEMORY, 1, true);
    r.rgb = 0x1000000;
    assert(azsdk_cue_commands_submit(&r) == AZ_INVALID);
    r.rgb = 0x2ba6d9;
    unsigned before_palette = persist_calls;
    assert(azsdk_cue_commands_submit(&r) == AZ_UNSUPPORTED);
    assert(persist_calls == before_palette);
    rows = {row(0, 48000, true, "retained cue \xe2\x9c\x93")};
    ++revision;
    const uint32_t palette[] = {0, 0xff00c6, 0xff0000, 0xff7100, 0xffff00, 0x00e300, 0x00c3ff, 0x0051ff, 0x9c08ff};
    for (unsigned i = 0; i < 9; ++i) {
        r = request(1000 + i, AZ_CMD_CUE_COLOR, AZ_CUE_MEMORY, 1, true);
        r.rgb = palette[i];
        dispatch(r);
        assert(persist_calls == before_palette + i + 1);
        put<uint32_t>(&rows[0].cue, 0x78, 0xff000000u | r.rgb);
        rows[0].cue.bytes[0x7c] = i;
        ++revision;
        reply();
        assert(result(1000 + i).outcome == AZ_COMMAND_APPLIED);
        release(1000 + i);
    }
    r = request(12, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 1, true);
    r.position_s = 0.5;
    assert(azsdk_cue_commands_submit(&r) == AZ_INVALID);
    r = request(13, AZ_CMD_MEMORY_SET);
    dispatch(r);
    rows = {row(0, 48000, true)};
    ++revision;
    reply();
    assert(result(13).outcome == AZ_COMMAND_APPLIED);
    release(13);
    r = request(14, AZ_CMD_SAVED_SAVE);
    dispatch(r);
    reply();
    assert(result(14).outcome == AZ_COMMAND_APPLIED);
    release(14);
    revision = 50;
    unsigned before_recall_projection = canonical_calls;
    r = request(15, AZ_CMD_SAVED_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(r);
    assert(recalls == 2);
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(result(15).outcome == AZ_COMMAND_APPLIED && result(15).revision == 50);
    assert(canonical_calls == before_recall_projection);
    release(15);
    r = request(90, AZ_CMD_MEMORY_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(r);
    bridge_result.token = 91;
    bridge_done = true;
    azsdk_cue_commands_tick();
    azexit_result no_match = {};
    assert(azsdk_cue_commands_poll(0, &no_match) == AZ_PENDING);
    bridge_result.token = 90;
    bridge_result.load_generation = generation + 1;
    azsdk_cue_commands_tick();
    assert(result(90).outcome == AZ_COMMAND_UNCERTAIN && result(90).revision == 0 && result(90).reason == AZ_STALE);
    release(90);
    r = request(91, AZ_CMD_MEMORY_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(r);
    bridge_result.outcome = AZ_COMMAND_UNCERTAIN; // Native DSP source/seek correlation failed.
    bridge_result.reason = AZ_STALE;
    bridge_result.revision = 0;
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(result(91).outcome == AZ_COMMAND_UNCERTAIN && result(91).revision == 0);
    release(91);
    r = request(16, AZ_CMD_CUE_COMMENT, AZ_CUE_MEMORY, 1, true);
    std::strcpy(r.comment, "saved note");
    dispatch(r);
    put<uintptr_t>(&rows[0].cue, 0x70, uintptr_t("saved note"));
    ++revision;
    reply();
    assert(result(16).outcome == AZ_COMMAND_APPLIED);
    release(16);
    r = request(17, AZ_CMD_SAVED_DELETE, AZ_CUE_MEMORY, 1, true);
    dispatch(r);
    rows.clear();
    ++revision;
    reply();
    assert(result(17).outcome == AZ_COMMAND_APPLIED);
    release(17);
    rows = {row(0, 48000, false)};
    ++revision;
    r = request(18, AZ_CMD_SAVED_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(r);
    assert(result(18).reason == AZ_INVALID);
    release(18);
    assert(recalls == 4);
    property(main_property, 48000, false);
    r = request(19, AZ_CMD_SAVED_SAVE);
    dispatch(r);
    assert(result(19).reason == AZ_INVALID);
    release(19);
    // Actual 44.1kHz native persistence truncates the usec round trip by one
    // sample. The producer must still receive the untouched original property.
    property(main_property, 150025, false);
    put<uint32_t>(&main_property, 0x60, 44100);
    put<uint64_t>(&main_property, 0x68, 3401927);
    r = request(92, AZ_CMD_MEMORY_SET);
    dispatch(r);
    assert(compare_timestamps(submitted_sample, 150025, 44100, 44100, 1) == AZ_TIME_EQUAL);
    Row projected = row(0, 150024, false);
    put<uint32_t>(&projected.property, 0x60, 44100);
    put<uint64_t>(&projected.property, 0x68, 3401927);
    rows = {projected};
    ++revision;
    reply();
    assert(result(92).outcome == AZ_COMMAND_APPLIED);
    release(92);
    r = request(93, AZ_CMD_MEMORY_SET);
    dispatch(r);
    put<uint64_t>(&rows[0].property, 0x40, 150023);
    ++revision;
    reply();
    assert(result(93).outcome == AZ_COMMAND_UNCERTAIN);
    release(93);
    property(main_property, 48000, false);
    rows = {row(0, 48000, false)};
    ++revision;
    r = request(20, AZ_CMD_MEMORY_SET);
    dispatch(r);
    generation = 43;
    reply();
    assert(result(20).outcome == AZ_COMMAND_UNCERTAIN);
    release(20);
    snapshot.size = sizeof snapshot;
    assert(azsdk_cue_commands_native_cues(0, &snapshot) == AZ_STALE);
    // A background refresh never causes a random public BUSY rejection.
    azsdk_cue_commands_tick(); // New load: internal query now owns the actor mailbox.
    unsigned before_wait = persist_calls;
    r = request(21, AZ_CMD_MEMORY_SET);
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    r.operation = AZ_CMD_HOT_CLEAR; // Queued public request is an owned copy.
    azsdk_cue_commands_tick(); // Query drains; public command wins before another refresh.
    assert(persist_calls == before_wait + 1 && active_persist.operation == AZ_CMD_MEMORY_SET);
    reply();
    assert(result(21).outcome == AZ_COMMAND_APPLIED);
    release(21);
    // Cancelling the copied waiting request has no native effect and is pollable
    // even while its preceding query still occupies the native actor mailbox.
    ++generation;
    azsdk_cue_commands_tick();
    before_wait = persist_calls;
    r = request(22, AZ_CMD_MEMORY_SET);
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    assert(azsdk_cue_commands_cancel(0, 22) == AZ_OK);
    assert(result(22).outcome == AZ_COMMAND_CANCELLED);
    release(22);
    assert(persist_calls == before_wait);
    // Expiration while waiting rejects before any native public producer call.
    ++generation;
    azsdk_cue_commands_tick();
    r = request(23, AZ_CMD_MEMORY_SET);
    r.identity.deadline_ns = 1;
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(result(23).outcome == AZ_COMMAND_REJECTED && result(23).reason == AZ_STALE);
    release(23);
    assert(persist_calls == before_wait);
    // The native HOT vector contains eight fixed slots, including explicit
    // absent rows. Only those markers are skipped, before native conversion.
    stored_memory_count = 2;
    rows = {row(0, 48000, false), row(0, 96000, true)};
    Row empty = row(0xff, 0, false);
    put<uint64_t>(&empty.cue, 8, UINT64_C(0x7fffffffffffffff));
    put<uint64_t>(&empty.cue, 0x10, UINT64_C(0x7fffffffffffffff));
    for (unsigned slot = 1; slot <= 7; ++slot) rows.push_back(empty);
    rows.push_back(row(8, 192000, false));
    // Native validity also treats either sentinel coordinate as absent.
    rows[3].cue.bytes[0] = 2;
    put<uint64_t>(&rows[3].cue, 0x10, 0);
    rows[4].cue.bytes[0] = 3;
    put<uint64_t>(&rows[4].cue, 8, 0);
    ++revision;
    azexit_cue_request query = request(UINT64_C(0x8000000000007777), 0);
    azexit_result queried = {};
    unsigned converted = property_calls;
    assert(actor.execute(&query, manager, &queried, nullptr) == AZ_OK);
    assert(property_calls == converted + 3);
    snapshot.size = sizeof snapshot;
    assert(azsdk_cue_commands_native_cues(0, &snapshot) == AZ_OK && snapshot.count == 3);
    assert(snapshot.entries[0].entry_id == 1 && snapshot.entries[1].entry_id == 2);
    assert(snapshot.entries[2].classification == AZ_CUE_HOT && snapshot.entries[2].entry_id == 8);
    // A populated HOT row with an invalid converted position fails closed.
    rows[2] = row(1, 144000, false);
    rows[2].property.bytes[0x30] = 0;
    ++revision;
    assert(actor.execute(&query, manager, &queried, nullptr) == AZ_UNSUPPORTED);
    rows[2] = empty;
    // A cross-source/native conversion refusal is never interpreted as empty.
    fail_property = 10;
    assert(actor.execute(&query, manager, &queried, nullptr) == AZ_UNSUPPORTED);
    fail_property = -1;
    rows[2] = row(9, 144000, false);
    assert(actor.execute(&query, manager, &queried, nullptr) == AZ_UNSUPPORTED);
    rows[2] = empty;
    rows[0].cue.bytes[0] = 0xff; // Absent markers are not permitted in memory rows.
    assert(actor.execute(&query, manager, &queried, nullptr) == AZ_UNSUPPORTED);
    rows[0].cue.bytes[0] = 0;
    // A same-point native HOT duplicate with old RGB cannot be acknowledged.
    unsigned previous_stages = stages;
    r = request(81, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8);
    r.rgb = 0x123456;
    dispatch(r);
    assert(stages == previous_stages + 1);
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(submitted_witness.revision == revision && r.expected_revision == 0);
    assert(compare_timestamps(submitted_sample, 192000, 48000, 48000, 1) == AZ_TIME_EQUAL);
    reply();
    assert(result(81).outcome == AZ_COMMAND_UNCERTAIN);
    release(81);
    r = request(82, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8);
    r.rgb = 0x123456;
    dispatch(r);
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(compare_timestamps(submitted_sample, 192000, 48000, 48000, 1) == AZ_TIME_EQUAL);
    put<uint32_t>(&rows.back().cue, 0x78, 0xff123456);
    ++revision;
    reply();
    assert(result(82).outcome == AZ_COMMAND_APPLIED);
    release(82);
    // Enabling a saved loop durably disables each old loop through a separate
    // native request, then enables the selected loop under the same public ID.
    stored_memory_count = 3;
    rows = {row(0, 48000, true, "old loop", 0x112233), row(0, 144000, true, "selected loop", 0x445566), row(0, 288000, false, "point", 0x778899), row(8, 384000, false, "hot", 0x123456)};
    rows[0].cue.bytes[0x28] = 1;
    ++revision;
    unsigned before_auto = persist_calls;
    r = request(1000, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    r.identity.deadline_ns = UINT64_MAX - 100;
    dispatch(r);
    assert(persist_calls == before_auto + 1 && active_persist.entry_id == 1 && active_persist.position_s == 0);
    uint64_t first_cookie = cookie;
    assert(active_persist.identity.token == 1000 && compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    rows[0].cue.bytes[0x28] = 0;
    rows[0].cue.bytes[1] = 0xa5;
    rows[0].cue.bytes[2] = 0x5a; // Undefined native padding is not an identity/effect field.
    ++revision;
    reply();
    assert(azsdk_cue_commands_poll(0, &pending_result) == AZ_PENDING);
    assert(persist_calls == before_auto + 2 && active_persist.entry_id == 2 && active_persist.position_s == 1);
    assert(cookie != first_cookie && active_persist.expected_revision == revision);
    assert(active_persist.identity.token == 1000 && compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    rows[1].cue.bytes[0x28] = 1;
    ++revision;
    reply();
    assert(result(1000).outcome == AZ_COMMAND_APPLIED && rows[0].cue.bytes[0x28] == 0 && rows[1].cue.bytes[0x28] == 1);
    assert(submissions.back().operation == AZ_CMD_SAVED_AUTO && submissions[submissions.size() - 2].operation == AZ_CMD_SAVED_AUTO);
    uintptr_t old_comment = 0, selected_comment = 0;
    uint32_t old_rgb = 0, selected_rgb = 0;
    std::memcpy(&old_comment, rows[0].cue.bytes + 0x70, 8);
    std::memcpy(&selected_comment, rows[1].cue.bytes + 0x70, 8);
    std::memcpy(&old_rgb, rows[0].cue.bytes + 0x78, 4);
    std::memcpy(&selected_rgb, rows[1].cue.bytes + 0x78, 4);
    assert(!std::strcmp(reinterpret_cast<const char *>(old_comment), "old loop") && !std::strcmp(reinterpret_cast<const char *>(selected_comment), "selected loop"));
    assert(old_rgb == 0xff112233 && selected_rgb == 0xff445566 && rows.size() == 4);
    release(1000);
    // Disable changes only its selected loop, even when another loop is active.
    rows[0].cue.bytes[0x28] = 1;
    ++revision;
    before_auto = persist_calls;
    r = request(1001, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 0;
    dispatch(r);
    assert(active_persist.entry_id == 2 && active_persist.position_s == 0);
    rows[1].cue.bytes[0x28] = 0;
    ++revision;
    reply();
    assert(result(1001).outcome == AZ_COMMAND_APPLIED && rows[0].cue.bytes[0x28] == 1 && persist_calls == before_auto + 1);
    release(1001);
    // A foreign revision after our first receipt prevents the next native write.
    before_auto = persist_calls;
    r = request(1002, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(r);
    rows[0].cue.bytes[0x28] = 0;
    ++revision;
    inject_foreign_after_confirm = true;
    reply();
    assert(result(1002).outcome == AZ_COMMAND_UNCERTAIN && result(1002).reason == AZ_STALE && persist_calls == before_auto + 1);
    assert(rows[0].cue.bytes[0x28] == 0 && rows[1].cue.bytes[0x28] == 0);
    release(1002);
    // A failed first receipt must not issue a selected-enable follow-up.
    rows[0].cue.bytes[0x28] = 1;
    ++revision;
    before_auto = persist_calls;
    r = request(1003, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(r);
    force_native_failure = true;
    reply();
    force_native_failure = false;
    assert(result(1003).outcome == AZ_COMMAND_UNCERTAIN && persist_calls == before_auto + 1 && rows[0].cue.bytes[0x28] == 1);
    release(1003);
    // The public deadline is never renewed between its native steps.
    before_auto = persist_calls;
    r = request(1004, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    r.identity.deadline_ns = UINT64_MAX - 100;
    dispatch(r);
    rows[0].cue.bytes[0x28] = 0;
    ++revision;
    expire_after_confirm = true;
    reply();
    assert(result(1004).outcome == AZ_COMMAND_UNCERTAIN && persist_calls == before_auto + 1);
    assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL && rows[1].cue.bytes[0x28] == 0);
    release(1004);
    azsdk_cue_commands_test_time(0);
    // An unrelated publication before confirmation cannot qualify as our receipt.
    rows[0].cue.bytes[0x28] = 1;
    ++revision;
    before_auto = persist_calls;
    r = request(1005, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(r);
    rows[0].cue.bytes[0x28] = 0;
    ++revision;
    publication_matches = false;
    reply();
    publication_matches = true;
    assert(result(1005).outcome == AZ_COMMAND_UNCERTAIN && persist_calls == before_auto + 1);
    release(1005);
    // Even correlated root-only receipt proof requires full semantic equality,
    // including comment bytes beyond the public 127-byte snapshot prefix.
    std::string before_tail(130, 'x'), after_tail(130, 'x');
    before_tail += 'A';
    after_tail += 'B';
    put<uintptr_t>(&rows.back().cue, 0x70, uintptr_t(before_tail.c_str()));
    rows[0].cue.bytes[0x28] = 1;
    ++revision;
    before_auto = persist_calls;
    publication_mode = 2;
    r = request(1006, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(r);
    rows[0].cue.bytes[0x28] = 0;
    put<uintptr_t>(&rows.back().cue, 0x70, uintptr_t(after_tail.c_str()));
    ++revision;
    reply();
    assert(result(1006).outcome == AZ_COMMAND_UNCERTAIN && persist_calls == before_auto + 1);
    release(1006);
    // Rebuilt pointers and padding alone are not foreign semantic changes.
    rows[0].cue.bytes[0x28] = 1;
    ++revision;
    r = request(1007, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(r);
    std::string same_old = "old loop";
    rows[0].cue.bytes[0x28] = 0;
    rows[0].cue.bytes[1] ^= 0xff;
    put<uintptr_t>(&rows[0].cue, 0x70, uintptr_t(same_old.c_str()));
    ++revision;
    reply();
    assert(azsdk_cue_commands_poll(0, &pending_result) == AZ_PENDING);
    rows[1].cue.bytes[0x28] = 1;
    ++revision;
    reply();
    assert(result(1007).outcome == AZ_COMMAND_APPLIED && rows[0].cue.bytes[0x28] == 0 && rows[1].cue.bytes[0x28] == 1);
    release(1007);
    publication_mode = 1;
}
