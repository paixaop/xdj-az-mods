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
    int active = model != DS_MODEL_KEY || ds_tempo_load(deck, &before);
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
    return writer >= g_ep_img_lo && writer + sizeof signature <= g_ep_img_hi &&
           img_q(profile->control[version] + 16, &value) && value == profile->dispose[version] &&
           !memcmp((void *) writer, signature, sizeof signature);
}
static void ds_models_install(void) {
    if (access("/home/root/settings/azmod.deck-status.off", F_OK) == 0 ||
        (g_ep147_build != EPB_V130 && g_ep147_build != EPB_V200)) return;
    const unsigned version = g_ep147_build == EPB_V200;
    void (*writers[])(void *, void *) = {ds_key_writer, ds_slip_writer, ds_slider_writer};
    void (*disposes[])(void *) = {ds_key_dispose, ds_slip_dispose, ds_slider_dispose};
    for (unsigned model = 0; model < DS_MODEL_COUNT; model++) {
        const struct ds_model_profile *profile = &ds_models[model];
        if (!ds_model_target_valid(profile, version)) continue;
        if (azm_patch_vtable(profile->control[version] + 16, (void *) disposes[model], (void **) &ds_model_dispose_original[model]) == 1 &&
            azm_install_hook(profile->writer[version], (void *) writers[model], (void **) &ds_model_original[model]) == 1)
            atomic_store_explicit(&ds_model_ready[model], 1, memory_order_release);
    }
}
