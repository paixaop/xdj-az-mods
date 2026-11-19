/* SPDX-License-Identifier: GPL-3.0-only */
/* CueCallManager chooses a row in the immutable memory holder. Its facade
 * sends a different object (a copied CueProperty) to the player task box. */
struct RecallProfile {
    uintptr_t calls[4], dispatcher, construct[3], task_table, facade_table;
    Entry entries[6];
};
static const RecallProfile recall_profiles[2] = {
    {{0x810be0, 0x811094, 0x8117a0, 0x8118b4}, 0x820908, {0x13726f0, 0x1372704, 0x13728a8}, 0x2877290, 0x28770b8, {{0x80e8c8, {0xa9a77bfd, 0x910003fd, 0xa90153f3, 0xa9025bf5}}, {0x13726c8, {0xd10bc3ff, 0xa9007bfd, 0x910003fd, 0xa90153f3}}, {0x80aac0, {0xa9b97bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3}}, {0x1371940, {0xa9bd7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3}}, {0x1371780, {0xa9bd7bfd, 0xd000a821, 0x91042021, 0x910003fd}}, {0x1371428, {0xa9bd7bfd, 0xd000a821, 0x91042021, 0x910003fd}}}},
    {{0x8565c8, 0x8569f0, 0x8570f8, 0x85720c}, 0x866250, {0x142da40, 0x142da54, 0x142dbf8}, 0x2bc94d8, 0x2bc9300, {{0x8542b0, {0xa9a77bfd, 0x910003fd, 0xa90153f3, 0xa9025bf5}}, {0x142da18, {0xd10bc3ff, 0xa9007bfd, 0x910003fd, 0xa90153f3}}, {0x8504a8, {0xa9b97bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3}}, {0x142cc90, {0xa9bd7bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3}}, {0x142cad0, {0xa9bd7bfd, 0xb000bce1, 0x910d4021, 0x910003fd}}, {0x142c778, {0xa9bd7bfd, 0xb000bce1, 0x910d4021, 0x910003fd}}}}};
static const RecallProfile *recall_profile;
static uintptr_t recall_bias;
static void *recall_original[6];
static _Atomic(bool) recall_ready = false;
struct MemoryEdition {
    uint64_t epoch, load, unit, provider, serial, pack_token, content_token;
    unsigned entry;
};
struct RecallCandidates {
    MemoryEdition deck[4];
};
static _Thread_local const RecallCandidates *selection = NULL;
struct RecallFrame {
    const RecallCandidates *candidates;
    uintptr_t facade, property, first, owned, retained;
    uint32_t retained_revision;
};
static _Thread_local RecallFrame *recall_frame = NULL;
/* Restore a thread-local borrowed pointer on every return and native unwind. */
struct PointerScope {
    void *address;
    uintptr_t previous;
};
static struct PointerScope pointer_scope(void *address, const void *next) {
    struct PointerScope scope = {.address = address};
    uintptr_t value = (uintptr_t) next;
    memcpy(&scope.previous, address, sizeof scope.previous);
    memcpy(address, &value, sizeof value);
    return scope;
}
static void pointer_restore(struct PointerScope *scope) { memcpy(scope->address, &scope->previous, sizeof scope->previous); }
#define POINTER_SCOPE(name, address, next) struct PointerScope name __attribute__((cleanup(pointer_restore))) = pointer_scope(&(address), next)
static bool selected_row_bounds(uintptr_t start, uintptr_t end, unsigned entry) {
    return ptr(start) && end >= start && !((end - start) % 0x80) && (end - start) / 0x80 <= AZ_NATIVE_CUE_LIMIT && entry && entry <= (end - start) / 0x80;
}
static bool selected_objects_current(Pin *binding, uintptr_t holder, uintptr_t content, const Hold *h, const Hold *c) {
    return object_word(binding->object + 0xe8) == holder && object_word(holder + 0x28) == content &&
           atomic_load_explicit(&(binding->latest_pin), memory_order_acquire) == c->token &&
           atomic_load_explicit(&(binding->holder_pin), memory_order_acquire) == h->token;
}
static bool selected_writes_current(const Writing *replacement, const Writing *writes, uint64_t rc, uint64_t wc) {
    return atomic_load_explicit(&(replacement->interference), memory_order_acquire) == rc &&
           atomic_load_explicit(&(writes->interference), memory_order_acquire) == wc &&
           !atomic_load_explicit(&(replacement->count), memory_order_acquire) && !atomic_load_explicit(&(writes->count), memory_order_acquire);
}
static bool selected_loop_row(uintptr_t row) {
    __auto_type times = azcue_read_timestamps((const void *) row);
    double start_s = 0, end_s = 0;
    const int interval = compare_timestamps(times.out_sample, times.in_sample, times.sample_rate, times.sample_rate, 1);
    return !SCALAR(unsigned char, row) && coordinate(times.in_sample, times.in_us, times.sample_rate, &start_s) &&
           coordinate(times.out_sample, times.out_us, times.sample_rate, &end_s) && interval != AZ_TIME_INVALID && interval == AZ_TIME_AFTER;
}
static bool selected_binding_current(unsigned d, const Hold *pack, Pin *binding) {
    return atomic_load_explicit(&(active[d]), memory_order_acquire) == pack->token && current(d, binding);
}
static MemoryEdition selected_published_edition(unsigned d, const Hold *active_pack, const Hold *h, const Hold *c, uintptr_t content, unsigned entry) {
    Pin *binding = pin_for(active_pack->token);
    uintptr_t holder = pin_for(h->token)->object;
    Writing *replacement = &holder_writers[cache_slot(binding->object)];
    Writing *writes = &writers[cache_slot(holder)];
    uint64_t rc = atomic_load_explicit(&(replacement->interference), memory_order_acquire), wc = atomic_load_explicit(&(writes->interference), memory_order_acquire);
    if (atomic_load_explicit(&(replacement->count), memory_order_acquire) || atomic_load_explicit(&(writes->count), memory_order_acquire) ||
        atomic_flag_test_and_set_explicit(&(binding->publishing), memory_order_acquire)) return (MemoryEdition) {0};
    PUBLISHING_GUARD(guard, binding);
    uintptr_t start = object_word(content + 0x28), end = object_word(content + 0x30);
    if (!selected_row_bounds(start, end, entry) || !selected_objects_current(binding, holder, content, h, c)) return (MemoryEdition) {0};
    if (!selected_binding_current(d, active_pack, binding) ||
        !selected_writes_current(replacement, writes, rc, wc)) return (MemoryEdition) {0};
    if (!selected_loop_row(start + (entry - 1) * 0x80u)) return (MemoryEdition) {0};
    return (MemoryEdition) {binding->epoch, binding->load, binding->unit, atomic_load_explicit(&(binding->provider), memory_order_relaxed),
                            atomic_load_explicit(&(binding->serial), memory_order_relaxed), active_pack->token, c->token, entry};
}
/* Match an original caller-held Content reference against the active binding.
 * Both this immutable content and the holder are live during the checks. */
static MemoryEdition selected_edition(unsigned d, uintptr_t pack, uintptr_t content, unsigned entry) {
    if (!observing()) return (MemoryEdition) {0};
    HOLD(active_pack, atomic_load_explicit(&(active[d]), memory_order_acquire));
    Pin *binding = pin_for(active_pack.token);
    if (!binding || binding->object != pack || !current(d, binding)) return (MemoryEdition) {0};
    HOLD(h, atomic_load_explicit(&(binding->holder_pin), memory_order_acquire));
    HOLD(c, atomic_load_explicit(&(binding->latest_pin), memory_order_acquire));
    if (!h.token || !c.token || pin_for(c.token)->object != content || !typed(content, profile.content)) return (MemoryEdition) {0};
    return selected_published_edition(d, &active_pack, &h, &c, content, entry);
}
static void recall_select(RecallCandidates *candidates, uintptr_t caller, uintptr_t caller_sp, uintptr_t pack, uintptr_t index) {
    for (unsigned n = 0; n < 4; ++n) {
        if (caller != recall_bias + recall_profile->calls[n]) continue;
        if (!typed(pack, profile.pack)) return;
        uintptr_t content = object_word(caller_sp + (n < 2 ? 0x78 : 0x90));
        for (unsigned d = 0; d < 4; ++d) candidates->deck[d] = selected_edition(d, pack, content, (unsigned) index + 1);
        return;
    }
}
__attribute__((visibility("hidden"))) void azn_recall_accept(uintptr_t manager, uintptr_t track, uintptr_t pack,
                                                             uintptr_t name, uintptr_t row, uintptr_t option, uintptr_t caller_sp, uintptr_t index) {
    RecallCandidates candidates = {0};
    uintptr_t caller = ((uintptr_t) (__builtin_return_address(0)));
    if (atomic_load_explicit(&(recall_ready), memory_order_acquire) && ptr(caller_sp) && row == caller_sp + 0xa0 &&
        index < AZ_NATIVE_CUE_LIMIT) {
        recall_select(&candidates, caller, caller_sp, pack, index);
    }
    bool selected = false;
    for (unsigned deck_index = 0; deck_index < sizeof(candidates.deck) / sizeof(candidates.deck)[0]; ++deck_index) {
        const MemoryEdition *deck = &(candidates.deck[deck_index]);
        selected |= deck->entry != 0;
    }
    POINTER_SCOPE(scope, selection, selected ? &candidates : NULL);
    ((void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))(recall_original[0]))(
        manager, track, pack, name, row, option);
}
#if defined(__aarch64__)
/* The four audited callers keep the actual row index in x22, pass the copied
 * row at sp+0xa0 in x4 and keep the originating intrusive reference on stack.
 * x0..x5 and the original LR are forwarded unchanged. */
void azn_recall_accept_entry();
__asm__(".text\n.p2align 2\n.global azn_recall_accept_entry\n.hidden azn_recall_accept_entry\n.type azn_recall_accept_entry,%function\n"
        "azn_recall_accept_entry:\nmov x6,sp\nmov x7,x22\nb azn_recall_accept\n.size azn_recall_accept_entry,.-azn_recall_accept_entry\n");
#endif
struct RecallJob {
    _Atomic(uintptr_t) task;
    uintptr_t facade;
    uint64_t sequence;
    RecallCandidates candidates;
};
static RecallJob recall_jobs[32];
static bool consume_job(uintptr_t task, uint64_t seq, uintptr_t facade, RecallCandidates *out) {
    for (unsigned job_index = 0; job_index < sizeof(recall_jobs) / sizeof(recall_jobs)[0]; ++job_index) {
        RecallJob *job = &(recall_jobs[job_index]);
        uintptr_t match = task;
        if (!atomic_compare_exchange_strong_explicit(&(job->task), &(match), UINTPTR_MAX, memory_order_acq_rel, memory_order_acquire)) continue;
        const bool valid = job->sequence == seq && job->facade == facade;
        if (valid && out) *out = job->candidates;
        atomic_store_explicit(&(job->task), valid ? 0 : task, memory_order_release);
        if (valid) return true;
    }
    return false;
}
static void seal_job(uintptr_t task, const RecallFrame *frame) {
    for (unsigned job_index = 0; job_index < sizeof(recall_jobs) / sizeof(recall_jobs)[0]; ++job_index) {
        RecallJob *job = &(recall_jobs[job_index]);
        uintptr_t empty = 0;
        if (!atomic_compare_exchange_strong_explicit(&(job->task), &(empty), UINTPTR_MAX, memory_order_acq_rel, memory_order_acquire)) continue;
        job->sequence = object_word(task + 0x10);
        job->facade = frame->facade;
        job->candidates = *frame->candidates;
        atomic_store_explicit(&(job->task), task, memory_order_release);
        return;
    }
    /* A later working copy without its task token explicitly withdraws any
     * older association. Never wait or overwrite a still-executable task. */
}
static void memory_call(uintptr_t facade, unsigned kind, uintptr_t property) {
    RecallFrame frame = {0};
    __auto_type caller = ((uintptr_t) (__builtin_return_address(0)));
    if (atomic_load_explicit(&(recall_ready), memory_order_acquire) && selection && !kind &&
        caller == recall_bias + recall_profile->dispatcher && typed(facade, recall_bias + recall_profile->facade_table)) {
        frame.candidates = selection;
        frame.facade = facade;
        frame.property = property;
    }
    POINTER_SCOPE(scope, recall_frame, frame.candidates ? &frame : NULL);
    ((void (*)(uintptr_t, unsigned, uintptr_t))(recall_original[1]))(facade, kind, property);
}
static void recall_seal_task(uintptr_t dst, const RecallFrame *f) {
    uintptr_t task = dst - 0x28;
    if (typed(task, recall_bias + recall_profile->task_table) && object_word(task + 0x18) == f->facade &&
        SCALAR(unsigned, task + 0x20) == 0 && object_word(task + 0x10)) seal_job(task, f);
}
static uintptr_t recall_ctor(uintptr_t dst, uintptr_t src) {
    __auto_type caller = ((uintptr_t) (__builtin_return_address(0)));
    uintptr_t result = ((uintptr_t (*)(uintptr_t, uintptr_t))(recall_original[2]))(dst, src);
    __auto_type f = recall_frame;
    if (!f) return result;
    __auto_type pc = recall_profile->construct;
    if (caller == recall_bias + pc[0] && src == f->property) f->first = dst;
    else if (caller == recall_bias + pc[1] && src == f->first) f->owned = dst;
    else if (caller == recall_bias + pc[2] && src == f->owned && dst >= 0x10028) {
        recall_seal_task(dst, f);
    }
    return result;
}
static void recall_execute(uintptr_t task) {
    RecallCandidates captured = {0};
    RecallFrame frame = {0};
    uintptr_t facade = object_word(task + 0x18);
    if (atomic_load_explicit(&(recall_ready), memory_order_acquire) && typed(task, recall_bias + recall_profile->task_table) &&
        !SCALAR(unsigned, task + 0x20) && consume_job(task, object_word(task + 0x10), facade, &captured)) {
        frame.candidates = &captured;
        frame.facade = facade;
        frame.owned = task + 0x28;
    }
    POINTER_SCOPE(scope, recall_frame, frame.candidates ? &frame : NULL);
    ((void (*)(uintptr_t))(recall_original[3]))(task);
}
static uintptr_t recall_destroy_4(uintptr_t task) {
    if (atomic_load_explicit(&(recall_ready), memory_order_acquire))
        consume_job(task, object_word(task + 0x10), object_word(task + 0x18), NULL);
    return ((uintptr_t (*)(uintptr_t))(recall_original[4]))(task);
}
static uintptr_t recall_destroy_5(uintptr_t task) {
    if (atomic_load_explicit(&(recall_ready), memory_order_acquire))
        consume_job(task, object_word(task + 0x10), object_word(task + 0x18), NULL);
    return ((uintptr_t (*)(uintptr_t))(recall_original[5]))(task);
}
static void install_recall(int build, uintptr_t bias, azexit_install_fn install) {
#if defined(__aarch64__)
    recall_profile = &recall_profiles[build - 1];
    recall_bias = bias;
    void *hooks[] = {((void *) (azn_recall_accept_entry)), ((void *) (memory_call)),
                     ((void *) (recall_ctor)), ((void *) (recall_execute)),
                     ((void *) (recall_destroy_4)), ((void *) (recall_destroy_5))};
    for (unsigned entry_index = 0; entry_index < sizeof(recall_profile->entries) / sizeof(recall_profile->entries)[0]; ++entry_index) {
        const Entry *entry = &(recall_profile->entries[entry_index]);
        if (!recall_original[entry_index] && memcmp(((const void *) (bias + entry->address)), entry->prologue, sizeof entry->prologue)) return;
    }
    for (unsigned i = 0; i < 6; ++i)
        if (!recall_original[i] && (!install(bias + recall_profile->entries[i].address, hooks[i], &recall_original[i]) || !recall_original[i])) return;
    atomic_store_explicit(&(recall_ready), true, memory_order_release);
#else
    (void) build;
    (void) bias;
    (void) install;
#endif
}
