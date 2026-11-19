/* SPDX-License-Identifier: GPL-3.0-only */
/* The confirmed master and BpmState writers share one nonblocking pipe. An
 * individual write is smaller than PIPE_BUF. A thread paused before write has
 * no reservation that can prevent another source or a loss marker from draining. */
#include <limits.h>
enum {
    DS_GUI_MASTER = 0,
    DS_GUI_TEMPO = 1,
    DS_GUI_EFFECTIVE_KEY = 2,
    DS_GUI_PAD = 3,
    DS_GUI_SLIDER = 4,
    DS_GUI_SLIP = 5
};
struct ds_gui_record {
    uint64_t revision, observed, owner, epoch;
    unsigned kind, deck, availability;
    int master;
    double bpm;
    uint64_t original_key_revision;
    unsigned original_key_availability;
    char original_key[4];
    union {
        char key[4];
        int setting;
        double slider;
    } value;
};
_Static_assert(sizeof(struct ds_gui_record) <= PIPE_BUF, "atomic GUI receipt");
static int ds_gui_pipe[2] = {-1, -1}, ds_gui_wake = -1;
static _Atomic unsigned ds_gui_armed, ds_gui_gap;
static void ds_gui_signal(void) {
    uint64_t one = 1;
    ssize_t n = write(ds_gui_wake, &one, sizeof one);
    (void) n;
}
static void ds_gui_send(const struct ds_gui_record *r, unsigned bit, int captured) {
    if (!(atomic_load_explicit(&ds_gui_armed, memory_order_acquire) & bit)) return;
    if (!captured || (atomic_load_explicit(&ds_gui_gap, memory_order_acquire) & bit) ||
        write(ds_gui_pipe[1], r, sizeof *r) != (ssize_t) sizeof *r)
        atomic_fetch_or_explicit(&ds_gui_gap, bit, memory_order_release);
    ds_gui_signal();
}
static void ds_master_capture(uint64_t revision, uint64_t observed, int master, int captured) {
    struct ds_gui_record r = {.kind = DS_GUI_MASTER, .revision = revision, .observed = observed, .master = master, .availability = master >= 0 && observed ? AZ_AVAIL_READY : AZ_AVAIL_STALE};
    ds_gui_send(&r, 1, captured);
}
/* Caller holds the master writer's publication lock. */
static struct ds_gui_record ds_master_seed(void) {
    uint64_t revision = atomic_load_explicit(&g_master_event, memory_order_acquire);
    int valid = revision && revision != UINT64_MAX && revision == g_master_state.revision &&
                g_master_state.deck >= 0 && g_master_state.observed_ns;
    return (struct ds_gui_record) {.kind = DS_GUI_MASTER, .revision = revision, .observed = valid ? g_master_state.observed_ns : ds_now(), .master = valid ? g_master_state.deck : -1, .availability = valid ? AZ_AVAIL_READY : revision ? AZ_AVAIL_STALE
                                                                                                                                                                                                                                         : AZ_AVAIL_LOADING};
}
static void ds_master_apply(const struct ds_gui_record *r, int gap) {
    static int previous[4] = {-99, -99, -99, -99};
    static uint64_t revisions[4];
    for (unsigned d = 0; d < 4; d++) {
        int value = (int) d == r->master;
        int code = r->availability == AZ_AVAIL_READY ? value : -(int) r->availability - 1;
        if (code != previous[d] && revisions[d] != UINT64_MAX) {
            previous[d] = code;
            revisions[d]++;
        }
        uint64_t id = AZSP_MASTER;
        struct ds_field f = {AZ_STATUS_SYNC, AZ_STATUS_SYNC_MASTER, {.info = {r->availability, AZ_PROV_NATIVE, revisions[d], r->observed}, .scope = AZ_STATUS_UNIT, .scope_id = id, .provider_id = id, .count = r->availability == AZ_AVAIL_READY}, &value, NULL};
        ds_publish(d, 0, DS_UNIT_INPUT, r->observed, &f, 1, NULL, NULL,
                   gap ? AZ_STATUS_TOPIC(AZ_STATUS_SYNC) : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
    }
}
#include "sdk_status_tempo.c"
#include "sdk_status_gui_models.c"
#include "sdk_status_pad.c"
static int ds_gui_dispatch(const struct ds_gui_record *record, int gap) {
    switch (record->kind) {
    case DS_GUI_MASTER: ds_master_apply(record, gap); break;
    case DS_GUI_TEMPO: ds_tempo_apply(record, gap); break;
    case DS_GUI_PAD: ds_pad_apply(record, gap); break;
    case DS_GUI_EFFECTIVE_KEY: return ds_model_apply(DS_MODEL_KEY, record, gap);
    case DS_GUI_SLIP: return ds_model_apply(DS_MODEL_SLIP, record, gap);
    case DS_GUI_SLIDER: return ds_model_apply(DS_MODEL_SLIDER, record, gap);
    default: return AZ_INVALID;
    }
    return AZ_OK;
}
static int ds_gui_delivered(const struct ds_gui_record *record, int gap, const uint64_t delivered[21], unsigned index) {
    return record->deck >= 4 || index >= 21 || (!gap && delivered[index] && record->revision <= delivered[index]);
}
static int ds_gui_commit(const struct ds_gui_record *r, int gap, uint64_t delivered[21]) {
    unsigned i = r->kind == DS_GUI_MASTER ? 0 : (r->kind - 1) * 4 + r->deck + 1;
    if (ds_gui_delivered(r, gap, delivered, i)) return AZ_OK;
    int rc = ds_gui_dispatch(r, gap);
    if ((rc == AZ_OK || rc == AZ_LIMIT) && r->revision > delivered[i]) delivered[i] = r->revision;
    return rc;
}
enum {
    DS_GUI_PENDING = 32
};
struct ds_gui_pending {
    struct {
        struct ds_gui_record record;
        int gap;
    } entries[DS_GUI_PENDING];
    unsigned count;
};
struct ds_gui_cursor {
    uint64_t delivered[21];
    struct ds_gui_pending pending[4];
};
/* Only this reducer thread owns the queues. Native LOAD adoption signals its
 * eventfd; queued GUI receipts never require another firmware write or poll. */
static void ds_gui_flush(unsigned deck, struct ds_gui_cursor *cursor) {
    struct ds_gui_pending *q = &cursor->pending[deck];
    while (q->count) {
        const struct ds_gui_record *r = &q->entries[0].record;
        int obsolete = 0;
        if (r->owner && r->epoch) {
            pthread_mutex_lock(&ds.mu);
            uint64_t epoch = ds.deck[deck].current.epoch, last = ds.deck[deck].last_epoch;
            pthread_mutex_unlock(&ds.mu);
            if (r->epoch != epoch) {
                if (r->epoch > last) return;
                obsolete = 1;
            }
        }
        if (!obsolete && ds_gui_commit(r, q->entries[0].gap, cursor->delivered) == AZ_STALE) return;
        --q->count;
        memmove(q->entries, q->entries + 1, q->count * sizeof q->entries[0]);
    }
}
static void ds_gui_apply(const struct ds_gui_record *r, int gap, struct ds_gui_cursor *cursor) {
    if (r->kind != DS_GUI_EFFECTIVE_KEY || r->deck >= 4) {
        (void) ds_gui_commit(r, gap, cursor->delivered);
        return;
    }
    unsigned index = (r->kind - 1) * 4 + r->deck + 1;
    if (!gap && r->revision <= cursor->delivered[index]) return;
    struct ds_gui_pending *q = &cursor->pending[r->deck];
    if (!gap && q->count && r->revision <= q->entries[q->count - 1].record.revision) return;
    if (q->count == DS_GUI_PENDING) {
        q->count = 0;
        gap = 1;
    }
    q->entries[q->count].record = *r;
    q->entries[q->count++].gap = gap;
    /* Recheck after parking: LOAD may have signaled before this write reached
     * the queue. This closes both orders of the adoption/wake race. */
    ds_gui_flush(r->deck, cursor);
}
static void ds_gui_load(unsigned deck) {
    if (deck < 4 && (atomic_load_explicit(&ds_gui_armed, memory_order_acquire) & ds_model_bit(DS_MODEL_KEY, deck)))
        ds_gui_signal();
}
static void ds_gui_reseed_models(unsigned gap, struct ds_gui_cursor *cursor) {
    for (unsigned model = 0; model < DS_MODEL_COUNT; model++)
        for (unsigned deck = 0; deck < 4; deck++)
            if (gap & ds_model_bit(model, deck)) {
                struct ds_gui_record record = ds_model_seed(model, deck);
                ds_gui_apply(&record, 1, cursor);
            }
}
static void ds_gui_reseed(unsigned gap, struct ds_gui_cursor *cursor) {
    if (gap & 1) {
        pthread_mutex_lock(&g_master_state_mu);
        struct ds_gui_record record = ds_master_seed();
        pthread_mutex_unlock(&g_master_state_mu);
        ds_gui_apply(&record, 1, cursor);
    }
    for (unsigned deck = 0; deck < 4; deck++)
        if (gap & (2u << deck)) {
            struct ds_gui_record record = ds_tempo_seed(deck);
            ds_gui_apply(&record, 1, cursor);
        }
    ds_gui_reseed_models(gap, cursor);
    for (unsigned deck = 0; deck < 4; deck++)
        if (gap & ds_pad_bit(deck)) {
            struct ds_gui_record record = ds_pad_seed(deck);
            ds_gui_apply(&record, 1, cursor);
        }
}
static void ds_gui_drain(struct ds_gui_cursor *cursor) {
    struct ds_gui_record record;
    while (read(ds_gui_pipe[0], &record, sizeof record) == (ssize_t) sizeof record) ds_gui_apply(&record, 0, cursor);
    unsigned gap = atomic_exchange_explicit(&ds_gui_gap, 0, memory_order_acq_rel);
    ds_gui_reseed(gap, cursor);
    for (unsigned deck = 0; deck < 4; ++deck) ds_gui_flush(deck, cursor);
}
static void *ds_gui_run(void *unused) {
    (void) unused;
    struct pollfd fd = {.fd = ds_gui_wake, .events = POLLIN};
    struct ds_gui_cursor cursor = {0};
    for (;;) {
        int rc = poll(&fd, 1, -1);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0 || !(fd.revents & POLLIN)) break;
        uint64_t signal;
        if (read(fd.fd, &signal, sizeof signal) < 0 && errno != EAGAIN && errno != EINTR) break;
        ds_gui_drain(&cursor);
    }
    return NULL;
}
static unsigned ds_gui_models_wanted(uint64_t topics) {
    unsigned wanted = 0;
    for (unsigned model = 0; model < DS_MODEL_COUNT; model++)
        if ((topics & AZ_STATUS_TOPIC(ds_models[model].topic)) && atomic_load(&ds_model_ready[model]))
            wanted |= 15u * ds_model_bit(model, 0);
    return wanted;
}
static unsigned ds_gui_wanted(uint64_t topics) {
    unsigned wanted = (topics & AZ_STATUS_TOPIC(AZ_STATUS_SYNC)) && master_state_supported() ? 1u : 0u;
    if ((topics & AZ_STATUS_TOPIC(AZ_STATUS_TEMPO)) && atomic_load(&ds_tempo_ready)) wanted |= 30u;
    wanted |= ds_gui_models_wanted(topics);
    if ((topics & AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS)) && atomic_load(&ds_pad_ready)) wanted |= 15u << 9;
    return wanted;
}
static int ds_gui_start(void) {
    if (pipe2(ds_gui_pipe, O_NONBLOCK | O_CLOEXEC) < 0) return 0;
    ds_gui_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_t worker;
    if (ds_gui_wake < 0 || az_thread_create(&worker, NULL, ds_gui_run, NULL)) {
        close(ds_gui_pipe[0]);
        close(ds_gui_pipe[1]);
        if (ds_gui_wake >= 0) close(ds_gui_wake);
        return 0;
    }
    pthread_detach(worker);
    return 1;
}
static void ds_gui_arm_master(unsigned added) {
    if (!(added & 1)) return;
    pthread_mutex_lock(&g_master_state_mu);
    atomic_fetch_or_explicit(&ds_gui_armed, 1, memory_order_release);
    struct ds_gui_record seed = ds_master_seed();
    ds_gui_send(&seed, 1, 1);
    pthread_mutex_unlock(&g_master_state_mu);
}
static void ds_gui_arm_tempo(unsigned added) {
    for (unsigned deck = 0; deck < 4; deck++)
        if (added & (2u << deck)) {
            atomic_fetch_or_explicit(&ds_gui_armed, 2u << deck, memory_order_release);
            struct ds_gui_record seed = ds_tempo_seed(deck);
            if (seed.availability != AZ_AVAIL_STALE || seed.revision) ds_gui_send(&seed, 2u << deck, 1);
        }
}
static void ds_gui_arm_models(unsigned added) {
    for (unsigned model = 0; model < DS_MODEL_COUNT; model++)
        for (unsigned deck = 0; deck < 4; deck++) {
            unsigned bit = ds_model_bit(model, deck);
            if (!(added & bit)) continue;
            atomic_fetch_or_explicit(&ds_gui_armed, bit, memory_order_release);
            struct ds_gui_record seed = ds_model_seed(model, deck);
            if (seed.revision) ds_gui_send(&seed, bit, 1);
        }
}
static void ds_gui_arm_pads(unsigned added) {
    for (unsigned deck = 0; deck < 4; deck++)
        if (added & ds_pad_bit(deck)) {
            unsigned bit = ds_pad_bit(deck);
            atomic_fetch_or(&ds_gui_armed, bit);
            struct ds_gui_record seed = ds_pad_seed(deck);
            if (seed.revision) ds_gui_send(&seed, bit, 1);
        }
}
static void ds_gui_interest(uint64_t topics) {
    unsigned wanted = ds_gui_wanted(topics);
    unsigned before = atomic_load(&ds_gui_armed);
    unsigned added = wanted & ~before;
    if (!added) return;
    if (!before && !ds_gui_start()) return;
    ds_gui_arm_master(added);
    ds_gui_arm_tempo(added);
    ds_gui_arm_models(added);
    ds_gui_arm_pads(added);
}
