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
static int ds_path_map_edition(const struct ds_cell *phrases, const az_status_grid_ref_v1 *grid, const struct ds_path *p) {
    return phrases->info.provider_id == p->phrase_provider && phrases->info.info.revision == p->phrase_revision &&
           p->phrase_provider && p->phrase_revision && !memcmp(grid, &p->grid, sizeof *grid);
}
static int ds_map_at(const struct ds_image *im, const struct ds_path *p, struct ds_map *map) {
    const struct ds_cell *phrases = ds_ready(im, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT);
    const struct ds_cell *beats = ds_ready(im, AZ_STATUS_GRID, AZ_STATUS_GRID_CURRENT);
    const az_status_grid_ref_v1 *grid = ds_value(im, AZ_STATUS_PHRASES, AZ_STATUS_PHRASES_CURRENT_GRID);
    if (!phrases || !beats || !beats->clock.ticks || !grid || !ds_grid_matches(im, grid, AZ_STATUS_GRID_CURRENT) ||
        !ds_path_map_edition(phrases, grid, p)) return 0;
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
static unsigned ds_phrase_at(const struct ds_map *m, const az_status_coord_v1 *point, unsigned rate, unsigned window) {
    unsigned lo = 0, hi = m->count;
    while (lo + 1 < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        az_status_coord_v1 edge = ds_grid_point(m, m->rows[mid].start_beat);
        int order = ds_coord_order(point, &edge, rate, window);
        if (order == AZ_TIME_INVALID) return UINT_MAX;
        if (order == AZ_TIME_BEFORE) hi = mid;
        else lo = mid;
    }
    return lo;
}
static az_status_phrase_endpoint_v1 ds_at(const struct ds_map *m, const az_status_coord_v1 *point,
                                          int opening, int ended, unsigned rate, unsigned window) {
    az_status_coord_v1 edge = ds_grid_point(m, m->rows[0].start_beat);
    int first = ds_coord_order(point, &edge, rate, window);
    if (first == AZ_TIME_INVALID) return (az_status_phrase_endpoint_v1) {0};
    if (ended) return ds_endpoint(m, AZ_PHRASE_END, 0);
    if ((opening && ds_opening_at(m, point, rate, window)) || first == AZ_TIME_BEFORE) return ds_endpoint(m, AZ_PHRASE_START, 0);
    unsigned lo = ds_phrase_at(m, point, rate, window);
    if (lo == UINT_MAX) return (az_status_phrase_endpoint_v1) {0};
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
static int ds_point_inside(int start, int end) {
    return ds_time_nonnegative(start) && end != AZ_TIME_INVALID && end == AZ_TIME_BEFORE;
}
static int ds_inside_item(const struct ds_map *m, const az_status_phrase_endpoint_v1 *active, const struct ds_path *p) {
    if (!active || active->kind != AZ_PHRASE_ITEM || active->index >= m->count || p->ended) return 0;
    const az_phrase *row = &m->rows[active->index];
    az_status_coord_v1 start = ds_grid_point(m, row->start_beat), end = ds_grid_point(m, row->end_beat);
    int from_start = ds_coord_order(&p->from, &start, p->from_window_rate, p->from_window_samples);
    int to_start = ds_coord_order(&p->to, &start, p->observed_window_rate, p->observed_window_samples);
    int from_end = ds_coord_order(&p->from, &end, p->from_window_rate, p->from_window_samples);
    int to_end = ds_coord_order(&p->to, &end, p->observed_window_rate, p->observed_window_samples);
    return ds_point_inside(from_start, from_end) && ds_point_inside(to_start, to_end);
}
static void ds_classify_terminal(const struct ds_map *map, const struct ds_path *p, int from,
                                 const az_status_coord_v1 *point, unsigned rate, unsigned window,
                                 az_status_phrase_endpoint_v1 *out) {
    if (out->kind != AZ_PHRASE_GAP || !ds_terminal_edge(map, p, map->count - 1)) return;
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
static int ds_path_point_observed(const struct ds_path *p, uint32_t bit, const az_status_coord_v1 *point, unsigned rate, unsigned window) {
    az_status_coord_v1 origin = {.rate_hz = point->rate_hz};
    int zero = ds_coord_order(point, &origin, rate, window);
    return (p->endpoints & bit) && zero != AZ_TIME_INVALID && zero != AZ_TIME_BEFORE;
}
static int ds_classify_point(const struct ds_map *map, const struct ds_path *p, int from, az_status_phrase_endpoint_v1 *out) {
    uint32_t bit = from ? AZ_STATUS_OCCURRENCE_FROM : AZ_STATUS_OCCURRENCE_TO;
    const az_status_coord_v1 *point = from ? &p->from : &p->to;
    unsigned rate = from ? p->from_window_rate : p->observed_window_rate;
    unsigned window = from ? p->from_window_samples : p->observed_window_samples;
    if (!ds_path_point_observed(p, bit, point, rate, window)) return 0;
    *out = ds_at(map, point, !from && p->opening, !from && p->ended, rate, window);
    ds_classify_terminal(map, p, from, point, rate, window, out);
    return out->kind != 0;
}
static int ds_classify(const struct ds_map *map, const struct ds_path *p, int from, az_status_phrase_endpoint_v1 *out) {
    const az_status_phrase_endpoint_v1 *known = from ? p->departure : p->landing;
    if (known && ds_endpoint_known(map, known)) {
        *out = *known;
        return 1;
    }
    return ds_classify_point(map, p, from, out);
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
struct ds_crossing_bounds {
    az_status_coord_v1 start, end;
    int start_from, start_to, end_from, end_to;
};
static int ds_crossing_bounds(const struct ds_map *m, const struct ds_path *p, unsigned i, struct ds_crossing_bounds *b) {
    b->start = ds_grid_point(m, m->rows[i].start_beat);
    b->end = ds_grid_point(m, m->rows[i].end_beat);
    b->start_from = ds_coord_order(&b->start, &p->from, p->from_window_rate, p->from_window_samples);
    b->start_to = ds_coord_order(&b->start, &p->to, p->observed_window_rate, p->observed_window_samples);
    b->end_from = ds_coord_order(&b->end, &p->from, p->from_window_rate, p->from_window_samples);
    b->end_to = ds_coord_order(&b->end, &p->to, p->observed_window_rate, p->observed_window_samples);
    return b->start_from != AZ_TIME_INVALID && b->start_to != AZ_TIME_INVALID && b->end_from != AZ_TIME_INVALID && b->end_to != AZ_TIME_INVALID;
}
static int ds_forward_entry(const struct ds_crossing_bounds *b) {
    return (b->start_from == AZ_TIME_EQUAL || b->start_from == AZ_TIME_AFTER) &&
           (b->start_to == AZ_TIME_EQUAL || b->start_to == AZ_TIME_BEFORE);
}
static int ds_forward_exit(const struct ds_map *m, const struct ds_path *p, unsigned i, const struct ds_crossing_bounds *b) {
    return b->end_from == AZ_TIME_AFTER && (b->end_to == AZ_TIME_EQUAL || b->end_to == AZ_TIME_BEFORE) && !ds_terminal_edge(m, p, i);
}
static void ds_cross_forward(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p, unsigned i,
                             const struct ds_crossing_bounds *b, az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    if (ds_forward_entry(b)) {
        az_status_phrase_endpoint_v1 dest = ds_endpoint(m, AZ_PHRASE_ITEM, i);
        ds_cross(d, active, &dest, now, loss);
    }
    if (ds_forward_exit(m, p, i, b)) {
        az_status_phrase_endpoint_v1 dest = ds_at(m, &b->end, 0, p->ended && b->end_to == AZ_TIME_EQUAL,
                                                  p->observed_window_rate, p->observed_window_samples);
        if (dest.kind) ds_cross(d, active, &dest, now, loss);
    }
}
static int ds_backward_exit(const struct ds_map *m, const struct ds_path *p, unsigned i,
                            const struct ds_crossing_bounds *b, az_status_phrase_endpoint_v1 *dest) {
    unsigned kind = AZ_PHRASE_START;
    if (i) {
        az_status_coord_v1 previous_end = ds_grid_point(m, m->rows[i - 1].end_beat);
        int adjacent = ds_coord_order(&previous_end, &b->start, p->observed_window_rate, p->observed_window_samples);
        if (adjacent == AZ_TIME_INVALID) return 0;
        kind = adjacent == AZ_TIME_EQUAL ? AZ_PHRASE_ITEM : AZ_PHRASE_GAP;
    }
    *dest = ds_endpoint(m, kind, i ? i - 1 : 0);
    return 1;
}
static int ds_cross_backward(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p, unsigned i,
                             const struct ds_crossing_bounds *b, az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    if ((b->end_from == AZ_TIME_BEFORE || b->end_from == AZ_TIME_EQUAL) && b->end_to == AZ_TIME_AFTER) {
        az_status_phrase_endpoint_v1 dest = ds_endpoint(m, AZ_PHRASE_ITEM, i);
        ds_cross(d, active, &dest, now, loss);
    }
    if ((b->start_from == AZ_TIME_BEFORE || b->start_from == AZ_TIME_EQUAL) && b->start_to == AZ_TIME_AFTER) {
        az_status_phrase_endpoint_v1 dest;
        if (!ds_backward_exit(m, p, i, b, &dest)) return 0;
        ds_cross(d, active, &dest, now, loss);
    }
    return 1;
}
static int ds_cross_leg(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p, int forward,
                        az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    for (unsigned n = 0; n < m->count; ++n) {
        unsigned i = forward ? n : m->count - n - 1;
        struct ds_crossing_bounds b;
        if (!ds_crossing_bounds(m, p, i, &b)) return 0;
        if (forward) ds_cross_forward(d, m, p, i, &b, active, now, loss);
        else if (!ds_cross_backward(d, m, p, i, &b, active, now, loss)) return 0;
    }
    return 1;
}
static void ds_crossings(struct ds_deck *d, const struct ds_map *m, const struct ds_path *p,
                         az_status_phrase_endpoint_v1 *active, uint64_t now, uint64_t *loss) {
    if (ds_inside_item(m, active, p)) return;
    int direction = ds_coord_order(&p->to, &p->from, p->observed_window_rate, p->observed_window_samples);
    if (direction == AZ_TIME_INVALID) return;
    int forward = direction == AZ_TIME_EQUAL || direction == AZ_TIME_AFTER;
    if (!ds_cross_leg(d, m, p, forward, active, now, loss)) return;
    az_status_phrase_endpoint_v1 dest;
    if (ds_classify(m, p, 0, &dest)) ds_cross(d, active, &dest, now, loss);
}
/* Discrete actions classify each endpoint independently. A qualified landing
 * can seed active even without proof of departure. Only the phrase route loses
 * continuity in that case. The action remains deliverable to its own routes. */
struct ds_path_reduction {
    const struct ds_path *path;
    struct ds_map map;
    const az_status_phrase_endpoint_v1 *previous;
    az_status_phrase_endpoint_v1 from, to;
    int mapped, has_from, has_to, endpoint_order;
};
static void ds_path_prepare(struct ds_deck *d, const struct ds_path *p, struct ds_path_reduction *r) {
    r->path = p;
    r->previous = ds_value(&d->current, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE);
    r->mapped = r->has_from = r->has_to = 0;
    r->endpoint_order = AZ_TIME_INVALID;
    if (!p) return;
    r->mapped = ds_map_at(&d->current, p, &r->map);
    r->has_from = r->mapped && ds_classify(&r->map, p, 1, &r->from);
    r->has_to = r->mapped && ds_classify(&r->map, p, 0, &r->to);
    r->endpoint_order = ds_coord_order(&p->from, &p->to, p->observed_window_rate, p->observed_window_samples);
}
static int ds_path_travel_ready(const struct ds_path_reduction *r) {
    return r->path && r->path->kind == DS_PATH_TRAVEL && r->has_from && r->has_to && r->previous &&
           ds_endpoint_known(&r->map, r->previous);
}
static int ds_path_still_at_opening(const struct ds_path_reduction *r) {
    const struct ds_path *p = r->path;
    return !p->ended && r->previous->kind == AZ_PHRASE_START &&
           ds_opening_at(&r->map, &p->to, p->observed_window_rate, p->observed_window_samples);
}
static void ds_path_confirm_terminal(struct ds_deck *d, const struct ds_path_reduction *r, uint64_t now,
                                     az_status_phrase_transition_v1 *pair, uint64_t *routes, uint64_t *fail) {
    if (!memcmp(r->previous, &r->to, sizeof r->to)) return;
    *pair = (az_status_phrase_transition_v1) {.size = sizeof *pair, .version = AZ_DATA_VERSION, .from = *r->previous, .to = r->to};
    *routes |= DS_PHRASE_ROUTE;
    if (ds_derived(d, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, now, &r->to, sizeof r->to, fail))
        *routes |= DS_PHRASE_ROUTE;
}
static int ds_path_stationary(struct ds_deck *d, const struct ds_path_reduction *r, uint64_t now,
                              az_status_phrase_transition_v1 *pair, uint64_t *routes, uint64_t *fail) {
    if (!ds_path_travel_ready(r) || r->endpoint_order == AZ_TIME_INVALID || r->endpoint_order != AZ_TIME_EQUAL) return 0;
    if (r->path->confirm_terminal) {
        ds_path_confirm_terminal(d, r, now, pair, routes, fail);
        return 1;
    }
    return !r->path->began_play && (!memcmp(r->previous, &r->to, sizeof r->to) || ds_path_still_at_opening(r));
}
static int ds_path_contiguous(const struct ds_path_reduction *r) {
    if (!ds_path_travel_ready(r)) return 0;
    const struct ds_path *p = r->path;
    return !memcmp(r->previous, &r->from, sizeof r->from) || (r->previous->kind == AZ_PHRASE_START &&
                                                              ds_opening_at(&r->map, &p->from, p->from_window_rate, p->from_window_samples));
}
static int ds_path_action_ready(const struct ds_path_reduction *r, const az_status_occurrence_v1 *action) {
    return r->path && r->path->kind == DS_PATH_ACTION && action && r->has_from && r->has_to &&
           r->previous && ds_endpoint_known(&r->map, r->previous) && memcmp(&r->from, &r->to, sizeof r->from);
}
static int ds_path_lost(const struct ds_path_reduction *r) {
    if (!r->previous) return 0;
    if (!r->path) return 1;
    if (r->path->kind == DS_PATH_BASELINE) return 0;
    if (!r->has_from || !r->has_to || !r->mapped || !ds_endpoint_known(&r->map, r->previous)) return 1;
    return r->path->kind == DS_PATH_TRAVEL && memcmp(r->previous, &r->from, sizeof r->from);
}
static void ds_path_reduce(struct ds_deck *d, const struct ds_path *p, const az_status_occurrence_v1 *action,
                           uint64_t now, az_status_phrase_transition_v1 *pair, uint64_t *routes, uint64_t *lost) {
    struct ds_path_reduction r;
    ds_path_prepare(d, p, &r);
    uint64_t fail = 0;
    if (ds_path_stationary(d, &r, now, pair, routes, &fail)) {
        *lost |= fail;
        return;
    }
    if (ds_path_contiguous(&r)) {
        az_status_phrase_endpoint_v1 active = *r.previous;
        ds_crossings(d, &r.map, p, &active, now, &fail);
    } else if (ds_path_action_ready(&r, action)) {
        *pair = (az_status_phrase_transition_v1) {.size = sizeof *pair, .version = AZ_DATA_VERSION, .discontinuity = action->kind, .from = r.from, .to = r.to};
        *routes |= DS_PHRASE_ROUTE;
    } else if (ds_path_lost(&r)) *lost |= DS_PHRASE_ROUTE;
    if (ds_derived(d, AZ_STATUS_PHRASE_TRANSITION, AZ_STATUS_PHRASE_TRANSITION_ACTIVE, now,
                   r.has_to ? &r.to : NULL, r.has_to ? sizeof r.to : 0, &fail)) *routes |= DS_PHRASE_ROUTE;
    *lost |= fail;
}
