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
static void ds_tempo_birth(unsigned d, uint64_t owner) {
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
        if (__atomic_load_n(&g_bpmstate[d], __ATOMIC_ACQUIRE) == object) return (int) d;
    return -1;
}
static int ds_tempo_load(unsigned d, struct azstatus_native_receipt *r) {
    const struct azstatus_native_v1 *api = atomic_load_explicit(&ds_native_api, memory_order_acquire);
    *r = (struct azstatus_native_receipt) {.size = sizeof *r, .version = 1};
    return api && api->load_seed && api->load_seed(d, r) && r->load_generation;
}
static void ds_tempo_writer(void *self, void *input) {
    int d = ds_tempo_deck((uintptr_t) self);
    if (d < 0 || !atomic_load_explicit(&ds_tempo_ready, memory_order_acquire)) {
        ds_tempo_original(self, input);
        return;
    }
    struct ds_tempo_slot *s = &ds_tempo_slots[d];
    uint64_t owner = __atomic_load_n(&g_bpmstate_generation[d], __ATOMIC_ACQUIRE);
    if (atomic_exchange_explicit(&s->busy, 1, memory_order_acquire)) {
        atomic_store_explicit(&s->collision, 1, memory_order_release);
        ds_tempo_gap(d);
        ds_tempo_original(self, input);
        ds_tempo_gap(d);
        return;
    }
    struct azstatus_native_receipt before, after;
    int armed = ds_tempo_load(d, &before);
    ds_tempo_original(self, input);
    if (owner && armed && owner != atomic_load_explicit(&s->retired, memory_order_acquire) &&
        (uintptr_t) self == __atomic_load_n(&g_bpmstate[d], __ATOMIC_ACQUIRE) &&
        owner == __atomic_load_n(&g_bpmstate_generation[d], __ATOMIC_ACQUIRE)) {
        uint64_t raw = __atomic_load_n((uint64_t *) ((uintptr_t) self + BPMSTATE_LIVE), __ATOMIC_RELAXED);
        double bpm;
        memcpy(&bpm, &raw, sizeof bpm);
        int ok = isfinite(bpm) && bpm > 0 && bpm < DBL_MAX;
        int fenced = ds_tempo_load(d, &after) && after.epoch == before.epoch &&
                     after.unit == before.unit && after.load_generation == before.load_generation;
        if (atomic_exchange_explicit(&s->collision, 0, memory_order_acq_rel)) fenced = 0;
        ds_tempo_store(d, owner, fenced ? before.load_generation : 0, fenced && ok ? bpm : 0,
                       fenced && ok ? AZ_AVAIL_READY : AZ_AVAIL_STALE);
    } else ds_tempo_store(d, owner, 0, 0, AZ_AVAIL_STALE);
    atomic_store_explicit(&s->busy, 0, memory_order_release);
}
static void ds_tempo_dispose(void *ctrl) {
    int d = ds_tempo_deck((uintptr_t) ctrl + BPMCTRL_OFF);
    if (d >= 0 && atomic_load_explicit(&ds_tempo_ready, memory_order_acquire)) {
        struct ds_tempo_slot *s = &ds_tempo_slots[d];
        uint64_t owner = __atomic_load_n(&g_bpmstate_generation[d], __ATOMIC_ACQUIRE);
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
                    r.owner != __atomic_load_n(&g_bpmstate_generation[d], __ATOMIC_ACQUIRE))) r.availability = AZ_AVAIL_STALE;
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
static void ds_tempo_install(void) {
    if (access("/home/root/settings/azmod.deck-status.off", F_OK) == 0 ||
        (g_ep147_build != EPB_V130 && g_ep147_build != EPB_V200)) return;
    const uintptr_t writer = g_ep147_build == EPB_V130 ? 0x1f5c8c8 : 0x20c8c40;
    const uintptr_t dispose = g_ep147_build == EPB_V130 ? 0x1fcbe20 : 0x21394f0;
    const uint32_t expected[] = {0xa9b87bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3};
    uintptr_t target;
    if (!img_q(BPMCTRL_VT + 16, &target) || target != dispose || writer < g_ep_img_lo ||
        writer + sizeof expected > g_ep_img_hi || memcmp((void *) writer, expected, sizeof expected)) return;
    if (azm_patch_vtable(BPMCTRL_VT + 16, (void *) ds_tempo_dispose, (void **) &ds_tempo_dispose_original) != 1 ||
        azm_install_hook(writer, (void *) ds_tempo_writer, (void **) &ds_tempo_original) != 1) return;
    atomic_store_explicit(&ds_tempo_ready, 1, memory_order_release);
}
