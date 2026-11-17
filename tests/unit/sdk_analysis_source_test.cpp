/* SPDX-License-Identifier: GPL-3.0-only */
/* Isolated AArch64 fixture. Production has no synthetic source registration. */
#define AZ_STATUS_NATIVE_PRODUCER
#include "../../src/azstem/sdk_analysis_source.cpp"
#include <cassert>
#include <cstdio>
#include <sys/mman.h>
static bool loaded[4] = {true,true,false,false};
static uint64_t load_epoch=7, load_generation=8;
static unsigned char load_id[12] = {1,0,0,0,6,0,1,0,42,0,0,0};
extern "C" int azsdk_load_read(int deck, azsdk_load_sample *out) noexcept {
    if(deck<0||deck>=4||!loaded[deck]) return 0;
    *out={1,load_generation,load_epoch,1,1,0}; return 1;
}
extern "C" int azn_load_track_matches(unsigned deck,uint64_t epoch,uint64_t load,const unsigned char id[12]) {
    return deck<4&&loaded[deck]&&epoch==load_epoch&&load==load_generation&&!memcmp(id,load_id,12);
}
static Track fake_state;
static Track get_state(const void*) { return fake_state; }
static int native_result;
static int wait_result(uintptr_t,const WeakTrack*,int) { return native_result; }
struct Native { unsigned char bytes[0x200]={}; };
template<class T> void put(Native &n,unsigned off,T v) {memcpy(n.bytes+off,&v,sizeof v);}
static int released=0;
static azn_analysis_source opening(unsigned deck=0) {
    azn_analysis_source out={};out.size=sizeof out;out.version=1;
    assert(open_source(deck,load_epoch,load_generation,&out)==AZ_OK);return out;
}
int main() {
    assert(install_source(1,nullptr)==AZ_UNSUPPORTED);
    // Real AAPCS64 nontrivial x8 return exercised through the fixed profile entry.
    void *page=mmap((void*)0x178d000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (page!=(void*)0x178d000) {
        if(page!=MAP_FAILED) munmap(page,4096);
        puts("SKIP: native profile address unavailable"); return 77;
    }
    uint32_t jump[]={0x58000050,0xd61f0200};
    memcpy((void*)0x178d760,jump,sizeof jump);uintptr_t target=(uintptr_t)get_state;
    memcpy((void*)0x178d768,&target,8);
    assert(mprotect(page,4096,PROT_READ|PROT_EXEC)==0);
    __builtin___clear_cache((char*)page,(char*)page+4096);
    auto state=std::make_shared<Native>();
    put(*state,0,uintptr_t(0x2c5b0b8));put(*state,0x10,100);put(*state,0x14,100);put(*state,0x18,(unsigned char)1);
    fake_state=state;
    char path[]="/tmp/beatport-source-XXXXXX";int file=mkstemp(path);assert(file>=0);assert(write(file,"audio",5)==5);close(file);
    auto track=std::shared_ptr<Native>(new Native,[](Native*p){++released;delete p;});
    put(*track,0,uintptr_t(0x2c5b028));put(*track,0x28,uint64_t(123));put(*track,0x78,(const char*)path);
    WeakTrack weak=track;
    Native owner;int generation=11;
    put(owner,0x28,(uintptr_t)&generation);put(owner,0x160,uint64_t(123));memcpy(owner.bytes+0x68,load_id,12);
    original_wait=(void*)wait_result;installed.store(true);
    azn_analysis_source out={};out.size=sizeof out;out.version=1;
    native_result=1;assert(wait_decrypt((uintptr_t)&owner,&weak,generation)==1);
    assert(open_source(0,7,8,&out)==AZ_NOT_FOUND);
    native_result=0;put(*state,0x18,(unsigned char)0);wait_decrypt((uintptr_t)&owner,&weak,generation);
    assert(open_source(0,7,8,&out)==AZ_NOT_FOUND);
    put(*state,0x18,(unsigned char)1);put(*state,0x14,99);
    wait_decrypt((uintptr_t)&owner,&weak,generation);
    assert(open_source(0,7,8,&out)==AZ_NOT_FOUND);
    put(*state,0x14,100);put(*state,0xc,3);
    wait_decrypt((uintptr_t)&owner,&weak,generation);
    assert(open_source(0,7,8,&out)==AZ_NOT_FOUND);
    put(*state,0xc,0);wait_decrypt((uintptr_t)&owner,&weak,generation-1);
    assert(open_source(0,7,8,&out)==AZ_NOT_FOUND);
    wait_decrypt((uintptr_t)&owner,&weak,generation);
    load_id[9]^=1;assert(open_source(0,7,8,&out)==AZ_NOT_FOUND);load_id[9]^=1;
    auto first=opening();assert(!strcmp(first.audio_path,path));assert(!memcmp(first.track_id,load_id,12));
    assert(current_source(first.token)==AZ_OK);
    auto other=opening(1);assert(current_source(other.token)==AZ_OK);close_source(other.token);
    put(*state,0xc,5);assert(current_source(first.token)==AZ_STALE);put(*state,0xc,0);
    ++load_epoch;assert(current_source(first.token)==AZ_STALE);--load_epoch;
    loaded[0]=false;assert(current_source(first.token)==AZ_STALE);loaded[0]=true;
    azn_analysis_source many[7];for(auto &i:many)i=opening();
    assert(open_source(0,7,8,&out)==AZ_LIMIT);
    for(auto &i:many)close_source(i.token);
    track.reset();assert(released==0);assert(current_source(first.token)==AZ_OK);
    // Replacing the pathname cannot substitute different audio for this lease.
    char old[256];snprintf(old,sizeof old,"%s.old",path);assert(rename(path,old)==0);
    file=open(path,O_CREAT|O_WRONLY|O_EXCL,0600);assert(file>=0);assert(write(file,"other",5)==5);close(file);
    assert(current_source(first.token)==AZ_STALE);
    close_source(first.token);assert(released==1);assert(current_source(first.token)==AZ_STALE);
    close_source(first.token);assert(released==1);
    assert(open_source(0,7,8,&out)==AZ_NOT_FOUND);
    unlink(path);unlink(old);munmap(page,4096);
    puts("Beatport native resource lease: readiness, fences, ownership, multi-deck, limits, replacement passed");
}
