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
static uintptr_t ds_model_ctrl(unsigned model) { return ds_models[model].control[g_ep147_build == EPB_V200]; }
static int ds_model_deck(unsigned model, uintptr_t self) {
    const struct ds_model_profile *p = &ds_models[model];
    unsigned d = *(const uint32_t *) (self + p->deck_offset);
    if (d >= 4) return -1;
    uintptr_t block = perdeck_block((int) d);
    if (!block || *(const uintptr_t *) (block + p->parent_offset) != self ||
        *(const uintptr_t *) (block + p->parent_offset + 8) != self - 16 ||
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
/* Called with the slot locked. Recovery can reserve a newer revision even
 * if a firmware writer is suspended while holding this slot. */
static void ds_model_store(unsigned model, unsigned d, struct ds_gui_record r, uint64_t fault) {
    struct ds_model_slot *s = &ds_model_slots[model][d];
    struct ds_gui_record *old = &s->latest;
    if (old->revision && old->revision == atomic_load(&s->revision) && old->owner == r.owner && old->epoch == r.epoch &&
        old->availability == r.availability && old->original_key_availability == r.original_key_availability &&
        !memcmp(old->original_key, r.original_key, sizeof r.original_key) && fault == s->sealed_fault && !memcmp(&old->value, &r.value, sizeof r.value)) return;
    r.revision = ds_model_revision(s);
    if (!r.revision || fault != atomic_load_explicit(&s->fault, memory_order_acquire)) {
        ds_model_gap(model, d);
        return;
    }
    r.original_key_revision = old->owner == r.owner && old->epoch == r.epoch && fault == s->sealed_fault &&
                                      old->original_key_availability == r.original_key_availability &&
                                      !memcmp(old->original_key, r.original_key, sizeof r.original_key) && old->original_key_revision
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
    if (atomic_load_explicit(&s->object, memory_order_acquire) != object) {
        if (s->incarnation == (UINT64_C(1) << 40) - 1) {
            ds_model_gap(model, d);
            original(self, input);
            return;
        }
        ++s->incarnation;
        atomic_store_explicit(&s->object, object, memory_order_release);
    }
    uint64_t fault = atomic_load_explicit(&s->fault, memory_order_acquire);
    struct azstatus_native_receipt before, after;
    int active = model != DS_MODEL_KEY || ds_tempo_load(d, &before);
    original(self, input);
    if (!active || atomic_load_explicit(&s->object, memory_order_acquire) != object ||
        atomic_load_explicit(&s->fault, memory_order_acquire) != fault || ds_model_deck(model, object) != d) return;
    struct ds_gui_record r = {.availability = AZ_AVAIL_STALE,
                              .owner = AZSP_GUI_MODEL | ((uint64_t) model << 48) | ((uint64_t) d << 40) | s->incarnation};
    if (model == DS_MODEL_KEY) {
        if (!ds_tempo_load(d, &after) || after.epoch != before.epoch || after.unit != before.unit ||
            after.load_generation != before.load_generation) return;
        r.epoch = after.load_generation;
        const unsigned char *key = (const unsigned char *) self + 0xa4;
        if (key[3] == 1 && key[0] < 12 && key[1] <= 1) {
            ct_camelot(key[0], key[1], r.value.key);
            r.availability = AZ_AVAIL_READY;
        } else if (!key[3]) r.availability = AZ_AVAIL_EMPTY;
        const unsigned char *original_key = (const unsigned char *) self + 0xb0;
        r.original_key_availability = AZ_AVAIL_STALE;
        if (original_key[3] == 1 && original_key[0] < 12 && original_key[1] <= 1) {
            ct_camelot(original_key[0], original_key[1], r.original_key);
            r.original_key_availability = AZ_AVAIL_READY;
        } else if (!original_key[3]) r.original_key_availability = AZ_AVAIL_EMPTY;
    } else if (model == DS_MODEL_SLIDER) {
        float ratio;
        memcpy(&ratio, (const unsigned char *) self + 0xa4, sizeof ratio);
        if (isfinite(ratio) && fabsf(ratio) < FLT_MAX) {
            r.value.slider = 100.0 * (double) ratio;
            r.availability = AZ_AVAIL_READY;
        }
    } else {
        unsigned enabled = *((const unsigned char *) self + 0xa8);
        if (enabled <= 1) {
            r.value.setting = (int) enabled;
            r.availability = AZ_AVAIL_READY;
        }
    }
    ds_model_store(model, (unsigned) d, r, fault);
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
static int ds_model_apply(unsigned model, const struct ds_gui_record *r, int gap) {
    const struct ds_model_profile *p = &ds_models[model];
    uint64_t topic = AZ_STATUS_TOPIC(p->topic), epoch = r->epoch;
    unsigned flags = model != DS_MODEL_KEY ? DS_UNIT_INPUT : 0;
    unsigned count = model == DS_MODEL_KEY ? 2 : 1;
    struct ds_field fields[2] = {{.topic = p->topic, .field = p->field},
                                 {.topic = AZ_STATUS_KEY, .field = AZ_STATUS_KEY_ORIGINAL}};
    if (!r->owner) {
        if (!gap) return AZ_OK;
        pthread_mutex_lock(&ds.mu);
        struct ds_image *im = &ds.deck[r->deck].current;
        epoch = im->epoch;
        for (unsigned i = 0; i < count; i++) {
            struct ds_field *f = &fields[i];
            const struct ds_cell *old = im->field[f->topic][f->field];
            int withdraw = old && old->info.info.provenance == AZ_PROV_NATIVE &&
                           (old->info.scope == AZ_STATUS_UNIT || (epoch && old->info.scope_id == epoch));
            if (withdraw) f->info = old->info;
            else {
                f->info.scope = model != DS_MODEL_KEY ? AZ_STATUS_UNIT : AZ_STATUS_LOAD_SCOPE;
                f->info.scope_id = model == DS_MODEL_KEY ? epoch : 0;
                f->info.info.provenance = AZ_PROV_NATIVE;
            }
            f->info.info.availability = AZ_AVAIL_STALE;
            f->info.info.observed_ns = r->observed;
            f->info.info.revision = r->revision;
            f->info.count = 0;
        }
        pthread_mutex_unlock(&ds.mu);
        int rc = ds_publish(r->deck, epoch, model == DS_MODEL_KEY ? DS_FIELD_LOSS | DS_NATIVE_INPUT : DS_UNIT_INPUT, r->observed,
                            fields, count, NULL, NULL, topic, AZ_STATUS_CAPTURE_GAP);
        if (model == DS_MODEL_KEY && (rc == AZ_OK || rc == AZ_LIMIT)) sdk_status_store_reseed(r->deck, epoch, topic);
        return rc;
    }
    fields[0].info = (az_status_field_info_v1) {.info = {r->availability, AZ_PROV_NATIVE, r->revision, r->observed},
                                                .scope = model != DS_MODEL_KEY ? AZ_STATUS_UNIT : AZ_STATUS_LOAD_SCOPE,
                                                .scope_id = model != DS_MODEL_KEY ? r->owner : epoch,
                                                .provider_id = r->owner};
    fields[0].value = model == DS_MODEL_KEY ? (const void *) r->value.key : model == DS_MODEL_SLIDER ? (const void *) &r->value.slider
                                                                                                     : (const void *) &r->value.setting;
    if (r->availability == AZ_AVAIL_READY) fields[0].info.count = model != DS_MODEL_KEY ? 1u : (unsigned) strlen(r->value.key);
    if (model == DS_MODEL_KEY) {
        fields[1].info = fields[0].info;
        fields[1].info.info.availability = r->original_key_availability;
        fields[1].info.info.revision = r->original_key_revision;
        fields[1].info.count = r->original_key_availability == AZ_AVAIL_READY ? (unsigned) strlen(r->original_key) : 0;
        fields[1].value = r->original_key;
    }
    if (gap && model == DS_MODEL_KEY) flags |= DS_FIELD_LOSS | DS_NATIVE_INPUT;
    int rc = ds_publish(r->deck, epoch, flags, r->observed, fields, count, NULL, NULL, gap ? topic : 0, gap ? AZ_STATUS_CAPTURE_GAP : 0);
    if (model == DS_MODEL_KEY && (rc == AZ_OK || rc == AZ_LIMIT) && r->original_key_availability != AZ_AVAIL_READY)
        sdk_status_store_reseed(r->deck, epoch, topic);
    return rc;
}
static void ds_key_writer(void *self, void *input) { ds_model_writer(DS_MODEL_KEY, self, input); }
static void ds_key_dispose(void *ctrl) { ds_model_dispose(DS_MODEL_KEY, ctrl); }
static void ds_slip_writer(void *self, void *input) { ds_model_writer(DS_MODEL_SLIP, self, input); }
static void ds_slip_dispose(void *ctrl) { ds_model_dispose(DS_MODEL_SLIP, ctrl); }
static void ds_slider_writer(void *self, void *input) { ds_model_writer(DS_MODEL_SLIDER, self, input); }
static void ds_slider_dispose(void *ctrl) { ds_model_dispose(DS_MODEL_SLIDER, ctrl); }
static void ds_models_install(void) {
    if (access("/home/root/settings/azmod.deck-status.off", F_OK) == 0 ||
        (g_ep147_build != EPB_V130 && g_ep147_build != EPB_V200)) return;
    const unsigned v = g_ep147_build == EPB_V200;
    void (*writers[])(void *, void *) = {ds_key_writer, ds_slip_writer, ds_slider_writer};
    void (*disposes[])(void *) = {ds_key_dispose, ds_slip_dispose, ds_slider_dispose};
    for (unsigned m = 0; m < DS_MODEL_COUNT; m++) {
        const struct ds_model_profile *p = &ds_models[m];
        uintptr_t value, writer = p->writer[v];
        const uint32_t signature[] = {p->first, 0x910003fd, 0xa90153f3, 0xaa0003f3};
        if (writer < g_ep_img_lo || writer + sizeof signature > g_ep_img_hi ||
            !img_q(p->control[v] + 16, &value) || value != p->dispose[v] || memcmp((void *) writer, signature, sizeof signature)) continue;
        if (azm_patch_vtable(p->control[v] + 16, (void *) disposes[m], (void **) &ds_model_dispose_original[m]) == 1 &&
            azm_install_hook(writer, (void *) writers[m], (void **) &ds_model_original[m]) == 1)
            atomic_store_explicit(&ds_model_ready[m], 1, memory_order_release);
    }
}
