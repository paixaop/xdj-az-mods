/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual SDK registration/tick/binding code. Native observation is a typed
 * boundary double here; the combined GNU fixture also links the real observer. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "azmod-mod.h"
#include "sdk_analysis.h"
#include "../common/azcueexport.h"
#define AZMOD_STEM_DECKS 4
#define AZMOD_PAD_DECKS 2
#define AZCM_LOADED 2
#define CM_RA() ((void *)1)
typedef struct { int state; uintptr_t lo, hi; } cm_mod;
static cm_mod g_cm[2] = {{AZCM_LOADED, 1, UINTPTR_MAX}, {0, 1, UINTPTR_MAX}};
static int __attribute__((unused)) master_state_supported(void) { return 0; }
static int g_cm_loading, g_st_enabled = 1;
static cm_mod *cm_self(const char *id, void *ra, const char *fn) {
    (void)ra; (void)fn;
    return !strcmp(id,"one") ? &g_cm[0] : !strcmp(id,"two") ? &g_cm[1] : NULL;
}
#define cm_logf(...) ((void)0)
static struct azstem_cue_observation_v1 values[4];
static int statuses[4] = {1,1,1,1}, calls[4];
static unsigned registered = 15;
static unsigned native_mask(void) { return registered; }
static unsigned events[2][4], other;
static int native_take(int deck, struct azstem_cue_observation_v1 *out) {
    ++calls[deck];
    if (statuses[deck]) return statuses[deck];
    uint32_t size = out->size; *out = values[deck]; out->size = size;
    statuses[deck] = 1; return 0;
}
static int native_read(int deck, struct azstem_cue_observation_v1 *out) {
    if (statuses[deck] == -1 || !values[deck].unit_generation) return -1;
    uint32_t size = out->size; *out = values[deck]; out->size = size; return 0;
}
static int master_state_read(int *deck, uint64_t *revision, uint64_t *ns) {
    (void)deck; (void)revision; (void)ns; return 0;
}
static int cm_tempo_read(int deck, double *bpm, uint64_t *generation, uint64_t *revision, uint64_t *ns) {
    (void)deck; (void)bpm; (void)generation; (void)revision; (void)ns; return 0;
}
static void *fixture_dlsym(void *scope, const char *name) {
    (void)scope;
    if (!strcmp(name,"azstem_cue_take_v1")) return (void *)native_take;
    if (!strcmp(name,"azstem_cue_observer_mask_v1")) return (void *)native_mask;
    return !strcmp(name,"azstem_cue_observation_v1") ? (void *)native_read : NULL;
}
#define RTLD_DEFAULT ((void *)0)
#define dlsym fixture_dlsym
#include SDKCUEEVENTS_INC
#undef dlsym
static void changed(int deck, int what, void *user) {
    if (what == AZ_DECK_CUES_CHANGED) ++events[(uintptr_t)user][deck]; else ++other;
}
static void next(int deck, uint64_t gen, uint64_t revision) {
    values[deck] = (struct azstem_cue_observation_v1){40,1,gen,revision,1000+revision,1,0};
    statuses[deck] = 0;
}
static void revision(int deck, int expected) {
    az_deck_snapshot out = {.size = sizeof out};
    assert(sd_deck_snapshot_v1(deck, &out) == AZ_OK);
    assert(!out.load_generation && !out.revision && !out.native_load_generation && !out.track.source_id);
    if (expected) {
        assert((out.valid & AZ_LIVE_CUE_REVISION) && "missing public cue revision");
        assert(out.cue_unit_generation == values[deck].unit_generation &&
               out.cue_revision == values[deck].revision && out.cue_observed_ns == values[deck].collected_ns);
    } else assert(!(out.valid & AZ_LIVE_CUE_REVISION) && !out.cue_unit_generation && !out.cue_revision && !out.cue_observed_ns);
    const size_t prefixes[] = {offsetof(az_deck_snapshot, loop_unit_generation), offsetof(az_deck_snapshot, cue_unit_generation), sizeof out};
    for (unsigned i=0;i<sizeof prefixes/sizeof prefixes[0];i++) {
        _Alignas(az_deck_snapshot) unsigned char bytes[sizeof out+16];
        memset(bytes,0xa5,sizeof bytes);uint32_t size=prefixes[i];memcpy(bytes,&size,4);
        assert(sd_deck_snapshot_v1(deck,(az_deck_snapshot *)bytes)==AZ_OK);
        for(size_t k=size;k<sizeof bytes;k++)assert(bytes[k]==0xa5);
    }
}
int main(void) {
    _Static_assert(AZ_DECK_LOADED == 1 && AZ_DECK_EJECTED == 2 && AZ_DECK_PLAY == 3 &&
                   AZ_DECK_STOP == 4 && AZ_DECK_CUES_CHANGED == 5, "append-only event constants");
    assert(sd_on_deck_event("one",changed,(void *)0) == 0);
    g_cm_loading = 1; assert(sd_on_deck_event("two",changed,(void *)1) == 0); g_cm_loading = 0;
    sdk_deck_tick(100);
    assert(!other);
    puts("SDK_CUE_EVENT_BASELINE_OK: real SDK init registration and UI tick ran"); fflush(stdout);
    for(int deck=0;deck<4;deck++)revision(deck,0);
    assert((sd_live_fields() & AZ_LIVE_CUE_REVISION) && "registered cue observer capability must not require a receipt");
    registered=0;assert(!(sd_live_fields() & AZ_LIVE_CUE_REVISION));registered=15;
    for (int deck=0;deck<4;deck++) {
        next(deck,10+deck,1); sdk_deck_tick(200+deck);
        assert(events[0][deck] == 1 && "missing public native cue-change deck event");
        revision(deck,1);
        assert(sd_live_fields() & AZ_LIVE_CUE_REVISION);
        assert(!events[1][deck]);
        for (int other_deck=deck+1;other_deck<4;other_deck++) { assert(!events[0][other_deck]);revision(other_deck,0); }
        sdk_deck_tick(300+deck); assert(events[0][deck] == 1);
        next(deck,10+deck,2); sdk_deck_tick(400+deck); assert(events[0][deck] == 2);
        revision(deck,1);
    }
    assert(calls[0] && calls[1] && calls[2] && calls[3]);
    g_cm[0].state = 0; g_cm[1].state = AZCM_LOADED;
    for (int deck=0;deck<4;deck++) { next(deck,10+deck,3); sdk_deck_tick(500+deck); assert(events[0][deck] == 2 && events[1][deck] == 1); }
    for (int deck=0;deck<4;deck++) { statuses[deck] = -1; sdk_deck_tick(600+deck); assert(events[1][deck] == 2);revision(deck,0); }
    sdk_deck_tick(700); for (int deck=0;deck<4;deck++) assert(events[1][deck] == 2);
    // Missing/malformed native data does not become a valid event.
    for (unsigned fault=0;fault<7;fault++) {
        next(0,20,4+fault);
        if (fault==0) values[0].version=9;
        if (fault==1) values[0].unit_generation=0;
        if (fault==2) values[0].revision=0;
        if (fault==3) values[0].collected_ns=0;
        if (fault==4) values[0].changed_index_mask=0;
        if (fault==5) values[0].reserved=1;
        if (fault==6) values[0].changed_index_mask=512;
        sdk_deck_tick(800+fault); assert(events[1][0] == 2);
        revision(0,0);
    }
    next(0,30,100); sdk_deck_tick(900);revision(0,1);
    g_st_enabled=0;revision(0,0);assert(!(sd_live_fields() & AZ_LIVE_CUE_REVISION));g_st_enabled=1;
    assert(!other);
    puts("PASS SDK cue events/revisions: four decks, GUI collection, prefix compatibility, opt-out, retirement and malformed refusal");
}
