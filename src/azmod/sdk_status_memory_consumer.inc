/* SPDX-License-Identifier: GPL-3.0-only */
/* Runs on the native reducer thread. A taken content token owns a reference
 * even if an intervening load or newer seed means its payload is obsolete. */
static const struct azn_memory_api *ds_memory_api(void) {
    const struct azstatus_native_v1 *api = atomic_load_explicit(&ds_native_api, memory_order_acquire);
    if (!api || api->size < offsetof(struct azstatus_native_v1, memory) + sizeof api->memory || !api->memory) return NULL;
    const struct azn_memory_api *m = api->memory();
    return m && m->size >= sizeof *m && m->version == 1 && m->read && m->release && m->seed ? m : NULL;
}
static uint64_t ds_mem_epoch[4], ds_mem_provider[4], ds_mem_revision[4], ds_mem_public_revision[4];
static void ds_native_memory(const struct azstatus_native_receipt *r) {
    unsigned d = r->deck;
    const struct azn_memory_api *m = ds_memory_api();
    if (!m) return;
    if (d < 4 && r->epoch == ds_native_fence[d] && r->unit && r->load_generation &&
        r->load_generation == ds_native_epoch[d] &&
        ((r->lost_topics && !r->attempt) || r->load_generation != ds_mem_epoch[d] || r->attempt > ds_mem_provider[d] ||
         (r->attempt == ds_mem_provider[d] && r->memory_serial >= ds_mem_revision[d]))) {
        struct azn_memory_rows values = {.size = sizeof values, .version = 1, .availability = (unsigned) r->valid};
        if (r->memory_pin && (!m->read(r->memory_pin, &values) || values.count > AZ_NATIVE_CUE_LIMIT)) {
            values.availability = AZ_AVAIL_STALE;
            values.count = 0;
        }
        int64_t ticks[AZ_NATIVE_CUE_LIMIT * 2];
        struct ds_time_axis clock = {0};
        int has_clock = values.clock_version == 1 || values.clock_version == 2;
        if (values.availability == AZ_AVAIL_READY && has_clock &&
            !ds_native_cue_clock(values.count, values.rows, values.points, ticks, &clock)) {
            values.availability = AZ_AVAIL_STALE;
            values.count = 0;
        }
        struct ds_field f = ds_native_field(r, AZ_STATUS_CUES, AZ_STATUS_CUES_LIVE_MEMORY, values.availability, values.rows);
        if (values.availability == AZ_AVAIL_READY && has_clock) {
            if (values.clock_version == 2) clock.stored_cue_ms = &values.stored_ms[0][0];
            f.clock = &clock;
        }
        f.info.provider_id = r->attempt;
        f.info.count = values.count;
        f.info.info.revision = r->memory_serial;
        struct ds_native_revision *edition = &ds_native_revisions[d][AZ_STATUS_CUES][AZ_STATUS_CUES_LIVE_MEMORY];
        uint64_t prior = edition->revision;
        ds_mem_public_revision[d] = 0;
        if (ds_native_stamp(d, &f, 1)) {
            if ((r->lost_topics || (r->attempt == ds_mem_provider[d] && r->memory_serial != ds_mem_revision[d])) &&
                prior == f.info.info.revision && prior < UINT64_MAX)
                f.info.info.revision = ++edition->revision;
            int rc = ds_publish(d, r->load_generation, DS_NATIVE_INPUT | (r->lost_topics ? DS_FIELD_LOSS : 0), r->observed_ns, &f, 1,
                                NULL, NULL, r->lost_topics, r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0);
            if (r->attempt) {
                ds_mem_epoch[d] = r->load_generation;
                ds_mem_provider[d] = r->attempt;
                ds_mem_revision[d] = r->memory_serial;
                if (rc == AZ_OK && values.availability == AZ_AVAIL_READY) ds_mem_public_revision[d] = f.info.info.revision;
            }
        }
    }
    if (r->memory_pin) m->release(r->memory_pin);
}
static void ds_native_memory_seed(unsigned deck) {
    const struct azn_memory_api *m = ds_memory_api();
    struct azstatus_native_receipt seed = {.size = sizeof seed, .version = 1};
    if (m && m->seed(deck, &seed)) ds_native_memory(&seed);
}
