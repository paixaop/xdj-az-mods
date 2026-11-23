/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by the command worker. The public bank owns one record and deck
 * barrier. Each native action has its own delivery reservation and receipt. */
#include "../common/azcuetext.h"

#define SDK_BATCH_WRITES ((UINT64_C(1) << AZ_CMD_CUE_POINT_SET) | \
                          (UINT64_C(1) << AZ_CMD_CUE_LOOP_SET) | \
                          (UINT64_C(1) << AZ_CMD_HOT_CLEAR) | \
                          (UINT64_C(1) << AZ_CMD_MEMORY_DELETE))

static int sdk_batch_envelope(const az_cue_batch_request *plan) {
    return plan && plan->size >= sizeof *plan && plan->version == AZ_DATA_VERSION &&
           plan->deck < SCMD_DECKS && !plan->reserved && plan->count && plan->count <= AZ_CUE_BATCH_LIMIT;
}
static int sdk_batch_identity(const az_cue_batch_request *plan) {
    return plan->request_id && plan->expected_load_generation && plan->expected_revision &&
           plan->deadline_ns && (plan->mode == AZ_CUE_BATCH_IF_EMPTY || plan->mode == AZ_CUE_BATCH_REPLACE);
}
static int sdk_batch_comment(const char *text) {
    if (!memchr(text, 0, sizeof(((az_cue_batch_entry *) 0)->comment))) return 0;
    char copy[128];
    azcue_copy_utf8_prefix(copy, text);
    return !strcmp(copy, text);
}
static int sdk_batch_color(const az_cue_batch_entry *entry) {
    if (entry->classification == AZ_CUE_HOT) return entry->rgb <= 0xffffffu;
    static const uint32_t palette[] = {0, 0xff00c6, 0xff0000, 0xff7100, 0xffff00,
                                       0x00e300, 0x00c3ff, 0x0051ff, 0x9c08ff};
    for (unsigned i = 0; i < sizeof palette / sizeof *palette; ++i)
        if (entry->rgb == palette[i]) return 1;
    return 0;
}
static int sdk_batch_entry_shape(const az_cue_batch_entry *entry) {
    return (entry->type == AZ_CUE_POINT || entry->type == AZ_CUE_LOOP) &&
           sdk_batch_color(entry) && sdk_batch_comment(entry->comment);
}
static int sdk_batch_mark_slot(const az_cue_batch_entry *entry, unsigned *hot, unsigned *memory) {
    if (entry->classification == AZ_CUE_MEMORY) return !entry->slot && ++*memory <= 10;
    if (entry->classification != AZ_CUE_HOT || !entry->slot || entry->slot > 8) return 0;
    unsigned bit = 1u << entry->slot;
    if (*hot & bit) return 0;
    *hot |= bit;
    return 1;
}
static int sdk_batch_entries(const az_cue_batch_request *plan) {
    unsigned hot = 0, memory = 0;
    for (unsigned i = 0; i < plan->count; ++i) {
        const az_cue_batch_entry *entry = &plan->entries[i];
        if (!sdk_batch_mark_slot(entry, &hot, &memory) || !sdk_batch_entry_shape(entry)) return 0;
    }
    return 1;
}
static int sdk_batch_endpoint(double seconds, uint32_t rate) {
    int start = compare_timestamps(seconds, 0., 1, rate, 1);
    int frame = compare_timestamps(round(seconds * rate), 9007199254740992., rate, rate, 1);
    int usec = compare_timestamps(seconds * 1000000., 9007199254740992., 1000000, rate, 1);
    return start != AZ_TIME_INVALID && start != AZ_TIME_BEFORE &&
           frame != AZ_TIME_INVALID && frame == AZ_TIME_BEFORE &&
           usec != AZ_TIME_INVALID && usec == AZ_TIME_BEFORE;
}
static int sdk_batch_times_valid(const az_cue_batch_entry *entry, uint32_t rate) {
    if (!sdk_batch_endpoint(entry->position_s, rate)) return 0;
    if (entry->type == AZ_CUE_LOOP) {
        int end = compare_timestamps(entry->end_s, entry->position_s, 1, rate, 1);
        return end != AZ_TIME_INVALID && end == AZ_TIME_AFTER && sdk_batch_endpoint(entry->end_s, rate);
    }
    int end = compare_timestamps(entry->end_s, 0., 1, rate, 1);
    return end != AZ_TIME_INVALID && end == AZ_TIME_EQUAL;
}
static double sdk_batch_frame(double seconds, uint32_t rate) {
    return fmax(0., round(seconds * rate));
}
static int sdk_batch_distinct_memory(double incoming, double saved, uint32_t rate) {
    double saved_frame = sdk_batch_frame(saved, rate);
    int original = compare_timestamps(incoming, saved_frame / rate, 1, rate, 1);
    int rounded = compare_timestamps(sdk_batch_frame(incoming, rate), saved_frame, rate, rate, 1);
    return original != AZ_TIME_INVALID && original != AZ_TIME_EQUAL &&
           rounded != AZ_TIME_INVALID && rounded != AZ_TIME_EQUAL;
}
static int sdk_batch_memory_unique(const az_cue_batch_request *plan, unsigned index, uint32_t rate) {
    const az_cue_batch_entry *entry = &plan->entries[index];
    if (entry->classification != AZ_CUE_MEMORY) return 1;
    for (unsigned i = 0; i < index; ++i) {
        const az_cue_batch_entry *other = &plan->entries[i];
        if (other->classification != AZ_CUE_MEMORY) continue;
        if (!sdk_batch_distinct_memory(entry->position_s, other->position_s, rate)) return 0;
    }
    return 1;
}
static int sdk_batch_validate_times(const az_cue_batch_request *plan, uint32_t rate) {
    if (!rate) return AZ_UNSUPPORTED;
    for (unsigned i = 0; i < plan->count; ++i)
        if (!sdk_batch_times_valid(&plan->entries[i], rate) || !sdk_batch_memory_unique(plan, i, rate)) return AZ_INVALID;
    return AZ_OK;
}
static int sdk_batch_removable(const az_native_cue_snapshot *snapshot) {
    for (unsigned i = 0; i < snapshot->count; ++i) {
        const az_cue_entry *entry = &snapshot->entries[i];
        if (entry->classification == AZ_CUE_MEMORY && entry->entry_id) continue;
        if (entry->classification == AZ_CUE_HOT && entry->slot && entry->slot <= 8) continue;
        return 0;
    }
    return 1;
}
static int sdk_batch_validate_bank(const az_cue_batch_request *plan, const az_native_cue_snapshot *snapshot) {
    if (plan->mode == AZ_CUE_BATCH_IF_EMPTY && snapshot->count) return AZ_STALE;
    if (!sdk_batch_removable(snapshot)) return AZ_UNSUPPORTED;
    return sdk_batch_validate_times(plan, snapshot->source_rate_hz);
}
static void sdk_batch_delete(az_command_request *request, const az_cue_entry *entry) {
    request->classification = entry->classification;
    request->operation = entry->classification == AZ_CUE_HOT ? AZ_CMD_HOT_CLEAR : AZ_CMD_MEMORY_DELETE;
    request->entry_id = entry->classification == AZ_CUE_HOT ? entry->slot : entry->entry_id;
}
static void sdk_batch_create(az_command_request *request, const az_cue_batch_entry *entry) {
    request->operation = entry->type == AZ_CUE_LOOP ? AZ_CMD_CUE_LOOP_SET : AZ_CMD_CUE_POINT_SET;
    request->classification = entry->classification;
    request->entry_id = entry->slot;
    request->position_s = entry->position_s;
    request->end_s = entry->end_s;
    request->rgb = entry->rgb;
    memcpy(request->comment, entry->comment, sizeof request->comment);
}
static int sdk_batch_snapshot_current(const struct scmd_cue_batch *batch, const az_native_cue_snapshot *snapshot) {
    return snapshot->size >= sizeof *snapshot && snapshot->version == AZ_DATA_VERSION &&
           snapshot->deck == batch->plan.deck && snapshot->native_load_generation == batch->plan.expected_load_generation &&
           snapshot->revision == batch->revision && snapshot->count <= AZ_NATIVE_CUE_LIMIT;
}
static int sdk_batch_step(const struct azexit_service *api, const struct scmd_cue_batch *batch,
                          az_command_request *request) {
    az_native_cue_snapshot snapshot = {.size = sizeof snapshot};
    int rc = api->native_cues(request->deck, &snapshot);
    if (rc != AZ_OK) return rc;
    if (!sdk_batch_snapshot_current(batch, &snapshot)) return AZ_STALE;
    if (!batch->changed) {
        rc = sdk_batch_validate_bank(&batch->plan, &snapshot);
        if (rc != AZ_OK) return rc;
    }
    request->expected_revision = batch->revision;
    if (!batch->created && snapshot.count) sdk_batch_delete(request, &snapshot.entries[0]);
    else sdk_batch_create(request, &batch->plan.entries[batch->created]);
    return AZ_OK;
}
