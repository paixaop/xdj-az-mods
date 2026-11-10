/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "sdk_status_source.c"

static unsigned char ticket[0x110], info[0x60], pack[0x130], recv, client, server;
static uintptr_t wire[2][16];
static struct {
    unsigned char msg[64],args[2][48];
} msg[24];
static unsigned used;
static void put(void *obj,unsigned off,unsigned n,uint64_t value) { memcpy((char*)obj+off,&value,n); }
static void *packet(unsigned kind,unsigned seq,unsigned row) {
    assert(used<24);
    unsigned i=used++;
    memset(&msg[i],0,sizeof msg[i]);
    put(msg[i].msg,0,2,kind);put(msg[i].msg,4,4,0x872349ae);
    put(msg[i].msg,8,4,seq);
    put(msg[i].msg,40,8,(uintptr_t)msg[i].args);
    put(msg[i].msg,48,8,(uintptr_t)msg[i].args+96);
    put(msg[i].msg,56,8,(uintptr_t)msg[i].args+96);
    put(msg[i].args[0],0,4,1);put(msg[i].args[0],8,4,525057);
    put(msg[i].args[1],0,4,1);put(msg[i].args[1],8,4,row);
    return msg[i].msg;
}
static uint64_t send_noop(void *s,void *p,unsigned peer,void *err) {(void)s;(void)p;(void)err;return UINT64_C(0x85000000001)+peer;}
static int wire_noop(void *s,void *p) {(void)s;(void)p;return 0;}
static uint64_t in_noop(void *s,void *p,unsigned peer) {(void)s;(void)p;return UINT64_C(0x73000000001)+peer;}
static uint64_t retire_noop(void *s) {(void)s;return UINT64_C(0x92000000001);}
enum failure { NONE, WRONG_SOURCE, WRONG_PEER, OTHER_QUERY, WRONG_CLIENT_PACKET, FOREIGN_RECEPTION, LOST };
static uint64_t run(enum failure fault) {
    memset(js.routes,0,sizeof js.routes);memset(js.tickets,0,sizeof js.tickets);memset(js.packs,0,sizeof js.packs);
    js.build=1;js.ready=1;js.lost_routes=js.lost_packs=0;used=0;
    js.original[0]=send_noop;js.original[1]=wire_noop;js.original[2]=in_noop;js.original[3]=in_noop;
    js.original[5]=retire_noop;js.original[6]=retire_noop;
    unsigned char id[12]={1,0,0,0,2,1,1,0,36};
    memset(ticket,0,sizeof ticket); put(ticket,8,8,97);put(ticket,16,8,41);put(ticket,0x80,1,2);
    put(ticket,0x88,8,(uintptr_t)&recv);put(ticket,0x90,8,(uintptr_t)ticket);
    put(ticket,0xb0,4,36);memcpy(info+8,id,12);info[0]=1;
    memset(pack,0,sizeof pack);put(pack,0,8,js_profiles[0].pack_vptr);put(pack,0x30,4,5);pack[0x78]=1;
    memcpy(pack+0x38,id,12);memcpy(pack+0x80,id,12);
    void *query=packet(0x2102,23,36),*next=packet(0x3000,24,0);
    for (int stage=0;stage<2;stage++) {
        uint32_t seq=23+stage,kind=stage?0x3000:0x2102;
        if (stage) query=fault==OTHER_QUERY?packet(kind,seq,0):next;
        assert(js_data_send(&client,query,0,NULL)==UINT64_C(0x85000000001));js_wire_send(wire[0],query);
        void *received=packet(kind,seq,stage?0:36);
        assert(js_server_in(&server,received,fault==WRONG_PEER)==UINT64_C(0x73000000001)+(fault==WRONG_PEER));
        azjs_server((uintptr_t)received,0,stage && fault==WRONG_SOURCE?12:11,1);
        unsigned reply=stage?0x4201:0x4000;
        void *response=packet(reply,seq,7);
        js_data_send(&server,response,0,NULL);azjs_server(0,0,0,0);js_wire_send(wire[1],response);
        void *delivered=packet(reply,seq,7);assert(js_client_in(&client,delivered,0)==UINT64_C(0x73000000001));
        if (fault==WRONG_CLIENT_PACKET) delivered=packet(reply,seq,7);
        put(ticket,0x20,4,seq);
        azjs_ticket((uintptr_t)ticket,(uintptr_t)delivered,1);
        if (stage) azjs_reception(fault==FOREIGN_RECEPTION?0:(uintptr_t)&recv,5,info);
        else {
            void *args=(void *)(uintptr_t)js_word(next,40,8);
            put(args,0,4,0); /* arg 0 has no integer route until the client sends */
            azjs_ticket_return((uintptr_t)ticket,(uintptr_t)next);
            put(args,0,4,1);
        }
        azjs_ticket(0,0,0);
    }
    void *p=pack;
    if (fault==LOST) atomic_store(&js.lost_packs,js_pack_mask(5));
    js_pack_commit(&p,1);
    uint64_t token=azjs_capture(0,1,1,(uintptr_t)pack,id);
    if (fault!=NONE) return token;
    assert(token);
    struct azjs_origin src;
    assert(azjs_lookup(token,&src)==1 && src.track_id==36 && src.owner==11);
    atomic_store(&js.lost_routes,js_route_mask(77));
    atomic_store(&js.lost_packs,js_pack_mask(6));
    assert(azjs_capture(2,2,3,(uintptr_t)pack,id)==token);
    id[8]=37;assert(!azjs_capture(2,2,3,(uintptr_t)pack,id));
    assert(js_destroy(pack,5)==UINT64_C(0x92000000001));
    assert(!azjs_lookup(token,&src));id[8]=36;
    assert(!azjs_capture(3,1,2,(uintptr_t)pack,id));
    return token;
}
int main(void) {
    assert(run(NONE));
    for(int f=WRONG_SOURCE;f<=LOST;f++) assert(!run((enum failure)f));
    puts("source join: two routed requests, source/peer/packet identity, cached pack reuse, retirement and capture loss passed");
}
