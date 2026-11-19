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
    uintptr_t expected = g_ep147_build == EPB_V130 ? 0x2e069b0 : 0x31b9db0;
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
static void ds_pad_fresh(uintptr_t p) {
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
    return selector >= g_ep_img_lo && selector + sizeof signature <= g_ep_img_hi &&
           !memcmp((void *) selector, signature, sizeof signature);
}
static int ds_pad_entries_valid(const struct ds_pad_profile *profile) {
    const uint32_t expected[] = {0xa9af7bfd, 0xa9b47bfd, 0xf9400c01, 0xa9b87bfd, 0xa9b37bfd, 0xa9b87bfd, 0xa9bc7bfd, 0xa9b47bfd};
    for (unsigned i = 0; i < 8; i++)
        if (profile->entry[i] < g_ep_img_lo || profile->entry[i] + 4 > g_ep_img_hi ||
            memcmp((void *) profile->entry[i], &expected[i], 4)) return 0;
    return 1;
}
static int ds_pad_targets_valid(const struct ds_pad_profile *profile, uintptr_t selector) {
    uintptr_t dispose = 0;
    if (!ds_pad_selector_valid(selector)) return 0;
    return img_q(profile->ctrl + 16, &dispose) && dispose == profile->dispose && ds_pad_entries_valid(profile);
}
static int ds_pad_install_entries(const struct ds_pad_profile *profile, uintptr_t selector) {
    void *hooks[] = {ds_pad_construct, ds_pad_direct, ds_pad_async, ds_pad_alternate, ds_pad_pc_direct, ds_pad_pc_async,
                     ds_pad_cert_update, ds_pad_cert_construct};
    for (unsigned i = 0; i < 8; i++)
        if (azm_install_island_hook_at(profile->entry[i], hooks[i], &ds_pad_original[i], "status-pad") != 1) return 0;
    return azm_install_island_hook_at(selector, ds_pad_select, &ds_pad_select_original, "status-pad-layer") == 1;
}
static void ds_pad_install(void) {
    if (g_ep147_build != EPB_V130 && g_ep147_build != EPB_V200) return;
    if (access("/home/root/settings/azmod.deck-status.off", F_OK) == 0) return;
    const struct ds_pad_profile *profile = &ds_pad_profiles[g_ep147_build == EPB_V130 ? 0 : 1];
    uintptr_t selector = g_ep147_build == EPB_V130 ? 0x2039d58 : 0x21a7650;
    if (!ds_pad_targets_valid(profile, selector)) return;
    ds_pad_profile = profile;
    if (azm_patch_vtable(profile->ctrl + 16, ds_pad_dispose, &ds_pad_dispose_original) != 1) return;
    if (!ds_pad_install_entries(profile, selector)) return;
    atomic_store_explicit(&ds_pad_ready, 1, memory_order_release);
}
