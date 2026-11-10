/* SPDX-License-Identifier: GPL-3.0-only */
/* Active A-H slots are indexed CueContext pointer members. The owner and
 * validated property types are shared with prepare_cue's hot edit path.
 * No retained native object escapes the frame. One commit pins all 8 slots. */
static std::atomic<uint64_t> hot_providers {0};
static std::atomic<bool> hot_metadata {false};
static void *hot_edit_original;
struct HotEdit {
    uintptr_t property;
    uint32_t before_revision;
};
static thread_local uintptr_t editing_hot;
static uint64_t new_hot_provider() noexcept {
    constexpr uint64_t prefix = AZSP_HOT;
    uint64_t id = hot_providers.load(std::memory_order_relaxed);
    for (unsigned n = 0; n < 8 && id < AZSP_SERIAL_MAX; ++n)
        if (hot_providers.compare_exchange_strong(id, id + 1, std::memory_order_relaxed))
            return prefix | (id + 1);
    return 0;
}
static bool status_hot_member(Slot &slot, uintptr_t property) noexcept {
    uintptr_t context = word(slot.facade + 0x68);
    for (unsigned i = 0; i < 8; ++i)
        if (word(context + 0x20 + i * 8) == property)
            return table(property, state().profile.cue_table) || table(property, state().profile.cue_base_table);
    return false;
}
static void status_hot_metadata(uintptr_t property, azn_hot_slot &row, char (&out)[128]) noexcept {
    if (row.in.state != AZN_COORD) return;
    unsigned automatic = byte(property + 0xe1);
    if (row.out.state == AZN_COORD && automatic <= 1)
        row.reserved |= AZN_HOT_AUTO_ACTIVE | (automatic ? AZN_HOT_AUTO_ON : 0);
    /* The owned CueProperty retains CueComment, which owns a shared GNU string.
     * Copy while the player still owns the property and keep no native pointer. */
    uintptr_t comment = word(property + 0xe8);
    if (!pointer(comment) || !pointer(word(property + 0xf0))) return;
    uintptr_t string = word(comment);
    if (!pointer(string) || !pointer(word(comment + 8)) || !word(string + 8)) return;
    uintptr_t bytes = word(string);
    if (bytes < 0x10000 || bytes > UINT64_C(0x0000ffffffffffff)) return;
    azcue_copy_utf8_prefix(out, reinterpret_cast<const char *>(bytes));
    if (out[0]) row.reserved |= AZN_HOT_COMMENT;
}
static void status_hot_capture(unsigned d, Slot &slot, uintptr_t written, const azsdk_load_sample *before,
                               const HotEdit *edit = nullptr) noexcept {
    if (editing_hot || !(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_CUES))) return;
    auto &h = slot.hot;
    if (h.busy.test_and_set(std::memory_order_acquire)) {
        azn_hot_gap(d);
        return;
    }
    struct Unlock {
        Slot::HotState &hot;
        ~Unlock() { hot.busy.clear(std::memory_order_release); }
    } guard {h};
    azsdk_load_sample load {};
    if (!azsdk_load_read(int(d), &load) || (before &&
                                            (load.invalidation_epoch != before->invalidation_epoch || load.load_generation != before->load_generation ||
                                             load.unit_generation != before->unit_generation))) return;
    uintptr_t retained, active;
    if (!status_properties(slot, retained, active)) {
        if (h.ready) {
            h.ready = false;
            azn_hot_gap(d);
            azn_capability(d, AZN_CAP_HOT | AZN_CAP_HOT_LINK, 0);
        }
        return;
    }
    if (!status_source_loaded(d, slot, load)) {
        if (h.ready && h.load == load.load_generation) {
            h.ready = false;
            /* The load withdrawal already covers a revoked bank while no
             * replacement is active. A same-epoch mismatch still loses history. */
            if (!azn_load_history_revoked(d, load.invalidation_epoch)) azn_hot_gap(d);
        }
        return;
    }
    const uintptr_t context = word(slot.facade + 0x68);
    const auto &p = state().profile;
    bool baseline = !h.ready || h.load != load.load_generation;
    bool gap = false, changed = baseline, replaced = baseline;
    unsigned changed_slots = 0;
    uintptr_t properties[8];
    uint32_t revisions[8];
    for (unsigned i = 0; i < 8; ++i) {
        uintptr_t source = word(context + 0x20 + i * 8);
        if ((!table(source, p.cue_table) && !table(source, p.cue_base_table)) ||
            source == retained || source == active) {
            if (h.ready) {
                h.ready = false;
                azn_hot_gap(d);
                azn_capability(d, AZN_CAP_HOT | AZN_CAP_HOT_LINK, 0);
            }
            return;
        }
        for (unsigned j = 0; j < i; ++j)
            if (properties[j] == source) {
                if (h.ready) {
                    h.ready = false;
                    azn_hot_gap(d);
                    azn_capability(d, AZN_CAP_HOT | AZN_CAP_HOT_LINK, 0);
                }
                return;
            }
        properties[i] = source;
        revisions[i] = unsigned(integer(source + 8));
        if (!baseline && (h.property[i] != source || h.revision[i] != revisions[i])) {
            changed = true;
            if (h.property[i] != source) replaced = true;
            changed_slots |= 1u << i;
            bool ordered_edit = edit && edit->property == source && edit->before_revision == h.revision[i] &&
                                revisions[i] == edit->before_revision + 2u;
            if (h.property[i] != source || (!ordered_edit && revisions[i] != h.revision[i] + 1u)) gap = true;
        }
    }
    uint64_t seed = azn_seed_id(d), cut = azn_cue_loss_cut(d, 1);
    if (!changed && h.seed == seed && h.loss_cut == cut) return;
    azn_hot_receipt packet {};
    auto &r = packet.base;
    const bool metadata = hot_metadata.load(std::memory_order_acquire);
    r.kind = AZN_HOT_CUES;
    r.deck = d;
    r.epoch = load.invalidation_epoch;
    r.unit = load.unit_generation;
    r.load_generation = load.load_generation;
    r.valid = 1;
    for (unsigned i = 0; i < 8; ++i) {
        azn_point pair[2];
        status_points(properties[i], pair);
        status_qualify_points(slot, properties[i], pair);
        r.hot[i].in = pair[0];
        r.hot[i].out = pair[1];
        r.hot[i].rgb = (byte(properties[i] + 0x10) << 16) | (byte(properties[i] + 0x11) << 8) | byte(properties[i] + 0x12);
        if (metadata) {
            status_hot_metadata(properties[i], r.hot[i], packet.details.comments[i]);
            if (pair[0].state == AZN_COORD) packet.details.stored_ms[i][0] = word(properties[i] + 0x68) / 1000;
            if (pair[1].state == AZN_COORD) packet.details.stored_ms[i][1] = word(properties[i] + 0xc0) / 1000;
        }
        if (pair[0].state == AZN_INVALID || pair[1].state == AZN_INVALID ||
            (pair[0].state != AZN_COORD && pair[1].state == AZN_COORD)) r.valid = 0;
    }
    /* A single +1 source revision on an unchanged owner cannot conceal an
     * A->B->A interval. Ignore non-semantic initialization (e.g. setting an
     * already identical slot color). If multiple real slot values changed
     * between cuts without their individual writers, their order is unknown. */
    unsigned semantic_changes = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (!(changed_slots & (1u << i))) continue;
        int in_key = compare_timestamps(h.details.stored_ms[i][0], packet.details.stored_ms[i][0], 1000, 1000, 1);
        int out_key = compare_timestamps(h.details.stored_ms[i][1], packet.details.stored_ms[i][1], 1000, 1000, 1);
        if (in_key == AZ_TIME_INVALID || out_key == AZ_TIME_INVALID) gap = true;
        if (!status_same_point(h.previous[i].in, r.hot[i].in) || !status_same_point(h.previous[i].out, r.hot[i].out) ||
            h.previous[i].rgb != r.hot[i].rgb || h.previous[i].reserved != r.hot[i].reserved ||
            in_key != AZ_TIME_EQUAL || out_key != AZ_TIME_EQUAL ||
            std::memcmp(h.details.comments[i], packet.details.comments[i], sizeof h.details.comments[i]))
            semantic_changes |= 1u << i;
    }
    if ((semantic_changes & (semantic_changes - 1u)) != 0) gap = true;
    r.b = int(changed_slots);
    r.c = int(semantic_changes);
    for (unsigned i = 0; i < 8; ++i)
        if (written == properties[i]) r.a = int(i + 1);
    azsdk_load_sample after {};
    if (!azsdk_load_read(int(d), &after) || after.invalidation_epoch != load.invalidation_epoch ||
        after.unit_generation != load.unit_generation || after.load_generation != load.load_generation || !status_source_loaded(d, slot, after)) return;
    if (replaced) {
        h.provider = new_hot_provider();
        if (!h.provider) {
            h.ready = false;
            azn_hot_gap(d);
            azn_capability(d, AZN_CAP_HOT | AZN_CAP_HOT_LINK, 0);
            return;
        }
    }
    if (replaced || gap || semantic_changes || cut != h.loss_cut) {
        if (h.serial == UINT64_MAX) {
            h.ready = false;
            azn_hot_gap(d);
            return;
        }
        ++h.serial;
    }
    r.attempt = h.provider;
    r.revision = h.serial;
    if (!h.ready) azn_capability(d, AZN_CAP_HOT | AZN_CAP_HOT_LINK, 1);
    h.ready = true;
    h.load = load.load_generation;
    h.seed = seed;
    h.loss_cut = cut;
    memcpy(h.property, properties, sizeof properties);
    memcpy(h.revision, revisions, sizeof revisions);
    memcpy(h.previous, r.hot, sizeof h.previous);
    h.details = packet.details;
    r.lost_topics = gap ? AZ_STATUS_TOPIC(AZ_STATUS_CUES) : 0;
    if (metadata) azn_emit_hot(&packet);
    else azn_emit(&r);
}

static int status_hot_edit(uintptr_t owner, uintptr_t property, uintptr_t position, uintptr_t option, uintptr_t color) {
    auto original = trampoline<int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(&hot_edit_original);
    Slot *slot = processing;
    uintptr_t retained, active;
    if (!slot || editing_hot || !hot_metadata.load(std::memory_order_acquire) || !status_properties(*slot, retained, active) ||
        property == retained || property == active || !status_hot_member(*slot, property))
        return original(owner, property, position, option, color);
    unsigned d = unsigned(slot - state().decks);
    azsdk_load_sample before {};
    if (!(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_CUES)) || !azsdk_load_read(int(d), &before))
        return original(owner, property, position, option, color);
    HotEdit edit {property, unsigned(integer(property + 8))};
    struct EditScope {
        explicit EditScope(uintptr_t address) { editing_hot = address; }
        ~EditScope() { editing_hot = 0; }
    };
    int result;
    {
        EditScope scope(property);
        result = original(owner, property, position, option, color);
    }
    status_hot_capture(d, *slot, property, &before, &edit);
    return result;
}
