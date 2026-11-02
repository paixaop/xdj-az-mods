/* SPDX-License-Identifier: GPL-3.0-only */
/* AAPCS64 register transparency; synthetic native functions only. No stack args,
 * firmware entry/type declarations, owning object or public capability. */
#include "azcacheentry.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#define REQUIRE(x,why) do{if(!(x)){std::fprintf(stderr,"FAIL cache-entry-fp: %s\n",why);std::abort();}}while(0)
#if !defined(__aarch64__) || !defined(__linux__)
int main(){std::puts("SKIP: AArch64 Linux required");return 77;}
#else
typedef std::uint64_t Vec __attribute__((vector_size(16)));
struct HFA { double v[4]; };
struct HVA { Vec v[4]; };
static std::uint64_t ints[8];
static double floats[8];
static Vec vectors[8];
static unsigned originals,entered,completed,active,expected_kind;
static bool throwing;
struct Scope{Scope(){++active;}~Scope(){--active;}};
static void clobber(){asm volatile(
    "movi v0.16b,#0x90\nmovi v1.16b,#0x91\nmovi v2.16b,#0x92\nmovi v3.16b,#0x93\n"
    "movi v4.16b,#0x94\nmovi v5.16b,#0x95\nmovi v6.16b,#0x96\nmovi v7.16b,#0x97\n"
    :::"v0","v1","v2","v3","v4","v5","v6","v7","memory");}
extern "C" void azsdk_cache_dispatch_call(azce_call *call){
    ++entered;Scope scope;REQUIRE(call&&call->kind==expected_kind&&call->original,"actual kind/original routing");
    clobber();azsdk_cache_invoke_original(call);++completed;clobber();
}
#define IARGS std::uint64_t a,std::uint64_t b,std::uint64_t c,std::uint64_t d,std::uint64_t e,std::uint64_t f,std::uint64_t g,std::uint64_t h
#define FARGS double p0,double p1,double p2,double p3,double p4,double p5,double p6,double p7
#define VARGS Vec p0,Vec p1,Vec p2,Vec p3,Vec p4,Vec p5,Vec p6,Vec p7
#define IVALUES ints[0],ints[1],ints[2],ints[3],ints[4],ints[5],ints[6],ints[7]
#define FVALUES floats[0],floats[1],floats[2],floats[3],floats[4],floats[5],floats[6],floats[7]
#define VVALUES vectors[0],vectors[1],vectors[2],vectors[3],vectors[4],vectors[5],vectors[6],vectors[7]
static HFA native_floats(IARGS,FARGS){
    ++originals;std::uint64_t actual[]={a,b,c,d,e,f,g,h};double fp[]={p0,p1,p2,p3,p4,p5,p6,p7};
    REQUIRE(!std::memcmp(actual,ints,sizeof actual),"integer arguments coexist with floating register arguments");
    REQUIRE(!std::memcmp(fp,floats,sizeof fp),"floating/vector arguments changed by observer");
    if(throwing)throw std::runtime_error("native FP exception");
    return{{p0,p2,p5,p7}};
}
static HVA native_vectors(IARGS,VARGS){
    ++originals;std::uint64_t actual[]={a,b,c,d,e,f,g,h};Vec fp[]={p0,p1,p2,p3,p4,p5,p6,p7};
    REQUIRE(!std::memcmp(actual,ints,sizeof actual),"integer arguments coexist with full SIMD inputs");
    REQUIRE(!std::memcmp(fp,vectors,sizeof fp),"all128bits of eight vector arguments preserved");
    return{{p0,p2,p5,p7}};
}
template<class F>static F install(unsigned kind,F original){
    expected_kind=kind;azsdk_cache_entry_originals[kind]=reinterpret_cast<void*>(original);
    auto entry=azsdk_cache_entry(kind);REQUIRE(entry,"native target has a distinct entry/original route");return reinterpret_cast<F>(entry);
}
static void reset(){originals=entered=completed=active=0;}
int main(){
    for(unsigned i=0;i<8;++i){ints[i]=0xaabbccdd00000000ull+i;floats[i]=double(i)+0.125;vectors[i]=Vec{0x8899aabb00000000ull+i,0x1122334400000000ull-i};}
    const HFA expected={{floats[0],floats[2],floats[5],floats[7]}};
    auto native=native_floats(IVALUES,FVALUES);REQUIRE(!std::memcmp(&native,&expected,sizeof native),"unmodified native FP result");
    std::puts("NATIVE_FP_BASELINE_OK:8integer+8FP inputs,4FP results");std::fflush(stdout);
    for(unsigned kind=0;kind<AZCE_KIND_COUNT;++kind){
        auto fn=install(kind,native_floats);reset();auto result=fn(IVALUES,FVALUES);
        REQUIRE(!std::memcmp(&result,&expected,sizeof result),"floating/vector results changed by observer");
        REQUIRE(originals==1&&entered==1&&completed==1&&!active,"exact native/observer forwarding count");
        throwing=true;reset();bool caught=false;
        try{(void)fn(IVALUES,FVALUES);}catch(const std::runtime_error &x){caught=!std::strcmp(x.what(),"native FP exception");}
        throwing=false;REQUIRE(caught&&originals==1&&entered==1&&!completed&&!active,"expanded frame preserves native exception CFI");
        auto vf=install(kind,native_vectors);reset();auto vr=vf(IVALUES,VVALUES);
        const HVA ve={{vectors[0],vectors[2],vectors[5],vectors[7]}};
        REQUIRE(!std::memcmp(&vr,&ve,sizeof vr)&&originals==1&&entered==1&&completed==1&&!active,"all128bits of four SIMD result registers survive observer");
    }
    std::puts("PASS cache entry FP:8GPR+8SIMD args,4SIMD results,all kinds,exact forwarding and exception CFI");
}
#endif
