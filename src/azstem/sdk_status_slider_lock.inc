/* SPDX-License-Identifier: GPL-3.0-only */
/* The stock PlayerState writer booleanizes BeatSyncFunction +0x170 into
 * payload +0x74 (PlayerState +0x8c), the named TempoSliderLocked field.
 * The constructor initializes it before unit registration. Later stores
 * belong to these six methods or setSyncMode. */
static void *slider_lock_original[6];
static unsigned slider_lock_hooks;
struct SliderLockWrite;
static thread_local SliderLockWrite *active_slider_write;
struct SliderLockWrite {
    Slot::Setting *setting=nullptr;
    SliderLockWrite *previous=nullptr;
    uintptr_t object=0;
    uint64_t owner=0,fault=0;
    unsigned deck=0;
    bool acquired=false;
    explicit SliderLockWrite(uintptr_t self):object(self) {
        if (!self)return;
        for(unsigned d=0;d<4;d++) {
            auto &v=state().decks[d].settings[3];
            if(v.address.load(std::memory_order_acquire)!=self)continue;
            previous=active_slider_write;
            if(previous && previous->object==self && previous->setting==&v)previous->commit();
            else if(v.busy.test_and_set(std::memory_order_acquire)) {
                v.fault.fetch_add(1,std::memory_order_acq_rel);
                azn_setting_gap(d,AZN_SLIDER_LOCK);return;
            } else acquired=true;
            setting=&v;deck=d;
            owner=v.provider.load(std::memory_order_acquire);
            fault=v.fault.load(std::memory_order_acquire);
            active_slider_write=this;
            return;
        }
    }
    void commit() {
        if (!setting || !owner || setting->address.load(std::memory_order_acquire)!=object ||
            setting->provider.load(std::memory_order_acquire)!=owner || setting->fault.load(std::memory_order_acquire)!=fault)return;
        unsigned value=integer(object+0x170)!=0;
        if(setting->last==value)return;
        setting->last=value;
        setting_receipt(deck,3,owner,value,1);
    }
    ~SliderLockWrite() {
        if(setting){
            commit();
            active_slider_write=previous;
            if(acquired)setting->busy.clear(std::memory_order_release);
        }
    }
};
template<unsigned I,class... Args>
static void slider_lock_writer(void *self,Args... args) {
    SliderLockWrite capture(reinterpret_cast<uintptr_t>(self));
    trampoline<void(*)(void *,Args...)>(&slider_lock_original[I])(self,args...);
}
static void slider_lock_install(State &s,int build,azn_setting_patch_fn patch) {
    const uintptr_t sites[][6]={{0x13def18,0x13dfd28,0x13e0238,0x13e0290,0x13e02c8,0x13e08a0},
                                {0x1499f50,0x149ad60,0x149b2b0,0x149b308,0x149b340,0x149c3e0}};
    const uint32_t signature[][2]={{0xf9404401,0x39418821},{0xf940e002,0x7901881f},{0xa9be7bfd,0x910003fd},
        {0xa9be7bfd,0x910003fd},{0xa9bc7bfd,0xd0ffffa1},{0xa9be7bfd,0x72001c3f}};
    void *hooks[]={reinterpret_cast<void *>(slider_lock_writer<0>),reinterpret_cast<void *>(slider_lock_writer<1>),
        reinterpret_cast<void *>(slider_lock_writer<2>),reinterpret_cast<void *>(slider_lock_writer<3>),
        reinterpret_cast<void *>(slider_lock_writer<4>),reinterpret_cast<void *>(slider_lock_writer<5,unsigned,unsigned,unsigned>)};
    for(unsigned i=0;i<6;i++) {
        uintptr_t address=s.bias+sites[build-1][i];
        if (!(slider_lock_hooks&(1u<<i)) && !memcmp(reinterpret_cast<const void *>(address),signature[i],8) &&
            patch(address,hooks[i],&slider_lock_original[i],"status-slider-lock")==1)slider_lock_hooks|=1u<<i;
    }
    if(slider_lock_hooks==63 && (setting_installed.load(std::memory_order_acquire)&4))
        setting_installed.fetch_or(8,std::memory_order_release);
}
