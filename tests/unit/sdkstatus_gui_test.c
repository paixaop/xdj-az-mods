/* SPDX-License-Identifier: GPL-3.0-only */
/* Real GUI receipt worker, commit wrapper, reducer, timer-independent delivery
 * and retirement. The synthetic firmware owns the object through the call. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "sdk_status_engine.c"
#include "../common/azstatus_native.h"
static int ui;
static void ds_wake(void) {
    uint64_t one = 1;
    ssize_t n = write(ui, &one, sizeof one);
    assert(n == (ssize_t) sizeof one || errno == EAGAIN);
}
static void ds_deadline(uint64_t ns) { (void) ns; }
static int ds_live(int owner) { return owner == 1; }
static void ds_native_interest(unsigned d, uint64_t t) {
    (void) d;
    (void) t;
}
static void sdk_status_store_binding(unsigned d, uint64_t e, const az_track_ref *r, uint64_t c) {
    (void) d;
    (void) e;
    (void) r;
    (void) c;
}
static void sdk_status_store_interest(unsigned d, uint64_t t) {
    (void) d;
    (void) t;
}
static unsigned store_reseeds;
static void sdk_status_store_reseed(unsigned deck, uint64_t epoch, uint64_t topics) {
    assert(deck < 4 && epoch && topics == AZ_STATUS_TOPIC(AZ_STATUS_KEY));
    ++store_reseeds;
}

#define az_thread_create pthread_create
static pthread_mutex_t g_master_state_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_master_event;
static struct {
    int deck;
    uint64_t revision, observed_ns;
} g_master_state;
static int master_state_supported(void) { return 1; }
static unsigned long g_bpmstate[4], g_bpmstate_generation[4];
static _Atomic(const struct azstatus_native_v1 *) ds_native_api;
#define BPMSTATE_LIVE 0xa0
#define BPMCTRL_OFF 16
#define BPMCTRL_VT 0
static const int EPB_V130 = 1, EPB_V200 = 2;
static int g_ep147_build;
static uintptr_t g_ep_img_lo, g_ep_img_hi;
static int img_q(uintptr_t a, uintptr_t *b) {
    (void) a;
    (void) b;
    return 0;
}
static int azm_patch_vtable(uintptr_t addr, void *fn, void **previous) {
    (void) addr;
    (void) fn;
    (void) previous;
    return -1;
}
static int azm_install_island_hook_at(uintptr_t target, void *hook, void **orig, const char *name) {
    (void) name;
    return azm_patch_vtable(target, hook, orig);
}
#define azm_install_hook azm_patch_vtable
static uintptr_t key_parent[4];
static uintptr_t perdeck_block(int d) { return key_parent[d]; }
#include "sdk_status_gui.c"

static uint64_t native_load = 1;
static int swap_in_write;
static int load_seed(unsigned d, struct azstatus_native_receipt *r) {
    if (d != 1 || !native_load) return 0;
    r->epoch = r->load_generation = native_load;
    r->unit = 2;
    return 1;
}
static void firmware_bpm(void *self, void *input) {
    if (swap_in_write) native_load++;
    memcpy((unsigned char *) self + BPMSTATE_LIVE, input, sizeof(double));
}
static int disposed;
static void firmware_dispose(void *ctrl) {
    (void) ctrl;
    disposed++;
}
struct log {
    double bpm;
    int master;
    unsigned bpm_ready, stale, count, master_changes;
};
static void receive(const az_deck_status_event_v1 *event, void *u) {
    struct log *log = u;
    log->count++;
    const az_status_tempo_v1 *tempo = event->view->tempo;
    const az_status_sync_v1 *sync = event->view->sync;
    if (tempo->header.changed & az_status_field_bit(AZ_STATUS_TEMPO_CURRENT_BPM)) {
        if (tempo->current_bpm_info.info.availability == AZ_AVAIL_READY) {
            assert(tempo->current_bpm_info.scope == AZ_STATUS_UNIT && tempo->current_bpm_info.scope_id);
            log->bpm = tempo->current_bpm;
            log->bpm_ready++;
        } else log->stale++;
    }
    if ((sync->header.changed & az_status_field_bit(AZ_STATUS_SYNC_MASTER)) &&
        sync->master_info.info.availability == AZ_AVAIL_READY) {
        log->master = sync->master;
        log->master_changes++;
    }
}
static void until(unsigned *count, unsigned wanted) {
    for (int i = 0; i < 32 && *count < wanted; i++) {
        struct pollfd fd = {ui, POLLIN, 0};
        assert(poll(&fd, 1, 1000) > 0);
        uint64_t signal;
        assert(read(ui, &signal, sizeof signal) == sizeof signal);
        ds_drain(8);
    }
    assert(*count >= wanted);
}
struct key_log {
    unsigned changed;
    char text[4];
    unsigned availability;
    unsigned original_changed, original_availability;
    uint64_t original_revision;
    char original[4];
};
static void key_change(const az_deck_status_event_v1 *e, void *user) {
    struct key_log *log = user;
    const az_status_key_v1 *k = e->view->key;
    if (k->header.changed & az_status_field_bit(AZ_STATUS_KEY_ORIGINAL)) {
        ++log->original_changed;
        log->original_availability = k->original_info.info.availability;
        log->original_revision = k->original_info.info.revision;
        if (log->original_availability == AZ_AVAIL_READY) {
            assert(k->original_info.info.provenance == AZ_PROV_NATIVE && k->original_info.scope_id == e->load_epoch);
            snprintf(log->original, sizeof log->original, "%s", k->original);
        }
    }
    if (!(k->header.changed & az_status_field_bit(AZ_STATUS_KEY_EFFECTIVE))) return;
    log->availability = k->effective_info.info.availability;
    if (log->availability == AZ_AVAIL_READY) {
        assert(k->effective_info.info.provenance == AZ_PROV_NATIVE && k->effective_info.scope_id == e->load_epoch);
        snprintf(log->text, sizeof log->text, "%s", k->effective);
    }
    ++log->changed;
}
static int key_swaps_load;
static void firmware_key(void *self, void *input) {
    memcpy((char *) self + 0xa4, input, 4);
    memcpy((char *) self + 0xb0, (char *) input + 4, 4);
    if (key_swaps_load) ++native_load;
}
static void effective_keys(void) {
    uint64_t parent[0x1100 / 8] = {0}, control[32] = {0};
    unsigned char *self = (unsigned char *) control + 16;
    g_ep147_build = EPB_V200;
    control[0] = ds_model_ctrl(DS_MODEL_KEY);
    control[1] = 1;
    key_parent[1] = (uintptr_t) parent;
    parent[0x1090 / 8] = (uintptr_t) self;
    parent[0x1098 / 8] = (uintptr_t) control;
    uint32_t deck = 1;
    memcpy(self + 0xd4, &deck, sizeof deck);
    ds_model_original[DS_MODEL_KEY] = firmware_key;
    ds_model_dispose_original[DS_MODEL_KEY] = firmware_dispose;
    atomic_store(&ds_model_ready[DS_MODEL_KEY], 1);
    const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 2, 1, AZ_STATUS_TOPIC(AZ_STATUS_KEY)};
    struct key_log log = {0};
    az_deck_subscription sub;
    assert(ds_register(1, &watch, key_change, &log, &sub) == AZ_OK);
    ds_drain(8);
    unsigned n = log.changed;
    unsigned char value[] = {0, 0, 100, 1, 0, 0, 100, 1};
    ds_key_writer(self, value);
    until(&log.changed, n + 1);
    assert(!strcmp(log.text, "8A") && !strcmp(log.original, "8A"));
    uint64_t original_revision = log.original_revision;
    unsigned original_changed = log.original_changed;
    value[0] = 1;
    ds_key_writer(self, value);
    until(&log.changed, n + 2);
    assert(!strcmp(log.text, "3A"));
    value[0] = 11;
    ds_key_writer(self, value);
    until(&log.changed, n + 3);
    assert(!strcmp(log.text, "1A"));
    assert(!strcmp(log.original, "8A") && log.original_revision == original_revision &&
           log.original_changed == original_changed);
    ds_key_writer(self, value);
    assert(ds_model_slots[DS_MODEL_KEY][1].latest.revision == 3);
    key_swaps_load = 1;
    value[0] = 4;
    ds_key_writer(self, value);
    assert(ds_model_slots[DS_MODEL_KEY][1].latest.revision == 3);
    --native_load;
    key_swaps_load = 0;
    memset(value, 0, sizeof value);
    ds_key_writer(self, value);
    until(&log.changed, n + 4);
    assert(log.availability == AZ_AVAIL_EMPTY && log.original_availability == AZ_AVAIL_EMPTY);
    ds_key_dispose(control);
    until(&log.changed, n + 5);
    assert(log.availability == AZ_AVAIL_STALE && log.original_availability == AZ_AVAIL_STALE && disposed == 1);
    assert(store_reseeds >= 2);
    disposed = 0;
    key_parent[1] = 0;
    assert(ds_release(1, sub) == AZ_OK);
}
struct slip_log {
    unsigned changes;
    int enabled;
    uint64_t unit;
};
static void slip_change(const az_deck_status_event_v1 *e, void *user) {
    struct slip_log *log = user;
    const az_status_settings_v1 *s = e->view->settings;
    if (!(s->header.changed & az_status_field_bit(AZ_STATUS_SETTINGS_SLIP))) return;
    if (s->slip_info.info.availability == AZ_AVAIL_READY) {
        assert(s->slip_info.scope == AZ_STATUS_UNIT && s->slip_info.scope_id);
        if (log->unit) assert(s->slip_info.scope_id == log->unit);
        log->unit = s->slip_info.scope_id;
        log->enabled = s->slip;
    } else log->enabled = -1;
    ++log->changes;
}
static void firmware_slip(void *self, void *input) {
    memcpy((char *) self + 0xa8, input, 1);
}
static void slip_mode(void) {
    uint64_t parent[0x1150 / 8] = {0}, control[0x160 / 8] = {0};
    unsigned char *self = (unsigned char *) control + 16;
    key_parent[1] = (uintptr_t) parent;
    parent[0x1120 / 8] = (uintptr_t) self;
    parent[0x1128 / 8] = (uintptr_t) control;
    control[0] = ds_model_ctrl(DS_MODEL_SLIP);
    control[1] = 1;
    uint32_t d = 1;
    memcpy(self + 0x134, &d, 4);
    ds_model_original[DS_MODEL_SLIP] = firmware_slip;
    ds_model_dispose_original[DS_MODEL_SLIP] = firmware_dispose;
    atomic_store(&ds_model_ready[DS_MODEL_SLIP], 1);
    const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 2, 1, AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS)};
    struct slip_log log = {0};
    az_deck_subscription sub;
    assert(ds_register(1, &watch, slip_change, &log, &sub) == AZ_OK);
    ds_drain(8);
    unsigned n = log.changes;
    unsigned char enabled = 0;
    ds_slip_writer(self, &enabled);
    until(&log.changes, n + 1);
    assert(!log.enabled);
    enabled = 1;
    ds_slip_writer(self, &enabled);
    until(&log.changes, n + 2);
    assert(log.enabled == 1);
    self[0xa9] = 1;
    ds_slip_writer(self, &enabled);
    assert(ds_model_slots[DS_MODEL_SLIP][1].latest.revision == 2);
    enabled = 0;
    ds_slip_writer(self, &enabled);
    until(&log.changes, n + 3);
    assert(!log.enabled);
    ds_slip_dispose(control);
    until(&log.changes, n + 4);
    assert(log.enabled == -1);
    key_parent[1] = 0;
    disposed = 0;
    assert(ds_release(1, sub) == AZ_OK);
}
struct slider_log {
    unsigned changes, availability;
    double percent;
    uint64_t owner;
};
static void slider_change(const az_deck_status_event_v1 *e, void *user) {
    struct slider_log *log = user;
    const az_status_tempo_v1 *t = e->view->tempo;
    if (!(t->header.changed & az_status_field_bit(AZ_STATUS_TEMPO_SLIDER_PERCENT))) return;
    log->availability = t->slider_percent_info.info.availability;
    if (log->availability == AZ_AVAIL_READY) {
        assert(t->slider_percent_info.scope == AZ_STATUS_UNIT && t->slider_percent_info.provider_id);
        if (log->owner) assert(t->slider_percent_info.provider_id == log->owner);
        log->owner = t->slider_percent_info.provider_id;
        log->percent = t->slider_percent;
    }
    ++log->changes;
}
static void firmware_slider(void *self, void *input) { memcpy((char *) self + 0xa4, input, sizeof(float)); }
static void slider_percent(void) {
    uint64_t parent[0x1100 / 8] = {0}, control[32] = {0};
    unsigned char *self = (unsigned char *) control + 16;
    key_parent[0] = (uintptr_t) parent;
    parent[0x10e0 / 8] = (uintptr_t) self;
    parent[0x10e8 / 8] = (uintptr_t) control;
    control[0] = ds_model_ctrl(DS_MODEL_SLIDER);
    control[1] = 1;
    ds_model_original[DS_MODEL_SLIDER] = firmware_slider;
    ds_model_dispose_original[DS_MODEL_SLIDER] = firmware_dispose;
    atomic_store(&ds_model_ready[DS_MODEL_SLIDER], 1);
    const az_deck_watch_v1 w = {sizeof w, AZ_DATA_VERSION, 1, 1, AZ_STATUS_TOPIC(AZ_STATUS_TEMPO)};
    struct slider_log log = {0};
    az_deck_subscription sub;
    assert(ds_register(1, &w, slider_change, &log, &sub) == AZ_OK);
    ds_drain(8);
    unsigned n = log.changes;
    float ratio = -0.0295f;
    ds_slider_writer(self, &ratio);
    until(&log.changes, n + 1);
    assert(log.availability == AZ_AVAIL_READY && fabs(log.percent + 2.95) < 0.00001);
    ratio = 0.0f;
    ds_slider_writer(self, &ratio);
    until(&log.changes, n + 2);
    assert(log.availability == AZ_AVAIL_READY && log.percent == 0);
    ratio = INFINITY;
    ds_slider_writer(self, &ratio);
    until(&log.changes, n + 3);
    assert(log.availability == AZ_AVAIL_STALE);
    ratio = 0.08f;
    ds_slider_writer(self, &ratio);
    until(&log.changes, n + 4);
    assert(fabs(log.percent - 8) < 0.00001);
    ds_slider_dispose(control);
    until(&log.changes, n + 5);
    assert(log.availability == AZ_AVAIL_STALE);
    key_parent[0] = 0;
    disposed = 0;
    assert(ds_release(1, sub) == AZ_OK);
}
static uintptr_t fake_pad_direct(uintptr_t self, unsigned mode) {
    memcpy((void *) (self + 0xac), &mode, 4);
    return 17;
}
static uintptr_t fake_pad_async(uintptr_t closure) {
    uintptr_t self = ds_pad_word(closure + 0x18);
    unsigned mode = ds_pad_u32(closure + 0x20);
    memcpy((void *) (self + 0xac), &mode, 4);
    return 18;
}
static uintptr_t fake_pad_dispose(uintptr_t ctrl) {
    (void) ctrl;
    return 19;
}
static void pad_modes(void) {
    struct ds_pad_profile form = {.ctrl = 0x11100, .switcher = 0x11200, .cert = 0x11300};
    _Alignas(16) unsigned char owners[2][0x140] = {{0}}, sw[0x80] = {0}, cert[0x40] = {0}, closure[0x28] = {0};
    unsigned char *p = owners[0] + 16, *other = owners[1] + 16;
    uintptr_t swptr = (uintptr_t) sw, certptr = (uintptr_t) cert;
    memcpy(sw, &form.switcher, 8);
    memcpy(cert, &form.cert, 8);
    ds_pad_profile = &form;
    atomic_store(&ds_pad_ready, 1);
    ds_pad_original[1] = fake_pad_direct;
    ds_pad_original[2] = fake_pad_async;
    ds_pad_dispose_original = fake_pad_dispose;
    for (unsigned i = 0; i < 2; i++) {
        unsigned char *self = owners[i] + 16;
        memcpy(self - 16, &form.ctrl, 8);
        memcpy(self + 8, &swptr, 8);
        memcpy(self + 0x38, &certptr, 8);
        ds_pad_sample((uintptr_t) self, 1, (int) (2 * i));
    }
    assert(ds_pad_seed(0).master == 0 && ds_pad_seed(0).availability == AZ_AVAIL_READY);
    assert(ds_pad_direct((uintptr_t) p, 2) == 17 && ds_pad_seed(0).master == 1);
    assert(ds_pad_direct((uintptr_t) p, 6) == 17 && ds_pad_seed(0).master == 2);
    uintptr_t self = (uintptr_t) p;
    unsigned mode = 4;
    memcpy(closure + 0x18, &self, 8);
    memcpy(closure + 0x20, &mode, 4);
    assert(ds_pad_async((uintptr_t) closure) == 18 && ds_pad_seed(0).master == 3);
    ds_pad_layer(0, 2);
    assert(ds_pad_seed(0).availability == AZ_AVAIL_STALE && ds_pad_seed(2).availability == AZ_AVAIL_STALE);
    ds_pad_fresh((uintptr_t) other);
    assert(ds_pad_seed(2).availability == AZ_AVAIL_READY && ds_pad_seed(0).availability == AZ_AVAIL_STALE);
    ds_pad_fresh((uintptr_t) p);
    assert(ds_pad_seed(0).availability == AZ_AVAIL_STALE);
    ds_pad_layer(0, 0);
    sw[0x58] = 1;
    cert[0x22] = 1;
    p[0xb0] = 2;
    ds_pad_sample((uintptr_t) p, 0, -1);
    assert(ds_pad_seed(0).availability == AZ_AVAIL_UNSUPPORTED);
    p[0xb0] = 1;
    ds_pad_sample((uintptr_t) p, 0, -1);
    assert(ds_pad_seed(0).availability == AZ_AVAIL_READY && ds_pad_seed(0).master == 1);
    assert(ds_pad_dispose((uintptr_t) owners[0]) == 19 && ds_pad_seed(0).availability == AZ_AVAIL_UNSUPPORTED);
    assert(ds_pad_dispose((uintptr_t) owners[1]) == 19);
    atomic_store(&ds_pad_ready, 0);
    ds_pad_profile = NULL;
}
static int pad_dependency_collision(void) {
    struct ds_pad_profile form = {.ctrl = 0x11100, .switcher = 0x11200, .cert = 0x11300};
    _Alignas(16) unsigned char owners[2][0x140] = {{0}}, switches[2][0x180] = {{0}}, certs[2][0x180] = {{0}};
    ds_pad_profile = &form;
    atomic_store(&ds_pad_ready, 1);
    for (unsigned i = 0; i < 2; ++i) {
        unsigned char *self = owners[i] + 16;
        uintptr_t sw = (uintptr_t) switches[i] + 0xd8, cert = (uintptr_t) certs[i] + 0x130;
        memcpy((void *) sw, &form.switcher, 8);
        memcpy((void *) cert, &form.cert, 8);
        certs[i][0x152] = 1;
        memcpy(self - 16, &form.ctrl, 8);
        memcpy(self + 8, &sw, 8);
        memcpy(self + 0x38, &cert, 8);
        self[0xb0] = 1;
        ds_pad_sample((uintptr_t) self, 1, (int) (2 * i));
    }
    atomic_store(&ds_gui_armed, ds_pad_bit(0));
    atomic_store(&ds_gui_gap, 0);
    assert(ds_pad_lock(0));
    /* The lock-free seed does not poison an in-flight writer. Nor may a
     * different unit's dependency callback reserve this unit's writer slot. */
    assert(ds_pad_seed(0).availability == AZ_AVAIL_STALE && !atomic_load(&ds_pad_slots[0].poisoned));
    switches[1][0x130] = 1;
    ds_pad_dependency((uintptr_t) switches[1], 0);
    assert(!atomic_load(&ds_pad_slots[0].poisoned) && !atomic_load(&ds_gui_gap));
    assert(ds_pad_seed(2).availability == AZ_AVAIL_READY && ds_pad_seed(2).master == 1);
    certs[1][0x152] = 3;
    ds_pad_dependency((uintptr_t) certs[1], 1);
    assert(!atomic_load(&ds_pad_slots[0].poisoned) && !atomic_load(&ds_gui_gap));
    assert(ds_pad_seed(2).master == 0);
    /* Genuine same-unit writer collision retains the explicit capture gap. */
    ds_pad_dependency((uintptr_t) switches[0], 0);
    assert(atomic_load(&ds_pad_slots[0].poisoned) && (atomic_load(&ds_gui_gap) & ds_pad_bit(0)));
    atomic_store(&ds_pad_slots[0].writing, 0);
    atomic_store(&ds_gui_armed, 0);
    ds_pad_sample((uintptr_t) owners[0] + 16, 0, -1);
    assert(!atomic_load(&ds_pad_slots[0].poisoned));
    puts("PASS pad seed/unrelated dependency collision stays quiet; actual matching writer collision loses explicitly");
    return 0;
}
static unsigned pad_cert_value = UINT_MAX, pad_cert_nested;
static uintptr_t fake_pad_certificate(uintptr_t owner) {
    if (pad_cert_nested) {
        pad_cert_nested = 0;
        unsigned prior = ds_pad_byte(owner + 0x152);
        pad_cert_value = 1;
        ds_pad_cert_update(owner);
        pad_cert_value = prior;
    }
    if (pad_cert_value != UINT_MAX) *(unsigned char *) (owner + 0x152) = pad_cert_value;
    return 21;
}
static int pad_certificate_collision(void) {
    struct ds_pad_profile form = {.ctrl = 0x11100, .switcher = 0x11200, .cert = 0x11300};
    _Alignas(16) unsigned char owner[0x140] = {0}, sw[0x80] = {0}, cert[0x180] = {0};
    uintptr_t self = (uintptr_t) owner + 16, swptr = (uintptr_t) sw, certptr = (uintptr_t) cert + 0x130;
    memcpy(owner, &form.ctrl, 8);
    memcpy(sw, &form.switcher, 8);
    memcpy((void *) certptr, &form.cert, 8);
    memcpy((void *) (self + 8), &swptr, 8);
    memcpy((void *) (self + 0x38), &certptr, 8);
    sw[0x58] = 1; cert[0x152] = 1; *(unsigned char *) (self + 0xb0) = 1;
    ds_pad_profile = &form;
    ds_pad_original[6] = fake_pad_certificate;
    atomic_store(&ds_pad_ready, 1);
    ds_pad_sample(self, 1, 0);
    assert(ds_pad_seed(0).availability == AZ_AVAIL_READY && ds_pad_seed(0).master == 1);
    ds_pad_layer(0, 2);
    assert(ds_pad_seed(0).availability == AZ_AVAIL_STALE);
    atomic_store(&ds_gui_armed, ds_pad_bit(0));
    assert(ds_pad_lock(0));
    assert(ds_pad_cert_update((uintptr_t) cert) == 21);
    assert(!atomic_load(&ds_pad_slots[0].poisoned) && !atomic_load(&ds_gui_gap));
    pad_cert_value = 3;
    assert(ds_pad_cert_update((uintptr_t) cert) == 21);
    assert(atomic_load(&ds_pad_slots[0].poisoned) && (atomic_load(&ds_gui_gap) & ds_pad_bit(0)));
    atomic_store(&ds_pad_slots[0].writing, 0);
    atomic_store(&ds_gui_armed, 0);
    atomic_store(&ds_gui_gap, 0);
    ds_pad_layer(0, 0);
    ds_pad_sample(self, 0, -1);
    assert(!atomic_load(&ds_pad_slots[0].poisoned));
    assert(ds_pad_seed(0).availability == AZ_AVAIL_READY && ds_pad_seed(0).master == 0);
    atomic_store(&ds_gui_armed, ds_pad_bit(0));
    pad_cert_nested = 1;
    assert(ds_pad_cert_update((uintptr_t) cert) == 21);
    assert(cert[0x152] == 3);
    assert(atomic_load(&ds_pad_slots[0].poisoned) && (atomic_load(&ds_gui_gap) & ds_pad_bit(0)));
    puts("PASS isolated certificate no-op stays quiet; changed and overlapping commits retain loss");
    return 0;
}
static unsigned pad_refresh_mode = UINT_MAX;
static int pad_refresh_nested;
static uintptr_t pad_refresh_switcher;
static pthread_mutex_t pad_refresh_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pad_refresh_cv = PTHREAD_COND_INITIALIZER;
static unsigned pad_refresh_phase;
static _Thread_local unsigned pad_refresh_role;
static uintptr_t fake_pad_refresh(uintptr_t self) {
    if (pad_refresh_role) {
        pthread_mutex_lock(&pad_refresh_mu);
        if (pad_refresh_role == 1) {
            *(unsigned char *) (self + 0xb0) = 3;
            pad_refresh_phase = 1;
            pthread_cond_broadcast(&pad_refresh_cv);
            while (pad_refresh_phase < 2) pthread_cond_wait(&pad_refresh_cv, &pad_refresh_mu);
        } else {
            *(unsigned char *) (self + 0xb0) = 1;
            pad_refresh_phase = 2;
            pthread_cond_broadcast(&pad_refresh_cv);
            while (pad_refresh_phase < 3) pthread_cond_wait(&pad_refresh_cv, &pad_refresh_mu);
        }
        pthread_mutex_unlock(&pad_refresh_mu);
        return 20;
    }
    if (pad_refresh_nested) {
        unsigned outer = pad_refresh_mode;
        pad_refresh_nested = 0;
        pad_refresh_mode = 3;
        assert(ds_pad_alternate(self) == 20);
        pad_refresh_mode = outer;
    }
    if (pad_refresh_mode != UINT_MAX) *(unsigned char *) (self + 0xb0) = pad_refresh_mode;
    if (pad_refresh_switcher) memcpy((void *) (self + 8), &pad_refresh_switcher, sizeof pad_refresh_switcher);
    return 20;
}
struct pad_refresh_call { uintptr_t self; unsigned role; };
static void *pad_refresh_thread(void *arg) {
    const struct pad_refresh_call *call = arg;
    pad_refresh_role = call->role;
    assert(ds_pad_alternate(call->self) == 20);
    if (call->role == 1) {
        pthread_mutex_lock(&pad_refresh_mu);
        pad_refresh_phase = 3;
        pthread_cond_broadcast(&pad_refresh_cv);
        pthread_mutex_unlock(&pad_refresh_mu);
    }
    return NULL;
}
enum { PAD_REFRESH, PAD_INACTIVE, PAD_RESELECT };
static uintptr_t pad_select_object;
static unsigned pad_select_callback_writes;
static uintptr_t fake_pad_select(uintptr_t selector, uintptr_t topic, uintptr_t button, unsigned on) {
    (void) selector;
    (void) topic;
    (void) button;
    assert(on == 1);
    assert(ds_pad_seed(1).availability == AZ_AVAIL_STALE);
    assert(ds_pad_alternate(pad_select_object) == 20);
    assert(ds_pad_seed(1).availability == AZ_AVAIL_STALE);
    if (pad_select_callback_writes) {
        assert(ds_pad_direct(pad_select_object, 2) == 17);
        assert(ds_pad_seed(1).availability == AZ_AVAIL_READY && ds_pad_seed(1).master == 1);
    }
    return 22;
}
static int pad_select_collision(void) {
    struct ds_pad_profile form = {.ctrl = 0x11100, .switcher = 0x11200};
    _Alignas(16) unsigned char owner[0x140] = {0}, sw[0x80] = {0}, light[0x20] = {0};
    const uintptr_t primary = UINT64_C(0x0d174d4fd081f301);
    uintptr_t swptr = (uintptr_t) sw;
    uintptr_t selector[10] = {0x2e069b0, 0, 0, 0, primary, UINT64_C(0x3030e6ac95bc4d01),
                              (uintptr_t) light, 0, 0, 1};
    light[0x1d] = 1;
    memcpy(owner, &form.ctrl, 8);
    memcpy(sw, &form.switcher, 8);
    pad_select_object = (uintptr_t) owner + 16;
    memcpy((void *) (pad_select_object + 8), &swptr, 8);
    ds_pad_profile = &form;
    g_ep147_build = EPB_V130;
    ds_pad_original[1] = fake_pad_direct;
    ds_pad_original[3] = fake_pad_refresh;
    ds_pad_select_original = fake_pad_select;
    assert(!pipe2(ds_gui_pipe, O_NONBLOCK));
    ds_gui_wake = ui;
    atomic_store(&ds_pad_ready, 1);
    ds_pad_sample(pad_select_object, 1, 1);
    for (unsigned native_write = 0; native_write < 2; ++native_write) {
        ds_pad_layer(1, 3);
        assert(ds_pad_seed(1).availability == AZ_AVAIL_STALE);
        uint64_t before = ds_pad_seed(1).revision;
        atomic_store(&ds_gui_armed, ds_pad_bit(1));
        pad_select_callback_writes = native_write;
        assert(ds_pad_select((uintptr_t) selector, 0, primary, 1) == 22);
        if (!native_write) assert(ds_pad_seed(1).availability == AZ_AVAIL_STALE);
        assert(ds_pad_alternate(pad_select_object) == 20);
        assert(ds_pad_seed(1).availability == AZ_AVAIL_READY && ds_pad_seed(1).master == (int) native_write);
        struct ds_gui_record r;
        assert(read(ds_gui_pipe[0], &r, sizeof r) == sizeof r);
        assert(r.kind == DS_GUI_PAD && r.deck == 1 && r.revision == before + 1);
        assert(r.availability == AZ_AVAIL_READY && r.master == (int) native_write);
        assert(read(ds_gui_pipe[0], &r, sizeof r) == -1 && errno == EAGAIN);
        assert(!atomic_load(&ds_gui_gap) && !atomic_load(&ds_pad_slots[1].poisoned));
        atomic_store(&ds_gui_armed, 0);
    }
    close(ds_gui_pipe[0]);
    close(ds_gui_pipe[1]);
    puts("PASS periodic pad seed waits through native layer dispatch; direct writes and later seeds publish once");
    return 0;
}
static int pad_refresh_collision(int scenario) {
    struct ds_pad_profile form = {.ctrl = 0x11100, .switcher = 0x11200, .cert = 0x11300};
    _Alignas(16) unsigned char owner[0x140] = {0}, sw[2][0x80] = {{0}}, cert[0x40] = {0};
    uintptr_t self = (uintptr_t) owner + 16, swptr = (uintptr_t) sw[0], certptr = (uintptr_t) cert;
    memcpy(owner, &form.ctrl, 8);
    memcpy(sw[0], &form.switcher, 8); memcpy(sw[1], &form.switcher, 8);
    memcpy(cert, &form.cert, 8);
    memcpy((void *) (self + 8), &swptr, 8); memcpy((void *) (self + 0x38), &certptr, 8);
    sw[0][0x58] = 1; cert[0x22] = 1; *(unsigned char *) (self + 0xb0) = 1;
    ds_pad_profile = &form;
    ds_pad_original[3] = fake_pad_refresh;
    atomic_store(&ds_pad_ready, 1);
    ds_pad_sample(self, 1, 0);
    assert(ds_pad_seed(0).availability == AZ_AVAIL_READY && ds_pad_seed(0).master == 1);
    uint64_t revision = atomic_load(&ds_pad_slots[0].revision);
    atomic_store(&ds_gui_armed, ds_pad_bit(0));
    atomic_store(&ds_gui_gap, 0);
    assert(ds_pad_lock(0));
    assert(ds_pad_alternate(self) == 20);
    assert(!atomic_load(&ds_pad_slots[0].poisoned) && !atomic_load(&ds_gui_gap));
    assert(atomic_load(&ds_pad_slots[0].revision) == revision);
    if (scenario != PAD_REFRESH) {
        /* A nonselected logical deck remains intentionally unavailable. A
         * periodic unchanged ALT refresh is not a source recovery or a write. */
        atomic_store(&ds_pad_slots[0].writing, 0);
        atomic_store(&ds_gui_armed, 0);
        ds_pad_layer(0, 2);
        if (scenario == PAD_RESELECT) ds_pad_layer(0, 0);
        assert(ds_pad_seed(0).availability == AZ_AVAIL_STALE);
        revision = atomic_load(&ds_pad_slots[0].revision);
        atomic_store(&ds_gui_armed, ds_pad_bit(0));
        atomic_store(&ds_gui_gap, 0);
        assert(ds_pad_lock(0));
        assert(ds_pad_alternate(self) == 20);
        assert(!atomic_load(&ds_pad_slots[0].poisoned) && !atomic_load(&ds_gui_gap));
        assert(atomic_load(&ds_pad_slots[0].revision) == revision);
        if (scenario == PAD_RESELECT) {
            atomic_store(&ds_pad_slots[0].writing, 0);
            atomic_store(&ds_gui_armed, 0);
            assert(ds_pad_alternate(self) == 20);
            assert(ds_pad_seed(0).availability == AZ_AVAIL_READY);
            assert(atomic_load(&ds_pad_slots[0].revision) == revision + 1);
            /* After a genuine lost commit, a failed reseed leaves the
             * original loss in place and the next fresh observation recovers. */
            assert(ds_pad_lock(0));
            atomic_store(&ds_gui_armed, ds_pad_bit(0));
            pad_refresh_mode = 3;
            assert(ds_pad_alternate(self) == 20);
            assert(atomic_load(&ds_pad_slots[0].poisoned) && (atomic_load(&ds_gui_gap) & ds_pad_bit(0)));
            pad_refresh_mode = UINT_MAX;
            atomic_store(&ds_gui_gap, 0); /* reducer consumed the original gap */
            assert(ds_pad_alternate(self) == 20);
            assert(atomic_load(&ds_pad_slots[0].poisoned) && !atomic_load(&ds_gui_gap));
            atomic_store(&ds_pad_slots[0].writing, 0);
            atomic_store(&ds_gui_armed, 0);
            assert(ds_pad_alternate(self) == 20);
            assert(!atomic_load(&ds_pad_slots[0].poisoned));
            assert(ds_pad_seed(0).availability == AZ_AVAIL_READY && ds_pad_seed(0).master == 3);
            puts("PASS selected pad seed retries quietly and preserves actual changed input loss");
            return 0;
        }
        /* Changed native ALT remains a captured input even on the hidden layer. */
        pad_refresh_mode = 3;
        assert(ds_pad_alternate(self) == 20);
        assert(atomic_load(&ds_pad_slots[0].poisoned) && (atomic_load(&ds_gui_gap) & ds_pad_bit(0)));
        atomic_store(&ds_pad_slots[0].writing, 0);
        atomic_store(&ds_gui_armed, 0);
        atomic_store(&ds_gui_gap, 0);
        pad_refresh_mode = UINT_MAX;
        assert(ds_pad_alternate(self) == 20);
        assert(!atomic_load(&ds_pad_slots[0].poisoned) && ds_pad_seed(0).availability == AZ_AVAIL_STALE);
        ds_pad_layer(0, 0);
        assert(ds_pad_alternate(self) == 20);
        assert(ds_pad_seed(0).availability == AZ_AVAIL_READY && ds_pad_seed(0).master == 3);
        puts("PASS inactive-layer unchanged refresh stays quiet; real changed input reports loss, poison recovers and selection re-admits current mode");
        return 0;
    }
    /* A changed alternate mode still collides and reports loss. */
    pad_refresh_mode = 3;
    assert(ds_pad_alternate(self) == 20);
    assert(atomic_load(&ds_pad_slots[0].poisoned) && (atomic_load(&ds_gui_gap) & ds_pad_bit(0)));
    atomic_store(&ds_pad_slots[0].writing, 0);
    atomic_store(&ds_gui_armed, 0); atomic_store(&ds_gui_gap, 0);
    pad_refresh_mode = UINT_MAX;
    assert(ds_pad_alternate(self) == 20); /* No-op must still recover poison. */
    assert(!atomic_load(&ds_pad_slots[0].poisoned) && ds_pad_seed(0).master == 3);
    /* Lazy dependency binding is a real input change even at equal ALT. */
    assert(ds_pad_lock(0));
    atomic_store(&ds_gui_armed, ds_pad_bit(0));
    pad_refresh_switcher = (uintptr_t) sw[1];
    assert(ds_pad_alternate(self) == 20);
    assert(atomic_load(&ds_pad_slots[0].poisoned) && (atomic_load(&ds_gui_gap) & ds_pad_bit(0)));
    atomic_store(&ds_pad_slots[0].writing, 0);
    atomic_store(&ds_gui_armed, 0); atomic_store(&ds_gui_gap, 0);
    pad_refresh_switcher = 0;
    assert(ds_pad_alternate(self) == 20);
    assert(!atomic_load(&ds_pad_slots[0].poisoned) && ds_pad_seed(0).master == 0);
    atomic_store(&ds_pad_slots[0].availability, AZ_AVAIL_STALE);
    assert(ds_pad_alternate(self) == 20 && ds_pad_seed(0).availability == AZ_AVAIL_READY);
    /* Nested provider callbacks must retain both B and restored A, even though
     * the outer function's native before/after bytes are equal. */
    memcpy((void *) (self + 8), &swptr, 8);
    *(unsigned char *) (self + 0xb0) = 1;
    ds_pad_sample(self, 0, -1);
    revision = atomic_load(&ds_pad_slots[0].revision);
    pad_refresh_mode = 1;
    pad_refresh_nested = 1;
    assert(ds_pad_alternate(self) == 20);
    assert(ds_pad_seed(0).master == 1 && atomic_load(&ds_pad_slots[0].revision) == revision + 2);
    /* Both originals complete their writes before either SDK sample. Samples
     * then serialize, so writer-gate contention cannot reveal the lost B. */
    atomic_store(&ds_gui_armed, ds_pad_bit(0));
    atomic_store(&ds_gui_gap, 0);
    struct pad_refresh_call calls[] = {{self, 1}, {self, 2}};
    pthread_t threads[2];
    assert(!pthread_create(&threads[0], NULL, pad_refresh_thread, &calls[0]));
    pthread_mutex_lock(&pad_refresh_mu);
    while (pad_refresh_phase < 1) pthread_cond_wait(&pad_refresh_cv, &pad_refresh_mu);
    pthread_mutex_unlock(&pad_refresh_mu);
    assert(!pthread_create(&threads[1], NULL, pad_refresh_thread, &calls[1]));
    assert(!pthread_join(threads[0], NULL) && !pthread_join(threads[1], NULL));
    assert((atomic_load(&ds_gui_gap) & ds_pad_bit(0)) && ds_pad_seed(0).master == 1);
    atomic_store(&ds_gui_armed, 0);
    puts("PASS no-op pad refresh avoids writer contention; real changes, stale recovery, nested restoration and parallel history loss remain observable");
    return 0;
}
struct key_race_log {
    unsigned ready, markers, gaps;
    double marker;
    char keys[40][4];
    uint64_t epochs[40];
};
static void key_race_change(const az_deck_status_event_v1 *e, void *user) {
    struct key_race_log *log = user;
    if (e->lost_topics & AZ_STATUS_TOPIC(AZ_STATUS_KEY)) ++log->gaps;
    const az_status_key_v1 *key = e->view->key;
    if ((key->header.changed & az_status_field_bit(AZ_STATUS_KEY_EFFECTIVE)) &&
        key->effective_info.info.availability == AZ_AVAIL_READY) {
        assert(log->ready < 40 && key->effective_info.scope_id == e->load_epoch);
        snprintf(log->keys[log->ready], 4, "%s", key->effective);
        log->epochs[log->ready++] = e->load_epoch;
    }
    const az_status_tempo_v1 *tempo = e->view->tempo;
    if ((tempo->header.changed & az_status_field_bit(AZ_STATUS_TEMPO_SLIDER_PERCENT)) &&
        tempo->slider_percent_info.info.availability == AZ_AVAIL_READY &&
        (!log->markers || log->marker != tempo->slider_percent)) {
        log->marker = tempo->slider_percent;
        ++log->markers;
    }
}
static void adopt_key_load(uint64_t epoch) {
    assert(ds_publish(1, epoch, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
#ifdef SDK_GUI_BASELINE
    ds_gui_signal();
#else
    ds_gui_load(1);
#endif
}
static int key_load_race(void) {
    uint64_t parent[0x1100 / 8] = {0}, control[32] = {0}, slider_control[32] = {0};
    unsigned char *self = (unsigned char *) control + 16, *slider = (unsigned char *) slider_control + 16;
    g_ep147_build = EPB_V200;
    key_parent[1] = (uintptr_t) parent;
    uint32_t deck = 1;
    control[0] = ds_model_ctrl(DS_MODEL_KEY); control[1] = 1;
    slider_control[0] = ds_model_ctrl(DS_MODEL_SLIDER); slider_control[1] = 1;
    parent[0x1090 / 8] = (uintptr_t) self; parent[0x1098 / 8] = (uintptr_t) control;
    parent[0x10e0 / 8] = (uintptr_t) slider; parent[0x10e8 / 8] = (uintptr_t) slider_control;
    memcpy(self + 0xd4, &deck, sizeof deck);
    memcpy(slider + 0xb0, &deck, sizeof deck);
    ds_model_original[DS_MODEL_KEY] = firmware_key;
    ds_model_original[DS_MODEL_SLIDER] = firmware_slider;
    ds_model_dispose_original[DS_MODEL_KEY] = firmware_dispose;
    ds_model_dispose_original[DS_MODEL_SLIDER] = firmware_dispose;
    atomic_store(&ds_model_ready[DS_MODEL_KEY], 1);
    atomic_store(&ds_model_ready[DS_MODEL_SLIDER], 1);
    assert(ds_publish(1, 1, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, 2, 30,
                                   AZ_STATUS_TOPIC(AZ_STATUS_KEY) | AZ_STATUS_TOPIC(AZ_STATUS_TEMPO)};
    struct key_race_log log = {0};
    az_deck_subscription sub;
    assert(ds_register(1, &watch, key_race_change, &log, &sub) == AZ_OK);
    ds_drain(8);
    native_load = 2;
    unsigned char value[] = {0, 0, 100, 1, 0, 0, 100, 1};
    ds_key_writer(self, value);
    value[0] = 1;
    ds_key_writer(self, value);
    /* The unit-scoped marker follows both key records through the same pipe,
     * proving the worker handled them before the delayed LOAD boundary. */
    float ratio = .41f;
    ds_slider_writer(slider, &ratio);
    until(&log.markers, 1);
    assert(!log.ready && !log.gaps);
    adopt_key_load(2);
    until(&log.ready, 2);
    assert(!strcmp(log.keys[0], "8A") && !strcmp(log.keys[1], "3A") &&
           log.epochs[0] == 2 && log.epochs[1] == 2 && !log.gaps);
    /* A skipped native load cannot leak its parked values into a newer load. */
    native_load = 3; value[0] = 2;
    ds_key_writer(self, value);
    ratio = .42f; ds_slider_writer(slider, &ratio);
    until(&log.markers, 2);
    native_load = 4;
    adopt_key_load(4);
    value[0] = 3;
    ds_key_writer(self, value); /* adoption/wake precedes enqueue */
    until(&log.ready, 3);
    assert(!strcmp(log.keys[2], "5A") && log.epochs[2] == 4 && !log.gaps);
    /* Overflow while awaiting adoption must remain explicit, even when the
     * final value returns to the first value. Markers prevent pipe overflow. */
    native_load = 5;
    for (unsigned i = 0; i < 33; ++i) {
        value[0] = i % 12;
        ds_key_writer(self, value);
        ratio += .001f;
        ds_slider_writer(slider, &ratio);
        until(&log.markers, 3 + i);
    }
    assert(log.ready == 3);
    adopt_key_load(5);
    until(&log.gaps, 1);
    assert(log.ready == 4 && log.epochs[3] == 5);
    ds_key_dispose(control);
    ds_slider_dispose(slider_control);
    assert(ds_release(1, sub) == AZ_OK);
    puts("PASS key receipts before LOAD, ordered replay, wake-before-park, obsolete-load rejection and bounded overflow");
    return 0;
}
int main(int argc, char **argv) {
    ui = eventfd(0, EFD_NONBLOCK);
    assert(ui >= 0);
    ds.enabled = 1;
    static const struct azstatus_native_v1 api = {.load_seed = load_seed};
    atomic_store(&ds_native_api, &api);
    if (argc == 2 && !strcmp(argv[1], "load-race")) return key_load_race();
    if (argc == 2 && !strcmp(argv[1], "pad-race")) return pad_dependency_collision();
    if (argc == 2 && !strcmp(argv[1], "pad-certificate")) return pad_certificate_collision();
    if (argc == 2 && !strcmp(argv[1], "pad-selection")) return pad_select_collision();
    if (argc == 2 && !strcmp(argv[1], "pad-refresh")) return pad_refresh_collision(PAD_REFRESH);
    if (argc == 2 && !strcmp(argv[1], "pad-inactive")) return pad_refresh_collision(PAD_INACTIVE);
    if (argc == 2 && !strcmp(argv[1], "pad-reselect")) return pad_refresh_collision(PAD_RESELECT);
    union {
        double align;
        unsigned char bytes[256];
    } owner = {0};
    void *self = owner.bytes + BPMCTRL_OFF;
    g_bpmstate[1] = (uintptr_t) self;
    g_bpmstate_generation[1] = 5;
    ds_tempo_original = firmware_bpm;
    ds_tempo_dispose_original = firmware_dispose;
    atomic_store(&ds_tempo_ready, 1);
    ds_tempo_birth(1, 5);
    assert(ds_publish(1, 1, DS_BOUNDARY, ds_now(), NULL, 0, NULL, NULL, 0, 0) == AZ_OK);
    struct log log = {0};
    az_deck_subscription sub = 0;
    az_deck_watch_v1 w = {sizeof w, AZ_DATA_VERSION, 2, 30, AZ_STATUS_TOPIC(AZ_STATUS_SYNC) | AZ_STATUS_TOPIC(AZ_STATUS_TEMPO)};
    assert(ds_register(1, &w, receive, &log, &sub) == AZ_OK);
    until(&log.count, 1);
    double value = 128.125;
    ds_tempo_writer(self, &value);
    until(&log.bpm_ready, 1);
    assert(log.bpm == 128.125);
    value = 130.25;
    ds_tempo_writer(self, &value);
    until(&log.bpm_ready, 2);
    assert(log.bpm == 130.25);
    pthread_mutex_lock(&g_master_state_mu);
    g_master_state.deck = 1;
    g_master_state.observed_ns = ds_now();
    g_master_state.revision = 1;
    atomic_store(&g_master_event, 1);
    ds_master_capture(1, g_master_state.observed_ns, 1, 1);
    pthread_mutex_unlock(&g_master_state_mu);
    until(&log.master_changes, 1);
    assert(log.master == 1);
    slider_percent();
    effective_keys();
    slip_mode();
    unsigned stale = log.stale;
    swap_in_write = 1;
    value = 99.25;
    ds_tempo_writer(self, &value);
    until(&log.stale, stale + 1);
    assert(log.bpm_ready == 2);
    ds_tempo_dispose(owner.bytes);
    assert(disposed == 1 && ds_release(1, sub) == AZ_OK);
    pad_modes();
    puts("status GUI: committed pad modes, layer withdrawal, fresh per-deck seed and disposal passed");
    puts("status GUI: fractional BpmState commits, master handoff, racing load and disposal passed");
    return 0;
}
