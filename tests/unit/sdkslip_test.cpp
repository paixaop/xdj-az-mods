/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azslipobserver.h"
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
using namespace azsdk_slip;
struct Source {
    unsigned char prefix[0x134]={};unsigned deck;
    std::mutex mutex;std::vector<std::weak_ptr<Listener>> listeners;
    explicit Source(unsigned d):deck(d){}
    void dispatch(unsigned raw) {
        std::lock_guard<std::mutex> guard(mutex);
        for(auto &weak:listeners)if(auto receiver=weak.lock())receiver->changed(raw);
    }
};
struct Parent {unsigned char prefix[0x1120]={};std::shared_ptr<Source> source;};
static_assert(offsetof(Parent,source)==0x1120&&offsetof(Source,deck)==0x134,"reviewed field offsets");
static Profile profile;
static void (*inside)(azce_call *);
static bool fail_registration;
static void original(azce_call *c) {
    assert(errno==EINTR);++*reinterpret_cast<unsigned *>(c->args[7]);
    if(inside)inside(c);
    c->result[0]=0x130200;c->result[1]=0x87654321;errno=ERANGE;
}
static void invoke(Kind kind,void *object,std::uintptr_t caller=0,unsigned arg=0) {
    unsigned forwarded=0;azce_call c={};c.kind=kind;c.args[0]=reinterpret_cast<std::uintptr_t>(object);
    c.args[2]=c.args[5]=arg;c.args[7]=reinterpret_cast<std::uintptr_t>(&forwarded);c.caller_link=caller;
    errno=EINTR;forward(&c,original);assert(forwarded==1&&errno==ERANGE&&c.result[0]==0x130200&&c.result[1]==0x87654321);
}
template<class T> static std::uintptr_t control(const std::shared_ptr<T> &p){struct Pair{std::uintptr_t object,control;} pair;std::memcpy(&pair,&p,sizeof pair);return pair.control;}
static void registration(void *object,const std::weak_ptr<Listener> &receiver) {
    auto &s=*static_cast<Source *>(object);std::lock_guard<std::mutex> guard(s.mutex);
    if(fail_registration){fail_registration=false;throw std::bad_alloc();}
    s.listeners.push_back(receiver); // Reviewed native append/weak-copy contract, never replacement.
}
static void absent(unsigned deck){Receipt out;std::memset(&out,0xa5,sizeof out);Receipt before=out;assert(!read(deck,&out)&&!std::memcmp(&out,&before,sizeof out));}
struct NativeException{unsigned tag;};
class NativeReceiver final:public Listener {
public:
    unsigned calls=0,last=0;bool throwing=false;void (*reenter)()=nullptr;
    void changed(unsigned raw) override {calls++;last=raw;if(reenter)reenter();if(throwing){errno=EDOM;throw NativeException{7};}}
};
struct Model {
    unsigned deck;std::shared_ptr<Parent> parent=std::make_shared<Parent>();
    std::shared_ptr<NativeReceiver> native;
    Model(unsigned d,bool publish=true,bool native_view=true,bool fail=false):deck(d) {
        parent->source=std::make_shared<Source>(d);
        if(native_view){native=std::make_shared<NativeReceiver>();registration(parent->source.get(),native);}
        invoke(Construct,parent->source.get(),profile.construct_return,d);
        if(publish){fail_registration=fail;invoke(PublishParent,parent.get(),profile.parent_returns[d],d);}
    }
    void dispatch(unsigned raw){parent->source->dispatch(raw);}
};
int main() {
    auto p=std::make_shared<Parent>();auto s=std::make_shared<Source>(0);
    profile={0x130200,{0x130204,0x130208,0x13020c,0x130210},
        *reinterpret_cast<std::uintptr_t *>(control(s)),*reinterpret_cast<std::uintptr_t *>(control(p)),registration};
    assert(configure(profile));
    Model unbound(0,false);unbound.dispatch(1);assert(unbound.native->calls==1);absent(0);
    std::puts("SLIP_NATIVE_FORWARD_BASELINE_OK native weak receiver delivered");std::fflush(stdout);
    Model old(0);old.dispatch(255);Receipt r={};
    assert(read(0,&r)&&"missing SDK-owned Slip listener receipt");assert(r.raw==255&&r.generation&&r.revision==1&&r.observed_ns);
    azsdk_slip_collect_v1();const auto &debug=azsdk_slip_observation_v1[0];
    assert(!(debug.sequence&1)&&debug.registered==1&&debug.valid==1&&debug.raw==255&&debug.callbacks==1&&debug.generation==r.generation&&debug.revision==r.revision);
    assert(old.native->calls==1&&old.native->last==255&&old.parent->source->listeners.size()==2);
    {Model failed(3,true,true,true);failed.dispatch(91);assert(failed.native->calls==1&&failed.parent->source->listeners.size()==1);absent(3);}
    auto generation=r.generation;
    Model replacement(0);absent(0);old.dispatch(1);absent(0);replacement.dispatch(0);
    assert(read(0,&r)&&r.generation!=generation&&r.raw==0);generation=r.generation;
    old.dispatch(255);assert(old.native->calls==3&&read(0,&r)&&r.generation==generation&&r.raw==0);
    invoke(PublishParent,old.parent.get(),profile.parent_returns[0],0);assert(read(0,&r)&&r.generation==generation);
    Model no_view[3]={Model(1,true,false),Model(2,true,false),Model(3,true,false)};
    for(unsigned d=1;d<4;d++){no_view[d-1].dispatch(100+d);assert(read(d,&r)&&r.raw==100+d);}
    std::puts("SLIP_GETTER_NATIVE_FORWARD_BASELINE_OK");std::fflush(stdout);
    Model *getters[4]={&replacement,&no_view[0],&no_view[1],&no_view[2]};
    for(unsigned d=0;d<4;d++) {
        for(unsigned site=0;site<3;site++) {
            errno=EINTR;
            azsdk_slip_capture_getter(site,reinterpret_cast<std::uintptr_t>(getters[d]->parent->source.get()),0x900000+d*0x100+site*4);
            assert(errno==EINTR);
        }
    }
    azsdk_slip_collect_v1();
    for(unsigned d=0;d<4;d++) {
        const auto &g=azsdk_slip_getter_observation_v1[d];
        assert(g.seen_mask==7&&"genuine native getter targets missing");
        assert(!(g.sequence&1)&&g.generation==azsdk_slip_observation_v1[d].generation&&g.observed_ns&&!g.conflict_mask);
        assert(g.target_slot4==0x900000+d*0x100&&g.target_slot13==g.target_slot4+4&&g.target_slot14==g.target_slot4+8);
    }
    auto &g0=azsdk_slip_getter_observation_v1[0];
    azsdk_slip_capture_getter(0,reinterpret_cast<std::uintptr_t>(old.parent->source.get()),0xb00000);
    azsdk_slip_capture_getter(3,reinterpret_cast<std::uintptr_t>(replacement.parent->source.get()),0xb00000);
    azsdk_slip_capture_getter(0,reinterpret_cast<std::uintptr_t>(replacement.parent->source.get()),1);
    azsdk_slip_collect_v1();assert(g0.target_slot4==0x900000&&!g0.conflict_mask);
    azsdk_slip_capture_getter(0,reinterpret_cast<std::uintptr_t>(replacement.parent->source.get()),0xb00000);
    azsdk_slip_collect_v1();assert(g0.target_slot4==0x900000&&g0.conflict_mask==1);
    // Native weak expiry does not affect the SDK subscriber; no native proxy is installed.
    std::weak_ptr<NativeReceiver> expired=replacement.native;replacement.native.reset();assert(expired.expired());
    replacement.dispatch(17);assert(read(0,&r)&&r.raw==17);
    // Native exceptions retain their ordering: a preceding native receiver throws
    // before the SDK receiver, and the original exception/errno escapes intact.
    Model exceptions(0);exceptions.dispatch(3);exceptions.native->throwing=true;bool caught=false;
    azsdk_slip_collect_v1();assert(!g0.seen_mask&&!g0.conflict_mask&&!g0.target_slot4);
    try{exceptions.dispatch(9);}catch(const NativeException &e){caught=e.tag==7;}
    assert(caught&&errno==EDOM&&exceptions.native->calls==2&&read(0,&r)&&r.raw==3);
    exceptions.native->throwing=false;exceptions.dispatch(11);assert(read(0,&r)&&r.raw==11);
    // Native delivery may reenter another source without changing the immutable SDK deck binding.
    static Model *nested; nested=&no_view[1];
    exceptions.native->reenter=[](){nested->dispatch(23);};exceptions.dispatch(19);
    assert(read(0,&r)&&r.raw==19&&read(2,&r)&&r.raw==23);
    // A real promoted SDK receiver can outlive cancellation; it must remain inert.
    auto late=exceptions.parent->source->listeners.back().lock();assert(late);
    inside=[](azce_call *){inside=nullptr;absent(0);};
    invoke(DisposeSource,reinterpret_cast<void *>(control(exceptions.parent->source)));
    late->changed(88);absent(0);exceptions.dispatch(99);absent(0);assert(exceptions.native->last==99);
    Model latest(0);latest.dispatch(41);
    invoke(DisposeSource,reinterpret_cast<void *>(control(old.parent->source)));
    assert(read(0,&r)&&r.raw==41);
    auto widget_source=latest.parent->source;auto control_address=control(widget_source);auto address=widget_source.get();
    inside=[](azce_call *){inside=nullptr;absent(0);};invoke(DisposeParent,reinterpret_cast<void *>(control(latest.parent)));
    latest.parent.reset();widget_source->dispatch(77);absent(0);
    azsdk_slip_collect_v1();assert(!debug.registered&&!debug.valid&&!debug.raw&&!debug.observed_ns);
    assert(!g0.generation&&!g0.seen_mask&&!g0.target_slot4&&!g0.observed_ns);
    invoke(DisposeSource,reinterpret_cast<void *>(control_address));widget_source.reset();
    auto fresh=std::make_shared<Source>(0);assert(fresh.get()!=address&&control(fresh)!=control_address);
    invoke(Construct,address,profile.construct_return,0);absent(0);
    absent(4);std::puts("PASS SDK-owned weak subscription, native coexistence/expiry/exceptions, all4 decks and retirement");
}
