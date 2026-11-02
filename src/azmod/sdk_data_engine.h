/* SPDX-License-Identifier: GPL-3.0-only */
/* Core-owned bounded cache. The caller drives st_work on one background worker. Public
 * operations try-lock; only the worker allocates, frees, reads files or waits. */
#ifndef AZMOD_SDK_DATA_ENGINE_H
#define AZMOD_SDK_DATA_ENGINE_H
#include "sdk_analysis.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#define ST_SLOTS 8
#define ST_TICKETS 32
#define ST_PER_OWNER 4
#define ST_EVENTS 64
#define ST_OWNER_CAP 64
#define ST_DETAIL 32u
struct st_slot {
    az_track_ref ref;
    az_wave_query wave_query;
    uint32_t mask, refs;
    int state, running;
    uint64_t stamp;
    struct st_data *data;
};
struct st_ticket {
    uint64_t id;
    int owner, slot, cancelled, acquired;
};
struct st_event {
    int owner;
    az_data_change value;
};
struct st_engine {
    pthread_mutex_t mu;
    uint64_t next, revision, sequence;
    int events_exhausted;
    _Atomic uint64_t retired_owners;
    struct st_slot slots[ST_SLOTS];
    struct st_ticket tickets[ST_TICKETS];
    struct st_event events[ST_EVENTS];
};
#define ST_ENGINE_INIT {.mu = PTHREAD_MUTEX_INITIALIZER}
static inline int st_owner_retired(struct st_engine *e, int owner) {
    return owner >= 0 && owner < ST_OWNER_CAP &&
           (atomic_load_explicit(&e->retired_owners, memory_order_acquire) & (UINT64_C(1) << owner));
}
/* Core mod indices are never reused in a process. Retirement is nonblocking,
 * including when a worker is publishing or a caller has the cache mutex. */
static inline void st_retire_owner(struct st_engine *e, int owner) {
    if (owner >= 0 && owner < ST_OWNER_CAP)
        atomic_fetch_or_explicit(&e->retired_owners, UINT64_C(1) << owner, memory_order_release);
}
/* Under mu. Never free data on a consumer thread, and never revoke a pointer
 * already returned by acquire. An acquired ticket stays pinned until release. */
static inline void st_reap_retired_locked(struct st_engine *e) {
    for (unsigned i = 0; i < ST_TICKETS; ++i) {
        struct st_ticket *t = &e->tickets[i];
        if (t->id && !t->acquired && st_owner_retired(e, t->owner)) {
            --e->slots[t->slot].refs;
            t->id = 0;
        }
    }
}
static inline uint64_t st_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return 0;
    uint64_t seconds = (uint64_t) ts.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t) ts.tv_nsec) / UINT64_C(1000000000)) return 0;
    return seconds * UINT64_C(1000000000) + (uint64_t) ts.tv_nsec;
}
static inline int st_ref_equal(const az_track_ref *a, const az_track_ref *b) {
    return a->source_id == b->source_id && a->source_generation == b->source_generation &&
           a->library_id == b->library_id && a->track_id == b->track_id;
}
static inline void st_publish_event(struct st_engine *e, const struct st_ticket *t, int kind, int status) {
    /* UINT64_MAX is an invalid event-read cursor. Keep the last usable
     * sequence and report terminal exhaustion instead of wrapping the ring. */
    if (e->sequence >= UINT64_MAX - 1) {
        e->events_exhausted = 1;
        return;
    }
    struct st_slot *s = &e->slots[t->slot];
    uint64_t seq = ++e->sequence;
    struct st_event *ev = &e->events[(seq - 1) % ST_EVENTS];
    memset(ev, 0, sizeof *ev);
    ev->owner = t->owner;
    ev->value.size = sizeof ev->value;
    ev->value.version = AZ_DATA_VERSION;
    ev->value.kind = kind;
    ev->value.status = status;
    ev->value.fields = s->mask == ST_DETAIL ? AZ_DATA_WAVEFORM : s->mask;
    ev->value.track = s->ref;
    ev->value.ticket = t->id;
    ev->value.sequence = seq;
    ev->value.observed_ns = st_now();
    ev->value.revision = s->state == AZ_OK && s->data ? s->data->view.revision : 0;
}
static inline int st_request_with_wave(struct st_engine *e, int owner, const az_track_ref *ref, uint32_t mask, const az_wave_query *wave, uint64_t *ticket) {
    if (!ref || st_record_size(ref) < sizeof *ref || ref->version != AZ_DATA_VERSION || !ref->source_id ||
        !ref->source_generation || !ref->library_id || !ref->track_id || !ticket || !mask ||
        (wave ? mask != ST_DETAIL : (mask & ~31u))) return AZ_INVALID;
    if (wave) {
        if (st_record_size(wave) < sizeof *wave || wave->version != AZ_DATA_VERSION || wave->reserved ||
            !wave->column_count) return AZ_INVALID;
        if (wave->encoding < AZ_WAVE_PWV3_MONO || wave->encoding > AZ_WAVE_PWV7_LAYERED_RAW) return AZ_UNSUPPORTED;
        if (wave->column_count > AZ_WAVE_WINDOW_MAX) return AZ_LIMIT;
    }
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    st_reap_retired_locked(e);
    if (st_owner_retired(e, owner)) {
        pthread_mutex_unlock(&e->mu);
        return AZ_STALE;
    }
    /* Each request consumes a cache stamp and a ticket ID. Zero denotes an
     * unused slot/ticket, so neither counter increment may wrap. Existing
     * consumers retain their immutable data after terminal exhaustion. */
    if (e->next > UINT64_MAX - 2) {
        pthread_mutex_unlock(&e->mu);
        return AZ_LIMIT;
    }
    int nt = 0, ti = -1, slot = -1;
    for (int i = 0; i < ST_TICKETS; i++) {
        if (!e->tickets[i].id) ti = i;
        else if (e->tickets[i].owner == owner) nt++;
    }
    if (nt >= ST_PER_OWNER || ti < 0) {
        pthread_mutex_unlock(&e->mu);
        return AZ_LIMIT;
    }
    for (int i = 0; i < ST_SLOTS; i++) {
        struct st_slot *s = &e->slots[i];
        if (s->stamp && (s->state == AZ_OK || s->state == AZ_PENDING) && st_ref_equal(&s->ref, ref) && s->mask == mask &&
            (!wave || (s->wave_query.encoding == wave->encoding &&
                       s->wave_query.first_column == wave->first_column &&
                       s->wave_query.column_count == wave->column_count))) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < ST_SLOTS; i++)
            if (!e->slots[i].refs && !e->slots[i].running && e->slots[i].stamp < oldest) {
                oldest = e->slots[i].stamp;
                slot = i;
            }
        if (slot < 0) {
            pthread_mutex_unlock(&e->mu);
            return AZ_LIMIT;
        }
        /* Old data is freed by st_work, never on this consumer's thread. */
        struct st_slot *s = &e->slots[slot];
        s->ref = *ref;
        s->ref.size = sizeof s->ref; /* immutable records describe the stored layout */
        s->mask = mask;
        memset(&s->wave_query, 0, sizeof s->wave_query);
        if (wave) {
            s->wave_query = *wave;
            s->wave_query.size = sizeof s->wave_query;
        }
        s->state = AZ_PENDING;
    }
    struct st_slot *s = &e->slots[slot];
    s->stamp = ++e->next;
    s->refs++;
    struct st_ticket *t = &e->tickets[ti];
    *t = (struct st_ticket) {++e->next, owner, slot, 0, 0};
    *ticket = t->id;
    if (s->state != AZ_PENDING) st_publish_event(e, t, AZ_CHANGE_TRACK_DATA, s->state);
    pthread_mutex_unlock(&e->mu);
    return AZ_OK;
}
static inline int st_request(struct st_engine *e, int owner, const az_track_ref *ref, uint32_t mask, uint64_t *ticket) {
    return st_request_with_wave(e, owner, ref, mask, NULL, ticket);
}
static inline struct st_ticket *st_find(struct st_engine *e, int owner, uint64_t id) {
    for (int i = 0; i < ST_TICKETS; i++)
        if (e->tickets[i].id == id && e->tickets[i].owner == owner && id) return &e->tickets[i];
    return NULL;
}
static inline int st_acquire(struct st_engine *e, int owner, uint64_t id, const az_track_snapshot **out) {
    if (!out) return AZ_INVALID;
    *out = NULL;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    st_reap_retired_locked(e);
    struct st_ticket *t = st_find(e, owner, id);
    int rc = AZ_STALE;
    if (t && !st_owner_retired(e, owner)) {
        struct st_slot *s = &e->slots[t->slot];
        rc = s->mask == ST_DETAIL ? AZ_INVALID : t->cancelled ? AZ_CANCELLED
                                                              : s->state;
        if (rc == AZ_OK) {
            t->acquired = 1;
            *out = &s->data->view;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}
static inline int st_wave_acquire(struct st_engine *e, int owner, uint64_t id, const az_wave_snapshot **out) {
    if (!out) return AZ_INVALID;
    *out = NULL;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    st_reap_retired_locked(e);
    struct st_ticket *t = st_find(e, owner, id);
    int rc = AZ_STALE;
    if (t && !st_owner_retired(e, owner)) {
        struct st_slot *s = &e->slots[t->slot];
        rc = s->mask != ST_DETAIL ? AZ_INVALID : t->cancelled ? AZ_CANCELLED
                                                              : s->state;
        if (rc == AZ_OK) {
            t->acquired = 1;
            *out = &s->data->wave_view;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}
static inline int st_release(struct st_engine *e, int owner, uint64_t id, int cancel) {
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    struct st_ticket *t = st_find(e, owner, id);
    int rc = t ? AZ_OK : AZ_STALE;
    if (t) {
        if (cancel) {
            if (!t->cancelled) {
                t->cancelled = 1;
                st_publish_event(e, t, AZ_CHANGE_TRACK_DATA, AZ_CANCELLED);
            }
        } else {
            e->slots[t->slot].refs--;
            t->id = 0;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}
static inline int st_event_read(struct st_engine *e, int owner, uint64_t after, az_data_change *out) {
    if (!out || st_record_size(out) < sizeof *out || after == UINT64_MAX) return AZ_INVALID;
    if (pthread_mutex_trylock(&e->mu)) return AZ_BUSY;
    uint64_t first = e->sequence >= ST_EVENTS ? e->sequence - ST_EVENTS + 1 : 1;
    int missed = after < first - 1, rc = AZ_EMPTY;
    if (after > e->sequence) rc = AZ_INVALID;
    else
        for (uint64_t seq = after < first ? first : after + 1; seq <= e->sequence; seq++) {
            struct st_event *ev = &e->events[(seq - 1) % ST_EVENTS];
            if (ev->owner == owner) {
                uint32_t size = out->size;
                *out = ev->value;
                out->size = size;
                out->missed = missed;
                rc = AZ_OK;
                break;
            }
        }
    if (rc == AZ_EMPTY && e->events_exhausted) rc = AZ_LIMIT;
    if (rc == AZ_EMPTY && missed) {
        uint32_t size = out->size;
        memset(out, 0, sizeof *out);
        out->size = size;
        out->version = AZ_DATA_VERSION;
        out->missed = 1;
        out->sequence = e->sequence;
        rc = AZ_OK;
    }
    pthread_mutex_unlock(&e->mu);
    return rc;
}
typedef int (*st_load_fn)(const az_track_ref *, uint32_t, struct st_data *, void *);
typedef int (*st_fresh_fn)(const az_track_ref *, const struct st_data *, void *);
/* One job or a freshness sweep. Only this worker can replace/free data; running prevents
 * a consumer from evicting the slot while the worker is outside the lock. */
static inline void st_work(struct st_engine *e, st_load_fn load, st_fresh_fn fresh, void *ctx) {
    for (int k = 0; k < ST_SLOTS; k++) {
        pthread_mutex_lock(&e->mu);
        if (!k) st_reap_retired_locked(e);
        struct st_slot *s = &e->slots[k];
        if (!s->stamp || s->running || (s->state != AZ_PENDING && s->state != AZ_OK)) {
            pthread_mutex_unlock(&e->mu);
            continue;
        }
        s->running = 1;
        int pending = s->state == AZ_PENDING;
        az_track_ref ref = s->ref;
        uint32_t mask = s->mask;
        az_wave_query wave = s->wave_query;
        struct st_data *data = s->data;
        if (pending) s->data = NULL; /* cancellation must not inspect a retiring allocation */
        pthread_mutex_unlock(&e->mu);
        int rc = AZ_OK;
        if (pending) {
            free(data);
            data = malloc(sizeof *data);
            if (data) {
                st_data_init(data);
                data->view.track = ref;
                data->wave_view.track = ref;
                data->wave_query = wave;
                rc = load(&ref, mask, data, ctx);
            } else rc = AZ_FAILED;
            if (rc == AZ_OK && fresh && !fresh(&ref, data, ctx)) rc = AZ_STALE;
        } else if (fresh && !fresh(&ref, data, ctx)) rc = AZ_STALE;
        pthread_mutex_lock(&e->mu);
        s->data = data;
        s->running = 0;
        if (pending && rc == AZ_OK && e->revision == UINT64_MAX) rc = AZ_LIMIT;
        if (pending || rc != AZ_OK) {
            s->state = rc;
            if (pending && rc == AZ_OK) {
                data->view.revision = ++e->revision;
                data->view.observed_ns = st_now();
                if (mask == ST_DETAIL) {
                    data->wave_view.revision = data->view.revision;
                    data->wave_view.observed_ns = data->view.observed_ns;
                    data->wave_view.info.revision = data->view.revision;
                    data->wave_view.info.observed_ns = data->view.observed_ns;
                    data->wave_view.info.provenance = wave.encoding == AZ_WAVE_PWV7_LAYERED_RAW ? AZ_PROV_DERIVED : AZ_PROV_FILE;
                }
                az_component_info *ci[] = {&data->view.metadata_info, &data->view.cues_info, &data->view.grid_info, &data->view.phrases_info, &data->view.waveform_info};
                for (unsigned i = 0; i < 5; i++)
                    if (mask & (1u << i)) {
                        ci[i]->revision = data->view.revision;
                        if (ci[i]->availability == AZ_AVAIL_READY || ci[i]->availability == AZ_AVAIL_EMPTY) ci[i]->provenance = AZ_PROV_FILE;
                        ci[i]->observed_ns = data->view.observed_ns;
                    }
            }
            for (int i = 0; i < ST_TICKETS; i++)
                if (e->tickets[i].id && e->tickets[i].slot == k && !e->tickets[i].cancelled)
                    st_publish_event(e, &e->tickets[i], pending ? AZ_CHANGE_TRACK_DATA : AZ_CHANGE_INVALIDATED, rc);
        }
        pthread_mutex_unlock(&e->mu);
    }
}
/* Test shutdown, or after the worker has joined and every consumer has released. */
static inline void st_destroy(struct st_engine *e) {
    for (int i = 0; i < ST_SLOTS; i++) free(e->slots[i].data);
    pthread_mutex_destroy(&e->mu);
}
#endif
