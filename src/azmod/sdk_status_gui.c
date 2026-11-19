/* SPDX-License-Identifier: GPL-3.0-only */
/* The confirmed master and BpmState writers share one nonblocking pipe. An
 * individual write is smaller than PIPE_BUF. A thread paused before write has
 * no reservation that can prevent another source or a loss marker from draining. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sdk_status_gui.h"
#include "camelot_text.h"
#include "sdk_status_engine.h"
#include "sdk_status_native.h"
#include "sdk_status_store.h"
#include "sdk_status_native_host.h"
#include "../common/azstatus_identity.h"
#include "../common/aztimestamp.h"
#ifndef az_thread_create
#include "../common/azaltstack.h"
#endif
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <sys/eventfd.h>
#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#define DS_BPM_LIVE 0xa0
#define DS_BPM_CONTROL_OFFSET 16
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
void ds_master_capture(uint64_t revision, uint64_t observed, int master, int captured) {
    struct ds_gui_record r = {.kind = DS_GUI_MASTER, .revision = revision, .observed = observed, .master = master, .availability = master >= 0 && observed ? AZ_AVAIL_READY : AZ_AVAIL_STALE};
    ds_gui_send(&r, 1, captured);
}
/* Caller holds the master writer's publication lock. */
static struct ds_gui_record ds_master_seed(const struct ds_master_state *state) {
    uint64_t revision = state->event;
    int valid = revision && revision != UINT64_MAX && revision == state->revision &&
                state->deck >= 0 && state->observed;
    return (struct ds_gui_record) {.kind = DS_GUI_MASTER, .revision = revision, .observed = valid ? state->observed : ds_now(), .master = valid ? state->deck : -1, .availability = valid ? AZ_AVAIL_READY : revision ? AZ_AVAIL_STALE
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
/* SPDX-License-Identifier: GPL-3.0-only */
/* EP147's BpmState update has two confirmed stores to +0xa0 and an
 * unavailable (DBL_MAX) branch. Only its own object and generation are read
 * after the original returns. The +0xb0 value is unpitched BPM at the current
 * source position and may vary with the grid; it is not original metadata BPM. */
static void (*ds_tempo_original)(void *, void *);
static void (*ds_tempo_dispose_original)(void *);
static _Atomic int ds_tempo_ready;
struct ds_tempo_slot {
    _Atomic unsigned busy, reseed, collision;
    _Atomic uint64_t retired;
    struct ds_gui_record latest;
};
static struct ds_tempo_slot ds_tempo_slots[4];

static void ds_tempo_gap(unsigned d) {
    struct ds_gui_record gap = {.kind = DS_GUI_TEMPO, .deck = d};
    ds_gui_send(&gap, 2u << d, 0);
}
static void ds_tempo_store(unsigned d, uint64_t owner, uint64_t epoch, double bpm, unsigned avail) {
    struct ds_tempo_slot *s = &ds_tempo_slots[d];
    struct ds_gui_record *r = &s->latest;
    if (r->owner == owner && r->epoch == epoch && r->availability == avail && (avail != AZ_AVAIL_READY || r->bpm == bpm) &&
        !atomic_exchange_explicit(&s->reseed, 0, memory_order_acq_rel)) return;
    if (r->revision == UINT64_MAX) {
        ds_tempo_gap(d);
        return;
    }
    r->revision++;
    r->deck = d;
    r->kind = DS_GUI_TEMPO;
    r->observed = ds_now();
    r->epoch = epoch;
    r->owner = owner;
    r->bpm = bpm;
    r->availability = avail;
    ds_gui_send(r, 2u << d, 1);
}
/* Birth is called after the existing bounded weak pin has been installed.
 * The ctor owns its object. A failed capture cannot seed a previous owner. */
void ds_tempo_birth(unsigned d, uint64_t owner) {
    if (d >= 4 || !atomic_load_explicit(&ds_tempo_ready, memory_order_acquire)) return;
    struct ds_tempo_slot *s = &ds_tempo_slots[d];
    if (atomic_exchange_explicit(&s->busy, 1, memory_order_acquire)) {
        ds_tempo_gap(d);
        return;
    }
    ds_tempo_store(d, owner, 0, 0, owner ? AZ_AVAIL_LOADING : AZ_AVAIL_STALE);
    atomic_store_explicit(&s->busy, 0, memory_order_release);
}
static int ds_tempo_deck(uintptr_t object) {
    for (unsigned d = 0; d < 4; d++)
        if (az_status_host_bpm_object(d) == object) return (int) d;
    return -1;
}

static int ds_tempo_owner_current(unsigned deck, void *self, uint64_t owner) {
    return owner && owner != atomic_load_explicit(&ds_tempo_slots[deck].retired, memory_order_acquire) &&
           (uintptr_t) self == az_status_host_bpm_object(deck) &&
           owner == az_status_host_bpm_generation(deck);
}
static int ds_tempo_same_load(unsigned deck, const struct azstatus_native_receipt *before) {
    struct azstatus_native_receipt after;
    return ds_native_load_seed(deck, &after) && after.epoch == before->epoch &&
           after.unit == before->unit && after.load_generation == before->load_generation;
}
static void ds_tempo_publish_current(unsigned deck, void *self, uint64_t owner, const struct azstatus_native_receipt *before) {
    uint64_t raw = __atomic_load_n((uint64_t *) ((uintptr_t) self + DS_BPM_LIVE), __ATOMIC_RELAXED);
    double bpm;
    memcpy(&bpm, &raw, sizeof bpm);
    int ok = isfinite(bpm) && bpm > 0 && bpm < DBL_MAX;
    int fenced = ds_tempo_same_load(deck, before);
    if (atomic_exchange_explicit(&ds_tempo_slots[deck].collision, 0, memory_order_acq_rel)) fenced = 0;
    ds_tempo_store(deck, owner, fenced ? before->load_generation : 0, fenced && ok ? bpm : 0,
                   fenced && ok ? AZ_AVAIL_READY : AZ_AVAIL_STALE);
}
static void ds_tempo_writer(void *self, void *input) {
    int d = ds_tempo_deck((uintptr_t) self);
    if (d < 0 || !atomic_load_explicit(&ds_tempo_ready, memory_order_acquire)) {
        ds_tempo_original(self, input);
        return;
    }
    struct ds_tempo_slot *s = &ds_tempo_slots[d];
    uint64_t owner = az_status_host_bpm_generation(d);
    if (atomic_exchange_explicit(&s->busy, 1, memory_order_acquire)) {
        atomic_store_explicit(&s->collision, 1, memory_order_release);
        ds_tempo_gap(d);
        ds_tempo_original(self, input);
        ds_tempo_gap(d);
        return;
    }
    struct azstatus_native_receipt before;
    int armed = ds_native_load_seed(d, &before);
    ds_tempo_original(self, input);
    if (armed && ds_tempo_owner_current(d, self, owner))
        ds_tempo_publish_current(d, self, owner, &before);
    else ds_tempo_store(d, owner, 0, 0, AZ_AVAIL_STALE);
    atomic_store_explicit(&s->busy, 0, memory_order_release);
}
static void ds_tempo_dispose(void *ctrl) {
    int d = ds_tempo_deck((uintptr_t) ctrl + DS_BPM_CONTROL_OFFSET);
    if (d >= 0 && atomic_load_explicit(&ds_tempo_ready, memory_order_acquire)) {
        struct ds_tempo_slot *s = &ds_tempo_slots[d];
        uint64_t owner = az_status_host_bpm_generation(d);
        atomic_store_explicit(&s->retired, owner, memory_order_release);
        if (!atomic_exchange_explicit(&s->busy, 1, memory_order_acquire)) {
            ds_tempo_store(d, owner, 0, 0, AZ_AVAIL_STALE);
            atomic_store_explicit(&s->busy, 0, memory_order_release);
        } else ds_tempo_gap(d);
    }
    ds_tempo_dispose_original(ctrl);
}
static struct ds_gui_record ds_tempo_seed(unsigned d) {
    struct ds_tempo_slot *s = &ds_tempo_slots[d];
    struct ds_gui_record r = {.kind = DS_GUI_TEMPO, .deck = d, .observed = ds_now(), .availability = AZ_AVAIL_LOADING};
    atomic_store_explicit(&s->reseed, 1, memory_order_release);
    if (atomic_exchange_explicit(&s->busy, 1, memory_order_acquire)) {
        r.availability = AZ_AVAIL_STALE;
        return r;
    }
    if (s->latest.revision) r = s->latest;
    if (r.owner && (r.owner == atomic_load_explicit(&s->retired, memory_order_acquire) ||
                    r.owner != az_status_host_bpm_generation(d))) r.availability = AZ_AVAIL_STALE;
    atomic_store_explicit(&s->busy, 0, memory_order_release);
    return r;
}
static void ds_tempo_apply(const struct ds_gui_record *r, int gap) {
    uint64_t id = r->owner ? AZSP_TEMPO + r->owner : 0;
    struct ds_field f = {AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_CURRENT_BPM, {.info = {r->availability, AZ_PROV_NATIVE, r->revision, r->observed}, .scope = AZ_STATUS_UNIT, .scope_id = id, .provider_id = id, .count = r->availability == AZ_AVAIL_READY}, &r->bpm, NULL};
    /* The GUI owns the BPM value. A separate matched native fence prevents a
     * delayed observation from populating a replacement track on this deck. */
    ds_publish(r->deck, r->epoch, r->epoch ? 0 : DS_UNIT_INPUT, r->observed, &f, 1, NULL, NULL,
               gap ? AZ_STATUS_TOPIC(AZ_STATUS_TEMPO) : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
}
static int ds_tempo_target_valid(uintptr_t writer, uintptr_t dispose) {
    const uint32_t expected[] = {0xa9b87bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3};
    uintptr_t target;
    return az_status_host_image_word(az_status_host_bpm_control() + 16, &target) && target == dispose && az_status_host_image_contains(writer, sizeof expected) && !memcmp((void *) writer, expected, sizeof expected);
}
void ds_tempo_install(void) {
    if (access("/home/root/settings/azmod.deck-status.off", F_OK) == 0 ||
        (az_status_host_build() != DS_BUILD_130 && az_status_host_build() != DS_BUILD_200)) return;
    const uintptr_t writer = az_status_host_build() == DS_BUILD_130 ? 0x1f5c8c8 : 0x20c8c40;
    const uintptr_t dispose = az_status_host_build() == DS_BUILD_130 ? 0x1fcbe20 : 0x21394f0;
    if (!ds_tempo_target_valid(writer, dispose)) return;
    if (az_status_host_patch_vtable(az_status_host_bpm_control() + 16, (void *) ds_tempo_dispose, (void **) &ds_tempo_dispose_original) != 1 ||
        az_status_host_install_hook(writer, (void *) ds_tempo_writer, (void **) &ds_tempo_original) != 1) return;
    atomic_store_explicit(&ds_tempo_ready, 1, memory_order_release);
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* PlayerInfoState owns both per-deck GUI models. The key model displays the
 * effective pitch including a moving pitch fader. SlipState stores mode at
 * +0xa8; its independent +0xa9 flag means a background position is active. */
enum {
    DS_MODEL_KEY,
    DS_MODEL_SLIP,
    DS_MODEL_SLIDER,
    DS_MODEL_COUNT
};
struct ds_model_profile {
    unsigned kind, topic, field;
    unsigned parent_offset, deck_offset;
    uintptr_t control[2], writer[2], dispose[2];
    uint32_t first;
};
static const struct ds_model_profile ds_models[] = {
    {DS_GUI_EFFECTIVE_KEY, AZ_STATUS_KEY, AZ_STATUS_KEY_EFFECTIVE, 0x1090, 0xd4, {0x2dedea0, 0x31a12b0}, {0x1f8d298, 0x20f95f8}, {0x1fcbe18, 0x21394e8}, 0xa9b77bfd},
    {DS_GUI_SLIP, AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_SLIP, 0x1120, 0x134, {0x2dee098, 0x31a14a8}, {0x1fb5ce8, 0x2123400}, {0x1fcbe30, 0x2139500}, 0xa9b57bfd},
    {DS_GUI_SLIDER, AZ_STATUS_TEMPO, AZ_STATUS_TEMPO_SLIDER_PERCENT, 0x10e0, 0xb0, {0x2dedfb8, 0x31a13c8}, {0x1fb97f0, 0x2126f10}, {0x1fcbe10, 0x21394e0}, 0xa9b87bfd}};
static void (*ds_model_original[DS_MODEL_COUNT])(void *, void *);
static void (*ds_model_dispose_original[DS_MODEL_COUNT])(void *);
static _Atomic int ds_model_ready[DS_MODEL_COUNT];
struct ds_model_slot {
    pthread_mutex_t mu;
    _Atomic uintptr_t object;
    _Atomic uint64_t fault, revision;
    uint64_t sealed_fault, incarnation;
    struct ds_gui_record latest;
};
#define DS_MODEL_SLOT {.mu = PTHREAD_MUTEX_INITIALIZER}
static struct ds_model_slot ds_model_slots[DS_MODEL_COUNT][4] = {
    {DS_MODEL_SLOT, DS_MODEL_SLOT, DS_MODEL_SLOT, DS_MODEL_SLOT},
    {DS_MODEL_SLOT, DS_MODEL_SLOT, DS_MODEL_SLOT, DS_MODEL_SLOT},
    {DS_MODEL_SLOT, DS_MODEL_SLOT, DS_MODEL_SLOT, DS_MODEL_SLOT}};
#undef DS_MODEL_SLOT
static unsigned ds_model_bit(unsigned model, unsigned d) {
    return 1u << ((ds_models[model].kind - 1) * 4 + d + 1);
}
static uintptr_t ds_model_ctrl(unsigned model) { return ds_models[model].control[az_status_host_build() == DS_BUILD_200]; }
static int ds_model_deck(unsigned model, uintptr_t self) {
    const struct ds_model_profile *p = &ds_models[model];
    unsigned d = *(const uint32_t *) (self + p->deck_offset);
    if (d >= 4) return -1;
    if (!az_status_host_model_owner(d, p->parent_offset, self) ||
        *(const uintptr_t *) (self - 16) != ds_model_ctrl(model) ||
        __atomic_load_n((const int *) (self - 8), __ATOMIC_ACQUIRE) <= 0) return -1;
    return (int) d;
}
static void ds_model_gap(unsigned model, unsigned d) {
    atomic_fetch_add_explicit(&ds_model_slots[model][d].fault, 1, memory_order_release);
    struct ds_gui_record r = {.kind = ds_models[model].kind, .deck = d};
    ds_gui_send(&r, ds_model_bit(model, d), 0);
}
static uint64_t ds_model_revision(struct ds_model_slot *s) {
    uint64_t n = atomic_load_explicit(&s->revision, memory_order_relaxed);
    while (n < UINT64_MAX)
        if (atomic_compare_exchange_weak_explicit(&s->revision, &n, n + 1, memory_order_relaxed, memory_order_relaxed)) return n + 1;
    return 0;
}
static int ds_model_key_identity(const struct ds_model_slot *slot, const struct ds_gui_record *next, uint64_t fault) {
    const struct ds_gui_record *old = &slot->latest;
    return old->owner == next->owner && old->epoch == next->epoch && fault == slot->sealed_fault &&
           old->original_key_availability == next->original_key_availability &&
           !memcmp(old->original_key, next->original_key, sizeof next->original_key);
}
static int ds_model_same(const struct ds_model_slot *slot, const struct ds_gui_record *next, uint64_t fault) {
    const struct ds_gui_record *old = &slot->latest;
    return old->revision && old->revision == atomic_load(&slot->revision) &&
           ds_model_key_identity(slot, next, fault) && old->availability == next->availability &&
           !memcmp(&old->value, &next->value, sizeof next->value);
}
/* Called with the slot locked. Recovery can reserve a newer revision even
 * if a firmware writer is suspended while holding this slot. */
static void ds_model_store(unsigned model, unsigned d, struct ds_gui_record r, uint64_t fault) {
    struct ds_model_slot *s = &ds_model_slots[model][d];
    struct ds_gui_record *old = &s->latest;
    if (ds_model_same(s, &r, fault)) return;
    r.revision = ds_model_revision(s);
    if (!r.revision || fault != atomic_load_explicit(&s->fault, memory_order_acquire)) {
        ds_model_gap(model, d);
        return;
    }
    r.original_key_revision = ds_model_key_identity(s, &r, fault) && old->original_key_revision
                                  ? old->original_key_revision
                                  : r.revision;
    r.deck = d;
    r.kind = ds_models[model].kind;
    r.observed = ds_now();
    *old = r;
    s->sealed_fault = fault;
    ds_gui_send(&r, ds_model_bit(model, d), 1);
}
static void ds_model_unlock(pthread_mutex_t **mu) { pthread_mutex_unlock(*mu); }
static int ds_model_adopt(struct ds_model_slot *slot, uintptr_t object) {
    if (atomic_load_explicit(&slot->object, memory_order_acquire) == object) return 1;
    if (slot->incarnation == (UINT64_C(1) << 40) - 1) return 0;
    ++slot->incarnation;
    atomic_store_explicit(&slot->object, object, memory_order_release);
    return 1;
}
static unsigned ds_model_read_key(const unsigned char *key, char output[4]) {
    if (key[3] == 1 && key[0] < 12 && key[1] <= 1) {
        ct_camelot(key[0], key[1], output);
        return AZ_AVAIL_READY;
    }
    return !key[3] ? AZ_AVAIL_EMPTY : AZ_AVAIL_STALE;
}
static void ds_model_read_slider(void *self, struct ds_gui_record *record) {
    float ratio;
    memcpy(&ratio, (const unsigned char *) self + 0xa4, sizeof ratio);
    if (isfinite(ratio) && fabsf(ratio) < FLT_MAX) {
        record->value.slider = 100.0 * (double) ratio;
        record->availability = AZ_AVAIL_READY;
    }
}
static int ds_model_read(unsigned model, unsigned deck, void *self, const struct azstatus_native_receipt *before,
                         struct ds_gui_record *record) {
    if (model == DS_MODEL_KEY) {
        if (!ds_tempo_same_load(deck, before)) return 0;
        record->epoch = before->load_generation;
        record->availability = ds_model_read_key((const unsigned char *) self + 0xa4, record->value.key);
        record->original_key_availability = ds_model_read_key((const unsigned char *) self + 0xb0, record->original_key);
    } else if (model == DS_MODEL_SLIDER) ds_model_read_slider(self, record);
    else {
        unsigned enabled = *((const unsigned char *) self + 0xa8);
        if (enabled <= 1) {
            record->value.setting = (int) enabled;
            record->availability = AZ_AVAIL_READY;
        }
    }
    return 1;
}
static int ds_model_capture_current(unsigned model, unsigned deck, uintptr_t object, uint64_t fault) {
    const struct ds_model_slot *slot = &ds_model_slots[model][deck];
    return atomic_load_explicit(&slot->object, memory_order_acquire) == object &&
           atomic_load_explicit(&slot->fault, memory_order_acquire) == fault &&
           ds_model_deck(model, object) == (int) deck;
}
static void ds_model_observe(unsigned model, unsigned deck, void *self, void *input) {
    struct ds_model_slot *slot = &ds_model_slots[model][deck];
    uint64_t fault = atomic_load_explicit(&slot->fault, memory_order_acquire);
    struct azstatus_native_receipt before;
    int active = model != DS_MODEL_KEY || ds_native_load_seed(deck, &before);
    ds_model_original[model](self, input);
    if (!active || !ds_model_capture_current(model, deck, (uintptr_t) self, fault)) return;
    struct ds_gui_record record = {.availability = AZ_AVAIL_STALE,
                                   .owner = AZSP_GUI_MODEL | ((uint64_t) model << 48) | ((uint64_t) deck << 40) | slot->incarnation};
    if (ds_model_read(model, deck, self, &before, &record)) ds_model_store(model, deck, record, fault);
}
static void ds_model_writer(unsigned model, void *self, void *input) {
    void (*original)(void *, void *) = ds_model_original[model];
    if (!atomic_load_explicit(&ds_model_ready[model], memory_order_acquire)) {
        original(self, input);
        return;
    }
    uintptr_t object = (uintptr_t) self;
    int d = ds_model_deck(model, object);
    if (d < 0) {
        original(self, input);
        return;
    }
    struct ds_model_slot *s = &ds_model_slots[model][d];
    if (pthread_mutex_trylock(&s->mu)) {
        ds_model_gap(model, d);
        original(self, input);
        ds_model_gap(model, d);
        return;
    }
    pthread_mutex_t *locked __attribute__((cleanup(ds_model_unlock))) = &s->mu;
    (void) locked;
    if (!ds_model_adopt(s, object)) {
        ds_model_gap(model, d);
        original(self, input);
        return;
    }
    ds_model_observe(model, (unsigned) d, self, input);
}
static void ds_model_dispose(unsigned model, void *ctrl) {
    uintptr_t object = (uintptr_t) ctrl + 16;
    for (unsigned d = 0; d < 4; d++) {
        struct ds_model_slot *s = &ds_model_slots[model][d];
        uintptr_t expected = object;
        if (!atomic_compare_exchange_strong_explicit(&s->object, &expected, 0, memory_order_acq_rel, memory_order_acquire)) continue;
        if (!pthread_mutex_trylock(&s->mu)) {
            if (s->latest.owner && !atomic_load_explicit(&s->object, memory_order_acquire)) {
                struct ds_gui_record r = {.epoch = s->latest.epoch, .owner = s->latest.owner, .availability = AZ_AVAIL_STALE, .original_key_availability = AZ_AVAIL_STALE};
                ds_model_store(model, d, r, atomic_load(&s->fault));
            }
            pthread_mutex_unlock(&s->mu);
        } else ds_model_gap(model, d);
    }
    ds_model_dispose_original[model](ctrl);
}
static struct ds_gui_record ds_model_seed(unsigned model, unsigned d) {
    struct ds_model_slot *s = &ds_model_slots[model][d];
    if (pthread_mutex_trylock(&s->mu))
        return (struct ds_gui_record) {.deck = d, .kind = ds_models[model].kind, .availability = AZ_AVAIL_STALE, .observed = ds_now(), .revision = ds_model_revision(s)};
    if ((!atomic_load_explicit(&s->object, memory_order_acquire) || s->sealed_fault != atomic_load(&s->fault)) &&
        s->latest.revision && s->latest.availability != AZ_AVAIL_STALE) {
        s->latest.availability = AZ_AVAIL_STALE;
        s->latest.revision = ds_model_revision(s);
        s->latest.original_key_availability = AZ_AVAIL_STALE;
        s->latest.original_key_revision = s->latest.revision;
        memset(s->latest.original_key, 0, sizeof s->latest.original_key);
        s->latest.observed = ds_now();
        memset(&s->latest.value, 0, sizeof s->latest.value);
    }
    struct ds_gui_record r = s->latest;
    r.deck = d;
    r.kind = ds_models[model].kind;
    pthread_mutex_unlock(&s->mu);
    return r;
}
struct ds_model_output {
    struct ds_field fields[2];
    unsigned count, flags;
    uint64_t topic, epoch;
};
static struct ds_model_output ds_model_output(unsigned model, uint64_t epoch) {
    const struct ds_model_profile *profile = &ds_models[model];
    return (struct ds_model_output) {
        .fields = {{.topic = profile->topic, .field = profile->field},
                   {.topic = AZ_STATUS_KEY, .field = AZ_STATUS_KEY_ORIGINAL}},
        .count = model == DS_MODEL_KEY ? 2 : 1,
        .flags = model != DS_MODEL_KEY ? DS_UNIT_INPUT : 0,
        .topic = AZ_STATUS_TOPIC(profile->topic),
        .epoch = epoch};
}
static void ds_model_withdraw_field(unsigned model, const struct ds_gui_record *record, uint64_t epoch,
                                    struct ds_field *field) {
    int withdraw = field->info.info.provenance == AZ_PROV_NATIVE &&
                   (field->info.scope == AZ_STATUS_UNIT || (epoch && field->info.scope_id == epoch));
    if (!withdraw) {
        field->info = (az_status_field_info_v1) {0};
        field->info.scope = model != DS_MODEL_KEY ? AZ_STATUS_UNIT : AZ_STATUS_LOAD_SCOPE;
        field->info.scope_id = model == DS_MODEL_KEY ? epoch : 0;
        field->info.info.provenance = AZ_PROV_NATIVE;
    }
    field->info.info.availability = AZ_AVAIL_STALE;
    field->info.info.observed_ns = record->observed;
    field->info.info.revision = record->revision;
    field->info.count = 0;
}
static void ds_model_withdraw(unsigned model, const struct ds_gui_record *record, struct ds_model_output *output) {
    ds_read_receipts(record->deck, output->fields, output->count, &output->epoch);
    for (unsigned i = 0; i < output->count; i++) ds_model_withdraw_field(model, record, output->epoch, &output->fields[i]);
    output->flags = model == DS_MODEL_KEY ? DS_FIELD_LOSS | DS_NATIVE_INPUT : DS_UNIT_INPUT;
}
static const void *ds_model_value(unsigned model, const struct ds_gui_record *record) {
    if (model == DS_MODEL_KEY) return record->value.key;
    if (model == DS_MODEL_SLIDER) return &record->value.slider;
    return &record->value.setting;
}
static void ds_model_original_key(const struct ds_gui_record *record, struct ds_model_output *output) {
    struct ds_field *field = &output->fields[1];
    field->info = output->fields[0].info;
    field->info.info.availability = record->original_key_availability;
    field->info.info.revision = record->original_key_revision;
    field->info.count = record->original_key_availability == AZ_AVAIL_READY ? (unsigned) strlen(record->original_key) : 0;
    field->value = record->original_key;
}
static void ds_model_output_value(unsigned model, const struct ds_gui_record *record, struct ds_model_output *output) {
    struct ds_field *field = &output->fields[0];
    field->info = (az_status_field_info_v1) {
        .info = {record->availability, AZ_PROV_NATIVE, record->revision, record->observed},
        .scope = model != DS_MODEL_KEY ? AZ_STATUS_UNIT : AZ_STATUS_LOAD_SCOPE,
        .scope_id = model != DS_MODEL_KEY ? record->owner : output->epoch,
        .provider_id = record->owner};
    field->value = ds_model_value(model, record);
    if (record->availability == AZ_AVAIL_READY)
        field->info.count = model != DS_MODEL_KEY ? 1u : (unsigned) strlen(record->value.key);
    if (model == DS_MODEL_KEY) ds_model_original_key(record, output);
}
static void ds_model_reseed_file(unsigned model, const struct ds_gui_record *record, const struct ds_model_output *output,
                                 int result, int missing) {
    if (model == DS_MODEL_KEY && (result == AZ_OK || result == AZ_LIMIT) && missing)
        sdk_status_store_reseed(record->deck, output->epoch, output->topic);
}
static int ds_model_apply(unsigned model, const struct ds_gui_record *record, int gap) {
    struct ds_model_output output = ds_model_output(model, record->epoch);
    if (!record->owner) {
        if (!gap) return AZ_OK;
        ds_model_withdraw(model, record, &output);
        int rc = ds_publish(record->deck, output.epoch, output.flags, record->observed,
                            output.fields, output.count, NULL, NULL, output.topic, AZ_STATUS_CAPTURE_GAP);
        ds_model_reseed_file(model, record, &output, rc, 1);
        return rc;
    }
    ds_model_output_value(model, record, &output);
    if (gap && model == DS_MODEL_KEY) output.flags |= DS_FIELD_LOSS | DS_NATIVE_INPUT;
    int rc = ds_publish(record->deck, output.epoch, output.flags, record->observed, output.fields, output.count,
                        NULL, NULL, gap ? output.topic : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
    ds_model_reseed_file(model, record, &output, rc, record->original_key_availability != AZ_AVAIL_READY);
    return rc;
}
static void ds_key_writer(void *self, void *input) { ds_model_writer(DS_MODEL_KEY, self, input); }
static void ds_key_dispose(void *ctrl) { ds_model_dispose(DS_MODEL_KEY, ctrl); }
static void ds_slip_writer(void *self, void *input) { ds_model_writer(DS_MODEL_SLIP, self, input); }
static void ds_slip_dispose(void *ctrl) { ds_model_dispose(DS_MODEL_SLIP, ctrl); }
static void ds_slider_writer(void *self, void *input) { ds_model_writer(DS_MODEL_SLIDER, self, input); }
static void ds_slider_dispose(void *ctrl) { ds_model_dispose(DS_MODEL_SLIDER, ctrl); }
static int ds_model_target_valid(const struct ds_model_profile *profile, unsigned version) {
    uintptr_t value, writer = profile->writer[version];
    const uint32_t signature[] = {profile->first, 0x910003fd, 0xa90153f3, 0xaa0003f3};
    return az_status_host_image_contains(writer, sizeof signature) &&
           az_status_host_image_word(profile->control[version] + 16, &value) && value == profile->dispose[version] &&
           !memcmp((void *) writer, signature, sizeof signature);
}
void ds_models_install(void) {
    if (access("/home/root/settings/azmod.deck-status.off", F_OK) == 0 ||
        (az_status_host_build() != DS_BUILD_130 && az_status_host_build() != DS_BUILD_200)) return;
    const unsigned version = az_status_host_build() == DS_BUILD_200;
    void (*writers[])(void *, void *) = {ds_key_writer, ds_slip_writer, ds_slider_writer};
    void (*disposes[])(void *) = {ds_key_dispose, ds_slip_dispose, ds_slider_dispose};
    for (unsigned model = 0; model < DS_MODEL_COUNT; model++) {
        const struct ds_model_profile *profile = &ds_models[model];
        if (!ds_model_target_valid(profile, version)) continue;
        if (az_status_host_patch_vtable(profile->control[version] + 16, (void *) disposes[model], (void **) &ds_model_dispose_original[model]) == 1 &&
            az_status_host_install_hook(profile->writer[version], (void *) writers[model], (void **) &ds_model_original[model]) == 1)
            atomic_store_explicit(&ds_model_ready[model], 1, memory_order_release);
    }
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* PadState's message-thread setter and asynchronous closure both write HUI
 * mode. PC mode uses a separate byte and lookup table. Only borrowed native
 * arguments are dereferenced. Cached dependencies are comparison keys. */
struct ds_pad_profile {
    uintptr_t ctrl, dispose, construct_return, switcher, cert;
    uintptr_t entry[8];
};
static const struct ds_pad_profile ds_pad_profiles[2] = {
    {0x2dedf10, 0x1fcbf78, 0x1fca4e8, 0x26deee8, 0x27d19f8, {0x1fa89b0, 0x1fa5db0, 0x1fa5828, 0x1fa6918, 0xaa0c58, 0xaa1da0, 0xe32b20, 0xe33830}},
    {0x31a1320, 0x2139648, 0x2137bb8, 0x2a19958, 0x2b1e618, {0x21160c8, 0x21134c8, 0x2112f40, 0x2114030, 0xae6f50, 0xae8098, 0xebcc00, 0xebd910}}};
static const struct ds_pad_profile *ds_pad_profile;
static _Atomic unsigned ds_pad_ready;
static _Atomic uint64_t ds_pad_birth;
static void *ds_pad_original[8], *ds_pad_dispose_original;
struct ds_pad_slot {
    _Atomic unsigned writing, poisoned;
    _Atomic uintptr_t self;
    _Atomic uintptr_t switcher, cert;
    unsigned hui, alternate, pc, cert_mode, waiting;
    _Atomic uint64_t owner, revision, observed;
    _Atomic unsigned availability;
    _Atomic int value;
    _Atomic uint64_t alternate_calls;
    _Atomic unsigned alternate_active, alternate_exhausted;
    _Atomic uint64_t cert_calls;
    _Atomic unsigned cert_active, cert_exhausted;
};
static struct ds_pad_slot ds_pad_slots[4];
static _Atomic unsigned ds_pad_selected[2]; /* logical deck + 1 after a native selection */
static _Atomic unsigned ds_pad_selecting[2];
static void *ds_pad_select_original;
static uintptr_t ds_pad_word(uintptr_t p) {
    uintptr_t v;
    memcpy(&v, (void *) p, sizeof v);
    return v;
}
static unsigned ds_pad_byte(uintptr_t p) { return *(const unsigned char *) p; }
static unsigned ds_pad_u32(uintptr_t p) {
    uint32_t v;
    memcpy(&v, (void *) p, sizeof v);
    return v;
}
static unsigned ds_pad_bit(unsigned d) { return 512u << d; }
static struct ds_gui_record ds_pad_seed(unsigned d) {
    struct ds_pad_slot *s = &ds_pad_slots[d];
    struct ds_gui_record r = {0};
    r.kind = DS_GUI_PAD;
    r.deck = d;
    r.revision = atomic_load_explicit(&s->revision, memory_order_acquire);
    r.owner = atomic_load(&s->owner);
    r.observed = atomic_load(&s->observed);
    r.master = atomic_load(&s->value);
    r.availability = atomic_load(&s->availability);
    if (atomic_load(&s->writing) || atomic_load(&s->poisoned) || r.revision != atomic_load(&s->revision))
        r.availability = AZ_AVAIL_STALE;
    return r;
}
static void ds_pad_loss(unsigned d) {
    atomic_store(&ds_pad_slots[d].poisoned, 1);
    struct ds_gui_record r = {0};
    ds_gui_send(&r, ds_pad_bit(d), 0);
}
static int ds_pad_try_lock(unsigned d) {
    unsigned idle = 0;
    return atomic_compare_exchange_strong_explicit(&ds_pad_slots[d].writing, &idle, 1, memory_order_acquire, memory_order_relaxed);
}
static int ds_pad_lock(unsigned d) {
    if (ds_pad_try_lock(d)) return 1;
    ds_pad_loss(d);
    return 0;
}
static void ds_pad_publish(unsigned d, unsigned availability, int value, int force) {
    struct ds_pad_slot *s = &ds_pad_slots[d];
    if (!force && atomic_load(&s->availability) == availability &&
        (availability != AZ_AVAIL_READY || atomic_load(&s->value) == value)) return;
    uint64_t rev = atomic_load(&s->revision);
    if (rev == UINT64_MAX) return;
    struct ds_gui_record r = {.kind = DS_GUI_PAD, .deck = d, .revision = rev + 1, .owner = atomic_load(&s->owner), .observed = ds_now(), .master = value, .availability = availability};
    atomic_store(&s->value, value);
    atomic_store(&s->availability, availability);
    atomic_store(&s->observed, r.observed);
    atomic_store_explicit(&s->revision, r.revision, memory_order_release);
    ds_gui_send(&r, ds_pad_bit(d), 1);
}
static int ds_pad_decode(unsigned code) {
    switch (code) {
    case 1:
    case 2: return 0; /* Hot Cue and Gate Cue */
    case 3:
    case 4: return 1; /* Beat Loop pages */
    case 7:
    case 8: return 2; /* Slip Loop pages */
    case 5:
    case 6: return 3; /* Beat Jump pages */
    default: return -1;
    }
}
static unsigned ds_pad_mode_code(const struct ds_pad_slot *slot) {
    static const unsigned char alternate[] = {1, 3, 9, 5, 10, 11, 8, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    if (!slot->pc || slot->cert_mode == 3 || slot->cert_mode == 4)
        return slot->hui < 8 ? slot->hui + 1u : 0;
    return slot->alternate < sizeof alternate ? alternate[slot->alternate] : 0;
}
static void ds_pad_derive(unsigned d, int force) {
    struct ds_pad_slot *s = &ds_pad_slots[d];
    if (!atomic_load_explicit(&s->self, memory_order_acquire)) {
        ds_pad_publish(d, AZ_AVAIL_UNSUPPORTED, 0, force);
        return;
    }
    if (!s->switcher || (s->pc && !s->cert) || s->waiting || s->pc > 1) {
        ds_pad_publish(d, AZ_AVAIL_STALE, 0, force);
        return;
    }
    int value = ds_pad_decode(ds_pad_mode_code(s));
    ds_pad_publish(d, value >= 0 ? AZ_AVAIL_READY : AZ_AVAIL_UNSUPPORTED, value, force);
}
static int ds_pad_observation_deck(uintptr_t object, int born, int deck) {
    for (unsigned d = 0; d < 4; d++)
        if (born ? (int) d == deck : atomic_load(&ds_pad_slots[d].self) == object) return (int) d;
    return -1;
}
static int ds_pad_adopt(struct ds_pad_slot *slot, uintptr_t object, int born) {
    if (!born) return atomic_load(&slot->self) == object;
    uint64_t serial = atomic_fetch_add(&ds_pad_birth, 1) + 1;
    if (!serial || serial > AZSP_SERIAL_MAX) return 0;
    atomic_store(&slot->owner, AZSP_PAD | serial);
    atomic_store(&slot->self, object);
    return 1;
}
static void ds_pad_read(unsigned deck, uintptr_t object, int born) {
    struct ds_pad_slot *slot = &ds_pad_slots[deck];
    uintptr_t switcher = ds_pad_word(object + 8), cert = ds_pad_word(object + 0x38);
    slot->switcher = switcher && ds_pad_word(switcher) == ds_pad_profile->switcher ? switcher : 0;
    slot->cert = cert && ds_pad_word(cert) == ds_pad_profile->cert ? cert : 0;
    slot->hui = ds_pad_u32(object + 0xac);
    slot->alternate = ds_pad_byte(object + 0xb0);
    if (slot->switcher) slot->pc = ds_pad_byte(switcher + 0x58);
    if (slot->cert) slot->cert_mode = ds_pad_byte(cert + 0x22);
    unsigned selected = atomic_load_explicit(&ds_pad_selected[deck % 2], memory_order_acquire);
    slot->waiting = selected && selected != deck + 1;
    int recover = atomic_exchange(&slot->poisoned, 0);
    ds_pad_derive(deck, born || recover);
}
static void ds_pad_observe(uintptr_t object, int born, int deck, int seed_only) {
    if (!object || !atomic_load_explicit(&ds_pad_ready, memory_order_acquire)) return;
    int selected = ds_pad_observation_deck(object, born, deck);
    if (selected < 0) return;
    unsigned d = (unsigned) selected;
    struct ds_pad_slot *slot = &ds_pad_slots[d];
    if (ds_pad_word(object - 16) != ds_pad_profile->ctrl) return;
    if (!(seed_only ? ds_pad_try_lock(d) : ds_pad_lock(d))) return;
    if (!ds_pad_adopt(slot, object, born)) {
        atomic_store(&slot->writing, 0);
        return;
    }
    ds_pad_read(d, object, born);
    atomic_store_explicit(&slot->writing, 0, memory_order_release);
}
static void ds_pad_sample(uintptr_t p, int born, int deck) {
    ds_pad_observe(p, born, deck, 0);
}
/* DeckSelector has already matched the typed button and its native enabled
 * predicate when it dispatches to its listeners. An off-side repaint cannot
 * re-arm the previous logical deck after this cut. */
static void ds_pad_layer(unsigned side, unsigned selected) {
    if (side >= 2 || selected >= 4 || selected % 2 != side || !atomic_load(&ds_pad_ready)) return;
    if (atomic_exchange_explicit(&ds_pad_selected[side], selected + 1, memory_order_acq_rel) == selected + 1) return;
    for (unsigned d = side; d < 4; d += 2)
        if (atomic_load(&ds_pad_slots[d].self) && ds_pad_lock(d)) {
            ds_pad_slots[d].waiting = 1;
            ds_pad_publish(d, AZ_AVAIL_STALE, 0, 0);
            atomic_store_explicit(&ds_pad_slots[d].writing, 0, memory_order_release);
        }
}
static void ds_pad_selection_end(_Atomic unsigned **count) {
    atomic_fetch_sub_explicit(*count, 1, memory_order_release);
}
static int ds_pad_selector_buttons(uintptr_t self, uintptr_t button, unsigned *side, unsigned *alternate) {
    const uint64_t primary = UINT64_C(0x0d174d4fd081f300), secondary = UINT64_C(0x3030e6ac95bc4d00);
    uint64_t first = ds_pad_word(self + 0x20), second = ds_pad_word(self + 0x28);
    if (first < primary || first > primary + 1 || second != secondary + (first - primary) ||
        ds_pad_word(self + 0x40) == ds_pad_word(self + 0x48) || (button != first && button != second)) return 0;
    *side = (unsigned) (first - primary);
    *alternate = button == second;
    return 1;
}
static int ds_pad_selection(uintptr_t self, uintptr_t button, unsigned on, unsigned *side, unsigned *alternate) {
    uintptr_t expected = az_status_host_build() == DS_BUILD_130 ? 0x2e069b0 : 0x31b9db0;
    if (!(on & 255u) || !self || ds_pad_word(self) != expected) return 0;
    if (!ds_pad_selector_buttons(self, button, side, alternate)) return 0;
    uintptr_t light = ds_pad_word(self + (*alternate ? 0x38 : 0x30));
    return light && ds_pad_byte(light + 0x1d);
}
static uintptr_t ds_pad_select(uintptr_t self, uintptr_t topic, uintptr_t button, unsigned on) {
    uintptr_t (*original)(uintptr_t, uintptr_t, uintptr_t, unsigned) = ds_pad_select_original;
    unsigned side, alternate;
    if (ds_pad_selection(self, button, on, &side, &alternate)) {
        /* The selector publishes the new identity before withdrawal and calls
         * the PadState setter during native dispatch. A periodic seed seeing
         * that identity must wait through both. */
        _Atomic unsigned *count __attribute__((cleanup(ds_pad_selection_end))) = &ds_pad_selecting[side];
        atomic_fetch_add_explicit(count, 1, memory_order_acq_rel);
        ds_pad_layer(side, side + alternate * 2);
        return original(self, topic, button, on);
    }
    return original(self, topic, button, on);
}
void ds_pad_fresh(uintptr_t p) {
    for (unsigned d = 0; d < 4; d++) {
        if (atomic_load(&ds_pad_slots[d].self) != p) continue;
        unsigned selected = atomic_load_explicit(&ds_pad_selected[d % 2], memory_order_acquire);
        if (atomic_load_explicit(&ds_pad_selecting[d % 2], memory_order_acquire)) return;
        /* An unchanged refresh cannot re-admit a deliberately hidden layer.
         * Its dependency/HUI writers still capture changes independently, and
         * a changed ALT takes ds_pad_sample directly. Keep poisoned recovery
         * and a newly selected owner's first fresh sample unconditional. */
        /* A read after an unchanged ALT refresh carries no commit to lose.
         * If a writer owns the slot, keep the pending seed for the next
         * owned refresh. Any existing poison stays set until a read succeeds. */
        if (atomic_load(&ds_pad_slots[d].poisoned) ||
            (atomic_load(&ds_pad_slots[d].availability) == AZ_AVAIL_STALE &&
             (!selected || selected == d + 1)))
            ds_pad_observe(p, 0, -1, 1);
        return;
    }
}
static uintptr_t ds_pad_construct(uintptr_t self, unsigned deck) {
    uintptr_t caller = (uintptr_t) __builtin_return_address(0);
    uintptr_t result = ((uintptr_t (*)(uintptr_t, unsigned)) ds_pad_original[0])(self, deck);
    if (caller == ds_pad_profile->construct_return && deck < 4) ds_pad_sample(self, 1, (int) deck);
    return result;
}
static uintptr_t ds_pad_direct(uintptr_t self, unsigned mode) {
    uintptr_t result = ((uintptr_t (*)(uintptr_t, unsigned)) ds_pad_original[1])(self, mode);
    ds_pad_sample(self, 0, -1);
    return result;
}
static uintptr_t ds_pad_async(uintptr_t closure) {
    uintptr_t self = ds_pad_word(closure + 0x18);
    uintptr_t result = ((uintptr_t (*)(uintptr_t)) ds_pad_original[2])(closure);
    ds_pad_sample(self, 0, -1);
    return result;
}
struct ds_pad_alternate_call {
    struct ds_pad_slot *slot;
    unsigned matches, deck, overlap, alternate;
    uint64_t owner, call;
    uintptr_t switcher, cert;
};
static struct ds_pad_alternate_call ds_pad_alternate_enter(uintptr_t self) {
    struct ds_pad_alternate_call state = {0};
    for (unsigned d = 0; d < 4; d++)
        if (atomic_load_explicit(&ds_pad_slots[d].self, memory_order_acquire) == self) {
            state.slot = &ds_pad_slots[d];
            state.deck = d;
            state.matches++;
        }
    if (state.slot) {
        state.owner = atomic_load(&state.slot->owner);
        state.call = atomic_fetch_add(&state.slot->alternate_calls, 1) + 1;
        state.overlap = atomic_fetch_add(&state.slot->alternate_active, 1);
        if (!state.call || state.overlap == UINT_MAX) atomic_store(&state.slot->alternate_exhausted, 1);
    }
    state.switcher = ds_pad_word(self + 8);
    state.cert = ds_pad_word(self + 0x38);
    state.alternate = ds_pad_byte(self + 0xb0);
    return state;
}
static int ds_pad_alternate_owned(const struct ds_pad_alternate_call *state, uintptr_t self) {
    return state->matches == 1 && state->owner && atomic_load(&state->slot->owner) == state->owner &&
           atomic_load(&state->slot->self) == self;
}
static int ds_pad_alternate_unchanged(const struct ds_pad_alternate_call *state, uintptr_t self, int owned) {
    return owned && state->alternate == ds_pad_byte(self + 0xb0) &&
           state->switcher == ds_pad_word(self + 8) && state->cert == ds_pad_word(self + 0x38);
}
static int ds_pad_alternate_collided(const struct ds_pad_alternate_call *state) {
    return state->slot && (state->overlap || atomic_load(&state->slot->alternate_exhausted) ||
                           atomic_load(&state->slot->alternate_calls) != state->call || atomic_load(&state->slot->alternate_active) != 1);
}
static uintptr_t ds_pad_alternate(uintptr_t self) {
    /* An unchanged periodic refresh does not reserve the writer slot. Keep
     * admission counters across slot reincarnation; overlapping or nested
     * calls can hide A -> B -> A even when the final sampled bytes match. */
    struct ds_pad_alternate_call state = ds_pad_alternate_enter(self);
    uintptr_t result = ((uintptr_t (*)(uintptr_t)) ds_pad_original[3])(self);
    int owned = ds_pad_alternate_owned(&state, self);
    int unchanged = ds_pad_alternate_unchanged(&state, self, owned);
    int collided = ds_pad_alternate_collided(&state);
    /* Serialized SDK captures cannot prove an overlapping native history. */
    if (owned && collided) ds_pad_loss(state.deck);
    if (!unchanged || collided || atomic_load(&state.slot->owner) != state.owner || atomic_load(&state.slot->self) != self)
        ds_pad_sample(self, 0, -1);
    else ds_pad_fresh(self);
    if (state.slot) atomic_fetch_sub(&state.slot->alternate_active, 1);
    return result;
}
static void ds_pad_update_dependency(unsigned deck, uintptr_t owner, uintptr_t key, int cert) {
    struct ds_pad_slot *slot = &ds_pad_slots[deck];
    if ((cert ? slot->cert : slot->switcher) == key) {
        unsigned value = ds_pad_byte(owner + (cert ? 0x152u : 0x130u));
        if (cert) slot->cert_mode = value;
        else slot->pc = value;
        ds_pad_derive(deck, 0);
    }
    atomic_store_explicit(&slot->writing, 0, memory_order_release);
}
static void ds_pad_dependency(uintptr_t owner, int cert) {
    if (!atomic_load(&ds_pad_ready)) return;
    uintptr_t key = owner + (cert ? 0x130u : 0xd8u);
    for (unsigned d = 0; d < 4; d++)
        /* Dependency callbacks visit only their actual unit owners. A read of
         * another unit must not reserve or poison that unit's writer slot.
         * Recheck membership after admission to cover replacement races. */
        if (atomic_load(&ds_pad_slots[d].self) &&
            atomic_load_explicit(cert ? &ds_pad_slots[d].cert : &ds_pad_slots[d].switcher, memory_order_acquire) == key &&
            ds_pad_lock(d)) {
            ds_pad_update_dependency(d, owner, key, cert);
        }
}
static uintptr_t ds_pad_pc_direct(uintptr_t self, uintptr_t source) {
    uintptr_t r = ((uintptr_t (*)(uintptr_t, uintptr_t)) ds_pad_original[4])(self, source);
    ds_pad_dependency(self, 0);
    return r;
}
static uintptr_t ds_pad_pc_async(uintptr_t closure) {
    uintptr_t self = ds_pad_word(closure + 0x20);
    uintptr_t r = ((uintptr_t (*)(uintptr_t)) ds_pad_original[5])(closure);
    ds_pad_dependency(self, 0);
    return r;
}
struct ds_pad_certificate_call {
    uint64_t calls[4];
    unsigned entered, overlap;
};
static struct ds_pad_certificate_call ds_pad_certificate_enter(uintptr_t key) {
    struct ds_pad_certificate_call state = {0};
    for (unsigned d = 0; d < 4; d++) {
        struct ds_pad_slot *slot = &ds_pad_slots[d];
        if (!atomic_load(&slot->self) || atomic_load(&slot->cert) != key) continue;
        state.entered |= 1u << d;
        state.calls[d] = atomic_fetch_add(&slot->cert_calls, 1) + 1;
        unsigned prior = atomic_fetch_add(&slot->cert_active, 1);
        if (prior) state.overlap |= 1u << d;
        if (!state.calls[d] || prior == UINT_MAX) atomic_store(&slot->cert_exhausted, 1);
    }
    return state;
}
static int ds_pad_certificate_collided(const struct ds_pad_certificate_call *state, unsigned deck, uintptr_t key) {
    const struct ds_pad_slot *slot = &ds_pad_slots[deck];
    return ((state->overlap & (1u << deck)) || atomic_load(&slot->cert_exhausted) ||
            state->calls[deck] != atomic_load(&slot->cert_calls) || atomic_load(&slot->cert_active) != 1) &&
           atomic_load(&slot->self) && atomic_load(&slot->cert) == key;
}
static unsigned ds_pad_certificate_uncertain(const struct ds_pad_certificate_call *state, uintptr_t key) {
    unsigned uncertain = 0;
    for (unsigned deck = 0; deck < 4; deck++) {
        if (!(state->entered & (1u << deck))) continue;
        if (ds_pad_certificate_collided(state, deck, key)) {
            uncertain |= 1u << deck;
            ds_pad_loss(deck);
        }
    }
    return uncertain;
}
static void ds_pad_certificate_leave(const struct ds_pad_certificate_call *state) {
    for (unsigned deck = 0; deck < 4; deck++)
        if (state->entered & (1u << deck)) atomic_fetch_sub(&ds_pad_slots[deck].cert_active, 1);
}
static uintptr_t ds_pad_certificate(uintptr_t self, uintptr_t context, int constructing) {
    const int ready = self && atomic_load_explicit(&ds_pad_ready, memory_order_acquire);
    const uintptr_t key = self + 0x130u;
    struct ds_pad_certificate_call state = {0};
    if (ready) state = ds_pad_certificate_enter(key);
    unsigned before = ready && !constructing ? ds_pad_byte(self + 0x152u) : UINT_MAX;
    uintptr_t result = constructing
                           ? ((uintptr_t (*)(uintptr_t, uintptr_t)) ds_pad_original[7])(self, context)
                           : ((uintptr_t (*)(uintptr_t)) ds_pad_original[6])(self);
    if (!ready) return result;
    unsigned after = ds_pad_byte(self + 0x152u);
    unsigned uncertain = ds_pad_certificate_uncertain(&state, key);
    /* Both profiles return before their mode store when the value is equal.
     * An isolated no-op carries no pad commit. Nested or competing calls can
     * hide an intermediate value, even if the sampled byte is restored. */
    if (before != after || uncertain) ds_pad_dependency(self, 1);
    ds_pad_certificate_leave(&state);
    return result;
}
static uintptr_t ds_pad_cert_update(uintptr_t self) {
    return ds_pad_certificate(self, 0, 0);
}
static uintptr_t ds_pad_cert_construct(uintptr_t self, uintptr_t context) {
    return ds_pad_certificate(self, context, 1);
}
static uintptr_t ds_pad_dispose(uintptr_t control) {
    for (unsigned d = 0; d < 4; d++) {
        struct ds_pad_slot *s = &ds_pad_slots[d];
        uintptr_t object = control + 16;
        if (!atomic_compare_exchange_strong_explicit(&s->self, &object, 0, memory_order_acq_rel, memory_order_acquire)) continue;
        if (!ds_pad_lock(d)) continue;
        if (!atomic_load_explicit(&s->self, memory_order_acquire)) {
            s->switcher = s->cert = 0;
            ds_pad_publish(d, AZ_AVAIL_UNSUPPORTED, 0, 1);
        }
        atomic_store_explicit(&s->writing, 0, memory_order_release);
    }
    return ((uintptr_t (*)(uintptr_t)) ds_pad_dispose_original)(control);
}
static void ds_pad_apply(const struct ds_gui_record *r, int gap) {
    struct ds_field f = {AZ_STATUS_SETTINGS, AZ_STATUS_SETTINGS_PAD_MODE, {.info = {r->availability, AZ_PROV_NATIVE, r->revision, r->observed}, .scope = AZ_STATUS_UNIT, .scope_id = r->owner, .provider_id = r->owner, .count = r->availability == AZ_AVAIL_READY}, &r->master, NULL};
    ds_publish(r->deck, 0, DS_UNIT_INPUT, r->observed, &f, 1, NULL, NULL, gap ? AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS) : 0,
               gap ? AZ_STATUS_CAPTURE_GAP : 0);
}
static int ds_pad_selector_valid(uintptr_t selector) {
    const uint32_t signature[] = {0x72001c7f, 0x54000160, 0xa9be7bfd, 0x910003fd};
    return az_status_host_image_contains(selector, sizeof signature) &&
           !memcmp((void *) selector, signature, sizeof signature);
}
static int ds_pad_entries_valid(const struct ds_pad_profile *profile) {
    const uint32_t expected[] = {0xa9af7bfd, 0xa9b47bfd, 0xf9400c01, 0xa9b87bfd, 0xa9b37bfd, 0xa9b87bfd, 0xa9bc7bfd, 0xa9b47bfd};
    for (unsigned i = 0; i < 8; i++)
        if (!az_status_host_image_contains(profile->entry[i], 4) ||
            memcmp((void *) profile->entry[i], &expected[i], 4)) return 0;
    return 1;
}
static int ds_pad_targets_valid(const struct ds_pad_profile *profile, uintptr_t selector) {
    uintptr_t dispose = 0;
    if (!ds_pad_selector_valid(selector)) return 0;
    return az_status_host_image_word(profile->ctrl + 16, &dispose) && dispose == profile->dispose && ds_pad_entries_valid(profile);
}
static int ds_pad_install_entries(const struct ds_pad_profile *profile, uintptr_t selector) {
    void *hooks[] = {ds_pad_construct, ds_pad_direct, ds_pad_async, ds_pad_alternate, ds_pad_pc_direct, ds_pad_pc_async,
                     ds_pad_cert_update, ds_pad_cert_construct};
    for (unsigned i = 0; i < 8; i++)
        if (az_status_host_install_island(profile->entry[i], hooks[i], &ds_pad_original[i], "status-pad") != 1) return 0;
    return az_status_host_install_island(selector, ds_pad_select, &ds_pad_select_original, "status-pad-layer") == 1;
}
void ds_pad_install(void) {
    if (az_status_host_build() != DS_BUILD_130 && az_status_host_build() != DS_BUILD_200) return;
    if (access("/home/root/settings/azmod.deck-status.off", F_OK) == 0) return;
    const struct ds_pad_profile *profile = &ds_pad_profiles[az_status_host_build() == DS_BUILD_130 ? 0 : 1];
    uintptr_t selector = az_status_host_build() == DS_BUILD_130 ? 0x2039d58 : 0x21a7650;
    if (!ds_pad_targets_valid(profile, selector)) return;
    ds_pad_profile = profile;
    if (az_status_host_patch_vtable(profile->ctrl + 16, ds_pad_dispose, &ds_pad_dispose_original) != 1) return;
    if (!ds_pad_install_entries(profile, selector)) return;
    atomic_store_explicit(&ds_pad_ready, 1, memory_order_release);
}

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
            struct ds_deck_stamp stamp;
            ds_read_stamp(deck, &stamp);
            uint64_t epoch = stamp.epoch, last = stamp.last_epoch;
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
void ds_gui_load(unsigned deck) {
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
static void ds_master_copy_seed(const struct ds_master_state *state, void *out) {
    *(struct ds_gui_record *) out = ds_master_seed(state);
}
static void ds_master_arm_bit(void) { atomic_fetch_or_explicit(&ds_gui_armed, 1, memory_order_release); }
static void ds_master_send_seed(const struct ds_master_state *state, void *unused) {
    (void) unused;
    struct ds_gui_record seed = ds_master_seed(state);
    ds_gui_send(&seed, 1, 1);
}
static void ds_gui_reseed(unsigned gap, struct ds_gui_cursor *cursor) {
    if (gap & 1) {
        struct ds_gui_record record;
        az_status_host_master_cut(NULL, ds_master_copy_seed, &record);
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
    unsigned wanted = (topics & AZ_STATUS_TOPIC(AZ_STATUS_SYNC)) && az_status_host_master_supported() ? 1u : 0u;
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
    az_status_host_master_cut(ds_master_arm_bit, ds_master_send_seed, NULL);
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
void ds_gui_interest(uint64_t topics) {
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
