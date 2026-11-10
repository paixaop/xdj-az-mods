/* SPDX-License-Identifier: GPL-3.0-only */
/* The StateSetFacade and KeyControlFacade share the UnitFunctionSet children.
 * The stored addresses are used for comparison only outside an owner frame.
 * The setter itself owns its first argument for the native call. */
struct SettingProfile {
    uintptr_t state_facade,key_facade,key_function,sync_facade,sync_function,writer[3];
};
static const SettingProfile setting_profiles[]={
    {0x287e648,0x287b870,0x2892610,0x2875490,0x288d690,{0x144d240,0x1414558,0x13e06f0}},
    {0x2bd0a68,0x2bcdab0,0x2be4b10,0x2bc76d8,0x2bdfac0,{0x1508838,0x14cf810,0x149c230}}
};
static void *setting_original[3];
static std::atomic<unsigned> setting_installed {0};
static std::atomic<uint64_t> setting_incarnation {0};
static const unsigned setting_kinds[]={AZN_QUANTIZE,AZN_KEY_LOCK,AZN_SYNC,AZN_SLIDER_LOCK};
static const unsigned setting_caps[]={AZN_CAP_QUANTIZE,AZN_CAP_KEY_LOCK,AZN_CAP_SYNC,AZN_CAP_SLIDER_LOCK};

static void setting_receipt(unsigned deck,unsigned index,uint64_t owner,unsigned value,int valid,int raw=-1) noexcept {
    azstatus_native_receipt r {};
    r.deck=deck; r.unit=owner; r.kind=setting_kinds[index];
    r.a=int(value); r.b=raw; r.valid=valid;
    azn_emit(&r);
}
static uintptr_t setting_object(Slot &slot,unsigned i) noexcept {
    State &s=state();
    if (s.build<1 || s.build>2 || !coherent(slot)) return 0;
    const SettingProfile &p=setting_profiles[s.build-1];
    uintptr_t bundle=word(slot.unit+0x190);
    uintptr_t state_facade=word(bundle+0x30);
    if (!table(state_facade,s.bias+p.state_facade)) return 0;
    uintptr_t functions=word(slot.unit+0x188);
    if (!pointer(functions)) return 0;
    if (!i) {
        uintptr_t q=word(state_facade+0x48);
        return q==word(functions+0x20) && q==word(slot.facade+0x50) &&
            table(q,s.profile.quantize) ? q:0;
    }
    if (i>=2) {
        uintptr_t sync=word(state_facade+0x68), facade=word(bundle+0x60);
        return table(facade,s.bias+p.sync_facade) && sync==word(facade+0x68) && sync==word(functions+0x78) &&
            table(sync,s.bias+p.sync_function) ? sync:0;
    }
    uintptr_t key=word(state_facade+0x78), facade=word(bundle+0x48);
    return table(facade,s.bias+p.key_facade) && key==word(facade+0x78) &&
        table(key,s.bias+p.key_function) ? key+8:0;
}
#include "sdk_status_key.inc"
#include "sdk_status_slip.inc"
#include "sdk_status_slider_lock.inc"
#include "sdk_status_sync_role.inc"
static void setting_end(unsigned deck,unsigned i,Slot::Setting &v) noexcept {
    v.address.store(0,std::memory_order_release);
    uint64_t owner=v.provider.exchange(0,std::memory_order_acq_rel);
    if (owner) {
        if (i==2) sync_status_retire(deck,owner);
        setting_receipt(deck,i,owner,0,-1);
        azn_capability(deck,setting_caps[i],0);
    }
}
static void status_settings_retire(unsigned d,Slot &slot) noexcept {
    key_retire(d);
    slip_retire(d);
    for (unsigned i=0;i<4;i++) setting_end(d,i,slot.settings[i]);
}
static void status_settings_frame(unsigned d,Slot &slot) noexcept {
    key_frame(d,slot);
    slip_frame(d,slot);
    const uint64_t interest=azn_interest(d);
    unsigned installed=setting_installed.load(std::memory_order_acquire);
    uint64_t seed=azn_seed_id(d);
    for (unsigned i=0;i<4;i++) {
        if (!(interest&AZ_STATUS_TOPIC(i==3 ? AZ_STATUS_TEMPO:i==2 ? AZ_STATUS_SYNC:AZ_STATUS_SETTINGS))) continue;
        if (!(installed&(1u<<i))) continue;
        auto &v=slot.settings[i];
        uintptr_t address=setting_object(slot,i), previous=v.address.load(std::memory_order_acquire);
        if (address==previous && (!address || (v.seed==seed && v.sealed_fault==v.fault.load(std::memory_order_acquire)))) continue;
        if (v.busy.test_and_set(std::memory_order_acquire)) continue;
        uint64_t fence=v.fault.load(std::memory_order_acquire);
        if (address!=previous) {
            v.raw=-1;
            setting_end(d,i,v);
            if (address) {
                uint64_t next=setting_incarnation.fetch_add(1,std::memory_order_relaxed)+1;
                if (!next || next>=UINT64_C(0x0100000000000000)) address=0;
                else {
                    v.provider.store(AZSP_SETTINGS|next,std::memory_order_relaxed);
                    v.address.store(address,std::memory_order_release);
                    azn_capability(d,setting_caps[i],1);
                    if (i==2 && sync_installed.load(std::memory_order_acquire)) azn_capability(d,AZN_CAP_SYNC_ROLE,1);
                }
            }
        }
        if (address) {
            unsigned value=i==3 ? integer(address+0x170)!=0:byte(address+(i==2 ? 0xb8 : i ? 8:16));
            if (i==2 && v.raw<0) {
                int raw=-1;
                if (azn_sync_seed(d,slot.unit,&raw) && ((raw==2 && value==1) || (!raw && !value))) v.raw=raw;
            }
            if (v.fault.load(std::memory_order_acquire)==fence) {
                setting_receipt(d,i,v.provider.load(std::memory_order_relaxed),value,value<=1,v.raw);
                v.last=value;v.seed=seed;v.sealed_fault=fence;
            }
        }
        v.busy.clear(std::memory_order_release);
    }
}
static void set_raw_sync(Slot::Setting &,unsigned,unsigned,unsigned,unsigned) {}
static void set_raw_sync(Slot::Setting &v,unsigned index,unsigned value,unsigned mode) {
    if (index==2) v.raw=mode==2 && value==1 ? 2:mode==1 && !value ? 0:-1;
}
template<unsigned Index,class... Args>
static void setting_writer(void *self,Args... args) {
    auto original=trampoline<void (*)(void *,Args...)>(&setting_original[Index]);
    Slot::Setting *owned=nullptr;
    unsigned deck=0;
    uintptr_t address=reinterpret_cast<uintptr_t>(self);
    uint64_t provider=0,fence=0;
    for (unsigned d=0;d<4;d++) {
        auto &v=state().decks[d].settings[Index];
        if (v.address.load(std::memory_order_acquire)!=address) continue;
        if (v.busy.test_and_set(std::memory_order_acquire)) {
            v.fault.fetch_add(1,std::memory_order_acq_rel);
            azn_setting_gap(d,setting_kinds[Index]);
            break;
        }
        owned=&v; deck=d;
        provider=v.provider.load(std::memory_order_acquire);
        fence=v.fault.load(std::memory_order_acquire);
        break;
    }
    struct Release {
        Slot::Setting *v;
        ~Release() {if (v) v->busy.clear(std::memory_order_release);}
    } release {owned};
    /* Master Tempo's actual writer also resets the key-shift factor. */
    KeyCapture key(Index == 1 ? address + 16 : 0);
    SliderLockWrite lock(Index == 2 ? address : 0);
    SyncRoleWriter role(Index == 2 ? address : 0);
    original(self,args...);
    key.finish();
    if (owned && provider && owned->address.load(std::memory_order_acquire)==address &&
        owned->provider.load(std::memory_order_acquire)==provider && owned->fault.load(std::memory_order_acquire)==fence) {
        unsigned value=byte(address+(Index==2 ? 0xb8 : Index ? 8:16));
        set_raw_sync(*owned,Index,value,args...);
        setting_receipt(deck,Index,provider,value,value<=1,owned->raw);
    }
}
static int status_settings_install(State &s,int build,azn_setting_patch_fn patch) noexcept {
    /* One instruction only. The quantize leaf occupies eight bytes, and the
     * third instruction in the key entry is a conditional relative branch. */
    const uint32_t expected[][2]={{0x39004001,0xd65f03c0},{0x12001c42,0x72001c21},{0xa9be7bfd,0x910003fd}};
    void *hooks[]={reinterpret_cast<void *>(setting_writer<0,unsigned>),
        reinterpret_cast<void *>(setting_writer<1,unsigned,unsigned>),reinterpret_cast<void *>(setting_writer<2,unsigned>)};
    const char *names[]={"status-quantize","status-key-lock","status-sync-mode"};
    for (unsigned i=0;i<3;i++) {
        if (setting_installed.load(std::memory_order_acquire)&(1u<<i)) continue;
        uintptr_t target=s.bias+setting_profiles[build-1].writer[i];
        if (!memcmp(reinterpret_cast<void *>(target),expected[i],sizeof expected[i]) &&
            patch(target,hooks[i],&setting_original[i],names[i])==1 && setting_original[i])
            setting_installed.fetch_or(1u<<i,std::memory_order_release);
    }
    if (setting_installed.load(std::memory_order_acquire)&2u) key_install(s,build,patch);
    slip_install(s,build,patch);
    slider_lock_install(s,build,patch);
    sync_role_install(s,build,patch);
    return int(setting_installed.load(std::memory_order_acquire));
}
