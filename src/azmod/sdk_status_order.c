/* SPDX-License-Identifier: GPL-3.0-only */
/* Map activation and motion may arrive on separate non-real-time lanes. Order
 * is assigned at their common source, never inferred from arrival or time. */
#define DS_PATH_PENDING 16
#define DS_PATH_FIELDS 72
struct ds_pending_path {
    struct ds_path path;
    struct ds_image image;
    struct ds_field fields[DS_PATH_FIELDS];
    struct ds_time_axis clocks[DS_PATH_FIELDS];
    size_t count;
    az_status_occurrence_v1 action;
    az_status_phrase_endpoint_v1 departure, landing;
    unsigned flags;
    uint64_t epoch, observed, lost;
    unsigned reason;
};
static struct ds_pending_path ds_pending[4][DS_PATH_PENDING];
static pthread_mutex_t ds_path_mu[4] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
                                        PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};

static void ds_forget_path(struct ds_pending_path *pending) {
    pthread_mutex_lock(&ds.mu);
    ds_image_copy(&pending->image, NULL);
    pthread_mutex_unlock(&ds.mu);
    memset(pending, 0, sizeof *pending);
}
static struct ds_pending_path *ds_pending_slot(unsigned deck, uint64_t epoch, uint64_t order, int *duplicate) {
    struct ds_pending_path *slot = NULL;
    for (unsigned i = 0; i < DS_PATH_PENDING; i++) {
        struct ds_pending_path *pending = &ds_pending[deck][i];
        if (pending->epoch == epoch && pending->path.order == order) {
            *duplicate = 1;
            return pending;
        }
        if (!pending->path.order) slot = pending;
    }
    return slot;
}
static void ds_hold_endpoints(struct ds_pending_path *slot, const struct ds_path *path) {
    slot->path = *path;
    if (path->departure) {
        slot->departure = *path->departure;
        slot->path.departure = &slot->departure;
    }
    if (path->landing) {
        slot->landing = *path->landing;
        slot->path.landing = &slot->landing;
    }
}
static int ds_hold_field(struct ds_pending_path *slot, size_t index, const struct ds_field *field) {
    if (slot->image.field[field->topic][field->field] ||
        ds_set(&slot->image, field, ds_extent(field, slot->epoch)) == AZ_LIMIT) return AZ_LIMIT;
    struct ds_cell *cell = slot->image.field[field->topic][field->field];
    slot->fields[index] = *field;
    slot->fields[index].value = cell && cell->bytes ? cell->data : NULL;
    slot->fields[index].clock = NULL;
    if (cell && cell->clock.units_per_second) {
        slot->clocks[index] = cell->clock;
        slot->fields[index].clock = &slot->clocks[index];
    }
    return AZ_PENDING;
}
static int ds_hold_fields(struct ds_pending_path *slot, const struct ds_field *fields) {
    int rc = AZ_PENDING;
    pthread_mutex_lock(&ds.mu);
    for (size_t i = 0; i < slot->count; i++) {
        rc = ds_hold_field(slot, i, &fields[i]);
        if (rc != AZ_PENDING) break;
    }
    pthread_mutex_unlock(&ds.mu);
    return rc;
}
static int ds_hold_path(unsigned deck, const struct ds_path *p, uint64_t epoch, unsigned flags,
                        uint64_t observed, const struct ds_field *fields, size_t count,
                        const az_status_occurrence_v1 *action, uint64_t lost, unsigned reason) {
    int duplicate = 0;
    struct ds_pending_path *slot = ds_pending_slot(deck, epoch, p->order, &duplicate);
    if (duplicate) return AZ_PENDING;
    if (!slot || count > DS_PATH_FIELDS) return AZ_LIMIT;
    ds_hold_endpoints(slot, p);
    slot->epoch = epoch;
    slot->observed = observed;
    slot->lost = lost;
    slot->reason = reason;
    slot->flags = flags;
    slot->count = count;
    if (action) slot->action = *action;
    int rc = ds_hold_fields(slot, fields);
    if (rc != AZ_PENDING) ds_forget_path(slot);
    ds_wake(); /* budgeting pins can advance an eviction watermark */
    return rc;
}
static void ds_drop_old_paths(unsigned deck, uint64_t epoch) {
    for (unsigned i = 0; i < DS_PATH_PENDING; i++) {
        struct ds_pending_path *pending = &ds_pending[deck][i];
        if (pending->path.order && pending->epoch != epoch) ds_forget_path(pending);
    }
}
static struct ds_pending_path *ds_next_path(unsigned deck, uint64_t epoch) {
    struct ds_pending_path *next = NULL;
    for (unsigned i = 0; i < DS_PATH_PENDING; i++) {
        struct ds_pending_path *candidate = &ds_pending[deck][i];
        if (candidate->epoch != epoch || !candidate->path.order) continue;
        if (!next || candidate->path.order < next->path.order) next = candidate;
    }
    return next;
}
static void ds_flush_paths(unsigned deck, uint64_t epoch) {
    for (;;) {
        struct ds_pending_path *next = ds_next_path(deck, epoch);
        if (!next) return;
        int rc = ds_apply(&next->path, NULL, 0, deck, epoch, next->flags, next->observed,
                          next->fields, next->count, next->action.ordinal ? &next->action : NULL, NULL,
                          next->lost, next->reason);
        if (rc == AZ_PENDING) return;
        ds_forget_path(next);
    }
}
static uint64_t ds_skipped_paths(unsigned deck, uint64_t epoch, const struct ds_path *path) {
    uint64_t lost = 0;
    if (!path || path->kind != DS_PATH_BASELINE) return 0;
    for (unsigned i = 0; i < DS_PATH_PENDING; i++) {
        const struct ds_pending_path *pending = &ds_pending[deck][i];
        if (pending->epoch == epoch && pending->path.order && pending->path.order < path->order) {
            lost |= DS_PHRASE_ROUTE | AZ_STATUS_TOPIC(AZ_STATUS_POSITION) | AZ_STATUS_TOPIC(AZ_STATUS_PHASE);
            if (pending->action.ordinal) lost |= DS_ACTION_ROUTES;
        }
    }
    return lost;
}
static int ds_path_committed(int rc, unsigned deck, uint64_t epoch, const struct ds_path *path) {
    return rc == AZ_OK || (rc == AZ_LIMIT && path && ds.deck[deck].current.epoch == epoch &&
                           ds.deck[deck].path_order == path->order);
}
static int ds_resolve_path(int rc, unsigned deck, uint64_t epoch, const struct ds_path *path,
                           unsigned flags, uint64_t observed, const struct ds_field *fields, size_t count,
                           const az_status_occurrence_v1 *action, uint64_t lost, unsigned reason) {
    if (rc == AZ_PENDING && path && !path->first_order) {
        rc = ds_hold_path(deck, path, epoch, flags, observed, fields, count, action, lost, reason);
        if (rc == AZ_LIMIT) {
            uint64_t mask = DS_PHRASE_ROUTE | DS_ACTION_ROUTES | AZ_STATUS_TOPIC(AZ_STATUS_PHASE);
            ds_apply(NULL, NULL, 0, deck, epoch, 0, observed, NULL, 0, NULL, NULL, mask, AZ_STATUS_CAPTURE_GAP);
        }
    } else if (ds_path_committed(rc, deck, epoch, path)) {
        ds_drop_old_paths(deck, epoch);
        ds_flush_paths(deck, epoch);
    }
    return rc;
}
static int ds_publish_ordered(const struct ds_path *path, const az_track_ref *expected, int file_only,
                              unsigned deck, uint64_t epoch, unsigned flags, uint64_t observed,
                              const struct ds_field *fields, size_t count,
                              const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair,
                              uint64_t lost, unsigned reason, uint64_t *committed_order) {
    if (deck >= 4) return AZ_INVALID;
    if (!path && !(flags & DS_BOUNDARY))
        return ds_apply(NULL, expected, file_only, deck, epoch, flags, observed, fields, count, action, pair, lost, reason);
    pthread_mutex_lock(&ds_path_mu[deck]);
    uint64_t skipped = lost ? ds_skipped_paths(deck, epoch, path) : 0;
    if (skipped) {
        lost |= skipped;
        reason |= AZ_STATUS_CUT_SKIPPED;
    }
    int rc = ds_apply(path, expected, file_only, deck, epoch, flags, observed, fields, count, action, pair, lost, reason);
    rc = ds_resolve_path(rc, deck, epoch, path, flags, observed, fields, count, action, lost, reason);
    /* A parked successor can commit in this transaction. A limited receipt
     * can also have been rejected before committing. Report the actual cut
     * before another path or load boundary can change it. */
    if (committed_order)
        *committed_order = ds.deck[deck].current.epoch == epoch ? ds.deck[deck].path_order : 0;
    pthread_mutex_unlock(&ds_path_mu[deck]);
    return rc;
}
static int ds_publish_checked(const struct ds_path *path, const az_track_ref *expected, int file_only,
                              unsigned deck, uint64_t epoch, unsigned flags, uint64_t observed,
                              const struct ds_field *fields, size_t count,
                              const az_status_occurrence_v1 *action, const az_status_phrase_transition_v1 *pair,
                              uint64_t lost, unsigned reason) {
    return ds_publish_ordered(path, expected, file_only, deck, epoch, flags, observed, fields, count,
                              action, pair, lost, reason, NULL);
}
