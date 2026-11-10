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
static std::atomic<bool> recall_ready {false};
struct MemoryEdition {
    uint64_t epoch, load, unit, provider, serial, pack_token, content_token;
    unsigned entry;
};
struct RecallCandidates {
    MemoryEdition deck[4];
};
static thread_local const RecallCandidates *selection = nullptr;
struct RecallFrame {
    const RecallCandidates *candidates;
    uintptr_t facade, property, first, owned, retained;
    uint32_t retained_revision;
};
static thread_local RecallFrame *recall_frame = nullptr;
template <class T>
struct RecallScope {
    T &location;
    T previous;
    RecallScope(T &target, T next) : location(target), previous(target) { location = next; }
    ~RecallScope() { location = previous; }
};
/* Match an original caller-held Content reference against the active binding.
 * Both this immutable content and the holder are live during the checks. */
static MemoryEdition selected_edition(unsigned d, uintptr_t pack, uintptr_t content, unsigned entry) {
    if (!observing()) return {};
    Hold active_pack(active[d].load(std::memory_order_acquire));
    Pin *binding = active_pack.get();
    if (!binding || binding->object != pack || !current(d, *binding)) return {};
    Hold h(binding->holder_pin.load(std::memory_order_acquire)), c(binding->latest_pin.load(std::memory_order_acquire));
    if (!h.token || !c.token || c.get()->object != content || !typed(content, profile.content)) return {};
    const auto holder = h.get()->object;
    auto &replacement = holder_writers[cache_slot(pack)], &writes = writers[cache_slot(holder)];
    uint64_t rc = replacement.interference.load(std::memory_order_acquire), wc = writes.interference.load(std::memory_order_acquire);
    if (replacement.count.load(std::memory_order_acquire) || writes.count.load(std::memory_order_acquire) ||
        binding->publishing.test_and_set(std::memory_order_acquire)) return {};
    struct Unlock {
        Pin &p;
        ~Unlock() { p.publishing.clear(std::memory_order_release); }
    } guard {*binding};
    uintptr_t start = object_word(content + 0x28), end = object_word(content + 0x30);
    if (!ptr(start) || end < start || ((end - start) % 0x80) || (end - start) / 0x80 > AZ_NATIVE_CUE_LIMIT ||
        !entry || entry > (end - start) / 0x80 || object_word(pack + 0xe8) != holder || object_word(holder + 0x28) != content ||
        binding->latest_pin.load(std::memory_order_acquire) != c.token || binding->holder_pin.load(std::memory_order_acquire) != h.token ||
        active[d].load(std::memory_order_acquire) != active_pack.token || !current(d, *binding) ||
        replacement.interference.load(std::memory_order_acquire) != rc || writes.interference.load(std::memory_order_acquire) != wc ||
        replacement.count.load(std::memory_order_acquire) || writes.count.load(std::memory_order_acquire)) return {};
    const uintptr_t row = start + (entry - 1) * 0x80u;
    auto times = azcue_read_timestamps(reinterpret_cast<const void *>(row));
    double start_s = 0, end_s = 0;
    const int interval = compare_timestamps(times.out_sample, times.in_sample, times.sample_rate, times.sample_rate, 1);
    if (scalar<unsigned char>(row) || !coordinate(times.in_sample, times.in_us, times.sample_rate, start_s) ||
        !coordinate(times.out_sample, times.out_us, times.sample_rate, end_s) ||
        interval == AZ_TIME_INVALID || interval != AZ_TIME_AFTER) return {};
    return {binding->epoch, binding->load, binding->unit, binding->provider.load(std::memory_order_relaxed),
            binding->serial.load(std::memory_order_relaxed), active_pack.token, c.token, entry};
}
extern "C" __attribute__((visibility("hidden"))) void azn_recall_accept(uintptr_t manager, uintptr_t track, uintptr_t pack,
                                                                        uintptr_t name, uintptr_t row, uintptr_t option, uintptr_t caller_sp, uintptr_t index) {
    RecallCandidates candidates {};
    uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (recall_ready.load(std::memory_order_acquire) && ptr(caller_sp) && row == caller_sp + 0xa0 &&
        index < AZ_NATIVE_CUE_LIMIT) {
        for (unsigned n = 0; n < 4; ++n)
            if (caller == recall_bias + recall_profile->calls[n]) {
                if (!typed(pack, profile.pack)) break;
                uintptr_t content = object_word(caller_sp + (n < 2 ? 0x78 : 0x90));
                for (unsigned d = 0; d < 4; ++d) candidates.deck[d] = selected_edition(d, pack, content, unsigned(index) + 1);
                break;
            }
    }
    bool selected = false;
    for (const auto &deck : candidates.deck) selected |= deck.entry != 0;
    RecallScope<const RecallCandidates *> scope(selection, selected ? &candidates : nullptr);
    reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(recall_original[0])(
        manager, track, pack, name, row, option);
}
#if defined(__aarch64__)
/* The four audited callers keep the actual row index in x22, pass the copied
 * row at sp+0xa0 in x4 and keep the originating intrusive reference on stack.
 * x0..x5 and the original LR are forwarded unchanged. */
extern "C" void azn_recall_accept_entry();
__asm__(".text\n.p2align 2\n.hidden azn_recall_accept_entry\n.type azn_recall_accept_entry,%function\n"
        "azn_recall_accept_entry:\nmov x6,sp\nmov x7,x22\nb azn_recall_accept\n.size azn_recall_accept_entry,.-azn_recall_accept_entry\n");
#endif
struct RecallJob {
    std::atomic<uintptr_t> task {0};
    uintptr_t facade = 0;
    uint64_t sequence = 0;
    RecallCandidates candidates {};
};
static RecallJob recall_jobs[32];
static bool consume_job(uintptr_t task, uint64_t seq, uintptr_t facade, RecallCandidates *out) {
    for (auto &job : recall_jobs) {
        uintptr_t match = task;
        if (!job.task.compare_exchange_strong(match, UINTPTR_MAX, std::memory_order_acq_rel)) continue;
        const bool valid = job.sequence == seq && job.facade == facade;
        if (valid && out) *out = job.candidates;
        job.task.store(valid ? 0 : task, std::memory_order_release);
        if (valid) return true;
    }
    return false;
}
static void seal_job(uintptr_t task, const RecallFrame &frame) {
    for (auto &job : recall_jobs) {
        uintptr_t empty = 0;
        if (!job.task.compare_exchange_strong(empty, UINTPTR_MAX, std::memory_order_acq_rel)) continue;
        job.sequence = object_word(task + 0x10);
        job.facade = frame.facade;
        job.candidates = *frame.candidates;
        job.task.store(task, std::memory_order_release);
        return;
    }
    /* A later working copy without its task token explicitly withdraws any
     * older association. Never wait or overwrite a still-executable task. */
}
static void memory_call(uintptr_t facade, unsigned kind, uintptr_t property) {
    RecallFrame frame {};
    auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (recall_ready.load(std::memory_order_acquire) && selection && !kind &&
        caller == recall_bias + recall_profile->dispatcher && typed(facade, recall_bias + recall_profile->facade_table)) {
        frame.candidates = selection;
        frame.facade = facade;
        frame.property = property;
    }
    RecallScope<RecallFrame *> scope(recall_frame, frame.candidates ? &frame : nullptr);
    reinterpret_cast<void (*)(uintptr_t, unsigned, uintptr_t)>(recall_original[1])(facade, kind, property);
}
static uintptr_t recall_ctor(uintptr_t dst, uintptr_t src) {
    auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    uintptr_t result = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(recall_original[2])(dst, src);
    auto f = recall_frame;
    if (!f) return result;
    const auto *pc = recall_profile->construct;
    if (caller == recall_bias + pc[0] && src == f->property) f->first = dst;
    else if (caller == recall_bias + pc[1] && src == f->first) f->owned = dst;
    else if (caller == recall_bias + pc[2] && src == f->owned && dst >= 0x10028) {
        uintptr_t task = dst - 0x28;
        if (typed(task, recall_bias + recall_profile->task_table) && object_word(task + 0x18) == f->facade &&
            scalar<unsigned>(task + 0x20) == 0 && object_word(task + 0x10)) seal_job(task, *f);
    }
    return result;
}
static void recall_execute(uintptr_t task) {
    RecallCandidates captured {};
    RecallFrame frame {};
    uintptr_t facade = object_word(task + 0x18);
    if (recall_ready.load(std::memory_order_acquire) && typed(task, recall_bias + recall_profile->task_table) &&
        !scalar<unsigned>(task + 0x20) && consume_job(task, object_word(task + 0x10), facade, &captured)) {
        frame.candidates = &captured;
        frame.facade = facade;
        frame.owned = task + 0x28;
    }
    RecallScope<RecallFrame *> scope(recall_frame, frame.candidates ? &frame : nullptr);
    reinterpret_cast<void (*)(uintptr_t)>(recall_original[3])(task);
}
template <unsigned I>
static uintptr_t recall_destroy(uintptr_t task) {
    if (recall_ready.load(std::memory_order_acquire))
        consume_job(task, object_word(task + 0x10), object_word(task + 0x18), nullptr);
    return reinterpret_cast<uintptr_t (*)(uintptr_t)>(recall_original[I])(task);
}
static void install_recall(int build, uintptr_t bias, azexit_install_fn install) {
#if defined(__aarch64__)
    recall_profile = &recall_profiles[build - 1];
    recall_bias = bias;
    void *hooks[] = {reinterpret_cast<void *>(azn_recall_accept_entry), reinterpret_cast<void *>(memory_call),
                     reinterpret_cast<void *>(recall_ctor), reinterpret_cast<void *>(recall_execute),
                     reinterpret_cast<void *>(recall_destroy<4>), reinterpret_cast<void *>(recall_destroy<5>)};
    for (const auto &entry : recall_profile->entries)
        if (std::memcmp(reinterpret_cast<const void *>(bias + entry.address), entry.prologue, sizeof entry.prologue)) return;
    for (unsigned i = 0; i < 6; ++i)
        if (!install(bias + recall_profile->entries[i].address, hooks[i], &recall_original[i]) || !recall_original[i]) return;
    recall_ready.store(true, std::memory_order_release);
#else
    (void) build;
    (void) bias;
    (void) install;
#endif
}
