/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azslipobserver.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <new>
#include <time.h>
#include <type_traits>
#ifndef __GLIBCXX__
#error "Slip owner views require the reviewed GNU shared/weak ABI"
#endif
extern "C" {
struct azsdk_slip_debug_v1 azsdk_slip_observation_v1[4];
struct azsdk_slip_getter_debug_v1 azsdk_slip_getter_observation_v1[4];
}
static_assert(sizeof(azsdk_slip_debug_v1)==56,"internal scalar observation ABI");
static_assert(sizeof(azsdk_slip_getter_debug_v1)==56,"internal getter observation ABI");
namespace azsdk_slip {
namespace {
using Control=std::_Sp_counted_base<__gnu_cxx::_S_atomic>;
constexpr unsigned limit=64;
struct ErrnoGuard { int value=errno;~ErrnoGuard(){errno=value;} };
/* A real native strong owner is present at the checked constructor/publication
 * boundary. Only then may we copy its GNU weak count. Pointer/range checks are
 * never an ownership-acquisition substitute. Retained controls are never reused. */
struct Weak {
    Control *control=nullptr;
    void copy_owned(std::uintptr_t p) noexcept { control=reinterpret_cast<Control *>(p);control->_M_weak_add_ref(); }
    bool alive() const noexcept{return control&&control->_M_get_use_count()>0;}
};
struct Slot {
    std::atomic<unsigned> stage{0}; // 1: constructed candidate;2: actual parent publication;3: retired
    std::uintptr_t source=0,source_control=0,parent_control=0;
    unsigned deck=0;
    Weak source_owner,parent_owner;
    std::shared_ptr<Listener> listener;
    std::atomic<bool> writing{false};
    std::atomic<std::uint64_t> stamp{0},revision{0},observed{0},callbacks{0};
    std::atomic<unsigned> raw{0};
    std::atomic<std::uintptr_t> getter[3];
    std::atomic<std::uint64_t> getter_ns{0};
    std::atomic<unsigned> getter_seen{0},getter_conflict{0};
    std::uint64_t next_revision=0;
    Slot() noexcept {for(auto &target:getter)target.store(0);}
};
struct State {
    Profile profile={};std::atomic<bool> configured{false};
    std::atomic<unsigned> used{0},current[4]{};
    Slot slots[limit];
};
State &state() noexcept {
    // Native controls/code remain process-lifetime; no static-teardown order dependency.
    static typename std::aligned_storage<sizeof(State),alignof(State)>::type storage;
    static State *s=::new(static_cast<void *>(&storage)) State;
    return *s;
}
static_assert(ATOMIC_LLONG_LOCK_FREE==2&&ATOMIC_INT_LOCK_FREE==2,"native callbacks use lock-free scalar atomics");
template<class T> T scalar(std::uintptr_t p) noexcept {T value;std::memcpy(&value,reinterpret_cast<const void *>(p),sizeof value);return value;}
bool pointer(std::uintptr_t p) noexcept {return p>=0x10000 && !(p&7) && p<=UINT64_C(0x0000ffffffffffff);}
unsigned used() noexcept {unsigned n=state().used.load(std::memory_order_acquire);return n<limit?n:limit;}
unsigned number(const Slot &slot) noexcept {return unsigned(&slot-state().slots)+1;}
bool current(const Slot &slot) noexcept {
    return slot.stage.load(std::memory_order_acquire)==2 && state().current[slot.deck].load(std::memory_order_acquire)==number(slot) &&
        slot.parent_owner.alive() && slot.source_owner.alive();
}
void retire(Slot &slot) noexcept {
    if(slot.stage.exchange(3,std::memory_order_acq_rel)==3)return;
    unsigned expected=number(slot);
    state().current[slot.deck].compare_exchange_strong(expected,0,std::memory_order_acq_rel);
    slot.listener.reset();
}
void capture(Slot &,unsigned) noexcept;
class OwnedListener final:public Listener {
    Slot &slot_;
public:
    explicit OwnedListener(Slot &slot) noexcept:slot_(slot){}
    void changed(unsigned raw) noexcept override {ErrnoGuard preserve;capture(slot_,raw);}
};
void constructed(const azce_call &call) noexcept {
    auto &s=state();const auto object=std::uintptr_t(call.args[0]);const unsigned deck=unsigned(call.args[2]);
    if(call.caller_link!=s.profile.construct_return || deck>=4 || !pointer(object) || object<16 ||
       scalar<std::uintptr_t>(object-16)!=s.profile.source_control_vtable ||
       reinterpret_cast<Control *>(object-16)->_M_get_use_count()<=0 || scalar<unsigned>(object+0x134)!=deck)return;
    for(unsigned i=0;i<used();i++) {
        const auto &prior=s.slots[i];const auto stage=prior.stage.load(std::memory_order_acquire);
        if(stage && prior.source_control==object-16 && (stage!=3 || prior.source_owner.control))return;
    }
    unsigned n=s.used.load(std::memory_order_relaxed);
    do {if(n==limit)return;}while(!s.used.compare_exchange_weak(n,n+1,std::memory_order_acq_rel));
    auto &slot=s.slots[n];slot.source=object;slot.source_control=object-16;slot.deck=deck;
    slot.stage.store(1,std::memory_order_release);
}
void published(const azce_call &call) noexcept {
    auto &s=state();const unsigned deck=unsigned(call.args[5]);const auto parent=std::uintptr_t(call.args[0]);
    if(deck>=4 || call.caller_link!=s.profile.parent_returns[deck] || !pointer(parent) || parent<16 ||
       scalar<std::uintptr_t>(parent-16)!=s.profile.parent_control_vtable)return;
    /* The successful actual PlayerInfoState constructor has published this real
     * shared_ptr<gui::SlipState> into its owned member. It is not an SDK caller
     * inventing an owner from an address; the exact native caller holds the new
     * inplace PlayerInfoState owner throughout this constructor return. */
    struct Pair {std::uintptr_t source,control;};
    const auto pair=scalar<Pair>(parent+0x1120);
    for(unsigned i=0;i<used();i++) {
        const auto &prior=s.slots[i];const auto stage=prior.stage.load(std::memory_order_acquire);
        if((stage==2 || stage==3) && prior.parent_control==parent-16 &&
           prior.source==pair.source && prior.source_control==pair.control)return;
    }
    s.current[deck].store(0,std::memory_order_release);
    for(unsigned i=used();i;i--) {
        auto &slot=s.slots[i-1];
        if(slot.stage.load(std::memory_order_acquire)!=1 || slot.deck!=deck || slot.source!=pair.source || slot.source_control!=pair.control)continue;
        slot.source_owner.copy_owned(pair.control);
        slot.parent_owner.copy_owned(parent-16);
        slot.parent_control=parent-16;
        try {
            slot.listener=std::make_shared<OwnedListener>(slot);
            const std::weak_ptr<Listener> listener(slot.listener);
            // This is the reviewed native multi-listener API. It performs its
            // own mutex-protected weak append. No SDK mutex spans native code,
            // and the native parent's existing strong owners keep the pair live.
            s.profile.registration(reinterpret_cast<void *>(slot.source),listener);
        } catch(...) {retire(slot);return;}
        slot.stage.store(2,std::memory_order_release);
        s.current[deck].store(i,std::memory_order_release);return;
    }
}
void disposing(std::uintptr_t control,bool parent) noexcept {
    if(!control)return;
    for(unsigned i=0;i<used();i++) {
        auto &slot=state().slots[i];const auto stage=slot.stage.load(std::memory_order_acquire);
        if((stage==1 || stage==2) && (parent?slot.parent_control:slot.source_control)==control)retire(slot);
    }
}
std::uint64_t now() noexcept {
    timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0 || t.tv_nsec<0 || t.tv_nsec>=1000000000L)return 0;
    const auto sec=std::uint64_t(t.tv_sec),ns=std::uint64_t(t.tv_nsec);
    return sec>(UINT64_MAX-ns)/UINT64_C(1000000000)?0:sec*UINT64_C(1000000000)+ns;
}
void capture(Slot &value,unsigned raw) noexcept {
    Slot *slot=&value;
    if(!current(*slot))return;
    bool idle=false;if(!slot->writing.compare_exchange_strong(idle,true,std::memory_order_acquire))return;
    const auto ns=now();
    auto sequence=slot->stamp.load(std::memory_order_relaxed);
    if(sequence>UINT64_MAX-2){retire(*slot);slot->writing.store(false,std::memory_order_release);return;}
    const auto calls=slot->callbacks.load(std::memory_order_relaxed);
    if(calls==UINT64_MAX){retire(*slot);slot->writing.store(false,std::memory_order_release);return;}
    slot->callbacks.store(calls+1,std::memory_order_release);
    slot->stamp.store(sequence+1);
    if(ns && slot->next_revision!=UINT64_MAX) {
        slot->raw.store(raw);slot->observed.store(ns);slot->revision.store(++slot->next_revision);
    } else {slot->revision.store(0);slot->observed.store(0);}
    slot->stamp.store(sequence+2);slot->writing.store(false,std::memory_order_release);
}
}
bool configure(const Profile &value) noexcept {
    if(!value.construct_return || !value.registration || !value.source_control_vtable || !value.parent_control_vtable)return false;
    for(unsigned i=0;i<4;i++)if(!value.parent_returns[i])return false;
    auto &s=state();if(s.configured.load())return false;
    s.profile=value;s.configured.store(true,std::memory_order_release);return true;
}
void forward(azce_call *call,Original original) {
    if(!call || !original)return;
    bool reserved=call->reserved!=0;
#if AZCE_SIZE >= 320
    reserved=reserved || call->caller_reserved!=0;
#endif
    if(!state().configured.load(std::memory_order_acquire) || reserved){original(call);return;}
    switch(call->kind) {
    case Construct:original(call);{ErrnoGuard preserve;constructed(*call);}return;
    case PublishParent:original(call);{ErrnoGuard preserve;published(*call);}return;
    case DisposeSource:case DisposeParent:{ErrnoGuard preserve;disposing(call->args[0],call->kind==DisposeParent);}original(call);return;
    default:original(call);return;
    }
}
bool read(unsigned deck,Receipt *out) noexcept {
    ErrnoGuard preserve;
    if(deck>=4 || !out)return false;
    auto &s=state();const unsigned id=s.current[deck].load(std::memory_order_acquire);
    if(!id || id>used())return false;
    auto &slot=s.slots[id-1];if(!current(slot))return false;
    for(unsigned attempt=0;attempt<3;attempt++) {
        auto before=slot.stamp.load();if(before&1)continue;
        Receipt value={id,slot.revision.load(),slot.observed.load(),slot.raw.load(),0};
        if(before==slot.stamp.load() && value.revision && value.observed_ns && current(slot)){*out=value;return true;}
    }
    return false;
}
}
extern "C" void azsdk_slip_capture_getter(unsigned site,uintptr_t source,uintptr_t target) {
    using namespace azsdk_slip;
    ErrnoGuard preserve;
    if(site>=3 || target<0x10000 || target>UINT64_C(0x0000ffffffffffff) || (target&3))return;
    for(unsigned i=0;i<used();i++) {
        auto &slot=state().slots[i];
        if(!current(slot) || slot.source!=source)continue;
        bool idle=false;if(!slot.writing.compare_exchange_strong(idle,true,std::memory_order_acquire))return;
        const auto stamp=slot.stamp.load(std::memory_order_relaxed),ns=now();
        if(stamp<=UINT64_MAX-2 && ns && current(slot)) {
            slot.stamp.store(stamp+1);
            const auto prior=slot.getter[site].load();const unsigned bit=1u<<site;
            if(prior && prior!=target)slot.getter_conflict.fetch_or(bit);
            else {slot.getter[site].store(target);slot.getter_seen.fetch_or(bit);}
            slot.getter_ns.store(ns);slot.stamp.store(stamp+2);
        }
        slot.writing.store(false,std::memory_order_release);return;
    }
}
extern "C" void azsdk_slip_collect_v1(void) {
    using namespace azsdk_slip;
    ErrnoGuard preserve;
    static std::atomic<bool> collecting{false};bool idle=false;
    if(!collecting.compare_exchange_strong(idle,true,std::memory_order_acquire))return;
    for(unsigned d=0;d<4;d++) {
        Receipt r={};bool valid=read(d,&r);
        const auto id=state().current[d].load(std::memory_order_acquire);
        const bool registered=id && id<=used() && current(state().slots[id-1]);
        if(!registered || (valid && r.generation!=id))valid=false;
        auto &out=azsdk_slip_observation_v1[d];
        const auto seq=__atomic_load_n(&out.sequence,__ATOMIC_RELAXED);
        if(seq>UINT64_MAX-2)continue;
        __atomic_store_n(&out.sequence,seq+1,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.generation,registered?std::uint64_t(id):0,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.revision,valid?r.revision:0,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.observed_ns,valid?r.observed_ns:0,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.callbacks,registered?state().slots[id-1].callbacks.load():0,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.registered,registered?1u:0u,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.valid,valid?1u:0u,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.raw,valid?r.raw:0,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.reserved,0u,__ATOMIC_SEQ_CST);
        __atomic_store_n(&out.sequence,seq+2,__ATOMIC_SEQ_CST);
        auto &get=azsdk_slip_getter_observation_v1[d];
        azsdk_slip_getter_debug_v1 g={};
        if(registered)for(unsigned attempt=0;attempt<3;attempt++) {
            auto &slot=state().slots[id-1];const auto before=slot.stamp.load();
            if(before&1)continue;
            g.generation=id;g.observed_ns=slot.getter_ns.load();
            g.target_slot4=slot.getter[0].load();g.target_slot13=slot.getter[1].load();g.target_slot14=slot.getter[2].load();
            g.seen_mask=slot.getter_seen.load();g.conflict_mask=slot.getter_conflict.load();
            if(before==slot.stamp.load()&&current(slot))break;
            g={};
        }
        const auto gs=__atomic_load_n(&get.sequence,__ATOMIC_RELAXED);
        if(gs>UINT64_MAX-2)continue;
        __atomic_store_n(&get.sequence,gs+1,__ATOMIC_SEQ_CST);
        __atomic_store_n(&get.generation,g.generation,__ATOMIC_SEQ_CST);
        __atomic_store_n(&get.observed_ns,g.observed_ns,__ATOMIC_SEQ_CST);
        __atomic_store_n(&get.target_slot4,g.target_slot4,__ATOMIC_SEQ_CST);
        __atomic_store_n(&get.target_slot13,g.target_slot13,__ATOMIC_SEQ_CST);
        __atomic_store_n(&get.target_slot14,g.target_slot14,__ATOMIC_SEQ_CST);
        __atomic_store_n(&get.seen_mask,g.seen_mask,__ATOMIC_SEQ_CST);
        __atomic_store_n(&get.conflict_mask,g.conflict_mask,__ATOMIC_SEQ_CST);
        __atomic_store_n(&get.sequence,gs+2,__ATOMIC_SEQ_CST);
    }
    collecting.store(false,std::memory_order_release);
}
