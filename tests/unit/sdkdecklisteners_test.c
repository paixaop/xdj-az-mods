/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual public event registration + tick. Only mod loader/auth and private
 * cue collection boundaries are fixtures; listener capacity is production code. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "azmod-mod.h"
#include "common/azcueexport.h"
#define AZMOD_STEM_DECKS 4
#define AZMOD_PAD_DECKS 2
#define AZCM_LOADED 2
#define CM_RA() ((void *)1)
typedef struct { int state; uintptr_t lo, hi; } cm_mod;
static cm_mod g_cm[12];
static int g_cm_loading = -1, g_st_enabled = 1;
static cm_mod *cm_self(const char *id, void *ra, const char *fn) {
    unsigned owner;
    (void)ra; (void)fn;
    return sscanf(id,"m%u",&owner)==1 && owner<12 ? &g_cm[owner] : NULL;
}
#define cm_logf(...) ((void)0)
static unsigned calls[12][4], pending;
static uint64_t revision;
static int native_take(int deck, struct azstem_cue_observation_v1 *out) {
    if (!(pending & (1u<<deck))) return 1;
    pending &= ~(1u<<deck);
    *out = (struct azstem_cue_observation_v1){40,1,100u+(unsigned)deck,revision,revision+1000,1,0};
    return 0;
}
static void *fixture_dlsym(void *scope, const char *name) {
    (void)scope;
    return !strcmp(name,"azstem_cue_take_v1") ? (void *)native_take : NULL;
}
#define RTLD_DEFAULT ((void *)0)
#define dlsym fixture_dlsym
#include SDKLISTENERS_INC
#undef dlsym
static void changed(int deck, int what, void *user) {
    assert(what==AZ_DECK_CUES_CHANGED);
    ++calls[(uintptr_t)user][deck];
}
static int subscribe(unsigned owner) {
    char id[16];snprintf(id,sizeof id,"m%u",owner);
    return sd_on_deck_event(id,changed,(void *)(uintptr_t)owner);
}
static void tick(void) { ++revision;pending=15;sdk_deck_tick(100); }
static void failed_initializations(void) {
    for(unsigned owner=0;owner<8;owner++) {
        g_cm_loading=(int)owner;
        for(unsigned i=0;i<4;i++)assert(subscribe(owner)==0);
        g_cm[owner].state=3; // init returned failure: callbacks must never run
        g_cm_loading=-1;
    }
    tick();
    puts("FAILED_INIT_EVENT_BASELINE_OK: eight native-event owners failed without callbacks");fflush(stdout);
    g_cm_loading=8;
    assert(subscribe(8)==0 && "failed init event subscriptions must release later owner capacity");
    for(unsigned i=1;i<4;i++)assert(subscribe(8)==0);
    assert(subscribe(8)==-1); // preserve the per-owner limit during init
    g_cm[8].state=AZCM_LOADED;g_cm_loading=-1;
    g_cm_loading=9;assert(subscribe(9)==0);
    tick(); // a current init is not yet callable and must retain its subscription
    assert(calls[8][0]==4 && !calls[9][0]);
    g_cm[9].state=AZCM_LOADED;g_cm_loading=-1;tick();
    for(unsigned d=0;d<4;d++)assert(calls[8][d]==8 && calls[9][d]==1);
    for(unsigned owner=0;owner<8;owner++)for(unsigned d=0;d<4;d++)assert(!calls[owner][d]);
    puts("PASS event init cancellation: failed owners reclaim capacity; current init and loaded owners survive");
}
static void loaded_capacity(void) {
    for(unsigned owner=0;owner<8;owner++) {
        g_cm_loading=(int)owner;
        for(unsigned i=0;i<4;i++)assert(subscribe(owner)==0);
        assert(subscribe(owner)==-1);g_cm[owner].state=AZCM_LOADED;
    }
    g_cm_loading=8;assert(subscribe(8)==-1);g_cm_loading=-1;tick();
    for(unsigned owner=0;owner<8;owner++)for(unsigned d=0;d<4;d++)assert(calls[owner][d]==4);
    puts("PASS event limits:32 loaded subscriptions preserved; per-owner and global bounds enforced");
}
static void mixed_retirement(void) {
    for(unsigned owner=0;owner<8;owner++) {
        g_cm_loading=(int)owner;
        for(unsigned i=0;i<4;i++)assert(subscribe(owner)==0);
        g_cm[owner].state=AZCM_LOADED;
    }
    for(unsigned owner=1;owner<8;owner+=2)g_cm[owner].state=3;
    g_cm_loading=8;assert(subscribe(8)==0);g_cm[8].state=AZCM_LOADED;g_cm_loading=-1;
    tick();
    for(unsigned owner=0;owner<8;owner++)for(unsigned d=0;d<4;d++)
        assert(calls[owner][d] == (owner%2 ? 0u : 4u));
    for(unsigned d=0;d<4;d++)assert(calls[8][d]==1);
    puts("PASS event compaction: interleaved retired entries preserve loaded callback owners and userdata");
}
int main(int argc,char **argv) {
    assert(argc==2);
    for(unsigned i=0;i<12;i++) { g_cm[i].lo=1;g_cm[i].hi=UINTPTR_MAX; }
    if(!strcmp(argv[1],"failed"))failed_initializations();
    else if(!strcmp(argv[1],"mixed"))mixed_retirement();
    else loaded_capacity();
}
