/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/azcueexport.h"
#if __has_include("common/azcueobserver.h")
#include "common/azcueobserver.h"
#else
struct azsdk_cue_sample { uint64_t unit_generation, revision, observed_ns; uint32_t changed_index_mask, reserved; };
#endif
static struct azsdk_cue_sample samples[4];
static unsigned pending[4], active[4], take_calls[4], read_calls[4];
static unsigned native_mask, mask_calls;
unsigned azsdk_cue_capabilities(void) { ++mask_calls; return native_mask; }
int azsdk_cue_take(int deck, struct azsdk_cue_sample *out) {
    ++take_calls[deck]; if (!pending[deck] || !active[deck]) return 0;
    *out = samples[deck]; pending[deck] = 0; return 1;
}
int azsdk_cue_read(int deck, struct azsdk_cue_sample *out) {
    ++read_calls[deck]; if (!active[deck]) return 0; *out = samples[deck]; return 1;
}
#if __has_include("../../src/azstem/sdk_cue_export.c")
#include "../../src/azstem/sdk_cue_export.c"
#else
int azstem_cue_take_v1(int deck, struct azstem_cue_observation_v1 *out) { (void)deck;(void)out; return -1; }
int azstem_cue_observation_v1(int deck, struct azstem_cue_observation_v1 *out) { (void)deck;(void)out; return -1; }
#endif
_Alignas(struct azstem_cue_observation_v1) static unsigned char buffer[80];
static unsigned char before[80];
static struct azstem_cue_observation_v1 *prepare(uint32_t size, uint32_t version) {
    memset(buffer,0xa5,sizeof buffer); memcpy(buffer,&size,4);memcpy(buffer+4,&version,4);
    memcpy(before,buffer,sizeof before); return (struct azstem_cue_observation_v1 *)buffer;
}
static void untouched(void) { assert(!memcmp(buffer,before,sizeof buffer)); }
int main(void) {
    azstem_cue_observer_mask_v1_fn observer_mask=(azstem_cue_observer_mask_v1_fn)
        dlsym(RTLD_DEFAULT,"azstem_cue_observer_mask_v1");
    const unsigned masks[] = {0,1,5,15,UINT32_C(0x80000005),UINT32_MAX,0,0};
    for (unsigned i=0;i<sizeof masks/sizeof masks[0];++i) {
        native_mask=masks[i];
        assert(azsdk_cue_capabilities()==masks[i]);
        printf("CUE_EXPORT_NATIVE_MASK_OK mask=%u before_receipts=1\n",native_mask);fflush(stdout);
        unsigned before_mask=mask_calls;
        assert((observer_mask ? observer_mask() : 0)==(native_mask&15u) &&
               "missing actual cue observer support mask export");
        if (observer_mask) assert(mask_calls==before_mask+1);
        for (unsigned d=0;d<4;++d)
            assert(!take_calls[d] && !read_calls[d] && !samples[d].unit_generation && !pending[d]);
    }
    // Final zero cases model native disabled/retired registration support.
    // Receipt availability is tested below and never consulted by the mask.
    for (int d=0;d<4;d++) { samples[d]=(struct azsdk_cue_sample){100+d,10+d,1000+d,1u<<d,0}; active[d]=pending[d]=1; }
    struct azsdk_cue_sample native;
    assert(azsdk_cue_read(2,&native) && native.unit_generation==102);
    puts("CUE_EXPORT_NATIVE_BOUNDARY_OK: typed collection receipt available");fflush(stdout);
    for (int d=0;d<4;d++) {
        struct azstem_cue_observation_v1 *out=prepare(80,1);
        assert(azstem_cue_take_v1(d,out)==0 && "missing actual cue collection export");
        assert(out->size==80 && out->version==1 && out->unit_generation==100u+d &&
               out->revision==10u+d && out->collected_ns==1000u+d && out->changed_index_mask==(1u<<d) && !out->reserved);
        assert(!memcmp(buffer+40,before+40,40));
        out=prepare(40,1); assert(azstem_cue_take_v1(d,out)==1);untouched();
        assert(azstem_cue_observation_v1(d,out)==0 && out->unit_generation==100u+d);
        for(uint32_t size=0;size<40;size++) {
            unsigned count=take_calls[d]+read_calls[d];out=prepare(size,1);
            assert(azstem_cue_take_v1(d,out)==-1 && azstem_cue_observation_v1(d,out)==-1);untouched();
            assert(take_calls[d]+read_calls[d]==count);
        }
        out=prepare(40,2);assert(azstem_cue_take_v1(d,out)==-1);untouched();
        active[d]=0;out=prepare(40,1);assert(azstem_cue_take_v1(d,out)==-1 && azstem_cue_observation_v1(d,out)==-1);untouched();
        active[d]=1;
    }
    unsigned sample_calls=0;
    for (unsigned d=0;d<4;++d) sample_calls+=take_calls[d]+read_calls[d];
    assert(samples[0].unit_generation && samples[0].revision);
    native_mask=15;assert(observer_mask && observer_mask()==15);
    native_mask=0;assert(observer_mask()==0); // disabled registration with an old collected receipt
    active[0]=0;assert(observer_mask()==0); // retirement cannot be revived by that receipt
    active[0]=1;
    unsigned after_sample_calls=0;
    for (unsigned d=0;d<4;++d) after_sample_calls+=take_calls[d]+read_calls[d];
    assert(sample_calls==after_sample_calls);
    for (int d=-1;d<=4;d+=5) { struct azstem_cue_observation_v1 *out=prepare(40,1);assert(azstem_cue_take_v1(d,out)==-1);untouched(); }
    assert(azstem_cue_take_v1(0,NULL)==-1 && azstem_cue_observation_v1(0,NULL)==-1);
    struct azsdk_cue_sample good=samples[0];
    for(unsigned bad=0;bad<6;bad++) {
        samples[0]=good;
        if (bad == 0) samples[0].unit_generation = 0;
        if (bad == 1) samples[0].revision = 0;
        if (bad == 2) samples[0].observed_ns = 0;
        if (bad == 3) samples[0].changed_index_mask = 0;
        if (bad == 4) samples[0].changed_index_mask = 512;
        if (bad == 5) samples[0].reserved = 1;
        pending[0]=1;struct azstem_cue_observation_v1 *out=prepare(40,1);
        assert(azstem_cue_take_v1(0,out)==-1);untouched();
        assert(azstem_cue_observation_v1(0,out)==-1);untouched();
    }
    puts("PASS cue export: receipt-independent registration mask, four decks, prefix-before-drain validation and unknown tails");
}
