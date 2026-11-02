/* SPDX-License-Identifier: GPL-3.0-only */
#include "../common/azslipobserver.h"
#include "../common/azsha256.h"
#include <atomic>
#include <cstring>
extern "C" {
uintptr_t azsdk_slip_getter_returns[3];
uint32_t azsdk_slip_getter_hooks_v1;
void azsdk_slip_getter_entry_0(void),azsdk_slip_getter_entry_1(void),azsdk_slip_getter_entry_2(void);
}
extern "C" int azsdk_slip_getters_install(const azsdk_slip_native_profile *p,azsdk_slip_installer install,void *context) {
    static std::atomic<bool> attempted{false};
    if(!p || !install || attempted.exchange(true))return 0;
    if(!p->producer || p->producer_bytes<16 || p->producer_bytes>65536 || p->producer>UINTPTR_MAX-p->producer_bytes)return 0;
    char hash[65];azsha256_hex(reinterpret_cast<const void *>(p->producer),p->producer_bytes,hash);
    if(std::strcmp(hash,p->producer_sha256))return 0;
    constexpr uint32_t expected[]={0xf9401021,0xf9403421,0xf9403821};
    for(unsigned i=0;i<3;i++) {
        const auto site=p->getter_sites[i];uint32_t words[2];
        if((site&3) || site<p->producer || site>p->producer+p->producer_bytes-8)return 0;
        std::memcpy(words,reinterpret_cast<const void *>(site),sizeof words);
        if(words[0]!=expected[i] || words[1]!=0xd63f0020)return 0;
        for(unsigned j=0;j<i;j++)if(site==p->getter_sites[j])return 0;
    }
    void (*const entries[])(void)={azsdk_slip_getter_entry_0,azsdk_slip_getter_entry_1,azsdk_slip_getter_entry_2};
    static void *originals[3];
    for(unsigned i=0;i<3;i++) {
        /* The island is published only after this continuation is available.
         * We execute the one checked displaced LDR in the bridge, never the
         * installer's generic trampoline (which would execute it twice). */
        __atomic_store_n(&azsdk_slip_getter_returns[i],p->getter_sites[i]+4,__ATOMIC_RELEASE);
        if(!install(p->getter_sites[i],reinterpret_cast<void *>(entries[i]),&originals[i],context) || !originals[i])return 0;
        __atomic_fetch_or(&azsdk_slip_getter_hooks_v1,1u<<i,__ATOMIC_RELEASE);
    }
    return 1;
}
