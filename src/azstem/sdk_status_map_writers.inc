/* SPDX-License-Identifier: GPL-3.0-only */
/* A holder refresh reuses the witnessed pair only while both retained holder
 * identities are unchanged. Its replacement must be a genuine owned value. */
struct HolderEdit {
    struct {
        Pair prior;
        unsigned decks;
    } entries[4];
    unsigned component, count, mask;
    uint64_t bracket;
};
static bool holder_retain_pair(Pair *next, const Pair *p) {
    next->binding = p->binding;
    Object *to[] = {&next->pack, &next->holder[0], &next->holder[1], &next->content[0], &next->content[1]};
    const Object *from[] = {&p->pack, &p->holder[0], &p->holder[1], &p->content[0], &p->content[1]};
    for (unsigned n = 0; n < 5; n++) {
        if (!from[n]->token) continue;
        if (!azn_pin_hold(from[n]->token, &to[n]->address)) {
            discard(next);
            return false;
        }
        to[n]->token = from[n]->token;
    }
    return true;
}
static void holder_capture(struct HolderEdit *edit, unsigned deck, uintptr_t holder, const struct azn_pack_view *view) {
    for (unsigned i = 0; i < edit->count; i++) {
        if (edit->entries[i].prior.pack.address == view->object) {
            edit->entries[i].decks |= 1u << deck;
            edit->mask |= 1u << deck;
            return;
        }
    }
    Slot *slot = &cache[bucket(view->object)];
    SLOT_LOCK(lock, *slot);
    if (!lock.held) {
        azn_map_gap(deck);
        return;
    }
    const Pair *p = &slot->pair;
    if (p->pack.address != view->object || p->holder[edit->component].address != holder || !azn_pack_current(deck, view)) return;
    Pair *next = &edit->entries[edit->count].prior;
    if (!holder_retain_pair(next, p)) {
        azn_map_gap(deck);
        return;
    }
    edit->entries[edit->count++].decks = 1u << deck;
    edit->mask |= 1u << deck;
}
static void holder_begin(struct HolderEdit *edit, unsigned component, uintptr_t holder) {
    *edit = (struct HolderEdit) {.component = component};
    if (!holder || !atomic_load_explicit(&enabled, memory_order_acquire)) return;
    for (unsigned d = 0; d < 4; d++) {
        struct azn_pack_view view = {0};
        if (!(azn_interest(d) & routes) || !azn_pack_acquire(d, &view)) continue;
        if (read_ptr(view.object, component ? 0x120 : 0xd8) == holder) holder_capture(edit, d, holder, &view);
        azn_pin_drop(view.token);
    }
    edit->bracket = azn_map_begin_mask(edit->mask);
}
static void holder_gap(unsigned decks) {
    for (unsigned d = 0; d < 4; d++)
        if (decks & (1u << d)) azn_map_gap(d);
}
static void holder_finish(struct HolderEdit *edit, uintptr_t replacement, bool preserve) {
    for (unsigned i = 0; i < edit->count; i++) {
        const Pair *p = &edit->entries[i].prior;
        uintptr_t gc = edit->component == 0 && !preserve ? replacement : p->content[0].address;
        uintptr_t pc = edit->component == 1 && !preserve ? replacement : p->content[1].address;
        if (!edit->bracket || !update_pair(0, p->pack.address, p->holder[0].address, gc, p->holder[1].address, pc, p->binding))
            holder_gap(edit->entries[i].decks);
    }
}
static void holder_drop(struct HolderEdit *edit) {
    azn_map_end_mask(edit->bracket);
    for (unsigned i = 0; i < edit->count; i++) discard(&edit->entries[i].prior);
}
#define HOLDER_EDIT(name, component, holder)                            \
    struct HolderEdit name __attribute__((cleanup(holder_drop))) = {0}; \
    holder_begin(&name, component, holder)
static void *map_writer_original[4];
static unsigned map_writer_installed;
static uintptr_t grid_assign(uintptr_t dst, uintptr_t source) {
    MAP_MUTATION(mutation);
    HOLDER_EDIT(edit, 0, dst >= 0x28 ? dst - 0x28 : 0);
    uintptr_t incoming = edit.mask && source ? read_ptr(source, 0) : 0;
    PIN_DROP(pin, incoming && typed(incoming, profile->grid_content) ? azn_owned_pin(incoming) : 0);
    uintptr_t result = ((uintptr_t (*)(uintptr_t, uintptr_t)) map_writer_original[0])(dst, source);
    if (result && edit.mask) holder_finish(&edit, pin ? incoming : 0, false);
    return result;
}
static uintptr_t grid_retime(uintptr_t dst, int64_t offset) {
    MAP_MUTATION(mutation);
    HOLDER_EDIT(edit, 0, dst >= 0x28 ? dst - 0x28 : 0);
    uintptr_t result = ((uintptr_t (*)(uintptr_t, int64_t)) map_writer_original[1])(dst, offset);
    if (result && edit.mask) holder_finish(&edit, 0, true);
    return result;
}
static _Thread_local uintptr_t phrase_argument;
static void phrase_restore(uintptr_t *previous) { phrase_argument = *previous; }
static uintptr_t phrase_registry(uintptr_t registry, uintptr_t context, uintptr_t id, uintptr_t source, uintptr_t flags, uintptr_t version) {
    MAP_MUTATION(mutation);
    uintptr_t previous __attribute__((cleanup(phrase_restore))) = phrase_argument;
    phrase_argument = source; /* borrowed reference lives through this call */
    return ((uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)) map_writer_original[2])(registry, context, id, source, flags, version);
}
static uintptr_t phrase_broadcast(uintptr_t holder, unsigned reason) {
    MAP_MUTATION(mutation);
    if (phrase_argument) {
        HOLDER_EDIT(edit, 1, holder);
        if (edit.mask) holder_finish(&edit, read_ptr(phrase_argument, 0), false);
    }
    return ((uintptr_t (*)(uintptr_t, unsigned)) map_writer_original[3])(holder, reason);
}
