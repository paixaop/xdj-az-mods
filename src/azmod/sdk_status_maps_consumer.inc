/* SPDX-License-Identifier: GPL-3.0-only */
/* Only the native reducer thread decodes pinned immutable native rows. */
static const struct azn_maps_api *ds_map_service(void) {
    const struct azstatus_native_v1 *api = atomic_load_explicit(&ds_native_api, memory_order_acquire);
    if (!api || api->size < offsetof(struct azstatus_native_v1, maps) + sizeof api->maps || !api->maps) return NULL;
    const struct azn_maps_api *map = api->maps();
    return map && map->size >= offsetof(struct azn_maps_api, release) + sizeof map->release &&
                   map->version == 1 && map->read && map->release
               ? map
               : NULL;
}
static int ds_map_rows(const struct azn_maps_api *api, const struct aznm_component *part,
                       unsigned kind, void **owned, unsigned *n) {
    *n = 0;
    *owned = NULL;
    if (!part->content || part->availability != AZ_AVAIL_READY) return part->availability;
    uint32_t count = 0;
    int rc = api->read(part, kind, NULL, 0, &count);
    if (rc != AZ_OK) return rc == AZ_LIMIT ? AZ_LIMIT : AZ_AVAIL_STALE;
    if (!count) return AZ_AVAIL_EMPTY;
    size_t width = kind == AZNM_GRID ? sizeof(az_beat) : sizeof(az_phrase);
    if (count > (kind == AZNM_GRID ? 32768u : 1024u)) return AZ_LIMIT;
    *owned = malloc(count * width);
    if (!*owned) return AZ_LIMIT;
    rc = api->read(part, kind, *owned, count, &count);
    if (rc != AZ_OK) return rc == AZ_LIMIT ? AZ_LIMIT : AZ_AVAIL_STALE;
    *n = count;
    return AZ_AVAIL_READY;
}
static void ds_native_map(const struct azstatus_native_receipt *r) {
    const struct azn_maps_api *api = ds_map_service();
    if (!api) return;
    const struct aznm_packet *p = &r->map;
    void *rows[2] = {NULL, NULL};
    int64_t *ticks = NULL;
    struct ds_time_axis clock = {0};
    unsigned d = r->deck;
    if (d < 4 && r->load_generation && r->load_generation == ds_native_epoch[d] &&
        r->epoch == ds_native_fence[d] && p->changed) {
        struct ds_field f[3];
        unsigned n = 0;
        uint64_t lost = r->lost_topics;
        unsigned cause = lost ? AZ_STATUS_CAPTURE_GAP : 0;
        const struct aznm_component *parts[2] = {&p->grid, &p->phrases};
        int availability[2] = {AZ_AVAIL_STALE, AZ_AVAIL_STALE};
        for (unsigned i = 0; i < 2; i++) {
            const struct aznm_component *part = parts[i];
            if (!part->provider || !part->revision) continue;
            unsigned count = 0;
            unsigned kind = i ? AZNM_PHRASES : AZNM_GRID;
            availability[i] = ds_map_rows(api, part, kind, &rows[i], &count);
            if (!i && availability[i] == AZ_AVAIL_READY &&
                api->size >= offsetof(struct azn_maps_api, read_samples) + sizeof api->read_samples && api->read_samples) {
                ticks = malloc(count * sizeof *ticks);
                if (!ticks) availability[i] = AZ_LIMIT;
                else {
                    uint32_t samples = 0, rate = 0;
                    int rc = api->read_samples(part, ticks, count, &samples, &rate);
                    if (rc != AZ_OK || samples != count || !rate || rate != part->sample_rate)
                        availability[i] = rc == AZ_LIMIT ? AZ_LIMIT : AZ_AVAIL_STALE;
                    else clock = (struct ds_time_axis) {rate, ticks, 1, rate, NULL};
                }
                if (availability[i] != AZ_AVAIL_READY) count = 0;
            }
            int limited = availability[i] == AZ_LIMIT;
            if (limited) availability[i] = AZ_AVAIL_STALE;
            unsigned t = i ? AZ_STATUS_PHRASES : AZ_STATUS_GRID;
            f[n++] = (struct ds_field) {t, i ? AZ_STATUS_PHRASES_CURRENT : AZ_STATUS_GRID_CURRENT, {.info = {availability[i], AZ_PROV_NATIVE, part->revision, r->observed_ns}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = r->load_generation, .provider_id = part->provider, .count = count}, rows[i], !i && clock.ticks ? &clock : NULL};
            if (limited) {
                lost |= AZ_STATUS_TOPIC(t) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
                cause |= AZ_STATUS_REVISION_LOST;
            }
        }
        az_status_grid_ref_v1 grid = {r->load_generation, p->grid.provider, p->grid.revision,
                                      AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
        int paired = p->owner && availability[0] == AZ_AVAIL_READY && availability[1] == AZ_AVAIL_READY;
        const struct azn_file_cue_api *file = atomic_load_explicit(&ds_file_cues, memory_order_acquire);
        if (paired && azfile_map_pair && file && file->size >= offsetof(struct azn_file_cue_api, map_hold) + sizeof file->map_hold &&
            file->map_origins && file->map_hold) {
            uint64_t tokens[3] = {0};
            if (file->map_origins(p, tokens)) {
                azfile_map_pair(d, r->load_generation, tokens);
                file->map_hold(tokens, 3, 0);
            }
        }
        f[n++] = (struct ds_field) {AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID, {.info = {paired ? AZ_AVAIL_READY : AZ_AVAIL_STALE, AZ_PROV_NATIVE, p->owner, r->observed_ns}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = r->load_generation, .provider_id = p->owner, .count = paired}, &grid, NULL};
        struct ds_path path = {.kind = DS_PATH_BASELINE, .order = r->motion_order, .generation = r->motion_generation};
        ds_native_windows(&path, r);
        ds_native_map_stamp(&path, r);
        if (ds_native_coord(&r->points[1], &path.to)) {
            path.endpoints = AZ_STATUS_OCCURRENCE_TO;
            path.opening = !lost && r->size >= offsetof(struct azstatus_native_receipt, motion_flags) + sizeof r->motion_flags &&
                           r->motion_flags == AZN_AT_OPENING;
        }
        unsigned flags = DS_NATIVE_INPUT | (lost ? DS_FIELD_LOSS : 0);
        if (path.order) ds_native_path(&path, d, r->load_generation, flags, r->observed_ns, f, n, NULL, lost, cause);
        else ds_publish(d, r->load_generation, flags, r->observed_ns, f, n, NULL, NULL, lost, cause);
    }
    free(rows[0]);
    free(rows[1]);
    free(ticks);
    api->release(p);
}
