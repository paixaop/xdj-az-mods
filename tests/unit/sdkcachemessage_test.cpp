/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual owned-message forwarding/scalar snapshot fixture; no native code,
 * transport modifications, DB data or source/load identity is involved. */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#define REQUIRE(x, why) do { if (!(x)) { std::fprintf(stderr,"FAIL cache-message: %s (%s:%d)\n",why,__FILE__,__LINE__); std::abort(); } } while(0)
#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include("../../src/common/azcachemessage.h")
#include "../../src/common/azcachemessage.h"
#define MESSAGE_IMPLEMENTATION 1
#else
#define MESSAGE_IMPLEMENTATION 0
namespace azsdk_cache_message {
struct Borrow { unsigned build; const void *server, *message; std::uint32_t parameter; };
struct Envelope {
    std::uint16_t kind = 0;
    std::uint32_t sequence = 0, parameter = 0, present = 0, fields[8] = {};
    unsigned client = 0, selector = 0;
};
template<class F> void forward_owned(const Borrow &, F &&original) { std::forward<F>(original)(); }
inline bool current(const void *, Envelope &) noexcept { return false; }
}
#endif
namespace {
namespace cm = azsdk_cache_message;
unsigned profile;
struct alignas(8) Argument { std::uint32_t type = 1, reserved = 0, value = 0; unsigned char pad[36] = {}; };
struct Message {
    std::uint16_t kind = 0x2102, reserved = 0;
    std::uint32_t magic = 0x872349ae, sequence = 0, reserved_c = 0;
    std::uint64_t reserved_10 = 0, reserved_18 = 0, reserved_20 = 0;
    std::vector<Argument> arguments;
    Message(unsigned client, unsigned selector, unsigned content, std::uint32_t seq)
        : sequence(seq), arguments(2) { arguments[0].value = (client << 24) | (selector << 16); arguments[1].value = content; }
};
struct Owned {
    void *page; Message *message; bool destroyed = false;
    Owned(unsigned client, unsigned selector, unsigned content, std::uint32_t seq) {
        page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(page != MAP_FAILED,"native-shaped owned message allocation");
        message = new(page) Message(client,selector,content,seq);
        REQUIRE(reinterpret_cast<char*>(&message->arguments)-reinterpret_cast<char*>(message)==0x28,
                "actual native-shaped vector member offset");
    }
    void destroy_and_protect() { message->~Message(); destroyed = true; REQUIRE(mprotect(page,4096,PROT_NONE)==0,"native handler frees/protects input before return"); }
    ~Owned() { if (!destroyed) message->~Message(); munmap(page,4096); }
};
void absent(const void *server) { cm::Envelope e; e.kind = 0xfedc; e.fields[7] = 0xabcdef; REQUIRE(!cm::current(server,e) && e.kind==0xfedc && e.fields[7]==0xabcdef,"absent scope leaves output unchanged"); }
void four_contexts() {
    int servers[4] = {}; const unsigned clients[] = {0,3,1,2};
    for (unsigned i=0;i<4;++i) {
        Owned owned(clients[i],i+1,101+i,UINT32_MAX-i); absent(&servers[i]); unsigned forwards=0;
        cm::Borrow borrow {profile,&servers[i],owned.message,0x80000102u+i};
        cm::forward_owned(borrow,[&] {
            ++forwards;
            REQUIRE(owned.message->kind==0x2102 && owned.message->arguments[1].value==101+i,"native sees unchanged original message and arguments");
            std::printf("NATIVE_MESSAGE_FORWARD_OK build=%u context=%u implementation=%d\n",profile,i,MESSAGE_IMPLEMENTATION); std::fflush(stdout);
            cm::Envelope e;
            REQUIRE(cm::current(&servers[i],e),"missing owned pre-forward message scalar snapshot");
            REQUIRE(e.kind==0x2102 && e.sequence==UINT32_MAX-i && e.parameter==0x80000102u+i &&
                    e.present==3 && e.fields[1]==101+i && e.client==clients[i] && e.selector==i+1,"copied supported scalar context is exact");
            int foreign; absent(&foreign); owned.destroy_and_protect();
            cm::Envelope after;
            REQUIRE(cm::current(&servers[i],after) && after.fields[1]==101+i,"snapshot never follows message after native destruction");
        });
        REQUIRE(forwards==1 && owned.destroyed,"original handler forwarded exactly once"); absent(&servers[i]);
    }
}
void nested_and_exception() {
    int server; Owned outer(0,2,11,9), inner(3,8,22,10);
    cm::Borrow a {profile,&server,outer.message,1}, b {profile,&server,inner.message,2};
    cm::forward_owned(a,[&] {
        cm::Envelope e; REQUIRE(cm::current(&server,e)&&e.fields[1]==11,"outer frame visible");
        cm::forward_owned(b,[&] { cm::Envelope n; REQUIRE(cm::current(&server,n)&&n.fields[1]==22,"nested frame distinct"); });
        REQUIRE(cm::current(&server,e)&&e.fields[1]==11,"normal nesting restores outer scalar frame");
        bool caught=false;
        try { cm::forward_owned(b,[] {throw std::runtime_error("native message exception");}); }
        catch(const std::runtime_error &x) {caught=std::strcmp(x.what(),"native message exception")==0;}
        REQUIRE(caught && cm::current(&server,e)&&e.fields[1]==11,"native exception propagates and restores exact outer frame");
    }); absent(&server);
}
void canonical_and_bounds() {
    int server; Owned o(0,1,11,0); cm::Borrow borrow {profile,&server,o.message,2};
    auto rejected=[&] {unsigned count=0;cm::forward_owned(borrow,[&]{++count;absent(&server);});REQUIRE(count==1,"unsupported view still forwards native work");};
    o.message->kind=0x9999;rejected();o.message->kind=0x2102;
    o.message->magic=0;rejected();o.message->magic=0x872349ae;
    o.message->arguments[1].type=2;rejected();o.message->arguments[1].type=1;
    o.message->arguments.resize(1);rejected();o.message->arguments.resize(2);
    o.message->arguments[0].value=0;rejected();o.message->arguments[0].value=10u<<16;rejected();
    o.message->arguments[0].value=1u<<16;borrow.build=0;rejected();borrow.build=profile;
    // Corrupt only the pointer metadata while forwarding an opaque nonreading
    // handler, then restore actual native ownership before its destructor runs.
    std::uintptr_t saved[3];std::memcpy(saved,reinterpret_cast<char*>(o.message)+0x28,sizeof saved);
    const std::uintptr_t bad[][3]={{saved[0]+1,saved[1],saved[2]},{saved[1],saved[0],saved[2]},
        {saved[0],saved[1]+1,saved[2]},{saved[0],saved[1],saved[0]},
        {saved[0],saved[0]+48u*4097u,saved[0]+48u*4097u}};
    for(const auto &p:bad){std::memcpy(reinterpret_cast<char*>(o.message)+0x28,p,sizeof p);rejected();}
    std::memcpy(reinterpret_cast<char*>(o.message)+0x28,saved,sizeof saved);
}
void data_request_and_wrap() {
    int server;Owned o(2,9,0,UINT32_MAX);o.message->kind=0x3000;o.message->arguments.resize(8);
    o.message->arguments[1].value=17;o.message->arguments[2].value=4;o.message->arguments[3].value=0x1234;o.message->arguments[7].value=0xabcd;
    for(auto seq:{UINT32_MAX,0u}) {
        o.message->sequence=seq;
        cm::forward_owned({profile,&server,o.message,77},[&]{cm::Envelope e;REQUIRE(cm::current(&server,e)&&e.sequence==seq&&e.present==0x8f&&e.fields[1]==17&&e.fields[2]==4&&e.fields[3]==0x1234&&e.fields[7]==0xabcd,"copy raw wrapped sequence and bounded canonical data fields without inventing identity");});
    }
}
void bounded_nesting() {
    int server;Owned o(0,1,1,1);
    std::function<void(unsigned)> recurse;
    recurse=[&](unsigned depth) {
        cm::forward_owned({profile,&server,o.message,depth},[&] {
            cm::Envelope e;
            if(depth<=32) REQUIRE(cm::current(&server,e)&&e.parameter==depth,"supported nested frame retains its own scalar parameter");
            else absent(&server);
            if(depth<50)recurse(depth+1);
            if(depth<=32) REQUIRE(cm::current(&server,e)&&e.parameter==depth,"overflow scope never exposes or loses the outer frame");
            else absent(&server);
        });
    };
    recurse(1);absent(&server);
}
void thread_isolation() {
    std::thread threads[4];
    for(unsigned i=0;i<4;++i)threads[i]=std::thread([i]{int server;Owned o(i,i+1,31+i,i);cm::forward_owned({profile,&server,o.message,i},[&]{cm::Envelope e;REQUIRE(cm::current(&server,e)&&e.fields[1]==31+i,"separate actual caller threads cannot borrow another frame");});absent(&server);});
    for(auto &t:threads)t.join();
}
}
int main(int argc,char **argv) {
    static_assert(sizeof(Argument)==48 && sizeof(Message)==0x40,"verified canonical native message/argument prefixes");
    REQUIRE(argc==2&&(argv[1][0]=='1'||argv[1][0]=='2'),"separate fixture profile process");profile=unsigned(argv[1][0]-'0');
    four_contexts();nested_and_exception();canonical_and_bounds();data_request_and_wrap();bounded_nesting();thread_isolation();
    std::printf("PASS cache-message build=%u: owned scalar copy, original cleanup, nesting/exception restoration and independent threads\n",profile);
}
