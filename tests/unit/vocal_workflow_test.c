/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "analysis_runtime.h"
#include "analysis_events.h"
#include "analysis_vocals.h"
#include "sdk_status_native.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AZMOD_STEM_DECKS 4
#define AZF_VOCAL_INDICATOR 0
#define AZF_VOCAL_IND_PREVIEW 1
#define AZF_VOCAL_IND_WAVE 2
#define AZF_VOCAL_EXTRACTION 3
static int options[4] = {1, 1, 1, 1};
static int az_on(int i) { return options[i]; }
static int az_active(int i) {
    (void) i;
    return 1;
}
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sm_cv = PTHREAD_COND_INITIALIZER;
static void sm_lock(void) { assert(!pthread_mutex_lock(&lock)); }
static void sm_unlock(void) { assert(!pthread_mutex_unlock(&lock)); }
static long long now = 1000;
static long long drag_now_ms(void) { return now; }
static void analysis_diagnostic(const char *text) { assert(text); }
static struct azstatus_native_receipt loads[4];
static int status_rc, file_rc[4], reads[4], stem_reads, stem_ready, flip_load;
static unsigned stem_generation = 1;
static az_analysis_status_v1 statuses[4];
int ds_native_load_seed(unsigned d, struct azstatus_native_receipt *out) {
    *out = loads[d];
    return out->load_generation != 0;
}
static int status_read(unsigned d, az_analysis_status_v1 *out) {
    *out = statuses[d];
    return status_rc;
}
static int (*azmod_analysis_status)(unsigned, az_analysis_status_v1 *) = status_read;
static unsigned file_activity = 4;
int analysis_vocal_track_loaded(unsigned d, uint64_t load, struct vd_track *out, unsigned *seconds) {
    assert(load == loads[d].load_generation);
    ++reads[d];
    if (flip_load) ++loads[d].load_generation;
    if (file_rc[d] != AZ_OK) return file_rc[d];
    *out = (struct vd_track) {.v = malloc(22), .n = 22, .hop = 1024, .rate = 22050};
    assert(out->v);
    memset(out->v, file_activity, 22);
    *seconds = 1;
    return AZ_OK;
}
static int envelope(int d, unsigned cid, const unsigned char **env, int *nb, int *ns,
                    long *frames, int *rate, unsigned *gen) {
    static unsigned char data[2000];
    assert(d >= 0 && cid == 42);
    ++stem_reads;
    if (!stem_ready) return 0;
    memset(data, 200, 1000);
    memset(data + 1000, 1, 1000);
    *env = data;
    *nb = 1000;
    *ns = 2;
    *frames = 441000;
    *rate = 44100;
    *gen = stem_generation;
    return 1;
}
static unsigned envelope_generation(int d) {
    (void) d;
    return stem_generation;
}
static void *symbol(void *scope, const char *name) {
    (void) scope;
    if (!strcmp(name, "azstem_track_env")) return (void *) envelope;
    if (!strcmp(name, "azstem_env_gen")) return (void *) envelope_generation;
    abort();
}
#define dlsym symbol
#define RTLD_DEFAULT NULL
static int paints;
static uint32_t ink;
static void color(void *g, uint32_t *value) {
    assert(g);
    ink = *value;
}
static void rectangle(void *g, int x, int y, int w, int h) {
    (void) x;
    (void) y;
    assert(g && w > 0 && h > 0 && (ink >> 24));
    ++paints;
}
static void repaint(void *p) { assert(p); }
#define EP_23af140 ((uintptr_t) color)
#define EP_23af680 ((uintptr_t) rectangle)
#define JUCE_COMPONENT_REPAINT ((uintptr_t) repaint)
#define EP_2d0b700 101u
#define EP_2cee580 102u
#define EP_2cfe3a8 103u
#define KT_DI2_VT 104u
#define KT_DI4_VT 105u
#define OV_WIDGET_VT 106u
#define PM_VIEW_VT 107u
#define PM_PLAYHEAD_VT 108u
static int ht_ptr_ok(uintptr_t p) { return p >= 0x10000; }
static int ht_screen_rect(uintptr_t p, int r[4]) {
    (void) p;
    (void) r;
    return 0;
}
static int pm_trackinfo_open(uintptr_t p) {
    (void) p;
    return 0;
}
static uintptr_t pm_deckbox(uintptr_t p, int d, int r[4]) {
    (void) p;
    (void) d;
    (void) r;
    return 0;
}
static uintptr_t pm_child(uintptr_t p, uintptr_t vt, int deep) {
    (void) p;
    (void) vt;
    (void) deep;
    return 0;
}
#include "../../src/azmod/vocal_indicator.c"
static int shown(int d) {
    sm_lock();
    int source = vx_show(d, NULL, NULL);
    sm_unlock();
    return source;
}
static void poll(void) {
    now += 1000;
    vx_poll();
}
static void fixture(void) {
    for (unsigned d = 0; d < 4; ++d) {
        vx_state(d, 0, 0);
        loads[d] = (struct azstatus_native_receipt) {.load_generation = d + 10, .epoch = 1, .unit = 1, .a = AZN_LOADED, .content_id = 42};
        statuses[d] = (az_analysis_status_v1) {.load_epoch = d + 10, .state = AZ_ANALYSIS_QUEUED};
        file_rc[d] = AZ_NOT_FOUND;
        reads[d] = 0;
    }
    options[0] = options[1] = options[2] = options[3] = 1;
    status_rc = AZ_OK;
    stem_reads = 0;
    stem_ready = 1;
    flip_load = 0;
    file_activity = 4;
    azmod_analysis_status = status_read;
}
static void generated_and_reused(void) {
    fixture();
    poll();
    assert(shown(0) == VD_NONE && !stem_reads);
    statuses[0].state = AZ_ANALYSIS_RUNNING;
    poll();
    assert(shown(0) == VD_NONE && !stem_reads);
    statuses[0].state = AZ_ANALYSIS_SAVED;
    poll();
    assert(shown(0) == VD_NONE);
    file_rc[0] = AZ_OK;
    poll();
    assert(shown(0) == VD_NONE); /* saved file is not final completion */
    file_rc[0] = AZ_OK;
    statuses[0].state = AZ_ANALYSIS_COMPLETE;
    poll();
    assert(shown(0) == VD_RB && !stem_reads);
    int before = reads[0];
    poll();
    assert(reads[0] == before);
    options[3] = 0;
    poll();
    assert(shown(0) == VD_RB && shown(1) == VD_STEM);
    uint8_t cols[100];
    assert(vx_cols(0, 100));
    vx_strip((void *) 1, 0, 0, 100, 4, g_vx_cols[0].col);
    assert(paints > 0);
    int64_t position = 0;
    vx_upd(0, 1, (long) &position, 1);
    assert(vx_wave_cols(0, 0, 100, 0, cols));
    ++loads[0].load_generation;
    assert(shown(0) == VD_NONE); /* revoke old painting before the next worker turn */
    file_activity = 0;
    poll();
    assert(shown(0) == VD_RB);
    assert(vx_cols(0, 100));
    for (unsigned i = 0; i < 100; ++i) assert(!g_vx_cols[0].col[i]);
}
static void fallback_and_retry(void) {
    fixture();
    statuses[0].state = AZ_ANALYSIS_FAILED;
    poll();
    assert(shown(0) == VD_STEM && shown(1) == VD_NONE);
    statuses[1].state = AZ_ANALYSIS_CANCELLED;
    poll();
    assert(shown(1) == VD_STEM);
    options[3] = 0;
    poll();
    assert(shown(2) == VD_STEM);
    options[3] = 1;
    assert(shown(2) == VD_NONE);
    poll();
    assert(shown(2) == VD_NONE);
    statuses[2].load_epoch = 999;
    statuses[2].state = AZ_ANALYSIS_FAILED;
    poll();
    assert(shown(2) == VD_NONE);
    statuses[2].load_epoch = loads[2].load_generation;
    poll();
    assert(shown(2) == VD_STEM);
    file_rc[2] = AZ_OK;
    poll();
    assert(shown(2) == VD_RB);
    stem_ready = 0;
    ++stem_generation;
    poll();
    assert(shown(0) == VD_NONE);
    options[0] = 0;
    poll();
    assert(!g_vx[2].t.v && shown(2) == VD_NONE);
}
static void unavailable_and_stale(void) {
    fixture();
    status_rc = AZ_BUSY;
    poll();
    assert(shown(0) == VD_NONE);
    status_rc = AZ_UNSUPPORTED;
    poll();
    assert(shown(0) == VD_STEM);
    fixture();
    azmod_analysis_status = NULL;
    poll();
    assert(shown(0) == VD_STEM);
    fixture();
    file_rc[0] = AZ_OK;
    flip_load = 1;
    poll();
    assert(shown(0) == VD_NONE && !g_vx[0].t.v);
    fixture();
    options[3] = 0;
    file_rc[0] = AZ_STALE;
    file_rc[1] = AZ_BUSY;
    poll();
    assert(shown(0) == VD_NONE && shown(1) == VD_NONE);
    loads[2].a = 0;
    poll();
    assert(shown(2) == VD_NONE);
    vocal_indicator_events_init();
    az_analysis_complete_v1 event = {.available_components = AZ_ANALYSIS_VOCALS};
    analysis_events_publish(&event);
    analysis_events_core_tick();
}
int main(void) {
    generated_and_reused();
    fallback_and_retry();
    unavailable_and_stale();
    for (int d = 0; d < 4; ++d) vx_state(d, 0, 0);
    puts("Vocal workflow: PVDI preference, wait, failure/disabled stems, replacement, instrumental and upstream drawing passed");
}
