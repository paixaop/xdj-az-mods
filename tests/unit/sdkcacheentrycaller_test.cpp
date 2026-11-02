/* SPDX-License-Identifier: GPL-3.0-only */
/* Exact entry capture only. A saved register/caller PC is not itself a native
 * owner. The reply glue must bind them to the reviewed native promotion site. */
#include "azcacheentry.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#define CHECK(x,why) do { if (!(x)) { std::fprintf(stderr,"FAIL cache-caller: %s\n",why);std::abort(); } } while(0)
#if !defined(__aarch64__) || !defined(__linux__)
int main() { return 77; }
#else
struct Pair { std::uint64_t result, x21; };
extern "C" Pair cache_call_with_x21(void *,std::uint64_t,std::uint64_t);
extern "C" void cache_native_x21_return(void);
asm(".text\n.p2align 2\n.global cache_call_with_x21\n.type cache_call_with_x21,%function\n"
    "cache_call_with_x21:\n.cfi_startproc\nstp x21,x30,[sp,#-16]!\n.cfi_def_cfa_offset 16\n"
    ".cfi_offset 21,-16\n.cfi_offset 30,-8\nmov x9,x0\nmov x21,x1\nmov x0,x2\nblr x9\n"
    ".global cache_native_x21_return\ncache_native_x21_return:\nmov x1,x21\n"
    "ldp x21,x30,[sp],#16\n.cfi_def_cfa_offset 0\n.cfi_restore 21\n.cfi_restore 30\nret\n"
    ".cfi_endproc\n.size cache_call_with_x21,.-cache_call_with_x21\n");
static unsigned entered, originals;
static std::uint64_t captured_x21, captured_link;
static bool throwing;
static std::uint64_t original(std::uint64_t value) {
    ++originals;
    if (throwing) throw std::runtime_error("native caller exception");
    return value ^ UINT64_C(0xfedcba9876543210);
}
extern "C" void azsdk_cache_dispatch_call(azce_call *call) {
    ++entered;
#ifdef AZCE_CALLER_X21
    captured_x21=call->caller_x21;
#else
    captured_x21=0; // old entry has no such observation; reach runtime RED safely
#endif
    captured_link=call->caller_link;
    azsdk_cache_invoke_original(call);
}
int main() {
    const std::uint64_t witness=UINT64_C(0x0123456789abcdef), argument=UINT64_C(0xdeadbeef12345678);
    auto baseline=cache_call_with_x21(reinterpret_cast<void *>(original),witness,argument);
    CHECK(baseline.result==(argument^UINT64_C(0xfedcba9876543210)) && baseline.x21==witness,
          "unmodified native caller/result baseline");
    puts("NATIVE_X21_CALLER_BASELINE_OK");fflush(stdout);
    for(unsigned kind=0;kind<AZCE_KIND_COUNT;++kind) {
        azsdk_cache_entry_originals[kind]=reinterpret_cast<void *>(original);
        entered=originals=0;
        auto result=cache_call_with_x21(azsdk_cache_entry(kind),witness+kind,argument);
        CHECK(result.result==baseline.result && result.x21==witness+kind && entered==1 && originals==1,
              "native entry preserves original argument/result/callee-saved X21 exactly once");
        CHECK(captured_x21==witness+kind,"missing original caller X21 capture before C++ dispatch");
        CHECK(captured_link==reinterpret_cast<std::uintptr_t>(cache_native_x21_return),
              "native caller link remains exact");
        throwing=true;bool caught=false;
        try { (void)cache_call_with_x21(azsdk_cache_entry(kind),witness+kind,argument); }
        catch(const std::runtime_error &) { caught=true; }
        throwing=false;
        CHECK(caught,"caller capture keeps native exception unwind intact");
    }
    puts("PASS cache caller: original incoming X21/LR captured; arguments/results/CFI preserved for every entry");
}
#endif
