/* SPDX-License-Identifier: GPL-3.0-only */
/* Runs on the native reducer thread. A taken content token owns a reference
 * even if an intervening load or newer seed means its payload is obsolete. */
static int ds_memory_api_valid(const struct azn_memory_api *memory) {
    return memory && memory->size >= sizeof *memory && memory->version == 1 && memory->read && memory->release && memory->seed;
}
static const struct azn_memory_api *ds_memory_api(void) {
    const struct azstatus_native_v1 *api = atomic_load_explicit(&ds_native_api, memory_order_acquire);
    if (!api || api->size < offsetof(struct azstatus_native_v1, memory) + sizeof api->memory || !api->memory) return NULL;
    const struct azn_memory_api *memory = api->memory();
    return ds_memory_api_valid(memory) ? memory : NULL;
}
static uint64_t ds_mem_epoch[4], ds_mem_provider[4], ds_mem_revision[4], ds_mem_public_revision[4];
static int ds_memory_revision_current(const struct azstatus_native_receipt *r) {
    unsigned d = r->deck;
    return (r->lost_topics && !r->attempt) || r->load_generation != ds_mem_epoch[d] || r->attempt > ds_mem_provider[d] ||
           (r->attempt == ds_mem_provider[d] && r->memory_serial >= ds_mem_revision[d]);
}
static int ds_memory_recover_edition(const struct azstatus_native_receipt *r) {
    return r->lost_topics || (r->attempt == ds_mem_provider[r->deck] && r->memory_serial != ds_mem_revision[r->deck]);
}
static void ds_memory_confirm_edition(const struct azstatus_native_receipt *r, const struct ds_field *field, int result) {
    if (!r->attempt) return;
    unsigned d = r->deck;
    ds_mem_epoch[d] = r->load_generation;
    ds_mem_provider[d] = r->attempt;
    ds_mem_revision[d] = r->memory_serial;
    if (result == AZ_OK && field->info.info.availability == AZ_AVAIL_READY) ds_mem_public_revision[d] = field->info.info.revision;
}
static void ds_memory_publish(const struct azstatus_native_receipt *r, struct ds_field *field) {
    unsigned d = r->deck;
    struct ds_native_revision *edition = &ds_native_revisions[d][AZ_STATUS_CUES][AZ_STATUS_CUES_LIVE_MEMORY];
    uint64_t prior = edition->revision;
    ds_mem_public_revision[d] = 0;
    if (!ds_native_stamp(d, field, 1)) return;
    if (ds_memory_recover_edition(r) && prior == field->info.info.revision && prior < UINT64_MAX) field->info.info.revision = ++edition->revision;
    int rc = ds_publish(d, r->load_generation, DS_NATIVE_INPUT | (r->lost_topics ? DS_FIELD_LOSS : 0), r->observed_ns, field, 1,
                        NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
    ds_memory_confirm_edition(r, field, rc);
}
static int ds_memory_clock(struct azn_memory_rows *values, int64_t *ticks, struct ds_time_axis *clock) {
    int has_clock = values->clock_version == 1 || values->clock_version == 2;
    if (values->availability != AZ_AVAIL_READY || !has_clock) return 0;
    if (!ds_native_cue_clock(values->count, values->rows, values->points, ticks, clock)) {
        values->availability = AZ_AVAIL_STALE;
        values->count = 0;
        return 0;
    }
    if (values->clock_version == 2) clock->stored_cue_ms = &values->stored_ms[0][0];
    return 1;
}
static void ds_memory_receipt(const struct azstatus_native_receipt *r, const struct azn_memory_api *memory) {
    struct azn_memory_rows values = {.size = sizeof values, .version = 1, .availability = (unsigned) r->valid};
    if (r->memory_pin && (!memory->read(r->memory_pin, &values) || values.count > AZ_NATIVE_CUE_LIMIT)) {
        values.availability = AZ_AVAIL_STALE;
        values.count = 0;
    }
    int64_t ticks[AZ_NATIVE_CUE_LIMIT * 2];
    struct ds_time_axis clock = {0};
    int has_clock = ds_memory_clock(&values, ticks, &clock);
    struct ds_field field = ds_native_field(r, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY, values.availability, values.rows);
    if (has_clock) field.clock = &clock;
    field.info.provider_id = r->attempt;
    field.info.count = values.count;
    field.info.info.revision = r->memory_serial;
    ds_memory_publish(r, &field);
}
static void ds_native_memory(const struct azstatus_native_receipt *r) {
    const struct azn_memory_api *memory = ds_memory_api();
    if (!memory) return;
    if (r->deck < 4 && ds_native_loaded(r) && r->unit && ds_memory_revision_current(r)) ds_memory_receipt(r, memory);
    if (r->memory_pin) memory->release(r->memory_pin);
}
static void ds_native_memory_seed(unsigned deck) {
    const struct azn_memory_api *m = ds_memory_api();
    struct azstatus_native_receipt seed = {.size = sizeof seed, .version = 1};
    if (m && m->seed(deck, &seed)) ds_native_memory(&seed);
}
