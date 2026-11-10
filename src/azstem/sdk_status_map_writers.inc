/* SPDX-License-Identifier: GPL-3.0-only */
/* A holder refresh can reuse the GUI's witnessed pairing while the pack and
 * both of its retained holder identities are unchanged. The changed content
 * must arrive as a genuine owned argument. The other component stays pinned. */
struct HolderEdit {
    struct Entry { Pair prior; unsigned decks=0; };
    Entry entries[4];
    unsigned component,count=0,mask=0;
    uint64_t bracket=0;
    HolderEdit(unsigned index,uintptr_t holder):component(index) {
        if (!holder || !enabled.load(std::memory_order_acquire))return;
        for(unsigned d=0;d<4;d++) {
            azn_pack_view view{};
            if (!(azn_interest(d)&routes) || !azn_pack_acquire(d,&view))continue;
            if (read_value<uintptr_t>(view.object,index ? 0x120:0xd8)==holder)capture(d,holder,view);
            azn_pin_drop(view.token);
        }
        bracket=azn_map_begin_mask(mask);
    }
    void capture(unsigned deck,uintptr_t holder,const azn_pack_view &view) {
        for(unsigned i=0;i<count;i++)if(entries[i].prior.pack.address==view.object) {
            entries[i].decks|=1u<<deck;mask|=1u<<deck;return;
        }
        Slot &slot=cache[bucket(view.object)];
        Lock lock(slot);
        if(!lock.held){azn_map_gap(deck);return;}
        const Pair &p=slot.pair;
        if(p.pack.address!=view.object || p.holder[component].address!=holder || !azn_pack_current(deck,&view))return;
        Pair &next=entries[count].prior;
        next.binding=p.binding;
        Object *to[]={&next.pack,&next.holder[0],&next.holder[1],&next.content[0],&next.content[1]};
        const Object *from[]={&p.pack,&p.holder[0],&p.holder[1],&p.content[0],&p.content[1]};
        for(unsigned n=0;n<5;n++) {
            if(!from[n]->token)continue;
            if(!azn_pin_hold(from[n]->token,&to[n]->address)){discard(next);azn_map_gap(deck);return;}
            to[n]->token=from[n]->token;
        }
        entries[count++].decks=1u<<deck;mask|=1u<<deck;
    }
    void finish(uintptr_t replacement,bool preserve=false) {
        for(unsigned i=0;i<count;i++) {
            const Pair &p=entries[i].prior;
            uintptr_t gc=component==0 && !preserve ? replacement:p.content[0].address;
            uintptr_t pc=component==1 && !preserve ? replacement:p.content[1].address;
            if (!bracket || !update_pair(0,p.pack.address,p.holder[0].address,gc,p.holder[1].address,pc,p.binding))
                for(unsigned d=0;d<4;d++)if(entries[i].decks&(1u<<d))azn_map_gap(d);
        }
    }
    ~HolderEdit(){
        azn_map_end_mask(bracket);
        for(unsigned i=0;i<count;i++)discard(entries[i].prior);
    }
};
static void *map_writer_original[4];
static unsigned map_writer_installed;
static uintptr_t grid_assign(uintptr_t dst,uintptr_t source) {
    HolderEdit edit(0,dst>=0x28 ? dst-0x28:0);
    uintptr_t incoming=edit.mask && source ? read_value<uintptr_t>(source):0;
    uint64_t pin=incoming && typed(incoming,profile->grid_content) ? azn_owned_pin(incoming):0;
    struct Drop {uint64_t pin;~Drop(){azn_pin_drop(pin);}} drop{pin};
    uintptr_t result=reinterpret_cast<uintptr_t(*)(uintptr_t,uintptr_t)>(map_writer_original[0])(dst,source);
    if(result && edit.mask)edit.finish(pin ? incoming:0);
    return result;
}
static uintptr_t grid_retime(uintptr_t dst,int64_t offset) {
    HolderEdit edit(0,dst>=0x28 ? dst-0x28:0);
    uintptr_t result=reinterpret_cast<uintptr_t(*)(uintptr_t,int64_t)>(map_writer_original[1])(dst,offset);
    if(result && edit.mask)edit.finish(0,true);
    return result;
}
static thread_local uintptr_t phrase_argument;
static uintptr_t phrase_registry(uintptr_t registry,uintptr_t context,uintptr_t id,uintptr_t source,uintptr_t flags,uintptr_t version) {
    struct Restore {uintptr_t previous;~Restore(){phrase_argument=previous;}} restore{phrase_argument};
    phrase_argument=source; /* native reference lives through the registry call */
    return reinterpret_cast<uintptr_t(*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t)>(
        map_writer_original[2])(registry,context,id,source,flags,version);
}
static uintptr_t phrase_broadcast(uintptr_t holder,unsigned reason) {
    if(phrase_argument) {
        HolderEdit edit(1,holder);
        if(edit.mask)edit.finish(read_value<uintptr_t>(phrase_argument));
    }
    return reinterpret_cast<uintptr_t(*)(uintptr_t,unsigned)>(map_writer_original[3])(holder,reason);
}
