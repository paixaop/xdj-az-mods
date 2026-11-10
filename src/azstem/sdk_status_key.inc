/* SPDX-License-Identifier: GPL-3.0-only */
/* The KeyFunction child (verified by both facades) owns master-tempo at +8
 * and the semitone adjustment at +24. Read the latter's committed ratio, so
 * rejected requests, including a refused out-of-range combination, stay quiet. */
struct KeyProjection {
    std::atomic<uintptr_t> address {0};
    std::atomic<uint64_t> provider {0}, fault {0};
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    uint64_t sampled_seed=0, sampled_load=0;
    int last=INT32_MAX;
};
static KeyProjection key_projection[4];
static std::atomic<unsigned> key_hooked {0};
static std::atomic<uint64_t> key_incarnations {0};
static void *key_original;

/* Exact positive-octave mantissas in both admitted firmware tables. The
 * setter uses the same mantissas one exponent lower for negative shifts. */
static int key_semitones(uint64_t ratio) noexcept {
    static const uint64_t within_octave[]={
        UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0f38f92d97963), UINT64_C(0x3ff1f59ac3c7d6c0),
        UINT64_C(0x3ff306fe0a31b716), UINT64_C(0x3ff428a2f98d728c), UINT64_C(0x3ff55b8108f0ec5f),
        UINT64_C(0x3ff6a09e667f3bce), UINT64_C(0x3ff7f910d768cfb2), UINT64_C(0x3ff965fea53d6e3f),
        UINT64_C(0x3ffae89f995ad3b0), UINT64_C(0x3ffc823e074ec12c), UINT64_C(0x3ffe3437e7101347)
    };
    if (ratio==UINT64_C(0x4000000000000000)) return 12;
    for (int n=0;n<12;++n) {
        if (ratio==within_octave[n]) return n;
        if (ratio==within_octave[n]-(UINT64_C(1)<<52)) return n-12;
    }
    return INT32_MAX;
}
static bool key_sample(unsigned deck,KeyProjection &p,uintptr_t address,uint64_t owner,
                       const azsdk_load_sample &before,uint64_t fault,bool seed) noexcept {
    if (!owner || !before.load_generation || p.address.load(std::memory_order_acquire)!=address ||
        p.provider.load(std::memory_order_acquire)!=owner || p.fault.load(std::memory_order_acquire)!=fault) return false;
    const int step=key_semitones(word(address+8));
    azsdk_load_sample after {};
    if (!azsdk_load_read(int(deck), &after) || before.load_generation!=after.load_generation ||
        before.unit_generation!=after.unit_generation || before.invalidation_epoch!=after.invalidation_epoch ||
        p.fault.load(std::memory_order_acquire)!=fault || p.provider.load(std::memory_order_acquire)!=owner ||
        p.address.load(std::memory_order_acquire)!=address) return false;
    if (seed || p.sampled_load!=after.load_generation || p.last!=step) {
        azstatus_native_receipt r {};
        r.deck=deck; r.kind=AZN_KEY_SHIFT; r.unit=after.unit_generation; r.epoch=after.invalidation_epoch;
        r.load_generation=after.load_generation; r.attempt=owner; r.a=step; r.valid=step!=INT32_MAX;
        if (!azn_offer(&r)) return false;
    }
    p.sampled_load=after.load_generation;
    p.last=step;
    return true;
}
static void key_retire(unsigned deck) noexcept {
    auto &p=key_projection[deck];
    p.address.store(0,std::memory_order_release);
    uint64_t owner=p.provider.exchange(0,std::memory_order_acq_rel);
    if (owner) {
        azsdk_load_sample load {};
        if (azsdk_load_read(int(deck),&load)) {
            azstatus_native_receipt r {};
            r.deck=deck; r.kind=AZN_KEY_SHIFT; r.valid=-1; r.attempt=owner;
            r.unit=load.unit_generation; r.epoch=load.invalidation_epoch; r.load_generation=load.load_generation;
            azn_emit(&r);
        }
        azn_capability(deck,AZN_CAP_KEY_SHIFT,0);
    }
}
static void key_frame(unsigned deck,Slot &slot) noexcept {
    if (!key_hooked.load(std::memory_order_acquire) || !(azn_interest(deck)&AZ_STATUS_TOPIC(AZ_STATUS_KEY))) return;
    uintptr_t mt=setting_object(slot,1), address=mt?mt+16:0;
    auto &p=key_projection[deck];
    if (p.busy.test_and_set(std::memory_order_acquire)) return;
    uint64_t fault=p.fault.load(std::memory_order_acquire);
    if (address!=p.address.load(std::memory_order_acquire)) {
        key_retire(deck);
        p.sampled_load=p.sampled_seed=0; p.last=INT32_MAX;
        if (address) {
            uint64_t n=key_incarnations.fetch_add(1,std::memory_order_relaxed)+1;
            if (n && n<=AZSP_SERIAL_MAX) {
                p.provider.store(AZSP_KEY|n,std::memory_order_release);
                p.address.store(address,std::memory_order_release);
                azn_capability(deck,AZN_CAP_KEY_SHIFT,1);
            }
        }
    }
    azsdk_load_sample live {};
    uint64_t seed=azn_seed_id(deck);
    if (address && azsdk_load_read(int(deck),&live) && (p.sampled_seed!=seed || p.sampled_load!=live.load_generation)) {
        if (key_sample(deck,p,address,p.provider.load(std::memory_order_relaxed),live,fault,true))
            p.sampled_seed=seed;
    }
    p.busy.clear(std::memory_order_release);
}
struct KeyCapture {
    uintptr_t address;
    KeyProjection *owner=nullptr;
    unsigned deck=0;
    uint64_t provider=0, fault=0;
    azsdk_load_sample before {};
    explicit KeyCapture(uintptr_t target) noexcept : address(target) {
        if (!target || !key_hooked.load(std::memory_order_acquire)) return;
        for (unsigned d=0;d<4;++d) {
            auto &p=key_projection[d];
            if (p.address.load(std::memory_order_acquire)!=target) continue;
            if (p.busy.test_and_set(std::memory_order_acquire)) {
                p.fault.fetch_add(1,std::memory_order_acq_rel);
                azn_key_gap(d);
                break;
            }
            owner=&p; deck=d; provider=p.provider.load(std::memory_order_acquire);
            fault=p.fault.load(std::memory_order_acquire);
            azsdk_load_read(int(d),&before);
            break;
        }
    }
    void finish() {
        if (owner) key_sample(deck,*owner,address,provider,before,fault,false);
    }
    ~KeyCapture() { if (owner) owner->busy.clear(std::memory_order_release); }
};
static void key_writer(void *self,int32_t semitones,unsigned mode) {
    auto original=trampoline<void (*)(void *,int32_t,unsigned)>(&key_original);
    KeyCapture capture(reinterpret_cast<uintptr_t>(self));
    original(self,semitones,mode);
    capture.finish();
}
static void key_install(State &s,int build,azn_setting_patch_fn patch) {
    if (key_hooked.load(std::memory_order_acquire)) return;
    static const uint32_t signatures[2][4]={
        {0xa9b67bfd,0x910003fd,0xa90153f3,0xaa0003f4}, {0xa9b67bfd,0x910003fd,0xa90153f3,0xa9025bf5}};
    uintptr_t target=s.bias+(build==1?0x141bb08:0x14d6ef8);
    if (!memcmp(reinterpret_cast<const void*>(target),signatures[build-1],sizeof signatures[0]) &&
        patch(target,reinterpret_cast<void *>(key_writer),&key_original,"status-key-shift")==1 && key_original)
        key_hooked.store(1,std::memory_order_release);
}
