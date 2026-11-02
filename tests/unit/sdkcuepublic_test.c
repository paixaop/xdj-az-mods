/* SPDX-License-Identifier: GPL-3.0-only */
/* Real private export + public registration/GUI tick. Native listener and
 * native ownership fixtures live in the separately linked C++ translation unit. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "azmod-mod.h"
#include "sdk_analysis.h"
#include "common/azcueexport.h"
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
    return !strcmp(id, "one") ? &g_cm[0] : !strcmp(id, "two") ? &g_cm[1] : NULL;
}
#define cm_logf(...) ((void)0)
#include "../../src/azstem/sdk_cue_export.c"
static int master_state_read(int *deck, uint64_t *revision, uint64_t *ns) {
    (void)deck; (void)revision; (void)ns; return 0;
}
static int cm_tempo_read(int deck, double *bpm, uint64_t *generation, uint64_t *revision, uint64_t *ns) {
    (void)deck; (void)bpm; (void)generation; (void)revision; (void)ns; return 0;
}
static void *fixture_dlsym(void *scope, const char *name) {
    (void)scope;
    if (!strcmp(name, "azstem_cue_take_v1")) return (void *)azstem_cue_take_v1;
    if (!strcmp(name, "azstem_cue_observer_mask_v1")) return (void *)azstem_cue_observer_mask_v1;
    return !strcmp(name, "azstem_cue_observation_v1") ? (void *)azstem_cue_observation_v1 : NULL;
}
#define RTLD_DEFAULT ((void *)0)
#define dlsym fixture_dlsym
#include SDKCUEEVENTS_INC
#undef dlsym
static unsigned changes[2][4];
static void changed(int deck, int what, void *user) {
    assert(deck >= 0 && deck < 4 && what == AZ_DECK_CUES_CHANGED);
    ++changes[(uintptr_t)user][deck];
}
void cue_public_init(void) {
    assert(sd_on_deck_event("one", changed, (void *)0) == 0);
    g_cm_loading = 1;
    assert(sd_on_deck_event("two", changed, (void *)1) == 0);
    g_cm_loading = 0;
}
void cue_public_tick(void) { sdk_deck_tick(100); }
unsigned cue_public_count(unsigned owner, unsigned deck) {
    assert(owner < 2 && deck < 4);
    return changes[owner][deck];
}
void cue_public_switch_owner(void) { g_cm[0].state = 0; g_cm[1].state = AZCM_LOADED; }
void cue_public_capability(int expected) {
    assert(!!(sd_live_fields() & AZ_LIVE_CUE_REVISION) == !!expected);
}
void cue_public_revision(unsigned deck, int expected) {
    struct azstem_cue_observation_v1 sample = {40,1,0,0,0,0,0};
    az_deck_snapshot out = {.size = sizeof out};
    assert(sd_deck_snapshot_v1((int)deck, &out) == AZ_OK);
    assert(!out.load_generation && !out.revision && !out.native_load_generation && !out.track.source_id);
    if (expected) {
        assert(azstem_cue_observation_v1((int)deck,&sample) == 0);
        assert((out.valid & AZ_LIVE_CUE_REVISION) && out.cue_unit_generation == sample.unit_generation &&
               out.cue_revision == sample.revision && out.cue_observed_ns == sample.collected_ns);
    } else assert(!(out.valid & AZ_LIVE_CUE_REVISION) && !out.cue_unit_generation && !out.cue_revision && !out.cue_observed_ns);
}
