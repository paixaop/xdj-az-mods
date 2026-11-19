/* SPDX-License-Identifier: GPL-3.0-only */
/* BeatSyncFunction commits its elected member, target beat notification and
 * audible sync job separately. +0x170 is the tempo slider, never this role.
 * The job is owned by the selected SlaveFunction for the whole writer call. */
struct SyncProfile {
    uintptr_t master, slave, server, job, period, election, beat;
};
static const SyncProfile sync_profiles[] = {
    {0x288c240, 0x288cb70, 0x288d610, 0x288cbf0, 0x13e0f08, 0x13df008, 0x13de990},
    {0x2bde660, 0x2bdef90, 0x2bdfa40, 0x2bdf010, 0x149b588, 0x149a040, 0x1499a68}};
struct SyncValues {
    int following;
    int locked;
};
static uintptr_t sync_child(uintptr_t parent, const struct SyncProfile *p, uintptr_t bias, unsigned role, unsigned enabled) {
    if (role > 1 || enabled > 1 || !table(word(parent + 0xb0), bias + p->server)) return 0;
    uintptr_t child = word(parent + 0x1c8 + role * 8);
    if (!table(child, bias + (role ? p->slave : p->master)) || word(child + 8) != parent) return 0;
    return child;
}
static bool sync_slave_job(uintptr_t parent, uintptr_t child, unsigned member, const struct SyncProfile *p, uintptr_t bias) {
    uintptr_t job = word(child + 0xb0);
    return member < 70 && child == word(parent + 0x1c0) && table(job, bias + p->job) && word(job + 0x30) == child;
}
static bool sync_target_valid(unsigned active, unsigned linked, unsigned timing, double target) {
    return active <= 1 && linked <= 1 && timing <= 1 && isfinite(target) && target >= 0;
}
static int sync_phase_locked(uintptr_t parent, uintptr_t child, unsigned timing, unsigned member) {
    unsigned phase = integer(child + 0x48);
    if (phase > 2) return -1;
    return phase == 2 && timing && (unsigned) integer(parent + 0x158) == member && byte(parent + 0x11a) == byte(parent + 0x154);
}
static SyncValues sync_audible_values(uintptr_t parent, uintptr_t child, unsigned member) {
    SyncValues v = {.following = -1, .locked = -1};
    double target;
    memcpy(&target, (const void *) (parent + 0x120), sizeof target);
    unsigned active = integer(child + 0x10), linked = byte(parent + 0x118), timing = byte(parent + 0x119);
    if (!sync_target_valid(active, linked, timing, target)) return v;
    /* The slave's tempo job selected the remote tempo calculator, received a
     * usable target and committed its rate. The phase job can still be idle. */
    if (!active || !linked || target <= 0 || target == DBL_MAX) return (SyncValues) {0, 0};
    v.following = 1;
    v.locked = sync_phase_locked(parent, child, timing, member);
    return v;
}
static SyncValues sync_values(uintptr_t parent, const SyncProfile p, uintptr_t bias) {
    SyncValues unknown = {.following = -1, .locked = -1};
    unsigned role = integer(parent + 0xbc), enabled = byte(parent + 0xb8), member = integer(parent + 0xc0);
    uintptr_t child = sync_child(parent, &p, bias, role, enabled);
    if (!child) return unknown;
    if (!role || !enabled || member == 0xff) return (SyncValues) {0, 0};
    if (!sync_slave_job(parent, child, member, &p, bias)) return unknown;
    return sync_audible_values(parent, child, member);
}
struct SyncRole {
    _Atomic(unsigned) writers;
    _Atomic(uint64_t) fault;
    _Atomic(uint64_t) serial;
    atomic_flag publish;
    uint64_t owner;
    uint64_t seed;
    uint64_t fault_seen;
    int following;
    int locked;
};
static SyncRole sync_roles[4] = {[0 ... 3] = {.writers = 0, .fault = 0, .serial = 0, .publish = ATOMIC_FLAG_INIT, .owner = 0, .seed = 0, .fault_seen = 0, .following = -2, .locked = -2}};
static void *sync_original[3];
static unsigned sync_hooks;
static _Atomic(bool) sync_installed = false;
static void sync_role_gap(unsigned deck) {
    atomic_fetch_add_explicit(&(sync_roles[deck].fault), 1, memory_order_acq_rel);
    azn_sync_role_gap(deck);
}
static void sync_status_retire(unsigned d, uint64_t owner) {
    if (!owner || !atomic_load_explicit(&(sync_installed), memory_order_acquire)) return;
    azstatus_native_receipt r = {0};
    r.kind = AZN_SYNC_ROLE;
    r.deck = d;
    r.unit = owner;
    r.valid = -1;
    azn_emit(&r);
    azn_capability(d, AZN_CAP_SYNC_ROLE, 0);
}
static bool sync_owner_current(const struct SyncRole *role, const Setting *setting, uintptr_t parent, uint64_t owner) {
    return owner && !atomic_load_explicit(&(role->writers), memory_order_acquire) &&
           atomic_load_explicit(&(setting->provider), memory_order_acquire) == owner &&
           atomic_load_explicit(&(setting->address), memory_order_acquire) == parent;
}
static bool sync_capture_current(const struct SyncRole *role, const Setting *setting, uintptr_t parent,
                                 uint64_t owner, uint64_t serial, uint64_t fault) {
    return atomic_load_explicit(&(role->serial), memory_order_acquire) == serial &&
           atomic_load_explicit(&(role->fault), memory_order_acquire) == fault &&
           !atomic_load_explicit(&(role->writers), memory_order_acquire) &&
           atomic_load_explicit(&(setting->provider), memory_order_acquire) == owner &&
           atomic_load_explicit(&(setting->address), memory_order_acquire) == parent;
}
static bool sync_values_unchanged(const struct SyncRole *role, uint64_t owner, uint64_t seed, uint64_t fault, const SyncValues *value) {
    return role->owner == owner && role->seed == seed && role->fault_seen == fault &&
           role->following == value->following && role->locked == value->locked;
}
static void sync_capture(unsigned d, uintptr_t parent, uint64_t owner) {
    __auto_type role = &(sync_roles[d]);
    __auto_type setting = &(state().decks[d].settings[2]);
    if (atomic_flag_test_and_set_explicit(&(role->publish), memory_order_acquire)) {
        sync_role_gap(d);
        return;
    }
    EXIT_FLAG_GUARD(release, &role->publish);
    uint64_t fault = atomic_load_explicit(&(role->fault), memory_order_acquire), seed = azn_seed_id(d);
    uint64_t serial = atomic_load_explicit(&(role->serial), memory_order_acquire);
    if (!sync_owner_current(role, setting, parent, owner)) return;
    State *s = &(state());
    if (!table(parent, s->bias + setting_profiles[s->build - 1].sync_function)) return;
    SyncValues v = sync_values(parent, sync_profiles[s->build - 1], s->bias);
    if (!sync_capture_current(role, setting, parent, owner, serial, fault)) return;
    if (sync_values_unchanged(role, owner, seed, fault, &v)) return;
    azstatus_native_receipt r = {0};
    r.kind = AZN_SYNC_ROLE;
    r.deck = d;
    r.unit = owner;
    r.valid = 1;
    r.a = v.following;
    r.b = v.locked;
    if (azn_offer(&r)) {
        role->owner = owner;
        role->seed = seed;
        role->fault_seen = fault;
        role->following = v.following;
        role->locked = v.locked;
    }
}
struct SyncRoleWriter;
static _Thread_local SyncRoleWriter *sync_writer_frame;
typedef struct SyncRoleWriter SyncRoleWriter;
struct SyncRoleWriter {
    SyncRoleWriter *previous;
    unsigned deck;
    uintptr_t parent;
    uint64_t owner;
};
static void sync_role_writer_init(SyncRoleWriter *ctx, uintptr_t address);
static void sync_role_writer_drop(SyncRoleWriter *ctx);
static void sync_role_writer_init(SyncRoleWriter *ctx, uintptr_t address) {
    (void) ctx;
    *ctx = (SyncRoleWriter) {.previous = NULL, .deck = 4, .parent = 0, .owner = 0};

    if (!address || !atomic_load_explicit(&(sync_installed), memory_order_acquire)) return;
    for (unsigned d = 0; d < 4; d++) {
        __auto_type setting = &(state().decks[d].settings[2]);
        if (atomic_load_explicit(&(setting->address), memory_order_acquire) != address || !(azn_interest(d) & AZ_STATUS_TOPIC(AZ_STATUS_SYNC))) continue;
        for (__auto_type f = sync_writer_frame; f; f = f->previous)
            if (f->parent == address) return;
        ctx->deck = d;
        ctx->parent = address;
        ctx->owner = atomic_load_explicit(&(setting->provider), memory_order_acquire);
        ctx->previous = sync_writer_frame;
        sync_writer_frame = ctx;
        atomic_fetch_add_explicit(&(sync_roles[d].serial), 1, memory_order_acq_rel);
        if (atomic_fetch_add_explicit(&(sync_roles[d].writers), 1, memory_order_acq_rel)) sync_role_gap(d);
        break;
    }
}
static void sync_role_writer_drop(SyncRoleWriter *ctx) {
    (void) ctx;

    if (ctx->deck < 4) {
        sync_writer_frame = ctx->previous;
        atomic_fetch_add_explicit(&(sync_roles[ctx->deck].serial), 1, memory_order_release);
        if (atomic_fetch_sub_explicit(&(sync_roles[ctx->deck].writers), 1, memory_order_acq_rel) == 1) sync_capture(ctx->deck, ctx->parent, ctx->owner);
    }
}

static void sync_writer_0(void *self) {
    SyncRoleWriter frame __attribute__((cleanup(sync_role_writer_drop)));
    sync_role_writer_init(&frame, ((uintptr_t) (self)));
    ((void (*)(void *)) trampoline_address(&sync_original[0]))(self);
}
static void sync_writer_1(void *self, unsigned arg0, unsigned arg1) {
    SyncRoleWriter frame __attribute__((cleanup(sync_role_writer_drop)));
    sync_role_writer_init(&frame, ((uintptr_t) (self)));
    ((void (*)(void *, unsigned, unsigned)) trampoline_address(&sync_original[1]))(self, arg0, arg1);
}
static void sync_writer_2(void *self, const void *arg0) {
    SyncRoleWriter frame __attribute__((cleanup(sync_role_writer_drop)));
    sync_role_writer_init(&frame, ((uintptr_t) (self)));
    ((void (*)(void *, const void *)) trampoline_address(&sync_original[2]))(self, arg0);
}
static void sync_role_install(State *s, int build, azn_setting_patch_fn patch) {
    const SyncProfile *p = &(sync_profiles[build - 1]);
    const uintptr_t sites[] = {p->period, p->election, p->beat};
    const uint32_t signature[][2] = {{0xa9be7bfd, 0x910003fd}, {0xa9bd7bfd, 0x72001c5f}, {0xb940bc02, build == 1 ? 0x34000922u : 0x34000902u}};
    void *hooks[] = {((void *) (sync_writer_0)), ((void *) (sync_writer_1)),
                     ((void *) (sync_writer_2))};
    for (unsigned i = 0; i < 3; ++i) {
        uintptr_t address = s->bias + sites[i];
        if (!(sync_hooks & (1u << i)) && !memcmp(((const void *) (address)), signature[i], 8) &&
            patch(address, hooks[i], &sync_original[i], "status-sync-role") == 1) sync_hooks |= 1u << i;
    }
    if (sync_hooks == 7 && (atomic_load_explicit(&(setting_installed), memory_order_acquire) & 4)) atomic_store_explicit(&(sync_installed), true, memory_order_release);
}
