/* SPDX-License-Identifier: GPL-3.0-only */
/* BeatSyncFunction commits its elected member, target beat notification and
 * audible sync job separately. +0x170 is the tempo slider, never this role.
 * The job is owned by the selected SlaveFunction for the whole writer call. */
struct SyncProfile {
    uintptr_t master, slave, server, job, period, election, beat;
};
static const SyncProfile sync_profiles[]={
    {0x288c240,0x288cb70,0x288d610,0x288cbf0,0x13e0f08,0x13df008,0x13de990},
    {0x2bde660,0x2bdef90,0x2bdfa40,0x2bdf010,0x149b588,0x149a040,0x1499a68}
};
struct SyncValues { int following=-1, locked=-1; };
static SyncValues sync_values(uintptr_t parent,const SyncProfile &p,uintptr_t bias) noexcept {
    SyncValues v;
    unsigned role=integer(parent+0xbc),enabled=byte(parent+0xb8),member=integer(parent+0xc0);
    if (role>1 || enabled>1 || !table(word(parent+0xb0),bias+p.server)) return v;
    uintptr_t child=word(parent+0x1c8+role*8);
    if (!table(child,bias+(role ? p.slave:p.master)) || word(child+8)!=parent) return v;
    if (!role || !enabled || member==0xff) {v.following=v.locked=0;return v;}
    uintptr_t job=word(child+0xb0);
    if (member>=70 || child!=word(parent+0x1c0) || !table(job,bias+p.job) || word(job+0x30)!=child) return v;
    double target;
    memcpy(&target,reinterpret_cast<const void *>(parent+0x120),sizeof target);
    unsigned active=integer(child+0x10),linked=byte(parent+0x118),timing=byte(parent+0x119);
    if (active>1 || linked>1 || timing>1 || !std::isfinite(target) || target<0) return v;
    /* The slave's tempo job selected the remote tempo calculator, received a
     * usable target and committed its rate. The phase job can still be idle. */
    if (!active || !linked || target<=0 || target==std::numeric_limits<double>::max()) {v.following=v.locked=0;return v;}
    v.following=1;
    unsigned phase=integer(child+0x48);
    if (phase<=2) v.locked=phase==2 && timing && unsigned(integer(parent+0x158))==member &&
        byte(parent+0x11a)==byte(parent+0x154);
    return v;
}
struct SyncRole {
    std::atomic<unsigned> writers{0};
    std::atomic<uint64_t> fault{0},serial{0};
    std::atomic_flag publish=ATOMIC_FLAG_INIT;
    uint64_t owner=0,seed=0,fault_seen=0;
    int following=-2,locked=-2;
};
static SyncRole sync_roles[4];
static void *sync_original[3];
static unsigned sync_hooks;
static std::atomic<bool> sync_installed{false};
static void sync_role_gap(unsigned deck) noexcept {
    sync_roles[deck].fault.fetch_add(1,std::memory_order_acq_rel);
    azn_sync_role_gap(deck);
}
static void sync_status_retire(unsigned d,uint64_t owner) noexcept {
    if (!owner || !sync_installed.load(std::memory_order_acquire)) return;
    azstatus_native_receipt r{};
    r.kind=AZN_SYNC_ROLE;r.deck=d;r.unit=owner;r.valid=-1;
    azn_emit(&r);
    azn_capability(d,AZN_CAP_SYNC_ROLE,0);
}
static void sync_capture(unsigned d,uintptr_t parent,uint64_t owner) noexcept {
    auto &role=sync_roles[d];
    auto &setting=state().decks[d].settings[2];
    if (role.publish.test_and_set(std::memory_order_acquire)) {sync_role_gap(d);return;}
    struct Release { std::atomic_flag &flag; ~Release(){flag.clear(std::memory_order_release);} } release{role.publish};
    uint64_t fault=role.fault.load(std::memory_order_acquire),seed=azn_seed_id(d);
    uint64_t serial=role.serial.load(std::memory_order_acquire);
    if (!owner || role.writers.load(std::memory_order_acquire) || setting.provider.load(std::memory_order_acquire)!=owner ||
        setting.address.load(std::memory_order_acquire)!=parent) return;
    State &s=state();
    if (!table(parent,s.bias+setting_profiles[s.build-1].sync_function)) return;
    SyncValues v=sync_values(parent,sync_profiles[s.build-1],s.bias);
    if (role.serial.load(std::memory_order_acquire)!=serial || role.fault.load(std::memory_order_acquire)!=fault || role.writers.load(std::memory_order_acquire) ||
        setting.provider.load(std::memory_order_acquire)!=owner || setting.address.load(std::memory_order_acquire)!=parent) return;
    if (role.owner==owner && role.seed==seed && role.fault_seen==fault &&
        role.following==v.following && role.locked==v.locked) return;
    azstatus_native_receipt r{};
    r.kind=AZN_SYNC_ROLE;r.deck=d;r.unit=owner;r.valid=1;r.a=v.following;r.b=v.locked;
    if (azn_offer(&r)) {
        role.owner=owner;role.seed=seed;role.fault_seen=fault;
        role.following=v.following;role.locked=v.locked;
    }
}
struct SyncRoleWriter;
static thread_local SyncRoleWriter *sync_writer_frame;
struct SyncRoleWriter {
    SyncRoleWriter *previous=nullptr;
    unsigned deck=4;uintptr_t parent=0;uint64_t owner=0;
    explicit SyncRoleWriter(uintptr_t address) noexcept {
        if (!address || !sync_installed.load(std::memory_order_acquire)) return;
        for (unsigned d=0;d<4;d++) {
            auto &setting=state().decks[d].settings[2];
            if (setting.address.load(std::memory_order_acquire)!=address || !(azn_interest(d)&AZ_STATUS_TOPIC(AZ_STATUS_SYNC))) continue;
            for (auto *f=sync_writer_frame;f;f=f->previous) if (f->parent==address) return;
            deck=d;parent=address;owner=setting.provider.load(std::memory_order_acquire);
            previous=sync_writer_frame;sync_writer_frame=this;
            sync_roles[d].serial.fetch_add(1,std::memory_order_acq_rel);
            if (sync_roles[d].writers.fetch_add(1,std::memory_order_acq_rel)) sync_role_gap(d);
            break;
        }
    }
    ~SyncRoleWriter() {
        if (deck<4) {
            sync_writer_frame=previous;
            sync_roles[deck].serial.fetch_add(1,std::memory_order_release);
            if (sync_roles[deck].writers.fetch_sub(1,std::memory_order_acq_rel)==1) sync_capture(deck,parent,owner);
        }
    }
};
template<unsigned I,class... Args> static void sync_writer(void *self,Args... args) {
    SyncRoleWriter frame(reinterpret_cast<uintptr_t>(self));
    trampoline<void (*)(void *,Args...)>(&sync_original[I])(self,args...);
}
static void sync_role_install(State &s,int build,azn_setting_patch_fn patch) {
    const SyncProfile &p=sync_profiles[build-1];
    const uintptr_t sites[]={p.period,p.election,p.beat};
    const uint32_t signature[][2]={{0xa9be7bfd,0x910003fd},{0xa9bd7bfd,0x72001c5f},
        {0xb940bc02,build==1 ? 0x34000922u:0x34000902u}};
    void *hooks[]={reinterpret_cast<void *>(sync_writer<0>),reinterpret_cast<void *>(sync_writer<1,unsigned,unsigned>),
        reinterpret_cast<void *>(sync_writer<2,const void *>)};
    for (unsigned i=0;i<3;++i) {
        uintptr_t address=s.bias+sites[i];
        if (!(sync_hooks&(1u<<i)) && !memcmp(reinterpret_cast<const void *>(address),signature[i],8) &&
            patch(address,hooks[i],&sync_original[i],"status-sync-role")==1) sync_hooks|=1u<<i;
    }
    if (sync_hooks==7 && (setting_installed.load(std::memory_order_acquire)&4)) sync_installed.store(true,std::memory_order_release);
}
