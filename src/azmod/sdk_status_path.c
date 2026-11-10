/* SPDX-License-Identifier: GPL-3.0-only */
/* This private input is for a common writer fence. A current-value getter, a
 * change in sign or a newer receipt timestamp cannot construct a ds_path. */
enum ds_path_kind {
    DS_PATH_BASELINE,
    DS_PATH_TRAVEL,
    DS_PATH_ACTION
};
struct ds_path {
    uint64_t order, generation, phrase_provider, phrase_revision;
    az_status_grid_ref_v1 grid;
    unsigned kind, endpoints, opening, ended, began_play, defer_terminal, confirm_terminal;
    az_status_coord_v1 from, to;
    az_status_coord_v1 terminal;
    uint32_t observed_window_samples, observed_window_rate;
    uint32_t from_window_samples, from_window_rate;
    const az_status_phrase_endpoint_v1 *departure, *landing;
    uint64_t first_order; /* nonzero only for consecutive ordinary measurements */
};
struct ds_map {
    const az_phrase *rows;
    const struct ds_time_axis *clock;
    unsigned count;
    uint64_t provider, revision;
    az_status_grid_ref_v1 grid;
};
static int ds_map_at(const struct ds_image *im, const struct ds_path *p, struct ds_map *map) {
    const struct ds_cell *phrases = ds_ready(im, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT);
    const struct ds_cell *beats = ds_ready(im, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    const az_status_grid_ref_v1 *grid = ds_value(im, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID);
    if (!phrases || !beats || !beats->clock.ticks || !grid || !ds_grid_matches(im, grid, AZ_STATUS_GRID_CURRENT) ||
        phrases->info.provider_id != p->phrase_provider || phrases->info.info.revision != p->phrase_revision ||
        !p->phrase_provider || !p->phrase_revision || memcmp(grid, &p->grid, sizeof *grid)) return 0;
    *map = (struct ds_map) {(const void *) phrases->data, &beats->clock, phrases->info.count,
                            p->phrase_provider, p->phrase_revision, *grid};
    return 1;
}
static az_status_phrase_endpoint_v1 ds_endpoint(const struct ds_map *m, unsigned kind, unsigned index) {
    az_status_phrase_endpoint_v1 e = {.kind = kind, .phrase_provider_id = m->provider, .phrase_revision = m->revision, .grid = m->grid};
    if (kind == AZ_PHRASE_ITEM) {
        e.index = index;
        e.phrase = m->rows[index];
    }
    return e;
}
static az_status_coord_v1 ds_grid_point(const struct ds_map *m, unsigned index) {
    return (az_status_coord_v1) {.samples = m->clock->ticks[index], .rate_hz = m->clock->units_per_second};
}
static int ds_coord_order(const az_status_coord_v1 *first, const az_status_coord_v1 *second,
                          unsigned rate, unsigned window) {
    return ds_compare_source_times(first->samples, first->rate_hz, second->samples, second->rate_hz, rate, window);
}
static int ds_opening_at(const struct ds_map *m, const az_status_coord_v1 *point, unsigned rate, unsigned window) {
    az_status_coord_v1 zero = {.rate_hz = point->rate_hz}, first = ds_grid_point(m, m->rows[0].start_beat);
    int at_zero = ds_coord_order(point, &zero, rate, window);
    int at_first = ds_coord_order(point, &first, rate, window);
    if (at_zero == AZ_TIME_INVALID || at_first == AZ_TIME_INVALID) return 0;
    return at_zero == AZ_TIME_EQUAL || at_first == AZ_TIME_EQUAL;
}
static az_status_phrase_endpoint_v1 ds_at(const struct ds_map *m, const az_status_coord_v1 *point,
                                          int opening, int ended, unsigned rate, unsigned window) {
    az_status_coord_v1 edge = ds_grid_point(m, m->rows[0].start_beat);
    int first = ds_coord_order(point, &edge, rate, window);
    if (first == AZ_TIME_INVALID) return (az_status_phrase_endpoint_v1) {0};
    if (ended) return ds_endpoint(m, AZ_PHRASE_END, 0);
    if ((opening && ds_opening_at(m, point, rate, window)) || first == AZ_TIME_BEFORE)
        return ds_endpoint(m, AZ_PHRASE_START, 0);
    unsigned lo = 0, hi = m->count;
    while (lo + 1 < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        edge = ds_grid_point(m, m->rows[mid].start_beat);
        int order = ds_coord_order(point, &edge, rate, window);
        if (order == AZ_TIME_INVALID) return (az_status_phrase_endpoint_v1) {0};
        if (order == AZ_TIME_BEFORE) hi = mid;
        else lo = mid;
    }
    edge = ds_grid_point(m, m->rows[lo].end_beat);
    int end = ds_coord_order(point, &edge, rate, window);
    if (end == AZ_TIME_INVALID) return (az_status_phrase_endpoint_v1) {0};
    if (end == AZ_TIME_BEFORE) return ds_endpoint(m, AZ_PHRASE_ITEM, lo);
    return ds_endpoint(m, AZ_PHRASE_GAP, 0);
}
static int ds_terminal_edge(const struct ds_map *m, const struct ds_path *p, unsigned row) {
    if (!p->defer_terminal || row + 1 != m->count) return 0;
    az_status_coord_v1 edge = ds_grid_point(m, m->rows[row].end_beat);
    int order = ds_coord_order(&edge, &p->terminal, p->observed_window_rate, p->observed_window_samples);
    if (order == AZ_TIME_INVALID) return 0;
    return order == AZ_TIME_EQUAL;
}
static int ds_endpoint_known(const struct ds_map *m, const az_status_phrase_endpoint_v1 *e) {
    if (!e || e->kind < AZ_PHRASE_START || e->kind > AZ_PHRASE_GAP ||
        (e->kind == AZ_PHRASE_ITEM && e->index >= m->count)) return 0;
    az_status_phrase_endpoint_v1 expected = ds_endpoint(m, e->kind, e->index);
    return !memcmp(&expected, e, sizeof expected);
}
static int ds_inside_item(const struct ds_map *m, const az_status_phrase_endpoint_v1 *active,
                          const struct ds_path *p) {
    if (!active || active->kind != AZ_PHRASE_ITEM || active->index >= m->count || p->ended) return 0;
    const az_phrase *row = &m->rows[active->index];
    az_status_coord_v1 start = ds_grid_point(m, row->start_beat), end = ds_grid_point(m, row->end_beat);
    int from_start = ds_coord_order(&p->from, &start, p->from_window_rate, p->from_window_samples);
    int to_start = ds_coord_order(&p->to, &start, p->observed_window_rate, p->observed_window_samples);
    int from_end = ds_coord_order(&p->from, &end, p->from_window_rate, p->from_window_samples);
    int to_end = ds_coord_order(&p->to, &end, p->observed_window_rate, p->observed_window_samples);
    if (from_start == AZ_TIME_INVALID || to_start == AZ_TIME_INVALID || from_end == AZ_TIME_INVALID || to_end == AZ_TIME_INVALID)
        return 0;
    return (from_start == AZ_TIME_EQUAL || from_start == AZ_TIME_AFTER) &&
           (to_start == AZ_TIME_EQUAL || to_start == AZ_TIME_AFTER) &&
           from_end == AZ_TIME_BEFORE && to_end == AZ_TIME_BEFORE;
}
static int ds_classify(const struct ds_map *map, const struct ds_path *p, int from,
                       az_status_phrase_endpoint_v1 *out) {
    const az_status_phrase_endpoint_v1 *known = from ? p->departure : p->landing;
    if (known && ds_endpoint_known(map, known)) {
        *out = *known;
        return 1;
    }
    uint32_t bit = from ? AZ_STATUS_OCCURRENCE_FROM : AZ_STATUS_OCCURRENCE_TO;
    const az_status_coord_v1 *point = from ? &p->from : &p->to;
    az_status_coord_v1 origin = {.rate_hz = point->rate_hz};
    unsigned rate = from ? p->from_window_rate : p->observed_window_rate;
    unsigned window = from ? p->from_window_samples : p->observed_window_samples;
    int zero = ds_coord_order(point, &origin, rate, window);
    if (!(p->endpoints & bit) || zero == AZ_TIME_INVALID || zero == AZ_TIME_BEFORE) return 0;
    *out = ds_at(map, point, !from && p->opening, !from && p->ended, rate, window);
    if (out->kind == AZ_PHRASE_GAP && ds_terminal_edge(map, p, map->count - 1)) {
        /* A terminal arrival can span consecutive native measurements.
         * Its prior landing remains the last ITEM while the source reader
         * refines/clamps the end. Classifying that same departure as GAP
         * would invent a missing crossing inside the prior sample window. */
        int terminal = ds_coord_order(point, &p->terminal, rate, window);
        az_status_coord_v1 start = ds_grid_point(map, map->rows[map->count - 1].start_beat);
        int final_start = ds_coord_order(point, &start, rate, window);
        if (!from || (terminal != AZ_TIME_INVALID && terminal <= AZ_TIME_EQUAL &&
                      final_start != AZ_TIME_INVALID && final_start >= AZ_TIME_EQUAL))
            *out = ds_endpoint(map, AZ_PHRASE_ITEM, map->count - 1);
    }
    return out->kind != 0;
}
static void ds_cross(struct ds_deck *d, az_status_phrase_endpoint_v1 *active,
                     const az_status_phrase_endpoint_v1 *to, uint64_t now, uint64_t *loss) {
    if (!memcmp(active, to, sizeof *active)) return;
    az_status_phrase_transition_v1 tr = {.size = sizeof tr, .version = AZ_DATA_VERSION, .from = *active, .to = *to};
    ds_derived(d, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, now, to, sizeof *to, loss);
    if (!*loss) {
        d->current.sequence++;
        d->current.observed = now;
        d->current.published = ds_now();
        ds_append(d, 0, DS_PHRASE_ROUTE, NULL, &tr, 0, 0);
    }
    *active = *to;
}
/* Cross every proven boundary on a monotone leg, before applying the measured
 * landing to POSITION. A half-open interval belongs to the later item. */
static void ds_crossings(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p,
                         az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    if (ds_inside_item(m, active, p)) return;
    int direction = ds_coord_order(&p->to, &p->from, p->observed_window_rate, p->observed_window_samples);
    if (direction == AZ_TIME_INVALID) return;
    if (direction == AZ_TIME_EQUAL || direction == AZ_TIME_AFTER) {
        for (unsigned i = 0; i < m->count; i++) {
            az_status_coord_v1 start = ds_grid_point(m, m->rows[i].start_beat);
            az_status_coord_v1 end = ds_grid_point(m, m->rows[i].end_beat);
            int start_from = ds_coord_order(&start, &p->from, p->from_window_rate, p->from_window_samples);
            int start_to = ds_coord_order(&start, &p->to, p->observed_window_rate, p->observed_window_samples);
            int end_from = ds_coord_order(&end, &p->from, p->from_window_rate, p->from_window_samples);
            int end_to = ds_coord_order(&end, &p->to, p->observed_window_rate, p->observed_window_samples);
            if (start_from == AZ_TIME_INVALID || start_to == AZ_TIME_INVALID || end_from == AZ_TIME_INVALID || end_to == AZ_TIME_INVALID) return;
            if ((start_from == AZ_TIME_EQUAL || start_from == AZ_TIME_AFTER) &&
                (start_to == AZ_TIME_EQUAL || start_to == AZ_TIME_BEFORE)) {
                az_status_phrase_endpoint_v1 dest = ds_endpoint(m, AZ_PHRASE_ITEM, i);
                ds_cross(d, active, &dest, now, loss);
            }
            if (end_from == AZ_TIME_AFTER && (end_to == AZ_TIME_EQUAL || end_to == AZ_TIME_BEFORE) && !ds_terminal_edge(m, p, i)) {
                az_status_phrase_endpoint_v1 dest = ds_at(m, &end, 0, p->ended && end_to == AZ_TIME_EQUAL,
                                                          p->observed_window_rate, p->observed_window_samples);
                if (dest.kind) ds_cross(d, active, &dest, now, loss);
            }
        }
    } else {
        for (unsigned n = m->count; n > 0; n--) {
            unsigned i = n - 1;
            az_status_coord_v1 start = ds_grid_point(m, m->rows[i].start_beat);
            az_status_coord_v1 end = ds_grid_point(m, m->rows[i].end_beat);
            int end_from = ds_coord_order(&end, &p->from, p->from_window_rate, p->from_window_samples);
            int end_to = ds_coord_order(&end, &p->to, p->observed_window_rate, p->observed_window_samples);
            int start_from = ds_coord_order(&start, &p->from, p->from_window_rate, p->from_window_samples);
            int start_to = ds_coord_order(&start, &p->to, p->observed_window_rate, p->observed_window_samples);
            if (start_from == AZ_TIME_INVALID || start_to == AZ_TIME_INVALID || end_from == AZ_TIME_INVALID || end_to == AZ_TIME_INVALID) return;
            if ((end_from == AZ_TIME_BEFORE || end_from == AZ_TIME_EQUAL) && end_to == AZ_TIME_AFTER) {
                az_status_phrase_endpoint_v1 dest = ds_endpoint(m, AZ_PHRASE_ITEM, i);
                ds_cross(d, active, &dest, now, loss);
            }
            if ((start_from == AZ_TIME_BEFORE || start_from == AZ_TIME_EQUAL) && start_to == AZ_TIME_AFTER) {
                unsigned kind = AZ_PHRASE_START;
                if (i) {
                    az_status_coord_v1 previous_end = ds_grid_point(m, m->rows[i - 1].end_beat);
                    int adjacent = ds_coord_order(&previous_end, &start, p->observed_window_rate, p->observed_window_samples);
                    if (adjacent == AZ_TIME_INVALID) return;
                    kind = adjacent == AZ_TIME_EQUAL ? AZ_PHRASE_ITEM : AZ_PHRASE_GAP;
                }
                az_status_phrase_endpoint_v1 dest = ds_endpoint(m, kind, i ? i - 1 : 0);
                ds_cross(d, active, &dest, now, loss);
            }
        }
    }
    az_status_phrase_endpoint_v1 dest;
    if (ds_classify(m, p, 0, &dest)) ds_cross(d, active, &dest, now, loss);
}
/* Discrete actions classify each endpoint independently. A qualified landing
 * can seed active even without proof of departure. Only the phrase route loses
 * continuity in that case. The action remains deliverable to its own routes. */
static void ds_path_reduce(struct ds_deck *d, const struct ds_path *p, const az_status_occurrence_v1 *action,
                           uint64_t now, az_status_phrase_transition_v1 *pair, uint64_t *routes, uint64_t *lost) {
    struct ds_map map;
    const az_status_phrase_endpoint_v1 *previous = ds_value(&d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    az_status_phrase_endpoint_v1 from, to;
    int mapped = p && ds_map_at(&d->current, p, &map);
    int has_from = mapped && ds_classify(&map, p, 1, &from), has_to = mapped && ds_classify(&map, p, 0, &to);
    int was_active = previous != NULL;
    uint64_t fail = 0;
    int endpoint_order = p ? ds_coord_order(&p->from, &p->to, p->observed_window_rate, p->observed_window_samples) : AZ_TIME_INVALID;
    if (p && p->confirm_terminal && p->kind == DS_PATH_TRAVEL && has_from && has_to && previous &&
        ds_endpoint_known(&map, previous) && endpoint_order != AZ_TIME_INVALID && endpoint_order == AZ_TIME_EQUAL) {
        if (memcmp(previous, &to, sizeof to)) {
            *pair = (az_status_phrase_transition_v1) {.size = sizeof *pair, .version = AZ_DATA_VERSION, .from = *previous, .to = to};
            *routes |= DS_PHRASE_ROUTE;
            if (ds_derived(d, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, now, &to, sizeof to, &fail))
                *routes |= DS_PHRASE_ROUTE;
        }
        *lost |= fail;
        return;
    } else if (p && p->kind == DS_PATH_TRAVEL && has_from && has_to && previous &&
               ds_endpoint_known(&map, previous) && endpoint_order != AZ_TIME_INVALID && endpoint_order == AZ_TIME_EQUAL && !p->began_play &&
               (!memcmp(previous, &to, sizeof to) || (!p->ended && previous->kind == AZ_PHRASE_START &&
                                                      ds_opening_at(&map, &p->to, p->observed_window_rate, p->observed_window_samples)))) return;
    if (p && p->kind == DS_PATH_TRAVEL && has_from && has_to && previous &&
        ds_endpoint_known(&map, previous) &&
        (!memcmp(previous, &from, sizeof from) || (previous->kind == AZ_PHRASE_START && ds_opening_at(&map, &p->from, p->from_window_rate, p->from_window_samples)))) {
        az_status_phrase_endpoint_v1 active = *previous;
        ds_crossings(d, &map, p, &active, now, &fail);
    } else if (p && p->kind == DS_PATH_ACTION && action && has_from && has_to &&
               previous && ds_endpoint_known(&map, previous) && memcmp(&from, &to, sizeof from)) {
        *pair = (az_status_phrase_transition_v1) {.size = sizeof *pair, .version = AZ_DATA_VERSION, .discontinuity = action->kind, .from = from, .to = to};
        *routes |= DS_PHRASE_ROUTE;
    } else if (was_active && (!p || (p->kind != DS_PATH_BASELINE &&
                                     (!has_from || !has_to || !mapped || !ds_endpoint_known(&map, previous) ||
                                      (p->kind == DS_PATH_TRAVEL && memcmp(previous, &from, sizeof from)))))) *lost |= DS_PHRASE_ROUTE;
    if (ds_derived(d, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, now,
                   has_to ? &to : NULL, has_to ? sizeof to : 0, &fail)) *routes |= DS_PHRASE_ROUTE;
    *lost |= fail;
}
