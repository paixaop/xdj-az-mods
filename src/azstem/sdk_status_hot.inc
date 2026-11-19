/* SPDX-License-Identifier: GPL-3.0-only */
/* Active uintptr_t-H slots are indexed CueContext pointer members. The owner and
 * validated property types are shared with prepare_cue's hot edit path.
 * No retained native object escapes the frame. One commit pins all 8 slots. */
static _Atomic(uint64_t) hot_providers = 0;
static _Atomic(bool) hot_metadata = false;
static void *hot_edit_original;
struct HotEdit {
    uintptr_t property;
    uint32_t before_revision;
};
static _Thread_local uintptr_t editing_hot;
static uint64_t new_hot_provider() {
    const uint64_t prefix = AZSP_HOT;
    uint64_t id = atomic_load_explicit(&(hot_providers), memory_order_relaxed);
    for (unsigned n = 0; n < 8 && id < AZSP_SERIAL_MAX; ++n)
        if (atomic_compare_exchange_strong_explicit(&(hot_providers), &(id), id + 1, memory_order_relaxed, memory_order_relaxed))
            return prefix | (id + 1);
    return 0;
}
static bool status_hot_member(Slot *slot, uintptr_t property) {
    uintptr_t context = word(slot->facade + 0x68);
    for (unsigned i = 0; i < 8; ++i)
        if (word(context + 0x20 + i * 8) == property)
            return table(property, state().profile.cue_table) || table(property, state().profile.cue_base_table);
    return false;
}
static const char *status_hot_comment(uintptr_t property) {
    /* The owned CueProperty retains CueComment, which owns a shared GNU string.
     * Copy while the player still owns the property and keep no native pointer. */
    uintptr_t comment = word(property + 0xe8);
    if (!pointer(comment) || !pointer(word(property + 0xf0))) return NULL;
    uintptr_t string = word(comment);
    if (!pointer(string) || !pointer(word(comment + 8)) || !word(string + 8)) return NULL;
    uintptr_t bytes = word(string);
    if (bytes < 0x10000 || bytes > UINT64_C(0x0000ffffffffffff)) return NULL;
    return (const char *) bytes;
}
static void status_hot_metadata(uintptr_t property, azn_hot_slot *row, char out[128]) {
    if (row->in.state != AZN_COORD) return;
    unsigned automatic = byte(property + 0xe1);
    if (row->out.state == AZN_COORD && automatic <= 1)
        row->reserved |= AZN_HOT_AUTO_ACTIVE | (automatic ? AZN_HOT_AUTO_ON : 0);
    const char *bytes = status_hot_comment(property);
    if (!bytes) return;
    azcue_copy_utf8_prefix(out, ((const char *) (bytes)));
    if (out[0]) row->reserved |= AZN_HOT_COMMENT;
}
struct HotCapture {
    azsdk_load_sample load;
    uintptr_t retained, active, properties[8];
    uint32_t revisions[8];
    uint64_t seed, cut;
    bool baseline, gap, changed, replaced, metadata;
    unsigned changed_slots, semantic_changes;
};
static void status_hot_disable(unsigned d, HotState *h) {
    h->ready = false;
    azn_hot_gap(d);
    azn_capability(d, AZN_CAP_HOT | AZN_CAP_HOT_LINK, 0);
}
static void status_hot_invalidate(unsigned d, HotState *h) {
    if (h->ready) status_hot_disable(d, h);
}
static bool status_hot_load(unsigned d, const azsdk_load_sample *before, azsdk_load_sample *load) {
    if (!azsdk_load_read((int) d, load)) return false;
    return !before || (load->invalidation_epoch == before->invalidation_epoch && load->load_generation == before->load_generation &&
                       load->unit_generation == before->unit_generation);
}
static bool status_hot_owners(unsigned d, Slot *slot, struct HotCapture *capture) {
    HotState *h = &slot->hot;
    if (!status_properties(slot, &capture->retained, &capture->active)) {
        status_hot_invalidate(d, h);
        return false;
    }
    if (!status_source_loaded(d, slot, capture->load)) {
        if (h->ready && h->load == capture->load.load_generation) {
            h->ready = false;
            /* The load withdrawal covers a revoked bank with no replacement.
             * A same-epoch mismatch still loses history. */
            if (!azn_load_history_revoked(d, capture->load.invalidation_epoch)) azn_hot_gap(d);
        }
        return false;
    }
    return true;
}
static bool status_hot_property_valid(uintptr_t source, const struct HotCapture *capture) {
    __auto_type p = &state().profile;
    return (table(source, p->cue_table) || table(source, p->cue_base_table)) && source != capture->retained && source != capture->active;
}
static bool status_hot_property_unique(const struct HotCapture *capture, unsigned index, uintptr_t source) {
    for (unsigned j = 0; j < index; ++j)
        if (capture->properties[j] == source) return false;
    return true;
}
static bool status_hot_ordered_edit(const HotEdit *edit, uintptr_t source, uint32_t before, uint32_t after) {
    return edit && edit->property == source && edit->before_revision == before && after == edit->before_revision + 2u;
}
static void status_hot_revision(const HotState *h, struct HotCapture *capture, unsigned i, const HotEdit *edit) {
    uintptr_t source = capture->properties[i];
    uint32_t revision = capture->revisions[i];
    if (capture->baseline || (h->property[i] == source && h->revision[i] == revision)) return;
    capture->changed = true;
    if (h->property[i] != source) capture->replaced = true;
    capture->changed_slots |= 1u << i;
    bool ordered_edit = status_hot_ordered_edit(edit, source, h->revision[i], revision);
    if (h->property[i] != source || (!ordered_edit && revision != h->revision[i] + 1u)) capture->gap = true;
}
static bool status_hot_read_bank(unsigned d, Slot *slot, const HotEdit *edit, struct HotCapture *capture) {
    HotState *h = &slot->hot;
    const uintptr_t context = word(slot->facade + 0x68);
    capture->baseline = !h->ready || h->load != capture->load.load_generation;
    capture->changed = capture->replaced = capture->baseline;
    for (unsigned i = 0; i < 8; ++i) {
        uintptr_t source = word(context + 0x20 + i * 8);
        if (!status_hot_property_valid(source, capture) || !status_hot_property_unique(capture, i, source)) {
            status_hot_invalidate(d, h);
            return false;
        }
        capture->properties[i] = source;
        capture->revisions[i] = (unsigned) integer(source + 8);
        status_hot_revision(h, capture, i, edit);
    }
    capture->seed = azn_seed_id(d);
    capture->cut = azn_cue_loss_cut(d, 1);
    return true;
}
static bool status_hot_unchanged(const HotState *h, const struct HotCapture *capture) {
    return !capture->changed && h->seed == capture->seed && h->loss_cut == capture->cut;
}
static bool status_hot_points_valid(const azn_point pair[2]) {
    return pair[0].state != AZN_INVALID && pair[1].state != AZN_INVALID && (pair[0].state == AZN_COORD || pair[1].state != AZN_COORD);
}
static void status_hot_row(Slot *slot, uintptr_t property, unsigned i, bool metadata, azn_hot_receipt *packet) {
    azn_point pair[2];
    status_points(property, pair);
    status_qualify_points(slot, property, pair);
    azn_hot_slot *row = &packet->base.hot[i];
    row->in = pair[0];
    row->out = pair[1];
    row->rgb = (byte(property + 0x10) << 16) | (byte(property + 0x11) << 8) | byte(property + 0x12);
    if (metadata) {
        status_hot_metadata(property, row, packet->details.comments[i]);
        if (pair[0].state == AZN_COORD) packet->details.stored_ms[i][0] = word(property + 0x68) / 1000;
        if (pair[1].state == AZN_COORD) packet->details.stored_ms[i][1] = word(property + 0xc0) / 1000;
    }
    if (!status_hot_points_valid(pair)) packet->base.valid = 0;
}
static void status_hot_packet(unsigned d, Slot *slot, struct HotCapture *capture, azn_hot_receipt *packet) {
    capture->metadata = atomic_load_explicit(&(hot_metadata), memory_order_acquire);
    azstatus_native_receipt *r = &packet->base;
    r->kind = AZN_HOT_CUES;
    r->deck = d;
    r->epoch = capture->load.invalidation_epoch;
    r->unit = capture->load.unit_generation;
    r->load_generation = capture->load.load_generation;
    r->valid = 1;
    for (unsigned i = 0; i < 8; ++i) status_hot_row(slot, capture->properties[i], i, capture->metadata, packet);
}
static bool status_hot_row_equal(const HotState *h, const azn_hot_receipt *packet, unsigned i, int in_key, int out_key) {
    const azn_hot_slot *row = &packet->base.hot[i];
    return status_same_point(h->previous[i].in, row->in) && status_same_point(h->previous[i].out, row->out) &&
           h->previous[i].rgb == row->rgb && h->previous[i].reserved == row->reserved &&
           in_key == AZ_TIME_EQUAL && out_key == AZ_TIME_EQUAL &&
           !memcmp(h->details.comments[i], packet->details.comments[i], sizeof h->details.comments[i]);
}
static bool status_hot_semantic_change(const HotState *h, const azn_hot_receipt *packet, unsigned i, bool *gap) {
    int in_key = compare_timestamps(h->details.stored_ms[i][0], packet->details.stored_ms[i][0], 1000, 1000, 1);
    int out_key = compare_timestamps(h->details.stored_ms[i][1], packet->details.stored_ms[i][1], 1000, 1000, 1);
    if (in_key == AZ_TIME_INVALID || out_key == AZ_TIME_INVALID) *gap = true;
    return !status_hot_row_equal(h, packet, i, in_key, out_key);
}
static void status_hot_changes(const HotState *h, struct HotCapture *capture, azn_hot_receipt *packet, uintptr_t written) {
    /* A single +1 source revision on an unchanged owner cannot conceal an
     * A/B/A interval. Ignore non-semantic initialization such as identical
     * slot color. Multiple real changes without writers have unknown order. */
    for (unsigned i = 0; i < 8; ++i) {
        if (!(capture->changed_slots & (1u << i))) continue;
        if (status_hot_semantic_change(h, packet, i, &capture->gap)) capture->semantic_changes |= 1u << i;
    }
    if ((capture->semantic_changes & (capture->semantic_changes - 1u)) != 0) capture->gap = true;
    packet->base.b = (int) capture->changed_slots;
    packet->base.c = (int) capture->semantic_changes;
    for (unsigned i = 0; i < 8; ++i)
        if (written == capture->properties[i]) packet->base.a = (int) (i + 1);
}
static bool status_hot_capture_current(unsigned d, Slot *slot, const azsdk_load_sample *load) {
    azsdk_load_sample after = {0};
    return azsdk_load_read((int) d, &after) && after.invalidation_epoch == load->invalidation_epoch &&
           after.unit_generation == load->unit_generation && after.load_generation == load->load_generation && status_source_loaded(d, slot, after);
}
static bool status_hot_advance(unsigned d, HotState *h, const struct HotCapture *capture) {
    if (capture->replaced) {
        h->provider = new_hot_provider();
        if (!h->provider) {
            status_hot_disable(d, h);
            return false;
        }
    }
    if (capture->replaced || capture->gap || capture->semantic_changes || capture->cut != h->loss_cut) {
        if (h->serial == UINT64_MAX) {
            h->ready = false;
            azn_hot_gap(d);
            return false;
        }
        ++h->serial;
    }
    return true;
}
static void status_hot_commit(unsigned d, HotState *h, const struct HotCapture *capture, azn_hot_receipt *packet) {
    packet->base.attempt = h->provider;
    packet->base.revision = h->serial;
    if (!h->ready) azn_capability(d, AZN_CAP_HOT | AZN_CAP_HOT_LINK, 1);
    h->ready = true;
    h->load = capture->load.load_generation;
    h->seed = capture->seed;
    h->loss_cut = capture->cut;
    memcpy(h->property, capture->properties, sizeof capture->properties);
    memcpy(h->revision, capture->revisions, sizeof capture->revisions);
    memcpy(h->previous, packet->base.hot, sizeof h->previous);
    h->details = packet->details;
    packet->base.lost_topics = capture->gap ? AZ_STATUS_TOPIC(AZ_STATUS_CUES) : 0;
    if (capture->metadata) azn_emit_hot(packet);
    else azn_emit(&packet->base);
}
static void status_hot_collect(unsigned d, Slot *slot, uintptr_t written, const azsdk_load_sample *before, const HotEdit *edit) {
    struct HotCapture capture = {0};
    if (!status_hot_load(d, before, &capture.load) || !status_hot_owners(d, slot, &capture)) return;
    if (!status_hot_read_bank(d, slot, edit, &capture) || status_hot_unchanged(&slot->hot, &capture)) return;
    azn_hot_receipt packet = {0};
    status_hot_packet(d, slot, &capture, &packet);
    status_hot_changes(&slot->hot, &capture, &packet, written);
    if (!status_hot_capture_current(d, slot, &capture.load) || !status_hot_advance(d, &slot->hot, &capture)) return;
    status_hot_commit(d, &slot->hot, &capture, &packet);
}
static void status_hot_capture(unsigned d, Slot *slot, uintptr_t written, const azsdk_load_sample *before, const HotEdit *edit) {
    if (editing_hot || !(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_CUES))) return;
    HotState *h = &slot->hot;
    if (atomic_flag_test_and_set_explicit(&(h->busy), memory_order_acquire)) {
        azn_hot_gap(d);
        return;
    }
    EXIT_FLAG_GUARD(guard, &h->busy);
    status_hot_collect(d, slot, written, before, edit);
}

typedef struct HotEditScope HotEditScope;
struct HotEditScope {
    unsigned char reserved;
};
static void hot_edit_scope_init(HotEditScope *ctx, uintptr_t address);
static void hot_edit_scope_drop(HotEditScope *ctx);
static void hot_edit_scope_init(HotEditScope *ctx, uintptr_t address) {
    (void) ctx;
    *ctx = (HotEditScope) {.reserved = 0};
    editing_hot = address;
}
static void hot_edit_scope_drop(HotEditScope *ctx) {
    (void) ctx;
    editing_hot = 0;
}

static bool status_hot_edit_candidate(Slot *slot, uintptr_t property) {
    uintptr_t retained, active;
    return slot && !editing_hot && atomic_load_explicit(&(hot_metadata), memory_order_acquire) && status_properties(slot, &retained, &active) &&
           property != retained && property != active && status_hot_member(slot, property);
}
static int status_hot_edit(uintptr_t owner, uintptr_t property, uintptr_t position, uintptr_t option, uintptr_t color) {
    __auto_type original = ((int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)) trampoline_address(&hot_edit_original));
    Slot *slot = processing;
    if (!status_hot_edit_candidate(slot, property)) return original(owner, property, position, option, color);
    unsigned d = (unsigned) (slot - state().decks);
    azsdk_load_sample before = {0};
    if (!(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_CUES)) || !azsdk_load_read((int) (d), &before))
        return original(owner, property, position, option, color);
    HotEdit edit = {property, (unsigned) (integer(property + 8))};

    int result;
    {
        HotEditScope scope __attribute__((cleanup(hot_edit_scope_drop)));
        hot_edit_scope_init(&scope, property);
        result = original(owner, property, position, option, color);
    }
    status_hot_capture(d, &(*slot), property, &before, &edit);
    return result;
}
