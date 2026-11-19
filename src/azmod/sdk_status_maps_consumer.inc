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
static void *ds_map_allocate(unsigned kind, unsigned count) {
    size_t width = kind == AZNM_GRID ? sizeof(az_beat) : sizeof(az_phrase);
    if (count > (kind == AZNM_GRID ? 32768u : 1024u)) return NULL;
    return malloc(count * width);
}
static int ds_map_payload(const struct azn_maps_api *api, const struct aznm_component *part, unsigned kind, void *owned, unsigned count, unsigned *n) {
    int rc = api->read(part, kind, owned, count, &count);
    if (rc != AZ_OK) return rc == AZ_LIMIT ? AZ_LIMIT : AZ_AVAIL_STALE;
    *n = count;
    return AZ_AVAIL_READY;
}
static int ds_map_rows(const struct azn_maps_api *api, const struct aznm_component *part, unsigned kind, void **owned, unsigned *n) {
    *n = 0;
    *owned = NULL;
    if (!part->content || part->availability != AZ_AVAIL_READY) return part->availability;
    uint32_t count = 0;
    int rc = api->read(part, kind, NULL, 0, &count);
    if (rc != AZ_OK) return rc == AZ_LIMIT ? AZ_LIMIT : AZ_AVAIL_STALE;
    if (!count) return AZ_AVAIL_EMPTY;
    *owned = ds_map_allocate(kind, count);
    if (!*owned) return AZ_LIMIT;
    return ds_map_payload(api, part, kind, *owned, count, n);
}
struct ds_map_receipt {
    void *rows[2];
    int64_t *ticks;
    struct ds_time_axis clock;
    struct ds_field fields[3];
    unsigned count, cause;
    uint64_t lost;
    int availability[2];
};
static int ds_map_samples(const struct azn_maps_api *api, const struct aznm_component *part, unsigned count, struct ds_map_receipt *map) {
    map->ticks = malloc(count * sizeof *map->ticks);
    if (!map->ticks) return AZ_LIMIT;
    uint32_t samples = 0, rate = 0;
    int rc = api->read_samples(part, map->ticks, count, &samples, &rate);
    if (rc != AZ_OK || samples != count || !rate || rate != part->sample_rate) return rc == AZ_LIMIT ? AZ_LIMIT : AZ_AVAIL_STALE;
    map->clock = (struct ds_time_axis) {rate, map->ticks, 1, rate, NULL};
    return AZ_AVAIL_READY;
}
static void ds_map_part_clock(const struct azn_maps_api *api, const struct aznm_component *part, unsigned i, unsigned *count, struct ds_map_receipt *map) {
    if (i || map->availability[i] != AZ_AVAIL_READY || api->size < offsetof(struct azn_maps_api, read_samples) + sizeof api->read_samples || !api->read_samples) return;
    map->availability[i] = ds_map_samples(api, part, *count, map);
    if (map->availability[i] != AZ_AVAIL_READY) *count = 0;
}
static struct ds_field ds_map_part_field(const struct azstatus_native_receipt *r, const struct aznm_component *part,
                                         unsigned topic, unsigned i, unsigned count, const struct ds_map_receipt *map) {
    return (struct ds_field) {topic, i ? AZ_STATUS_PHRASES_CURRENT : AZ_STATUS_GRID_CURRENT, {.info = {map->availability[i], AZ_PROV_NATIVE, part->revision, r->observed_ns}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = r->load_generation, .provider_id = part->provider, .count = count}, map->rows[i], !i && map->clock.ticks ? &map->clock : NULL};
}
static void ds_map_part(const struct azstatus_native_receipt *r, const struct azn_maps_api *api, unsigned i, struct ds_map_receipt *map) {
    const struct aznm_component *part = i ? &r->map.phrases : &r->map.grid;
    if (!part->provider || !part->revision) return;
    unsigned count = 0;
    map->availability[i] = ds_map_rows(api, part, i ? AZNM_PHRASES : AZNM_GRID, &map->rows[i], &count);
    ds_map_part_clock(api, part, i, &count, map);
    int limited = map->availability[i] == AZ_LIMIT;
    if (limited) map->availability[i] = AZ_AVAIL_STALE;
    unsigned topic = i ? AZ_STATUS_PHRASES : AZ_STATUS_GRID;
    map->fields[map->count++] = ds_map_part_field(r, part, topic, i, count, map);
    if (limited) {
        map->lost |= AZ_STATUS_TOPIC(topic) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASE_TRANSITION);
        map->cause |= AZ_STATUS_REVISION_LOST;
    }
}
static void ds_map_file_pair(const struct azstatus_native_receipt *r) {
    const struct azn_file_cue_api *file = atomic_load_explicit(&ds_file_cues, memory_order_acquire);
    if (!azfile_map_pair || !file || file->size < offsetof(struct azn_file_cue_api, map_hold) + sizeof file->map_hold || !file->map_origins || !file->map_hold) return;
    uint64_t tokens[3] = {0};
    if (file->map_origins(&r->map, tokens)) {
        azfile_map_pair(r->deck, r->load_generation, tokens);
        file->map_hold(tokens, 3, 0);
    }
}
static void ds_map_path(const struct azstatus_native_receipt *r, uint64_t lost, struct ds_path *path) {
    *path = (struct ds_path) {.kind = DS_PATH_BASELINE, .order = r->motion_order, .generation = r->motion_generation};
    ds_native_windows(path, r);
    ds_native_map_stamp(path, r);
    if (ds_native_coord(&r->points[1], &path->to)) {
        path->endpoints = AZ_STATUS_OCCURRENCE_TO;
        path->opening = !lost && r->size >= offsetof(struct azstatus_native_receipt, motion_flags) + sizeof r->motion_flags && r->motion_flags == AZN_AT_OPENING;
    }
}
static void ds_map_publish(const struct azstatus_native_receipt *r, struct ds_map_receipt *map) {
    const struct aznm_packet *packet = &r->map;
    az_status_grid_ref_v1 grid = {r->load_generation, packet->grid.provider, packet->grid.revision, AZ_PROV_NATIVE, AZ_STATUS_GRID_CURRENT};
    int paired = packet->owner && map->availability[0] == AZ_AVAIL_READY && map->availability[1] == AZ_AVAIL_READY;
    if (paired) ds_map_file_pair(r);
    map->fields[map->count++] = (struct ds_field) {AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID, {.info = {paired ? AZ_AVAIL_READY : AZ_AVAIL_STALE, AZ_PROV_NATIVE, packet->owner, r->observed_ns}, .scope = AZ_STATUS_LOAD_SCOPE, .scope_id = r->load_generation, .provider_id = packet->owner, .count = paired}, &grid, NULL};
    struct ds_path path;
    ds_map_path(r, map->lost, &path);
    unsigned flags = DS_NATIVE_INPUT | (map->lost ? DS_FIELD_LOSS : 0);
    if (path.order) ds_native_path(&path, r->deck, r->load_generation, flags, r->observed_ns, map->fields, map->count, NULL, map->lost, map->cause);
    else ds_publish(r->deck, r->load_generation, flags, r->observed_ns, map->fields, map->count, NULL, NULL, map->lost, map->cause);
}
static void ds_native_map(const struct azstatus_native_receipt *r) {
    const struct azn_maps_api *api = ds_map_service();
    if (!api) return;
    struct ds_map_receipt map = {.lost = r->lost_topics, .cause = r->lost_topics ? AZ_STATUS_CAPTURE_GAP : 0, .availability = {AZ_AVAIL_STALE, AZ_AVAIL_STALE}};
    if (r->deck < 4 && ds_native_loaded(r) && r->map.changed) {
        for (unsigned i = 0; i < 2; i++) ds_map_part(r, api, i, &map);
        ds_map_publish(r, &map);
    }
    free(map.rows[0]);
    free(map.rows[1]);
    free(map.ticks);
    api->release(&r->map);
}
